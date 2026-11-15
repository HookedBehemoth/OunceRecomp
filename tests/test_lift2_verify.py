"""host/lift2_verify.c, the summary verification runtime (lift 2's milestone 14), on its own: each kind of violation
is reported when a summary is wrong (noreturn that returns, a preserved register changed, a register written that
the summary says is not), nothing is reported when it is right, and the registers come back as the summary
promises (preserved and unwritten ones restored, unread results poisoned).

python -I -B tests/test_lift2_verify.py     (needs a C compiler; skipped without one)
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = shutil.which("clang") or shutil.which("cc")

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
void lift2_check_enter(uint64_t *x, const uint64_t *sum, uint64_t *frame);
void lift2_check_exit(uint64_t *x, const uint64_t *sum, const uint64_t *frame);
#define B(r) (1ull << (r))
static uint64_t x[32], fr[32];
static void start(void) { for (int r = 0; r < 32; r++) x[r] = 0x1000 + r; }
int main(void) {
    /* a function that reads x0, x1, writes x0, x9, x19 (saved and restored), returns x0 */
    const uint64_t good[6] = {B(0) | B(1) | B(19), B(0) | B(9) | B(19), B(0), B(19) | B(31), 1, 0x80001000};
    start(); lift2_check_enter(x, good, fr);
    x[0] = 7; x[9] = 8;                        /* (its body: x19 written and put back) */
    lift2_check_exit(x, good, fr);
    printf("ok %llx %llx %llx %llx\n", (unsigned long long)x[0], (unsigned long long)x[9], (unsigned long long)x[19],
           (unsigned long long)x[5]);
    /* wrong: claims x19 preserved, but the body leaves it changed */
    const uint64_t bad_pres[6] = {B(0) | B(19), B(0) | B(19), B(0), B(19) | B(31), 1, 0x80002000};
    start(); lift2_check_enter(x, bad_pres, fr); x[19] = 5; lift2_check_exit(x, bad_pres, fr);
    /* wrong: claims x5 not written, the body writes it */
    const uint64_t bad_may[6] = {B(0), B(0), B(0), B(31), 1, 0x80003000};
    start(); lift2_check_enter(x, bad_may, fr); x[5] = 1; lift2_check_exit(x, bad_may, fr);
    /* wrong: claims it does not return */
    const uint64_t bad_ret[6] = {B(0), B(0), B(0), B(31), 0, 0x80004000};
    start(); lift2_check_enter(x, bad_ret, fr); lift2_check_exit(x, bad_ret, fr);
    return 0;
}
"""


@unittest.skipUnless(CC, "a C compiler")
class Verify(unittest.TestCase):
    def test_violations(self):
        with tempfile.TemporaryDirectory() as d:
            c, exe = os.path.join(d, "h.c"), os.path.join(d, "h")
            with open(c, "w") as f:
                f.write(HARNESS)
            subprocess.run([CC, "-O1", "-w", "-DLIFT2_VERIFY_STANDALONE", c, os.path.join(ROOT, "host", "lift2_verify.c"),
                            "-o", exe, "-lpthread"], check=True)
            out = subprocess.run([exe], cwd=d, capture_output=True, text=True, check=True).stdout
            log = ""
            if os.path.exists(os.path.join(d, "lift2_violations.txt")):
                with open(os.path.join(d, "lift2_violations.txt")) as f:
                    log = f.read()
        regs = out.split()
        self.assertEqual(regs[0], "ok")
        self.assertEqual(int(regs[1], 16), 7)                     # (a result: kept)
        self.assertEqual(int(regs[2], 16) >> 48, 0xDEAE)          # (written, no caller reads it: poisoned)
        self.assertEqual(int(regs[3], 16), 0x1000 + 19)           # (preserved: the entry value)
        self.assertEqual(int(regs[4], 16), 0x1000 + 5)            # (not written: the entry value again)
        self.assertNotIn("f_80001000", log)                       # (the right summary: nothing reported)
        self.assertIn("f_80002000 not preserved x19", log)
        self.assertIn("f_80003000 written (summary: not) x5", log)
        self.assertIn("f_80004000 returned (summary: noreturn)", log)


if __name__ == "__main__":
    unittest.main()
