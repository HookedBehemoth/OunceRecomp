"""Labels for the lifted game's functions, so a profile or a backtrace names what we already know.

nx2/oot_labels.txt lists guest address -> label (one game build: the addresses are HOS_GAME_VERSION 100's). After the
link, the executable's symbol f_<address> becomes <label>.f_<address>: the label first (a profiler's column shows it),
the address kept (unique, and every tool that looks for f_<address> still finds it). Only the symbol table changes:
the generated C, the hooks (ENTRY_HOOK(0x...)), the linker map and the loaded bytes are as before, so a label costs
no rebuild beyond the 1 s rewrite of the executable.

The list holds descriptions, not the game's names: lower case, digits and underscores, each used once.

  python -I nx2/label_syms.py check                 the list alone (no build needed)
  python -I nx2/label_syms.py apply <executable>    rename in place (build.py does this after the link)
  python -I nx2/label_syms.py name <address> ...    the labels of addresses (for scripts)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

LIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "oot_labels.txt")
LINE = re.compile(r"^([0-9a-f]{8})\s+([a-z][a-z0-9_]*)\s*(?:#.*)?$")


def read(path=LIST):
    """{address: label}; exits on a malformed line, an address or a label listed twice"""
    labels, seen, errors = {}, {}, []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            m = LINE.match(line)
            if not m:
                errors.append("%s:%d: not '<8 hex digits> <label> [# note]': %r" % (path, n, line))
                continue
            a, name = int(m.group(1), 16), m.group(2)
            if a in labels:
                errors.append("%s:%d: %08x is listed twice (%s, %s)" % (path, n, a, labels[a], name))
            if name in seen:
                errors.append("%s:%d: the label %s is used twice (%08x, %08x)" % (path, n, name, seen[name], a))
            labels[a], seen[name] = name, a
    if errors:
        sys.exit("\n".join(errors))
    return labels


def symbol(a, name):
    return "%s.f_%08x" % (name, a)


def apply(exe, labels=None):
    """Rename the executable's f_<address> symbols; returns (renamed, the addresses with no such function)"""
    labels = read() if labels is None else labels
    objcopy = shutil.which("llvm-objcopy") or shutil.which("objcopy")
    nm = shutil.which("llvm-nm") or shutil.which("nm")
    if not objcopy or not nm:
        print("labels: no objcopy/nm found: the functions keep their f_<address> names")
        return 0, []
    have = set()
    for line in subprocess.run([nm, "--defined-only", exe], capture_output=True, text=True, check=True).stdout.splitlines():
        s = line.rsplit(" ", 1)[-1]
        if s.startswith("f_") or ".f_" in s:
            have.add(s)
    pairs = [(a, n) for a, n in sorted(labels.items()) if "f_%08x" % a in have]
    done = sum(1 for a, n in labels.items() if symbol(a, n) in have)   # (already applied: a second run changes nothing)
    missing = [a for a, n in labels.items() if "f_%08x" % a not in have and symbol(a, n) not in have]
    if pairs:
        with tempfile.NamedTemporaryFile("w", suffix=".syms", delete=False) as f:
            f.write("".join("f_%08x %s\n" % (a, symbol(a, n)) for a, n in pairs))
        try:
            subprocess.run([objcopy, "--redefine-syms=" + f.name, exe], check=True)
        finally:
            os.unlink(f.name)
    return len(pairs) + done, missing


def main(argv):
    if len(argv) >= 1 and argv[0] == "check":
        print("labels: %d, each address and each label once" % len(read()))
        return 0
    if len(argv) == 2 and argv[0] == "apply":
        labels = read()
        n, missing = apply(argv[1], labels)
        print("labels: %d of %d functions named in %s" % (n, len(labels), argv[1]))
        for a in missing:
            print("labels: no function f_%08x for '%s' (not a function's entry in this lift?)" % (a, labels[a]))
        return 0
    if len(argv) >= 2 and argv[0] == "name":
        labels = read()
        for s in argv[1:]:
            a = int(s.replace("f_", ""), 16)
            print("%08x %s" % (a, labels.get(a, "-")))
        return 0
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
