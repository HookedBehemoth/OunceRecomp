/* (hookshot) The guest's FP mode on the host: MXCSR follows the guest FPCR, and the FP routines the lifted code calls.

   The lifted code (docs/lifter.md section 8) computes FP natively, so the host's MXCSR is the guest's FPCR on every
   thread while guest code runs: FPCR.FZ is MXCSR.DAZ|FTZ, RMode is RC, every exception stays masked. It is set where
   FPCR changes or a guest context starts running on a host thread:
   - MSR FPCR: the lifted code calls a64_fpcr_write;
   - thread start (hos_thread.c thread_main) and both sides of a fiber switch (fiber_main, fiber_switch): a host
     thread or fiber does not reliably start from, or keep, the MXCSR its guest context needs.

   FPSR: the lifted code keeps no cumulative flags per instruction; MRS FPSR (a64_fpsr_read) is the context's FPSR with MXCSR's
   sticky bits folded in, MSR FPSR (a64_fpsr_write) clears them.

   FRECPE and FRSQRTE are Arm's estimate algorithm (the Arm ARM's FPRecipEstimate and FPRSqrtEstimate with
   RecipEstimate/RecipSqrtEstimate, FEAT_RPRES off), exact, called per lane; they read FZ and RMode from MXCSR.
   FPCR.DN is not modelled: a NaN operand comes back quieted (section 7.1). */
#ifndef HOS_FP_TEST
#include "a64.h"
#endif
#include <stdint.h>
#include <string.h>
#include <xmmintrin.h>

/* MXCSR for a guest FPCR: FZ -> DAZ (bit 6) | FTZ (bit 15); RMode RN/RP/RM/RZ -> RC 0/2/1/3; exceptions masked */
unsigned hos_fp_mxcsr(uint64_t fpcr) {
    unsigned rm = (unsigned)(fpcr >> 22) & 3, m = 0x1F80u;
    if (fpcr >> 24 & 1) m |= 0x8040u;
    return m | (rm == 1 ? 2u : rm == 2 ? 1u : rm) << 13;
}

/* MXCSR = the context's FPCR, keeping the sticky exception bits */
/* HOS_FP_LOG=1 (diagnostic): hos_thread.c sets this to log changes of a thread's FP mode */
void (*hos_fp_log_hook)(unsigned long long fpcr, unsigned from, unsigned to);

void a64_fpcr_sync(Ctx *A64_NOESC c) {
    const unsigned m = hos_fp_mxcsr(c->fpcr);
    if (hos_fp_log_hook && (_mm_getcsr() & ~0x3Fu) != m) hos_fp_log_hook(c->fpcr, _mm_getcsr(), m);
    _mm_setcsr(m | (_mm_getcsr() & 0x3Fu));
}

uint64_t a64_fpcr_read(Ctx *A64_NOESC c) { return c->fpcr & 0xFFFFFFFFu; }

/* (bits 32 and up of c->fpcr are the host's; MSR keeps them) */
void a64_fpcr_write(Ctx *A64_NOESC c, uint64_t v) {
    c->fpcr = (v & 0xFFFFFFFFu) | (c->fpcr & ~(uint64_t)0xFFFFFFFFu);
    a64_fpcr_sync(c);
}

/* FPSR = the context's | MXCSR's sticky bits: IE -> IOC, ZE -> DZC, OE -> OFC, UE -> UFC, PE -> IXC, and DE -> IDC
   under FZ only (without FZ ARM has no input denormal flag for what x86 flags as DE) */
uint64_t a64_fpsr_read(Ctx *A64_NOESC c) {
    unsigned m = _mm_getcsr();
    uint64_t f = c->fpsr;
    f |= (m & 1u ? 1u : 0) | (m & 4u ? 2u : 0) | (m & 8u ? 4u : 0) | (m & 16u ? 8u : 0) | (m & 32u ? 16u : 0);
    if ((m & 2u) && (c->fpcr >> 24 & 1)) f |= 128u;
    return f;
}

void a64_fpsr_write(Ctx *A64_NOESC c, uint64_t v) {
    c->fpsr = v;
    _mm_setcsr(_mm_getcsr() & ~0x3Fu);
}

static int fz(void) { return (_mm_getcsr() & 0x8040) != 0; }
static int rmode(void) { unsigned rc = _mm_getcsr() >> 13 & 3; return rc == 1 ? 2 : rc == 2 ? 1 : (int)rc; }

static unsigned recip_estimate(unsigned a) {      /* a: 256..511, [0.5, 1) in 1/512 */
    a = a * 2 + 1;
    unsigned b = (1u << 19) / a;
    return (b + 1) / 2;
}

static unsigned rsqrt_estimate(unsigned a) {      /* a: 128..511, [0.25, 1) in 1/512 */
    if (a < 256)
        a = a * 2 + 1;
    else {
        a = (a >> 1) << 1;
        a = (a + 1) * 2;
    }
    unsigned b = 512;
    while ((uint64_t)a * (b + 1) * (b + 1) < (1u << 28))
        b++;
    return (b + 1) / 2;
}

/* one routine for both widths: E exponent bits, F fraction bits */
static uint64_t frecpe(uint64_t op, int E, int F) {
    int N = 1 + E + F;
    uint64_t sign = op >> (N - 1) & 1, emask = (1ull << E) - 1, fmask = (1ull << F) - 1;
    int64_t exp = (int64_t)(op >> F & emask);
    uint64_t frac = op & fmask;
    uint64_t inf = sign << (N - 1) | emask << F;
    if (exp == (int64_t)emask) {
        if (frac) return op | 1ull << (F - 1);    /* NaN: quieted */
        return sign << (N - 1);                    /* infinity: zero */
    }
    if (exp == 0 && (frac == 0 || fz())) return inf;   /* zero (or flushed): infinity */
    /* |value| < 2^-128 (single), 2^-1024 (double): the result overflows */
    int bias = (int)(emask >> 1);
    if (exp == 0 && !(frac >> (F - 2))) {
        int r = rmode(), up = r == 0 || (r == 1 && !sign) || (r == 2 && sign);
        return up ? inf : (sign << (N - 1) | (emask - 1) << F | fmask);
    }
    if (fz() && exp >= 2 * bias - 1)                /* |value| >= 2^126 (2^1022): the result would be subnormal, flushed */
        return sign << (N - 1);
    uint64_t f52 = frac << (52 - F);               /* the fraction left-aligned in 52 bits */
    if (exp == 0) {
        if (!(f52 >> 51 & 1)) { exp = -1; f52 = (f52 << 2) & ((1ull << 52) - 1); }
        else f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    unsigned scaled = 256 | (unsigned)(f52 >> 44 & 255);
    int64_t rexp = 2 * bias - 1 - exp;
    uint64_t est = recip_estimate(scaled);
    uint64_t rf = (est & 255) << 44;
    if (rexp == 0) rf = 1ull << 51 | rf >> 1;
    else if (rexp == -1) { rf = 1ull << 50 | rf >> 2; rexp = 0; }
    return sign << (N - 1) | ((uint64_t)rexp & emask) << F | rf >> (52 - F);
}

static uint64_t frsqrte(uint64_t op, int E, int F) {
    int N = 1 + E + F;
    uint64_t sign = op >> (N - 1) & 1, emask = (1ull << E) - 1, fmask = (1ull << F) - 1;
    int64_t exp = (int64_t)(op >> F & emask);
    uint64_t frac = op & fmask;
    int bias = (int)(emask >> 1);
    if (exp == (int64_t)emask && frac) return op | 1ull << (F - 1);
    if (exp == 0 && (frac == 0 || fz())) return sign << (N - 1) | emask << F;
    if (sign) return emask << F | 1ull << (F - 1);           /* negative: the default NaN */
    if (exp == (int64_t)emask) return 0;                      /* +infinity: +0 */
    uint64_t f52 = frac << (52 - F);
    if (exp == 0) {
        while (!(f52 >> 51 & 1)) { f52 = (f52 << 1) & ((1ull << 52) - 1); exp--; }
        f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    unsigned scaled = (exp & 1) == 0 ? (256 | (unsigned)(f52 >> 44 & 255)) : (128 | (unsigned)(f52 >> 45 & 127));
    int64_t rexp = (3 * bias - 1 - exp) / 2;     /* 380 - exp (single), 3068 - exp (double), halved */
    uint64_t est = rsqrt_estimate(scaled);
    return ((uint64_t)rexp & emask) << F | ((est & 255) << 44) >> (52 - F);
}

static uint32_t f2u(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static float u2f(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }
static uint64_t d2u(double x) { uint64_t u; memcpy(&u, &x, 8); return u; }
static double u2d(uint64_t u) { double x; memcpy(&x, &u, 8); return x; }

float a64_frecpe_f32(float x) { return u2f((uint32_t)frecpe(f2u(x), 8, 23)); }
double a64_frecpe_f64(double x) { return u2d(frecpe(d2u(x), 11, 52)); }
float a64_frsqrte_f32(float x) { return u2f((uint32_t)frsqrte(f2u(x), 8, 23)); }
double a64_frsqrte_f64(double x) { return u2d(frsqrte(d2u(x), 11, 52)); }
