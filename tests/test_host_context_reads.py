"""What the host reads of the guest context, against what it declares (its patch declarations, host/patch.h, as
nx2/lift2/patch_scan.py reads them; the rules in nx2/lift2/host_effects.py): host/*.c compiled to LLVM IR (-O1, no
inlining), and every load from a context pointer traced through the functions it is passed to.

- A replaced function's adapter (PATCH_FUNC: hos_func_0x*, hos_func_sym_*) reads only its declared operands of the
  call's context (the host function takes them as values; its GuestCall goes only to guest_call).
- An HLE'd function (HLE: imp_*, and the NVN procs NV: n_nvn*) reads x19-x29 or v8-v15 of its caller only where its
  declaration's reads(...) says so.
- No host function compares the return address with a guest address any more except as a label: checked by the source
  (host/*.c) having no `x[30] ==` / `x[30] !=` / `lr ==` / `lr !=` against a literal outside the listed diagnostics.
- Every guest function a replaced function's or an At row's host function enters, on any path through the host's
  calls, is declared (the summaries read its inputs there): a constant target in its guest(...), a computed one as
  guest(computed) (rows). A replaced function's own body, orig, needs none (its record joins the function's). Guest
  code is entered through guest_call, guest_call8, hos_call_guest, a64_icall, a64_ijump or a call of an f_<addr> /
  body_<addr>; a guest function's host pointer (CODE_PTR(f_<addr>)) is a constant target. A guest function reaches host code by its
  name or from a64_lookup / a64_fn_of, which only the boot and the thread start call, and no static holds one
  (checked), so any other indirect call is of host code (orig's, through its GuestFn, is the body's).
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
import gamedesc  # noqa: E402
gamedesc.use(os.path.join(ROOT, "games", "oot"))   # (the game whose host files and patches these read)
import rtgen  # noqa: E402
from lift2 import host_effects, patch_scan, probes  # noqa: E402

DEF = re.compile(r"^define [^@]*@([\w.$]+)\((.*)\)[^{]*\{")
GUEST = re.compile(r"^(a64_icall|a64_ijump|hos_call_guest|hos_sj_call)$")
# (lr comparisons that only label or count: allowed)
LR_LABELS = {
    "host/direct_layer_ahead.c": "jobs recorded from another caller (a count for the log)",
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
    """the context fields function name reads through parameter k, the guest-call functions it reaches"""
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
        if callee in ("guest_call", "guest_call8"):
            api.add(callee)
            continue
        if GUEST.match(callee) or callee == "<indirect>":
            out.add("everything (%s)" % callee)
            continue
        if o == 0:
            out |= reads(funcs, callee, arg, seen, api)[0]
    return out, api


# ---- the guest code host code enters: every call, with constant arguments followed through the host's own calls
CALL = re.compile(r"call [^@%(]*?(?:@([\w.$]+)|(%[\w.]+))\((.*)\)")
DEF_ALL = re.compile(r"^define ([^@]*)@([\w.$]+)\((.*)\)[^{]*\{")
# (the call that enters guest code: the index of its target argument)
ENTER = {"guest_call": 2, "guest_call8": 2, "hos_call_guest": 1, "a64_icall": 1, "a64_ijump": 1}
INT = re.compile(r"-?\d+$")
GUEST_FN = re.compile(r"@(?:f|body)_([0-9a-f]{8})\b")
STORED_FN = re.compile(r"store ptr @(?:f|body)_([0-9a-f]{8}), ptr @")


def call_graph(folder):
    """{function: {"file", "params", "calls": [(callee or "%ptr", [argument values])]}}; an internal function is keyed
    (file, name), as the same static name recurs in several files"""
    funcs = {}
    for path in sorted(glob.glob(os.path.join(folder, "*.ll"))):
        f, cur = os.path.basename(path), None
        for line in open(path, errors="replace"):
            m = DEF_ALL.match(line)
            if m:
                params = [p.strip().split()[-1] if p.strip() else "" for p in re.split(r",(?![^(]*\))", m.group(3))]
                key = (f, m.group(2)) if re.search(r"\b(internal|private)\b", m.group(1)) else m.group(2)
                cur = funcs[key] = {"file": f, "params": params, "calls": [], "lines": []}
                continue
            if cur is None:
                continue
            if line.startswith("}"):
                cur = None
                continue
            cur["lines"].append(line)
            for c in CALL.finditer(line):
                args = [a.strip() for a in re.split(r",(?![^(]*\))", c.group(3))]
                cur["calls"].append((c.group(1) or c.group(2), args))
    return funcs


def guest_entries(funcs, root):
    """[(how, target or None (not a constant), path)]: the guest code root enters, through every host function it calls
    (a constant argument, or a host function's address, followed into the callee's parameters)"""
    out, seen = [], set()

    def key_of(name, file):
        return (file, name) if (file, name) in funcs else name if name in funcs else None

    def val(text, env):
        """an argument's value: a guest function's address (an f_ / body_ reference: CODE_PTR), an integer, a host
        function's name, or what the parameter it names was given"""
        g = GUEST_FN.search(text)
        if g:
            return int(g.group(1), 16)
        v = text.split()[-1] if text.split() else ""
        if INT.match(v):
            return int(v) & (2 ** 64 - 1)
        return v if v.startswith("@") else env.get(v)

    def walk(key, env, path):
        sig = (key, tuple(sorted(env.items())))
        if sig in seen:
            return
        seen.add(sig)
        f = funcs[key]
        path = path + [key if isinstance(key, str) else key[1]]
        for callee, args in f["calls"]:
            if callee.startswith("%"):   # (an indirect call: of a host function whose address is known, or host code)
                callee = val(callee, env)
                if not isinstance(callee, str):
                    continue
            callee = callee.lstrip("@")
            if callee.startswith("llvm."):
                continue
            if callee in ENTER:
                k = ENTER[callee]
                t = val(args[k], env) if len(args) > k else None
                out.append((callee, t if isinstance(t, int) else None, path))
            elif re.match(r"(f|body)_[0-9a-f]{8}$", callee):
                out.append((callee, int(callee[-8:], 16), path))
            else:
                k = key_of(callee, f["file"])
                if k is not None:
                    walk(k, {p: v for p, v in ((p, val(a, env)) for p, a in zip(funcs[k]["params"], args))
                             if v is not None}, path)
    walk(root, {}, [])
    return out


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
        cls.graph = call_graph(d)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_funcs(self):
        funcs = [n for n in self.funcs if re.match(r"hos_func_(0x[0-9a-f]{8}|sym_\w+)$", n)]
        decl = {f.name: f for f in patch_scan.patches().funcs}
        self.assertGreater(len(funcs), 20)
        self.assertEqual(sorted(funcs), sorted(decl))   # (each one declared)
        for name in funcs:
            got, _ = reads(self.funcs, name)
            allowed = decl[name].reads()
            self.assertFalse(got - allowed, "%s reads %s, beyond its operands (%s:%d)" % ((name, sorted(got - allowed)) +
                                                                                           decl[name].where))

    def test_guest_calls_declared(self):
        """every guest function a replaced function or a row's host function enters is declared where the summaries
        read it"""
        roots = {}
        for f in patch_scan.patches().funcs:   # (its adapter, which calls it)
            roots[f.name] = ("the guest(...) of the PATCH_FUNC of %s at %s:%d" % ((f.fn,) + f.where), set(f.guest), None)
        for p in probes.AT:   # (a row's host function: what its rows declare, all of them)
            what, decl, _ = roots.get(p.fn, ("the guest(...) of the PATCH_AT of %s" % p.fn, set(), None))
            decl = "computed" if "computed" in (decl, p.guest) else decl | set(p.guest or ())
            roots[p.fn] = (what, decl, None)
        for p in probes.PROBES:
            roots.setdefault(p.fn, ("Value probes cannot call guest code (%s)" % p.fn, set(), None))
        self.assertGreater(len(roots), 60)
        bad = []
        for name, (what, decl, own) in sorted(roots.items()):
            self.assertIn(name, self.graph, "%s is not defined in host/*.c" % name)
            for how, t, path in guest_entries(self.graph, name):
                if decl == "computed" or (t is not None and t in decl):
                    continue
                bad.append("%s enters guest %s through %s (%s): declare %s in %s" % (
                    name, "%#x" % t if t is not None else "code at a computed address", how, " > ".join(path),
                    "it" if t is not None else '"computed"', what))
        self.assertFalse(bad, "\n".join(bad))

    def test_guest_pointers_not_kept(self):
        """no host function keeps a guest function's host pointer (f_<addr>) in a static, and a64_lookup / a64_fn_of
        are called only by the boot and the thread start: so an indirect call elsewhere is of host code (or of a
        replaced function's orig, the body its record counts), and test_guest_calls_declared sees every way into guest
        code"""
        allowed = {"hos_main.ll", "hos_thread.ll"}
        for key, f in self.graph.items():
            name = key if isinstance(key, str) else "%s %s" % key
            looks = any(c in ("a64_lookup", "a64_fn_of") for c, _ in f["calls"])
            self.assertTrue(not looks or f["file"] in allowed, "%s resolves a guest code address" % name)
            kept = [line.strip() for line in f["lines"] if STORED_FN.search(line)]
            self.assertFalse(kept, "%s keeps a guest function in a static: %s" % (name, kept[:1]))

    def test_hle(self):
        cs = {"x%d" % i for i in range(19, 30)} | {"v%d" % i for i in range(8, 16)}
        n = 0
        for name in self.funcs:
            m = re.match(r"(?:imp_|n_)(\w+)$", name)
            if not m:
                continue
            n += 1
            got = reads(self.funcs, name)[0] & cs
            d = patch_scan.patches().effects(m.group(1))
            declared = set(d.reads if d is not None else ())
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
