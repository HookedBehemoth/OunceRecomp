"""host/hos_fp.c without the game: MXCSR follows the guest FPCR, the MRS/MSR FPCR/FPSR helpers the lifted code calls, and
MXCSR across a fiber switch (host/posix/win32_posix.c's fibers, cut out of the file,
switched the way host/hos_thread.c's fiber_switch does it, which this test also checks by text).

python -I -B tests/test_hos_fp.py      (the compile-and-run cases need a C compiler on x86-64 Linux)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOS_FP = os.path.join(ROOT, "host", "hos_fp.c")
THREAD = os.path.join(ROOT, "host", "hos_thread.c")
POSIX = os.path.join(ROOT, "host", "posix", "win32_posix.c")


def read(path):
    with open(path) as f:
        return f.read()


def cc():
    return shutil.which("clang") or shutil.which("cc")


def fibers_text():
    """win32_posix.c's fiber section, from its heading to the next one"""
    s = read(POSIX)
    a = s.index("/* ---- fibers ---- */")
    b = s.index("/* ---- ", a + 10)
    return s[a:b]


HARNESS = r"""
#define HOS_FP_TEST
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <xmmintrin.h>
#define A64_NOESC
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; int halted; uint64_t vreg[32][2];
                 uint64_t tpidr, tpidrro, fpcr, fpsr; } Ctx;
#include "hos_fp.c"
typedef unsigned long DWORD; typedef size_t SIZE_T; typedef int BOOL;
typedef void (*LPFIBER_START_ROUTINE)(void *);
#define TRUE 1
#define FALSE 0
#define ERROR_NOT_ENOUGH_MEMORY 8
static DWORD last_error;
static void ExitThread(DWORD code) { exit((int)code); }
%(fibers)s
static Ctx thread_ctx, fiber_ctx;
static void *thread_fiber, *guest_fiber;
static unsigned seen_in_fiber, seen_in_fiber2;
/* hos_thread.c's pattern: the switching side syncs MXCSR to its own context when it runs again; a fiber's first run
   syncs to the FPCR it inherited */
static void switch_to(Ctx *from, void *to) { SwitchToFiber(to); a64_fpcr_sync(from); }
static void fiber_fn(void *p) {
    (void)p;
    a64_fpcr_sync(&fiber_ctx);
    seen_in_fiber = _mm_getcsr() & ~0x3Fu;
    a64_fpcr_write(&fiber_ctx, 3u << 22);              /* the fiber: RZ, no FZ */
    switch_to(&fiber_ctx, thread_fiber);
    seen_in_fiber2 = _mm_getcsr() & ~0x3Fu;
    switch_to(&fiber_ctx, thread_fiber);
}
int main(void) {
    int bad = 0;
#define CHECK(c, ...) do { if (!(c)) { printf(__VA_ARGS__); printf("\n"); bad = 1; } } while (0)
    /* the mapping */
    CHECK(hos_fp_mxcsr(0) == 0x1F80, "fpcr 0");
    CHECK(hos_fp_mxcsr(1u << 24) == 0x9FC0, "FZ");
    CHECK(hos_fp_mxcsr(1u << 22) == 0x5F80, "RP -> RC up (10)");
    CHECK(hos_fp_mxcsr(2u << 22) == 0x3F80, "RM -> RC down (01)");
    CHECK(hos_fp_mxcsr(3u << 22 | 1u << 24) == 0xFFC0, "RZ|FZ");
    /* MSR/MRS FPCR: the host's bits 32+ (RECOMP_FPX_INHIBIT) kept, MXCSR set, sticky bits kept */
    Ctx c; memset(&c, 0, sizeof c);
    c.fpcr = 1ull << 32;
    _mm_setcsr(0x1F80 | 0x20);
    a64_fpcr_write(&c, 0xFFFFFFFF00000000ull | 1u << 24);
    CHECK(c.fpcr == (1ull << 32 | 1u << 24), "fpcr %llx", (unsigned long long)c.fpcr);
    CHECK(a64_fpcr_read(&c) == 1u << 24, "fpcr read");
    CHECK(_mm_getcsr() == (0x9FC0 | 0x20), "mxcsr %x", _mm_getcsr());
    /* FZ in effect: a subnormal input is zero (DAZ), a tiny product is flushed (FTZ) */
    volatile float tiny = 1e-40f, small = 1e-30f;
    CHECK(tiny * 1.0f == 0.0f, "DAZ");
    CHECK(small * small == 0.0f, "FTZ");
    /* FPSR: MXCSR's sticky bits folded in; DE only under FZ; MSR FPSR clears them */
    _mm_setcsr(_mm_getcsr() & ~0x3Fu);
    volatile float one = 1.0f, three = 3.0f, zero = 0.0f;
    volatile float q = one / three; (void)q;                      /* inexact */
    volatile float d = one / zero; (void)d;                       /* divide by zero */
    c.fpsr = 1u << 27;                                            /* (QC, kept) */
    uint64_t f = a64_fpsr_read(&c);
    CHECK(f == (1u << 27 | 16u | 2u), "fpsr %llx", (unsigned long long)f);
    a64_fpsr_write(&c, 0);
    CHECK((_mm_getcsr() & 0x3F) == 0 && a64_fpsr_read(&c) == 0, "fpsr write clears");
    c.fpcr = 0; a64_fpcr_sync(&c);
    _mm_setcsr(_mm_getcsr() | 2u);                                /* DE without FZ: no IDC */
    CHECK(a64_fpsr_read(&c) == 0, "DE without FZ");
    c.fpcr = 1u << 24;
    CHECK(a64_fpsr_read(&c) == 128u, "DE under FZ is IDC");
    /* a fiber switch: the thread runs with FZ, the fiber starts from the thread's FPCR (as nn::os inherits it) */
    thread_fiber = ConvertThreadToFiberEx(NULL, 0);
    guest_fiber = CreateFiberEx(1 << 16, 1 << 20, 0, fiber_fn, NULL);
    thread_ctx.fpcr = 1u << 24;
    a64_fpcr_sync(&thread_ctx);
    fiber_ctx.fpcr = thread_ctx.fpcr;
    _mm_setcsr(0x1F80);                       /* (whatever the host fiber's start has: not the guest's) */
    switch_to(&thread_ctx, guest_fiber);
    CHECK(seen_in_fiber == 0x9FC0, "fiber start: mxcsr %x", seen_in_fiber);
    CHECK((_mm_getcsr() & ~0x3Fu) == 0x9FC0, "back in the thread after the fiber set RZ: %x", _mm_getcsr());
    a64_fpcr_write(&thread_ctx, 1u << 22);   /* the thread: RP */
    switch_to(&thread_ctx, guest_fiber);
    CHECK(seen_in_fiber2 == 0x7F80, "fiber resumed: its RZ, %x", seen_in_fiber2);
    CHECK((_mm_getcsr() & ~0x3Fu) == 0x5F80, "thread resumed: its RP, %x", _mm_getcsr());
    printf(bad ? "FAIL\n" : "OK\n");
    return bad;
}
"""


class Text(unittest.TestCase):
    def test_hos_thread_syncs(self):
        s = read(THREAD)
        main = s[s.index("static DWORD WINAPI thread_main"):]
        main = main[:main.index("\n}\n")]
        self.assertIn("a64_fpcr_sync(c);", main[:main.index("f(c);")], "thread_main: MXCSR before guest code")
        fm = s[s.index("static void WINAPI fiber_main"):]
        fm = fm[:fm.index("\n}\n")]
        self.assertLess(fm.index("a64_fpcr_sync(c);"), fm.index("a64_call_received(c, f->fn);"), "fiber_main")
        sw = s[s.index("static void fiber_switch(Ctx *from, HosFiber *to) {"):]
        sw = sw[:sw.index("\n}\n")]
        self.assertTrue(re.search(r"SwitchToFiber\([^;]*\);\s*(/\*.*?\*/\s*)?a64_fpcr_sync\(from\);", sw, re.S),
                        "fiber_switch: MXCSR of `from` when it runs again")
        self.assertIn("to->ctx.fpcr = from->fpcr;", sw)


@unittest.skipUnless(sys.platform.startswith("linux") and cc() and os.uname().machine == "x86_64",
                     "a C compiler on x86-64 Linux")
class Run(unittest.TestCase):
    def test_helpers_and_fiber_switch(self):
        with tempfile.TemporaryDirectory() as d:
            c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
            with open(c, "w") as f:
                f.write(HARNESS.replace("%(fibers)s", fibers_text()))
            subprocess.run([cc(), "-O1", "-w", "-I" + os.path.join(ROOT, "host"), c, "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
