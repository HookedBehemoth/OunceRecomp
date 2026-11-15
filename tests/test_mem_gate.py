"""The compile's memory gate (nx2/build.py MemoryGate): a compile waits while the memory free, less what the compilers running may
still take, would not hold it; the floor's few always run; a compile that ends lets the next start; and the free memory
reads on this OS.

python -I -B tests/test_mem_gate.py
"""
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import build  # noqa: E402

def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


def main():
    free = build.free_mb()
    check("the free memory reads (%r)" % free, isinstance(free, int) and free > 0)

    check("a C++ file needs more than a C file", build.mem_need("x.cpp") > build.mem_need("x.c"))

    real = build.free_mb
    try:
        room = {"mb": 2500}
        build.free_mb = lambda: room["mb"]
        gate = build.MemoryGate()
        gate.reserve = 500
        gate.floor = 1
        need = 1200
        build.mem_need = lambda src: need
        a = gate.enter("a")   # (one always runs, whatever the memory)
        check("the first compile started", a in gate.running)
        got = []
        t = threading.Thread(target=lambda: got.append(gate.enter("b")))
        t.start()
        time.sleep(1.5)
        check("a second compile waits while the first may still take its need (2500 - 1200 - 500 < 1200)", not got)
        gate.started(a, None)
        room["mb"] = 4000   # (4000 - 1200 - 500 >= 1200)
        t.join(5)
        check("the second compile starts once the memory holds it", len(got) == 1 and got[0] in gate.running)
        gate.leave(a)
        gate.leave(got[0])
        check("nothing left running", not gate.running)
        c = gate.enter("c")
        check("a compile after the others ended starts at once", c in gate.running)
        gate.leave(c)
        gate.floor = 2
        room["mb"] = 0
        f = gate.enter("f")
        g = gate.enter("g")   # (no memory at all: still the floor's two)
        check("the floor's compilers start whatever the memory", {f, g} <= set(gate.running))
        gate.leave(f)
        gate.leave(g)
        room["mb"] = 500 + 3 * 1500   # (the countdown's estimate: 1.5 GB a compiler, between the floor and the jobs)
        check("the countdown expects the compilers the memory holds", gate.expected(24) == 3 and gate.expected(2) == 2)
        room["mb"] = 0
        check("the countdown expects the floor at the least", gate.expected(24) == gate.floor)
        build.free_mb = lambda: None   # (not known: as many as the jobs allow)
        d = gate.enter("d")
        e = gate.enter("e")
        check("with the free memory unknown, compiles do not wait", {d, e} <= set(gate.running))
        check("with the free memory unknown, the countdown expects the jobs", gate.expected(24) == 24)
    finally:
        build.free_mb = real
    print("ok")


if __name__ == "__main__":
    main()
