"""What the host reads of the guest context, against what it declares (nx2/lift2/host_effects.py): host/*.c compiled to
LLVM IR (-O1, no inlining), and every load from a context pointer traced through the functions it is passed to.

- An entry hook (entry_0x*, entry_sym_*) reads x0-x7 and x30 (host/hos.h HookArgs) and passes the call only to the
  hook_* functions; one that calls hook_call_guest declares SP in host_effects.HOOK_READS.
- An HLE'd function (imp_*, and the NVN procs n_nvn*) reads x19-x29 or v8-v15 of its caller only where
  host_effects.HLE_READS declares it.
- No host function compares the return address with a guest address any more except as a label: checked by the source
  (host/*.c) having no `x[30] ==` / `x[30] !=` / `lr ==` / `lr !=` against a literal outside the listed diagnostics.
No game is used.

python -I -B tests/test_host_context_reads.py
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import rtgen  # noqa: E402
from lift2 import host_effects  # noqa: E402

DEF = re.compile(r"^define [^@]*@([\w.$]+)\((.*)\)[^{]*\{")
HOOK_API = {"hook_return", "hook_set_arg", "hook_run", "hook_result", "hook_call_guest"}
GUEST = re.compile(r"^(a64_icall|a64_ijump|hos_call_guest|hos_sj_call)$")
# (lr comparisons that only label or count: allowed)
LR_LABELS = {
    "host/direct_layer_ahead.c": "jobs recorded from another caller (a count for the log)",
    "host/hos_thread.c": "MKW's idle thread loop skipped in the stall dump (hos_game_is_mkw)",
    "host/nvn_native.c": "MKW's frame pacer (hos_game_is_mkw); HOS_PRESENT_NOWAIT (an experiment, off)",
}


def field(off):
    if off < 248:
        return "x%d" % (off // 8)
    if off < 256:
        return "sp"
    if 0x110 <= off < 0x310:
        return "v%d" % ((off - 0x110) // 16)
    return "ctx+%#x" % off


def trace(folder):
    """{function: {"params": [...], "R": [set per param], "calls": [(param, offset, callee, arg)], "guest": bool}}"""
    funcs = {}
    for path in sorted(glob.glob(os.path.join(folder, "*.ll"))):
        cur = ptr = None
        for line in open(path, errors="replace"):
            m = DEF.match(line)
            if m:
                names = [(re.search(r"(%[\w.]+)\s*$", p.strip()) or [None, None])[1] if p.strip().startswith("ptr")
                         else None for p in re.split(r",(?![^(]*\))", m.group(2))]
                cur = funcs.setdefault(m.group(1), {"params": names, "R": [set() for _ in names], "calls": [],
                                                    "guest": False})
                ptr = {n: (i, 0) for i, n in enumerate(names) if n}
                continue
            if cur is None:
                continue
            if line.startswith("}"):
                cur = None
                continue
            g = re.match(r"\s*(%[\w.]+) = getelementptr (?:inbounds )?(?:nuw )?i8, ptr (%[\w.]+), i64 (-?\d+)", line)
            if g and g.group(2) in ptr:
                i, o = ptr[g.group(2)]
                ptr[g.group(1)] = (i, o + int(g.group(3)))
                continue
            g = re.match(r"\s*%[\w.]+ = load \S+, ptr (%[\w.]+)", line)
            if g and g.group(1) in ptr:
                i, o = ptr[g.group(1)]
                cur["R"][i].add(field(o))
                continue
            mc = re.search(r"call void @llvm\.mem(?:cpy|move)\.[^(]*\(ptr[^,]*, ptr[^%]*(%[\w.]+)", line)
            if mc and mc.group(1) in ptr:
                cur["R"][ptr[mc.group(1)][0]].add("a copy of the context")
                continue
            for c in re.finditer(r"call [^@%]*?(?:@([\w.$]+)|(%[\w.]+))\(([^)]*)", line):
                callee = c.group(1) or "<indirect>"
                if callee.startswith("llvm."):
                    continue
                if GUEST.match(callee):
                    cur["guest"] = True
                for k, a in enumerate(c.group(3).split(",")):
                    v = a.strip().split()[-1] if a.strip() else ""
                    if v in ptr:
                        cur["calls"].append((ptr[v][0], ptr[v][1], callee, k))
    return funcs


def reads(funcs, name, k=0, seen=None, api=None):
    """the context fields function name reads through parameter k, the hook API functions it reaches"""
    seen = set() if seen is None else seen
    api = set() if api is None else api
    f = funcs.get(name)
    if f is None or (name, k) in seen or k >= len(f["params"]):
        return set(), api
    seen.add((name, k))
    out = set(f["R"][k])
    for i, o, callee, arg in f["calls"]:
        if i != k:
            continue
        if callee in HOOK_API or callee == "guest_call":
            api.add(callee)
            continue
        if GUEST.match(callee) or callee == "<indirect>":
            out.add("everything (%s)" % callee)
            continue
        if o == 0:
            out |= reads(funcs, callee, arg, seen, api)[0]
    return out, api


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("clang"), "Linux and clang")
class HostReads(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = cls.tmp.name
        rt = rtgen.write(os.path.join(d, "rt64"))
        flags = ["-S", "-emit-llvm", "-O1", "-fno-inline", "-g0", "-Wno-everything", "-D_GNU_SOURCE",
                 "-I" + os.path.join(ROOT, "host", "posix"), "-I" + os.path.join(ROOT, "host"), "-I" + rt,
                 "-I" + os.path.join(ROOT, "gpu", "api")]

        def one(src):
            out = os.path.join(d, os.path.basename(src)[:-2] + ".ll")
            r = subprocess.run(["clang"] + flags + [src, "-o", out], capture_output=True, text=True)
            return src, r.returncode, r.stderr

        with ThreadPoolExecutor(8) as ex:
            res = list(ex.map(one, sorted(glob.glob(os.path.join(ROOT, "host", "*.c")))))
        # (a file whose third-party headers this PC lacks is left out: the check covers what compiles)
        cls.missing = [os.path.basename(s) for s, rc, err in res if rc and "file not found" in err]
        bad = [(s, err) for s, rc, err in res if rc and "file not found" not in err]
        assert not bad, bad[0]
        cls.funcs = trace(d)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_hooks(self):
        hooks = [n for n in self.funcs if re.match(r"entry_(0x[0-9a-f]{8}|sym_\w+)$", n)]
        self.assertGreater(len(hooks), 30)
        for name in hooks:
            got, api = reads(self.funcs, name)
            allowed = set(host_effects.hook_reads(name))
            self.assertFalse(got - allowed, "%s reads %s" % (name, sorted(got - allowed)))
            if "hook_call_guest" in api:
                self.assertIn("sp", host_effects.HOOK_READS.get(name, ()), "%s calls guest code: declare sp" % name)

    def test_hle(self):
        cs = {"x%d" % i for i in range(19, 30)} | {"v%d" % i for i in range(8, 16)}
        n = 0
        for name in self.funcs:
            m = re.match(r"(?:imp_|n_)(\w+)$", name)
            if not m:
                continue
            n += 1
            got = reads(self.funcs, name)[0] & cs
            declared = set(host_effects.HLE_READS.get(m.group(1), ((), ""))[0])
            self.assertFalse(got - declared, "%s reads its caller's %s" % (name, sorted(got - declared)))
        self.assertGreater(n, 300)

    def test_no_return_address_keys(self):
        """no host behaviour keyed on a guest return address (the caller's patch is an At row at its call site)"""
        pat = re.compile(r"(x\[30\]|\blr\b|->lr)\s*(==|!=)\s*0x8[0-9a-f]{7}|0x8[0-9a-f]{7}(ull|ULL)?\s*(==|!=)\s*"
                         r"(c->x\[30\]|\blr\b)")
        for path in sorted(glob.glob(os.path.join(ROOT, "host", "*.c"))):
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            with open(path, encoding="utf-8", errors="replace") as f:
                hits = [ln for ln in f if pat.search(ln)]
            if rel in LR_LABELS:
                continue
            self.assertFalse(hits, "%s: %s" % (rel, hits[:2]))


if __name__ == "__main__":
    unittest.main()
