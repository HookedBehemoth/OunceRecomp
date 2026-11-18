
#define HOS_FP_TEST
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
#include "os.h"
#define A64_NOESC
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; int halted; uint64_t vreg[32][2];
                 uint64_t tpidr, tpidrro, fpcr, fpsr; } Ctx;
#include "hos_fp.c"
static Ctx thread_ctx, fiber_ctx;
static os_fiber *thread_fiber, *guest_fiber;
static unsigned seen_in_fiber, seen_in_fiber2;
/* hos_thread.c's pattern: the switching side syncs MXCSR to its own context when it runs again; a fiber's first run
   syncs to the FPCR it inherited */
static void switch_to(Ctx *from, os_fiber *to) { os_fiber_switch(to); a64_fpcr_sync(from); }
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
    thread_fiber = os_fiber_from_thread();
    guest_fiber = os_fiber_create(1 << 20, fiber_fn, NULL);
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
