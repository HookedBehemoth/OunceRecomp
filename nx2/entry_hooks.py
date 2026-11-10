"""(native-vk) Host replacements for lifted guest functions: ENTRY_HOOK(0xADDR) and ENTRY_HOOK_SYM(name) in host/*.c.

host/hos.h has described these since the fork (TOTKPort's model), but nothing spliced them in: the hooks in
host/nn_libc.c (memcpy, memset, strlen, ...) were never linked, and the game's libc ran lifted. Here, after the lift
writes its C (chunks.edit_c), the entry of each hooked function gets

    if (entry_<addr>(c)) return;

before anything else, the label of its first instruction included (a branch back to the start does not run the hook
again). A hook returns 1 when it did the call's work (x0 holds the result), 0 to run the lifted body as usual.
ENTRY_HOOK_SYM names resolve through the image's exports (work/<sa>/image/image.json); a name the dump does not export
is reported and left out. Hooks are tied to this build of the game: a different main moves every address.

    python -I nx2/entry_hooks.py [work/sa/gen]      (by hand: list and apply)
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(os.path.dirname(HERE), "host")
sys.path.insert(0, HERE)
import whole_write   # noqa: E402

HOOK = re.compile(r"^ENTRY_HOOK\(0x([0-9a-fA-F]{8})\)", re.M)
HOOK_SYM = re.compile(r"^ENTRY_HOOK_SYM\((\w+)\)", re.M)
MARK = "/* (nx2/entry_hooks.py) */"


def wanted(gen, host=HOST):
    """{guest address: C function name of the hook} from host/*.c"""
    exports = {}
    meta = os.path.join(os.path.dirname(gen), "image", "image.json")
    if os.path.exists(meta):
        exports = json.load(open(meta)).get("exports", {})
    hooks, missing = {}, []
    for path in sorted(glob.glob(os.path.join(host, "*.c"))):
        src = open(path, encoding="utf-8", errors="replace").read()
        for m in HOOK.finditer(src):
            hooks[int(m.group(1), 16)] = "entry_0x%s" % m.group(1)
        for m in HOOK_SYM.finditer(src):
            addr = exports.get(m.group(1))
            if addr is None:
                missing.append(m.group(1))
            else:
                hooks[addr] = "entry_sym_%s" % m.group(1)
    return hooks, missing


def apply(gen, quiet=False, host=HOST):
    """Splice the hooks into the lifted C in gen; returns (functions hooked, files changed)"""
    hooks, missing = wanted(gen, host)
    if missing and not quiet:
        print("entry hooks: not exported by this dump, left out: %s" % ", ".join(missing))
    heads = {"void f_%x(Ctx *__restrict A64_NOESC c) {\n" % a: (a, fn) for a, fn in hooks.items()}
    found, changed = set(), 0
    for path in sorted(glob.glob(os.path.join(glob.escape(gen), "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        here = [h for h in heads if h in s]
        if not here:
            continue
        decls, t = [], s
        for h in here:
            a, fn = heads[h]
            call = "    if (%s(c)) return;   %s\n" % (fn, MARK)
            i = t.index(h) + len(h)
            if not t.startswith(call, i):
                t = t[:i] + call + t[i:]
            decl = "int %s(Ctx *c);   %s\n" % (fn, MARK)
            if decl not in t:
                decls.append(decl)
            found.add(a)
        if decls:
            inc = '#include "a64.h"\n'
            j = t.index(inc) + len(inc) if inc in t else 0
            t = t[:j] + "".join(decls) + t[j:]
        if t != s:
            whole_write.write_whole(path, t, newline="")
            changed += 1
    absent = sorted(set(hooks) - found)
    if not quiet:
        print("entry hooks: %d of %d functions hooked, %d files changed%s" % (
            len(found), len(hooks), changed,
            "; not in the lift: " + ", ".join("%#x %s" % (a, hooks[a]) for a in absent) if absent else ""))
    return len(found), changed


if __name__ == "__main__":
    gen = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "work", "sa", "gen")
    apply(gen)
