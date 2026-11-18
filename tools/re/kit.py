"""Paths: the vendored a64-kit (third_party/a64-kit) on sys.path. A game's dump is the one the user's config names
(tools/ounce_config.py); it is never copied into the repository.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KIT = os.path.join(ROOT, "third_party", "a64-kit")
for sub in ("lift", "tools", "hos", "rt"):
    p = os.path.join(KIT, sub)
    if p not in sys.path:
        sys.path.insert(0, p)
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

def game_modules(exefs):
    """The exefs's code modules in rtld's load order (rtld, main, subsdk0..9, sdk) that exist, without rtld"""
    order = ["main"] + ["subsdk%d" % i for i in range(10)] + ["sdk"]
    return [m for m in order if os.path.exists(os.path.join(exefs, m))]


# Other sessions' benchmarks need a quiet machine: they hold this file for the few minutes a timing run takes.
# Heavy jobs (builds, test runs, the full emit) wait while it exists; a job already running may finish.
BENCH_LOCK = os.path.join(os.path.dirname(ROOT), ".bench-lock")


def wait_for_quiet(what="this job"):
    import time
    waited = 0
    while os.path.exists(BENCH_LOCK):
        if not waited:
            print("waiting for %s to go (a benchmark is running) before %s" % (BENCH_LOCK, what), flush=True)
        time.sleep(60)
        waited += 1
    if waited:
        print("benchmark lock gone after ~%d min" % waited, flush=True)
