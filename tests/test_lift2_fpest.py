"""FRECPE and FRSQRTE inline (nx2/lift2/fp_estimate.py) against the reference arithmetic, without the game.

The reference is the Arm ARM's FPRecipEstimate / FPRSqrtEstimate with RecipEstimate / RecipSqrtEstimate (FEAT_RPRES
off) in C, as host/hos_fp.c ran them per lane before they were inlined (FZ and the rounding mode passed in; a NaN
quieted, FPCR.DN not modelled). Compiled with the helpers' IR and compared bit for bit:
- single: every mantissa (2^23) of the exponents 0 (denormals), 1 and 2, both signs; every
  other exponent at every table index and both neighbours of each index boundary; FZ off and on; for FRECPE's
  denormal overflow every rounding mode;
- double: every exponent at every table index and its neighbours, denormals with the leading one at every position,
  the special classes, random mantissas; FZ off and on, every rounding mode.
The vector forms run through the lifter in tests/test_lift2_fp.py (lane by lane against its Python reference).
Also: the helpers read FPCR at offsetof(GuestContext, fpcr).

python -I -B tests/test_lift2_fpest.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import fp_estimate, llvm  # noqa: E402
import rtgen  # noqa: E402

REF = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "a64.h"
_Static_assert(offsetof(GuestContext, fpcr) == %(off)d, "fp_estimate.FPCR_OFF");
static int FZ, RM;   /* (FPCR.FZ, FPCR.RMode) */
static int fz(void) { return FZ; }
static int rmode(void) { return RM; }
static unsigned recip_estimate(unsigned a) { a = a * 2 + 1; unsigned b = (1u << 19) / a; return (b + 1) / 2; }
static unsigned rsqrt_estimate(unsigned a) {
    if (a < 256) a = a * 2 + 1; else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    unsigned b = 512;
    while ((uint64_t)a * (b + 1) * (b + 1) < (1u << 28)) b++;
    return (b + 1) / 2;
}
static uint64_t frecpe(uint64_t op, int E, int F) {
    int N = 1 + E + F;
    uint64_t sign = op >> (N - 1) & 1, emask = (1ull << E) - 1, fmask = (1ull << F) - 1;
    int64_t exp = (int64_t)(op >> F & emask);
    uint64_t frac = op & fmask;
    uint64_t inf = sign << (N - 1) | emask << F;
    if (exp == (int64_t)emask) { if (frac) return op | 1ull << (F - 1); return sign << (N - 1); }
    if (exp == 0 && (frac == 0 || fz())) return inf;
    int bias = (int)(emask >> 1);
    if (exp == 0 && !(frac >> (F - 2))) {
        int r = rmode(), up = r == 0 || (r == 1 && !sign) || (r == 2 && sign);
        return up ? inf : (sign << (N - 1) | (emask - 1) << F | fmask);
    }
    if (fz() && exp >= 2 * bias - 1) return sign << (N - 1);
    uint64_t f52 = frac << (52 - F);
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
    if (sign) return emask << F | 1ull << (F - 1);
    if (exp == (int64_t)emask) return 0;
    uint64_t f52 = frac << (52 - F);
    if (exp == 0) {
        while (!(f52 >> 51 & 1)) { f52 = (f52 << 1) & ((1ull << 52) - 1); exp--; }
        f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    unsigned scaled = (exp & 1) == 0 ? (256 | (unsigned)(f52 >> 44 & 255)) : (128 | (unsigned)(f52 >> 45 & 127));
    int64_t rexp = (3 * bias - 1 - exp) / 2;
    uint64_t est = rsqrt_estimate(scaled);
    return ((uint64_t)rexp & emask) << F | ((est & 255) << 44) >> (52 - F);
}
float w_frecpe_f32(GuestContext *, float); float w_frsqrte_f32(GuestContext *, float);
double w_frecpe_f64(GuestContext *, double); double w_frsqrte_f64(GuestContext *, double);
static GuestContext ctx;
static long n_checked, n_bad;
static void one32(uint32_t u) {
    float x; memcpy(&x, &u, 4);
    float a = w_frecpe_f32(&ctx, x), b = w_frsqrte_f32(&ctx, x);
    uint32_t ua, ub; memcpy(&ua, &a, 4); memcpy(&ub, &b, 4);
    uint32_t ra = (uint32_t)frecpe(u, 8, 23), rb = (uint32_t)frsqrte(u, 8, 23);
    n_checked += 2;
    if (ua != ra && n_bad++ < 8) printf("frecpe f32 %%08x fz %%d rm %%d: %%08x, reference %%08x\n", u, FZ, RM, ua, ra);
    if (ub != rb && n_bad++ < 8) printf("frsqrte f32 %%08x fz %%d rm %%d: %%08x, reference %%08x\n", u, FZ, RM, ub, rb);
}
static void one64(uint64_t u) {
    double x; memcpy(&x, &u, 8);
    double a = w_frecpe_f64(&ctx, x), b = w_frsqrte_f64(&ctx, x);
    uint64_t ua, ub; memcpy(&ua, &a, 8); memcpy(&ub, &b, 8);
    uint64_t ra = frecpe(u, 11, 52), rb = frsqrte(u, 11, 52);
    n_checked += 2;
    if (ua != ra && n_bad++ < 8) printf("frecpe f64 %%016llx fz %%d rm %%d: %%016llx, reference %%016llx\n",
                                       (unsigned long long)u, FZ, RM, (unsigned long long)ua, (unsigned long long)ra);
    if (ub != rb && n_bad++ < 8) printf("frsqrte f64 %%016llx fz %%d rm %%d: %%016llx, reference %%016llx\n",
                                       (unsigned long long)u, FZ, RM, (unsigned long long)ub, (unsigned long long)rb);
}
static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
int main(void) {
    static const int full[] = {0, 1, 2};   /* (denormals, an odd and an even exponent: every mantissa) */
    for (FZ = 0; FZ < 2; FZ++)
        for (RM = 0; RM < 4; RM++) {
            ctx.fpcr = (uint64_t)FZ << 24 | (uint64_t)RM << 22 | 0xFFFFFFFF00000000ull;   /* (the host's high bits) */
            for (int s = 0; s < 2; s++) {
                for (unsigned k = 0; k < sizeof full / sizeof full[0]; k++) {
                    if (RM && full[k]) continue;   /* (the rounding mode decides only FRECPE's denormal overflow) */
                    for (uint32_t m = 0; m < (1u << 23); m++) one32((uint32_t)s << 31 | (uint32_t)full[k] << 23 | m);
                }
                if (RM) continue;
                for (uint32_t e = 0; e < 256; e++)
                    for (uint32_t t = 0; t < 256; t++)
                        for (int d = -1; d <= 1; d++) {
                            uint32_t m = ((t << 15) + (uint32_t)d) & 0x7FFFFF;
                            one32((uint32_t)s << 31 | e << 23 | m);
                            one32((uint32_t)s << 31 | e << 23 | (m | 0x7FFF));
                        }
            }
            for (int s = 0; s < 2; s++) {
                for (uint64_t e = 0; e < 2048; e++) {
                    if (RM && e) continue;
                    for (uint64_t t = 0; t < 256; t++)
                        for (int d = -1; d <= 1; d++) {
                            uint64_t m = ((t << 44) + (uint64_t)(int64_t)d) & ((1ull << 52) - 1);
                            one64((uint64_t)s << 63 | e << 52 | m);
                            one64((uint64_t)s << 63 | e << 52 | (m | ((1ull << 44) - 1)));
                            one64((uint64_t)s << 63 | e << 52 | (next() & ((1ull << 52) - 1)));
                        }
                }
                for (int k = 0; k < 52; k++)   /* (denormals: the leading one at every position) */
                    for (int r = 0; r < 64; r++) {
                        uint64_t m = 1ull << k | (next() & ((1ull << k) - 1));
                        one64((uint64_t)s << 63 | m);
                        one64((uint64_t)s << 63 | 1ull << k);
                    }
                one64((uint64_t)s << 63 | 0x7FF0000000000000ull);   /* infinity */
                one64((uint64_t)s << 63 | 0x7FF0000000000001ull);   /* signalling NaN */
                one64((uint64_t)s << 63 | 0x7FF8000000000123ull);   /* quiet NaN with a payload */
            }
        }
    printf("checked %%ld, bad %%ld\n", n_checked, n_bad);
    return n_bad != 0;
}
"""


def wrappers():
    out = []
    for kind in ("frecpe", "frsqrte"):
        for n, ft in ((32, "float"), (64, "double")):
            out.append("define %s @w_%s_f%d(ptr %%c, %s %%x) {\n  %%r = call %s @lift2_%s_f%d(ptr %%c, %s %%x)\n"
                       "  ret %s %%r\n}" % (ft, kind, n, ft, ft, kind, n, ft, ft))
    return "\n".join(out)


class Estimates(unittest.TestCase):
    def test_tables(self):
        # (spot values from the Arm ARM's tables: RecipEstimate(256) = 511, RecipSqrtEstimate(128) = 511, (511) = 256)
        self.assertEqual(fp_estimate.recip_estimate(256), 511)
        self.assertEqual(fp_estimate.rsqrt_estimate(128), 511)
        self.assertEqual(fp_estimate.rsqrt_estimate(511), 256)
        self.assertTrue(all(256 <= fp_estimate.recip_estimate(a) <= 511 for a in range(256, 512)))
        self.assertTrue(all(256 <= fp_estimate.rsqrt_estimate(a) <= 511 for a in range(128, 512)))

    def test_bit_exact(self):
        helpers = fp_estimate.helpers()
        decls = sorted({d for _, ds in helpers.values() for d in ds})
        ir = "\n".join(decls + [t for t, _ in helpers.values()]) + "\n" + wrappers() + "\n"
        with tempfile.TemporaryDirectory() as d:
            rt = rtgen.write(os.path.join(d, "rt64"))
            with open(os.path.join(d, "est.ll"), "w") as f:
                f.write(ir)
            with open(os.path.join(d, "ref.c"), "w") as f:
                f.write(REF % {"off": fp_estimate.FPCR_OFF})
            exe = os.path.join(d, "est")
            subprocess.run([llvm.check("clang"), "-O2", "-w", "-march=x86-64-v3", "-I" + rt,
                            os.path.join(d, "ref.c"), os.path.join(d, "est.ll"), "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:])
        self.assertIn("bad 0", r.stdout)


if __name__ == "__main__":
    unittest.main()
