"""Whether the lift in a work folder (work/sa: its gen/, image/ and lift2_plan.pickle) is whole.

nx2/lift.py --write marks the folder LIFT_INCOMPLETE before it writes anything and, as its last step, replaces the mark
by LIFT_OK (the lifter's commit and the switches it ran with). A lift that is killed (out of memory, a signal) leaves
LIFT_INCOMPLETE behind, and nx2/build.py and scripts/play-refresh.sh refuse to build from it: a half-written gen/ once
built and was stamped as the play build. A folder with neither mark (lifted before the marks) builds as before.

LIFT_OK also records a hash of the lift's inputs (inputs_hash: the lifter's sources, the lift-time patch sources, the
ENTRY_HOOK / SPLICE_AFTER lines of host/*.c, the hot list, the dump's code files, the switches). scripts/play-refresh.sh
asks stale() and lifts again when they changed, instead of building a lift older than the lifter (the merge of a
lifter change once stamped a play build made from the old lift); a host-only change keeps its fast path.
"""
import glob
import hashlib
import os
import re
import subprocess
import sys

INCOMPLETE = "LIFT_INCOMPLETE"
OK = "LIFT_OK"
SWITCHES = ("HOOKSHOT_LIFT2", "HOOKSHOT_LIFT2_CLASSES", "HOOKSHOT_LIFT2_VERIFY", "HOOKSHOT_LIFT2_SUMMARIES",
            "A64_HOT_FUNCS", "A64_HOT_CHUNK", "HOOKSHOT_LIFT_JOBS")


# what the lift reads, relative to the repository: whole files (globs), and host files of which only the lines the lift
# reads count (nx2/entry_hooks.py's ENTRY_HOOK, ENTRY_HOOK_SYM, SPLICE_AFTER)
LIFT_SOURCES = ("third_party/a64-kit/lift/*.py", "third_party/a64-kit/lift/liba64emit.so.sha1",
                "third_party/a64-kit/rt/a64.h", "nx2/*.py", "nx2/lift2/*.py")
HOST_LINES = ("host/*.c",)
HOST_LINE = re.compile(r"^(?:ENTRY_HOOK|ENTRY_HOOK_SYM|SPLICE_AFTER|SPLICE_AFTER_EACH)\(.*$", re.M)
NOT_LIFT = re.compile(r"(^|/)(test_|build\.py$|make_game\.py$|prompt_|prepare_|nup|win_manifest|make_icon|dll|"
                      r"dump\.py$|disasm\.py$|xref\.py$|samap|sasym|census\.py$|fault\.py$|label_syms|zbic|"
                      r"pgo_pieces|step_readers|version\.py$|unpack\.py$)")


def inputs_hash(root, env=None, hot=None, exefs=None):
    """a hash of what a lift of the repository at root reads: see LIFT_SOURCES, HOST_LINES; the hot list file
    (A64_HOT_FUNCS as the lift takes it); the dump's exefs files (name, size, modification time); the switches"""
    env = os.environ if env is None else env
    h = hashlib.sha1(usedforsecurity=False)
    for pat in LIFT_SOURCES:
        for path in sorted(glob.glob(os.path.join(root, pat))):
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if NOT_LIFT.search(rel):
                continue
            h.update(rel.encode() + b"\0")
            with open(path, "rb") as f:
                h.update(f.read())
    for pat in HOST_LINES:
        for path in sorted(glob.glob(os.path.join(root, pat))):
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = HOST_LINE.findall(f.read())
            if lines:
                h.update(os.path.basename(path).encode() + b"\0" + "\n".join(lines).encode())
    if hot and os.path.exists(hot):
        with open(hot, "rb") as f:
            h.update(b"hot\0" + f.read())
    if exefs and os.path.isdir(exefs):
        for name in sorted(os.listdir(exefs)):
            st = os.stat(os.path.join(exefs, name))
            h.update(("exefs %s %d %d" % (name, st.st_size, int(st.st_mtime))).encode())
    h.update(repr([(k, env.get(k)) for k in SWITCHES if k != "HOOKSHOT_LIFT_JOBS"]).encode())
    return h.hexdigest()


def recorded(folder):
    """the inputs hash LIFT_OK records, or None"""
    try:
        with open(os.path.join(folder, OK)) as f:
            for line in f:
                if line.startswith("inputs "):
                    return line.split()[1]
    except FileNotFoundError:
        pass
    return None


def stale(folder, root, env=None, hot=None, exefs=None):
    """None when the folder's lift was made from the current inputs, else why it is stale"""
    have = recorded(folder)
    if have is None:
        return "the lift in %s records no inputs (made before the record, or not whole)" % folder
    now = inputs_hash(root, env, hot, exefs)
    if have != now:
        return "the lift's inputs changed since the lift in %s (%s -> %s)" % (folder, have[:12], now[:12])
    return None


def default_inputs(root):
    """(the hot list, the exefs) as nx2/lift.py and scripts/relift.sh take them"""
    hot = next((f for f in (os.path.join(root, "work", "hot_funcs_play.txt"), os.path.join(root, "work", "hot_funcs.txt"))
                if os.path.exists(f)), None)
    hot = os.environ.get("A64_HOT_FUNCS") or hot
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import kit
        exefs = kit.EXEFS
    except Exception:
        exefs = None
    return hot, exefs


def begin(folder):
    os.makedirs(folder, exist_ok=True)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(folder, INCOMPLETE), "w") as f:   # (the inputs as the lift starts: what it reads)
        f.write("a lift is writing this folder (nx2/lift.py); a build waits for it to end\n")
        f.write("inputs %s\n" % inputs_hash(root, None, *default_inputs(root)))
    try:
        os.remove(os.path.join(folder, OK))
    except FileNotFoundError:
        pass


def done(folder):
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        commit = subprocess.run(["git", "-C", here, "rev-parse", "--short", "HEAD"], capture_output=True,
                                text=True).stdout.strip()
    except OSError:
        commit = "?"
    inputs = [l for l in open(os.path.join(folder, INCOMPLETE)) if l.startswith("inputs ")]
    lines = ["commit %s" % (commit or "?")] + [l.strip() for l in inputs[:1]] + \
        ["%s=%s" % (k, os.environ[k]) for k in SWITCHES if k in os.environ]
    tmp = os.path.join(folder, OK + ".tmp")
    with open(tmp, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, os.path.join(folder, OK))
    os.remove(os.path.join(folder, INCOMPLETE))


def check(folder):
    """None when the folder's lift may be built from, else why not"""
    if os.path.exists(os.path.join(folder, INCOMPLETE)):
        return ("the lift in %s is incomplete (%s: a lift was stopped before its end, e.g. for memory): lift again "
                "(scripts/relift.sh)" % (folder, INCOMPLETE))
    return None


if __name__ == "__main__":   # (scripts/play-refresh.sh: python nx2/lift_mark.py stale work/sa; exit 1 when stale)
    if sys.argv[1:2] == ["stale"]:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        why = stale(sys.argv[2], root, None, *default_inputs(root))
        if why:
            print(why)
        sys.exit(1 if why else 0)
