"""Whether the lift in a work folder (work/sa: its gen/, image/ and lift2_plan.pickle) is whole.

nx2/lift.py --write marks the folder LIFT_INCOMPLETE before it writes anything and, as its last step, replaces the mark
by LIFT_OK (the lifter's commit and the switches it ran with). A lift that is killed (out of memory, a signal) leaves
LIFT_INCOMPLETE behind, and nx2/build.py and scripts/play-refresh.sh refuse to build from it: a half-written gen/ once
built and was stamped as the play build. A folder with neither mark (lifted before the marks) builds as before.
"""
import os
import subprocess

INCOMPLETE = "LIFT_INCOMPLETE"
OK = "LIFT_OK"
SWITCHES = ("HOOKSHOT_LIFT2", "HOOKSHOT_LIFT2_CLASSES", "HOOKSHOT_LIFT2_VERIFY", "HOOKSHOT_LIFT2_SUMMARIES",
            "A64_HOT_FUNCS", "A64_HOT_CHUNK", "HOOKSHOT_LIFT_JOBS")


def begin(folder):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, INCOMPLETE), "w") as f:
        f.write("a lift is writing this folder (nx2/lift.py); a build waits for it to end\n")
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
    lines = ["commit %s" % (commit or "?")] + ["%s=%s" % (k, os.environ[k]) for k in SWITCHES if k in os.environ]
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
