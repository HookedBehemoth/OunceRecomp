"""Halving a mixed build's lift 2 functions to find the one that breaks the game (milestone 5's bisection).

    python -I nx2/lift2/bisect.py start <use.txt>     the functions of a build (lift2_functions.txt of its build folder)
    python -I nx2/lift2/bisect.py good | bad          the last build's half was good / bad
    python -I nx2/lift2/bisect.py show                the state

The state is <use.txt>.bisect (a JSON list of the suspects and the half being tried). Each step writes the half to
try to <use.txt>.try; build with HOOKSHOT_LIFT2=<that file> (only the partitions whose functions changed compile
again: their objects are cached by IR text), run the game, and answer good or bad. Before building the game, the
function test can rule out a half much sooner: python -I nx2/test_lift2_leaf.py --calls --funcs <use.txt>.try.
"""
import json
import os
import sys


def save(state_path, suspects, trying):
    with open(state_path, "w") as f:
        json.dump({"suspects": suspects, "trying": trying}, f)
    with open(state_path[:-len(".bisect")] + ".try", "w") as f:
        f.write("".join("%08x\n" % e for e in trying))
    print("%d suspects; trying %d (written to %s)" % (len(suspects), len(trying), state_path[:-len(".bisect")] + ".try"))
    if len(suspects) == 1:
        print("found: f_%08x" % suspects[0])


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "show"
    if cmd == "start":
        use = sys.argv[2]
        suspects = sorted(int(l.split()[0], 16) for l in open(use) if l.strip() and not l.startswith("#"))
        save(use + ".bisect", suspects, suspects[:len(suspects) // 2])
        return 0
    state = [f for f in os.listdir(".") if f.endswith(".bisect")]
    path = os.environ.get("LIFT2_BISECT") or (state[0] if len(state) == 1 else None)
    if path is None:
        raise SystemExit("bisect: LIFT2_BISECT=<use.txt>.bisect names the state")
    s = json.load(open(path))
    suspects, trying = s["suspects"], s["trying"]
    if cmd == "show":
        print("%d suspects, trying %d" % (len(suspects), len(trying)))
        return 0
    if cmd == "bad":       # (the half tried breaks the game: the culprit is in it)
        suspects = trying
    elif cmd == "good":    # (it does not: the culprit is in the other half)
        suspects = sorted(set(suspects) - set(trying))
    else:
        raise SystemExit(__doc__)
    save(path, suspects, suspects[:max(1, len(suspects) // 2)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
