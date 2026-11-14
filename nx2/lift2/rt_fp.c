/* Lifter 2's FP runtime routines: FRECPE and FRSQRTE, Arm's estimate algorithm (the Arm ARM's FPRecipEstimate and
   FPRSqrtEstimate with RecipEstimate/RecipSqrtEstimate, FEAT_RPRES off), exact. Lifted code calls them per lane;
   they read FPCR.FZ and RMode from MXCSR (DAZ|FTZ, RC), which the runtime keeps equal to the guest FPCR.
   FPCR.DN is not modelled: a NaN operand comes back quieted (docs/lifter2-design.md section 7.1). */
#include <stdint.h>
#include <string.h>
#include <immintrin.h>

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
