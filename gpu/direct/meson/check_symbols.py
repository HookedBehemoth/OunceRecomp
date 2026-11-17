"""Checks that hookshot_direct.so loads: the host dlopens it with RTLD_NOW, so one unresolved symbol stops the renderer
with no symbol named. Every undefined, non-weak symbol that is not the C or C++ runtime's must be one the executable
exports (os_* and hos_mem_ledger, its --export-dynamic-symbol patterns) and, when the executable is given, one it
defines. A failure usually means a host function lacks __attribute__((weak)), or an extern of one of our globals sits
inside an anonymous namespace.

    check_symbols.py <nm> <so> <stamp out> [<exe>]
"""
import fnmatch
import os
import subprocess
import sys

EXPORTED = ("os_*", "hos_mem_ledger")   # (the exe's link: -Wl,--export-dynamic-symbol=<each>)
RUNTIME = ("@GLIBC", "@CXXABI", "@GLIBCXX", "@GCC")


def nm(tool, path, which):
    out = subprocess.run([tool, "-D", which, path], capture_output=True, text=True, check=True).stdout
    return [line.split() for line in out.splitlines() if line.split()]


def main():
    tool, so, stamp = sys.argv[1:4]
    exe = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else None
    if exe and not os.path.exists(exe):
        raise SystemExit("check_symbols: no executable at %s (meson configure -Dexe=)" % exe)
    defined = {p[-1] for p in nm(tool, exe, "--defined-only")} if exe else None
    bad = []
    for parts in nm(tool, so, "--undefined-only"):
        if len(parts) < 2 or parts[0] != "U":   # ("w": weak, the host supplies it or not)
            continue
        name = parts[1]
        if any(r in name for r in RUNTIME):
            continue
        if not any(fnmatch.fnmatchcase(name, p) for p in EXPORTED):
            bad.append(name + "  (not an exported pattern: " + ", ".join(EXPORTED) + ")")
        elif defined is not None and name not in defined:
            bad.append(name + "  (not exported by " + os.path.basename(exe) + ")")
    if bad:
        print("%s has %d unresolved symbol(s); it would not load:" % (os.path.basename(so), len(bad)))
        print("\n".join("    " + b for b in bad[:20]))
        print("  a host function wants __attribute__((weak)); one of our own globals wants an extern at")
        print("  namespace scope, not inside an anonymous namespace")
        if os.path.exists(stamp):
            os.remove(stamp)
        return 1
    open(stamp, "w").write("ok %s\n" % (exe or "patterns only"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
