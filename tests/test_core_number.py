"""The core number a guest thread is told (runtime/hos_thread.c hos_thread_core, hos_cores), compiled with a stub HosThread:
a one-core mask's core; else the ideal core when the mask has it (or there is no mask); else the mask's lowest core;
never a core outside the mask; six cores.

python -I -B tests/test_core_number.py [--cc gcc]
"""
import argparse
from pathlib import Path
import shutil
import subprocess
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
typedef struct HosThread { int ideal_core; uint64_t core_mask; } HosThread;
CUT_CORES
CUT_CORE
static int fails;
static void check(int ideal, uint64_t mask, int want) {
    HosThread t = {ideal, mask};
    const int got = hos_thread_core(&t);
    if (got != want) { printf("FAIL: ideal %d mask %#llx: core %d, want %d\n", ideal, (unsigned long long)mask, got, want); fails++; }
    if (mask && (mask & 0x3f) && !(mask >> got & 1)) { printf("FAIL: ideal %d mask %#llx: core %d outside the mask\n", ideal, (unsigned long long)mask, got); fails++; }
}
int main(void) {
    if (hos_cores() != 6) { printf("FAIL: %d cores\n", hos_cores()); fails++; }
    check(-2, 0x2, 1);     /* one core: its core */
    check(3, 0x2, 1);      /* one core: its core, whatever the ideal says */
    check(1, 0x6, 1);      /* the ideal core, in the mask */
    check(0, 0x6, 1);      /* an ideal outside the mask: the mask's lowest */
    check(-2, 0x3f, 0);    /* no ideal: the mask's lowest */
    check(-2, 0x38, 3);
    check(-2, 0, 0);       /* no mask, no ideal: core 0 */
    check(2, 0, 2);        /* no mask: the ideal */
    check(-2, 0x80, 1);    /* a core past the six wraps */
    puts(fails ? "FAILED" : "PASS: core numbers from masks and ideal cores, never outside the mask, six cores");
    return fails != 0;
}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="gcc")
    a = ap.parse_args()
    src = (ROOT / "runtime/hos_thread.c").read_text(encoding="utf-8")
    code = HARNESS.replace("CUT_CORES", block(src, "int hos_cores(void) {")).replace("CUT_CORE\n", block(src, "int hos_thread_core(HosThread *t) {") + "\n")
    cc = shutil.which(a.cc)
    if not cc:
        raise SystemExit("compiler not found: " + a.cc)
    with tempfile.TemporaryDirectory(prefix="core-number-") as d:
        c = Path(d) / "t.c"
        c.write_text(code, encoding="utf-8")
        subprocess.run([cc, "-std=c11", "-Wall", "-Werror", str(c), "-o", str(Path(d) / "t")], check=True)
        subprocess.run([str(Path(d) / "t")], check=True)


if __name__ == "__main__":
    main()
