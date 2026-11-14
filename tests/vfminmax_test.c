/* tests/test_vfminmax.py's driver: each guest word's lift with the SSE min/max test (and its hot form) against the exact
   block alone, on random registers and FP states; the whole Ctx must match.
     vfminmax_test <cases> <seed>      exit 0: no mismatch; 1: a mismatch; 3: this CPU lacks the -march it was built for */
#include "a64.h"
#include <stdio.h>
#include <stdlib.h>
#include <cpuid.h>
#include "vfminmax_cases.h"

static uint64_t rng;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static uint32_t rf(int normal) {
    const uint32_t s = (uint32_t)rnd() & 0x80000000u;
    switch (normal ? 5 + rnd() % 11 : rnd() % 16) {
    case 0: return s;
    case 1: return s | ((uint32_t)rnd() & 0x7fffffu);
    case 2: return s | 0x7f800000u;
    case 3: return s | 0x7fc00000u | ((uint32_t)rnd() & 0x3fffffu);
    case 4: return s | 0x7f800001u | ((uint32_t)rnd() & 0x3ffffeu);
    case 5: return s | 0x00800000u;
    case 6: return s | ((uint32_t)(1 + rnd() % 40) << 23) | ((uint32_t)rnd() & 0x7fffffu);
    case 7: return s | ((uint32_t)(215 + rnd() % 39) << 23) | ((uint32_t)rnd() & 0x7fffffu);
    case 8: case 9: return s | ((uint32_t)(110 + rnd() % 36) << 23) | ((uint32_t)rnd() & 0x7f0000u);   /* midpoints */
    default: return s | ((uint32_t)(100 + rnd() % 56) << 23) | ((uint32_t)rnd() & 0x7fffffu);
    }
}

int main(int argc, char **argv) {
    unsigned eax, ebx, ecx, edx;
#if defined(__AVX2__)
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) || !(ebx >> 5 & 1)) return 3;
#endif
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx >> 20 & 1)) return 3;   /* (SSE4.2: x86-64-v2) */
    const long n = argc > 1 ? atol(argv[1]) : 1000000;
    rng = (argc > 2 ? strtoull(argv[2], 0, 10) : 1) | 1;
    static const uint64_t fpsrs[] = {0x9f, 0x10, 0x00, 0x1f, 0x90, 0x98, 0x94, 0x18, 0x14, 0x80, 0x08, 0x88};
    static const uint64_t fpcrs[] = {0, 1u << 24, (1u << 24) | (1u << 25), 1u << 22, 0x9f00, 1u << 25};
    const int ng = (int)(sizeof GUEST / sizeof GUEST[0]);
    long bad = 0;
    static Ctx A, B;
    for (long it = 0; it < n; it++) {
        const int g = (int)(rnd() % (uint64_t)ng), hot = (int)(rnd() & 1), normal = (int)(rnd() & 1);
        memset(&A, 0, sizeof A);
        for (int r = 0; r < 4; r++)
            for (int l = 0; l < 4; l++) { const uint32_t v = rf(normal); memcpy((uint8_t *)A.vreg[r] + 4 * l, &v, 4); }
        for (int l = 0; l < 4; l++) {   /* (equal operands and zeros of both signs: the cases MINPS / MAXPS order alone) */
            uint32_t v;
            memcpy(&v, (uint8_t *)A.vreg[1] + 4 * l, 4);
            switch (rnd() % 6) {
            case 0: memcpy((uint8_t *)A.vreg[2] + 4 * l, &v, 4); break;
            case 1: v = (uint32_t)rnd() & 0x80000000u; memcpy((uint8_t *)A.vreg[1] + 4 * l, &v, 4);
                    v = (uint32_t)rnd() & 0x80000000u; memcpy((uint8_t *)A.vreg[2] + 4 * l, &v, 4); break;
            default: break;
            }
        }
        A.fpsr = fpsrs[rnd() % (sizeof fpsrs / sizeof *fpsrs)];
        A.fpcr = rnd() % 4 ? fpcrs[rnd() % 2] : fpcrs[rnd() % (sizeof fpcrs / sizeof *fpcrs)];
        B = A;
        (hot ? GUEST[g].nh : GUEST[g].n)(&A);
        (hot ? GUEST[g].oh : GUEST[g].o)(&B);
        if (memcmp(&A, &B, sizeof A)) {
            if (bad++ < 8)
                printf("MISMATCH %s%s: fpcr %llx fpsr %llx/%llx\n", GUEST[g].name, hot ? " (hot)" : "",
                       (unsigned long long)A.fpcr, (unsigned long long)A.fpsr, (unsigned long long)B.fpsr);
        }
    }
    printf("vfminmax (%s): %ld runs of %d guest words, %ld mismatches\n",
#if defined(__AVX2__)
           "x86-64-v3",
#else
           "x86-64-v2",
#endif
           n, ng, bad);
    return bad != 0;
}
