"""Compile the virtual-core owner functions of host/hos_thread.c with a single-threaded harness and check
HOS_VCORE_BY_PRIO: one thread per core number, or per core number and priority with the switch.

python -I -B tests/test_vcore_by_prio.py [--cc clang]
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int SRWLOCK, CONDITION_VARIABLE;
typedef struct { volatile int poll; } Ctx;
typedef struct HosThread {
    Ctx ctx; int priority, ideal_core; uint64_t core_mask; char name[48];
    int vc_held, vc_last, vc_queued; volatile int vc_kick; int64_t vc_since; CONDITION_VARIABLE vc_cv;
    int64_t core_asks, vc_wait_ns; int vc_slot; Ctx *volatile run_ctx;
} HosThread;
static SRWLOCK vc_lock;
static int woken;
static void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; }
static void WakeConditionVariable(CONDITION_VARIABLE *c) { (void)c; woken++; }
static int hos_cores(void) { return 6; }
static int hos_thread_core(HosThread *t) { return t->vc_held ? t->vc_held - 1 : __builtin_ctzll(t->core_mask); }
static HosThread *vc_queue[1024];
static int vc_nq;
#include "cut.inc"
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
static HosThread mk(int prio, uint64_t mask) { HosThread t; memset(&t, 0, sizeof t); t.priority = prio; t.ideal_core = -2; t.core_mask = mask; return t; }
static void queue(HosThread *t) { t->vc_queued = 1; vc_queue[vc_nq++] = t; }
int main(int argc, char **argv) {
    const int by_prio = argc > 1 && !strcmp(argv[1], "1");
    HosThread a = mk(16, 0x2), b = mk(20, 0x2), c = mk(16, 0x2), d = mk(16, 0x4);
    int k = vc_free_for(&a);
    CHECK(k == 1);
    vc_take(&a, k);
    CHECK(a.vc_held == 2 && hos_thread_core(&a) == 1);
    CHECK(vc_free_for(&c) == -1);              /* one core 1 for one priority, either way */
    CHECK(vc_free_for(&d) == 2);               /* another core is free */
    if (by_prio) {
        k = vc_free_for(&b);
        CHECK(k == 1);                         /* another priority runs beside it on core 1 */
        vc_take(&b, k);
        CHECK(hos_thread_core(&b) == 1);
    } else {
        CHECK(vc_free_for(&b) == -1);          /* core 1 is held */
    }
    queue(&c);
    vc_release(&a);                            /* core 1 (priority 16) goes to c, the waiter of a's priority */
    CHECK(c.vc_held == 2 && !c.vc_queued && woken == 1);
    CHECK(vc_free_for(&a) == -1);
    if (by_prio) {
        vc_release(&b);                        /* nobody of priority 20 waits: no wake, c keeps core 1 */
        CHECK(woken == 1 && c.vc_held == 2 && !b.vc_held);
    }
    vc_release(&c);
    CHECK(vc_free_for(&a) == 1 && vc_free_for(&b) == 1);
    printf("%s: %s\n", by_prio ? "by priority" : "by core", fails ? "FAILED" : "ok");
    return fails != 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    cc = shutil.which(args.cc) or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        raise SystemExit("C compiler not found")
    src = (ROOT / "host/hos_thread.c").read_text(encoding="utf-8")
    owner = re.search(r"^static HosThread \*vc_owner\[[^\n]*\n", src, re.M).group(0)
    macro = re.search(r"^#define OWN\([^\n]*\n", src, re.M).group(0)
    parts = [owner] + [block(src, m) for m in ("static int vc_by_prio(", "static int vc_slot(")] + [macro] + [
        block(src, m) for m in ("static int vc_cores(", "static unsigned vc_allowed(", "static int vc_better(",
                                "static int vc_free_for(", "static void vc_take(", "static void vc_dequeue(",
                                "static void vc_grant(", "static void vc_release(")]
    cut = "\n".join(parts)
    cut = cut.replace("if (core_tls_slot", "if (0 && core_tls_slot")
    with tempfile.TemporaryDirectory(prefix="vcore-by-prio-") as tmp:
        folder = Path(tmp)
        (folder / "cut.inc").write_text(cut, encoding="utf-8")
        (folder / "t.c").write_text(HARNESS, encoding="utf-8")
        exe = folder / "t"
        subprocess.run([cc, "-std=c11", "-Wall", "-Werror", "-Wno-unused-function", "-I" + str(folder),
                        str(folder / "t.c"), "-o", str(exe)], check=True)
        ok = True
        for mode, env in (("0", {}), ("1", {"HOS_VCORE_BY_PRIO": "1"})):
            r = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True)
            print(r.stdout, end="")
            ok &= r.returncode == 0
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
