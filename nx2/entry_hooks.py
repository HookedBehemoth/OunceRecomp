"""Host replacements for lifted guest functions, as host/*.c declares them (host/hos.h):

- ENTRY_HOOK(0xADDR) and ENTRY_HOOK_SYM(name): the lifter calls entry_<addr>(c) at the function's entry (emit_llvm's
  entry hook); a hook returns 1 when it did the call's work (x0 holds the result), 0 to run the lifted body as usual.
  The caller's return address is in c->x[30] at the hook. ENTRY_HOOK_SYM names resolve through the image's exports
  (work/sa/image/image.json); a name the dump does not export is reported and left out. Hooks are tied to this build
  of the game: a different main moves every address.
- SPLICE_AFTER(0xADDR, "text") and SPLICE_AFTER_EACH: a host function the lifter calls in the middle of a function
  (nx2/lift2/probes.py's SPLICE_AT and SPLICE_BEFORE name the instructions by address); the text is the anchor's
  record.
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(os.path.dirname(HERE), "host")
sys.path.insert(0, HERE)

HOOK = re.compile(r"^ENTRY_HOOK\(0x([0-9a-fA-F]{8})\)", re.M)
HOOK_SYM = re.compile(r"^ENTRY_HOOK_SYM\((\w+)\)", re.M)
# SPLICE_AFTER(0xADDR, "text") / SPLICE_AFTER_EACH(0xADDR, "text") (host/hos.h); the text is a C string literal
SPLICE = re.compile(r'^SPLICE_AFTER(_EACH)?\(0x([0-9a-fA-F]{8}),\s*"((?:[^"\\\n]|\\.)*)"\)', re.M)


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
