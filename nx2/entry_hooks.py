"""(native-vk) Host replacements for lifted guest functions: ENTRY_HOOK(0xADDR) and ENTRY_HOOK_SYM(name) in host/*.c.

host/hos.h has described these since the fork (TOTKPort's model), but nothing spliced them in: the hooks in
host/nn_libc.c (memcpy, memset, strlen, ...) were never linked, and the game's libc ran lifted. Here, after the lift
writes its C (chunks.edit_c), the entry of each hooked function gets

    if (entry_<addr>(c)) return;

before anything else, the label of its first instruction included (a branch back to the start does not run the hook
again). A hook returns 1 when it did the call's work (x0 holds the result), 0 to run the lifted body as usual. The caller's
return address is in c->x[30] at the hook (a tail call leaves its caller's there, as on the console).
ENTRY_HOOK_SYM names resolve through the image's exports (work/<sa>/image/image.json); a name the dump does not export
is reported and left out. Hooks are tied to this build of the game: a different main moves every address.

SPLICE_AFTER(0xADDR, "text") and SPLICE_AFTER_EACH(0xADDR, "text") put

    splice_0xADDR(c);

after the first (every) line of f_ADDR's lifted C that contains the text: a patch in the middle of a function, with the
guest registers in c (c->x[]: the lift keeps registers in the context unless A64_LOCAL_REGS). One per function. The
text is matched as written (choose one that survives the lift's last edit: group_marks turns a grouped
"recomp_store64(" into "recomp_store64_sp(" after this splice ran, so anchor on what follows it). A function the lift
does not have is reported and left out (another game version); a function without the text stops the build.

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
# SPLICE_AFTER(0xADDR, "text") / SPLICE_AFTER_EACH(0xADDR, "text") (host/hos.h); the text is a C string literal
SPLICE = re.compile(r'^SPLICE_AFTER(_EACH)?\(0x([0-9a-fA-F]{8}),\s*"((?:[^"\\\n]|\\.)*)"\)', re.M)
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


def wanted_splices(host=HOST):
    """{guest address: (anchor text, after every line?)} from host/*.c"""
    out = {}
    for path in sorted(glob.glob(os.path.join(host, "*.c"))):
        src = open(path, encoding="utf-8", errors="replace").read()
        for m in SPLICE.finditer(src):
            a = int(m.group(2), 16)
            text = re.sub(r'\\(.)', r'\1', m.group(3))   # (the C literal's escapes: \" and \\)
            if a in out:
                sys.exit("entry hooks: two SPLICE_AFTERs for f_%x (one per function)" % a)
            if not text.strip():
                sys.exit("entry hooks: SPLICE_AFTER %#x: an empty anchor" % a)
            out[a] = (text, bool(m.group(1)))
    return out


def splice_body(t, addr, text, each):
    """(t with splice_0xADDR(c) after the anchor's line(s) in f_ADDR, lines matched); a second splice changes nothing"""
    head = HEAD % addr
    i = t.index(head) + len(head)
    j = t.find("\n}\n", i)
    j = len(t) if j < 0 else j + 1
    call = "    splice_0x%08x(c);   %s" % (addr, MARK)
    lines = t[i:j].split("\n")
    out, n = [], 0
    for line in lines:
        if line == call:
            continue   # (put back below, after its anchor)
        out.append(line)
        if text in line and (each or n == 0):
            out.append(call)
            n += 1
    return t[:i] + "\n".join(out) + t[j:], n


HEAD = "void f_%x(Ctx *__restrict A64_NOESC c) {\n"


def apply(gen, quiet=False, host=HOST):
    """Splice the hooks into the lifted C in gen; returns (functions hooked, files changed)"""
    hooks, missing = wanted(gen, host)
    splices = wanted_splices(host)
    if missing and not quiet:
        print("entry hooks: not exported by this dump, left out: %s" % ", ".join(missing))
    heads = {HEAD % a: (a, fn) for a, fn in hooks.items()}
    sheads = {HEAD % a: a for a in splices}
    found, changed, spliced, lines = set(), 0, set(), 0
    for path in sorted(glob.glob(os.path.join(glob.escape(gen), "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        here = [h for h in heads if h in s]
        shere = [h for h in sheads if h in s]
        if not here and not shere:
            continue
        decls, t = [], s
        for h in shere:   # (first: an entry hook's line goes before the body's lines)
            a = sheads[h]
            text, each = splices[a]
            t, n = splice_body(t, a, text, each)
            if n == 0:
                sys.exit("entry hooks: SPLICE_AFTER %#x: %r is not in f_%x (another game version or lift?)" % (a, text, a))
            decl = "void splice_0x%08x(Ctx *c);   %s\n" % (a, MARK)
            if decl not in t:
                decls.append(decl)
            spliced.add(a)
            lines += n
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
    sabsent = sorted(set(splices) - spliced)
    if not quiet:
        print("entry hooks: %d of %d functions hooked, %d files changed%s" % (
            len(found), len(hooks), changed,
            "; not in the lift: " + ", ".join("%#x %s" % (a, hooks[a]) for a in absent) if absent else ""))
        if splices:
            print("entry hooks: %d of %d functions spliced (%d lines)%s" % (
                len(spliced), len(splices), lines,
                "; not in the lift: " + ", ".join("%#x" % a for a in sabsent) if sabsent else ""))
    return len(found), changed


if __name__ == "__main__":
    gen = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "work", "sa", "gen")
    apply(gen)
