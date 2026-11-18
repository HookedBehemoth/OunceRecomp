"""An offset in the game's executable (a backtrace's <exe>+<offset>) -> the function there (f_<guest address>, its
label, or a runtime function) + offset, from the executable's symbol table (nm).

Usage:  python -I tools/re/samap.py [--exe <executable>] <hex offset> [...]
        (default: work/oot/game/<the game's exe>, games/oot/game.toml's exe)
"""
import bisect
import os
import subprocess
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def default_exe():
    with open(os.path.join(ROOT, "games", "oot", "game.toml"), "rb") as f:
        return os.path.join(ROOT, "work", "oot", "game", tomllib.load(f)["exe"])


def main(argv):
    exe = default_exe()
    if argv[:1] == ["--exe"]:
        exe, argv = argv[1], argv[2:]
    if not argv:
        sys.exit(__doc__)
    out = subprocess.run(["nm", "-n", "--defined-only", exe], capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) == 3 and parts[1] in "tTwW":
            rows.append((int(parts[0], 16), parts[2]))
    keys = [r[0] for r in rows]
    for a in argv:
        t = int(a, 16)
        i = bisect.bisect_right(keys, t) - 1
        print("%s: %s+%#x" % (a, rows[i][1], t - rows[i][0]) if i >= 0 else "%s: before the first function" % a)


if __name__ == "__main__":
    main(sys.argv[1:])
