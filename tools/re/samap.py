"""An offset in the game's executable (a backtrace's <exe>+<offset>) -> the function there (f_<guest address>, its
label, or a runtime function) + offset, from the executable's symbol table (nm).

Usage:  python -I tools/re/samap.py (--game <game> | --exe <executable>) <hex offset> [...]
        (--game: work/<game>/game/<the game's exe>, the exe of games/<game>/game.toml)
"""
import bisect
import os
import subprocess
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def game_exe(game):
    with open(os.path.join(ROOT, "games", game, "game.toml"), "rb") as f:
        return os.path.join(ROOT, "work", game, "game", tomllib.load(f)["exe"])


def main(argv):
    exe = None
    if argv[:1] == ["--exe"] and len(argv) > 1:
        exe, argv = argv[1], argv[2:]
    elif argv[:1] == ["--game"] and len(argv) > 1:
        exe, argv = game_exe(argv[1]), argv[2:]
    if exe is None or not argv:
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
