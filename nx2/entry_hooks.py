"""Host replacements for lifted guest functions, as host/*.c declares them (host/hos.h):

- ENTRY_HOOK(0xADDR) and ENTRY_HOOK_SYM(name): the lifter calls entry_<addr>(c) at the function's entry (emit_llvm's
  entry hook); a hook returns 1 when it did the call's work (x0 holds the result), 0 to run the lifted body as usual.
  The caller's return address is in c->x[30] at the hook. ENTRY_HOOK_SYM names resolve through the image's exports
  (work/sa/image/image.json); a name the dump does not export is reported and left out. Hooks are tied to this build
  of the game: a different main moves every address.
A patch in the middle of a function is an At row (nx2/lift2/probes.py), not a hook.
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
