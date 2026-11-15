/* HOS_DBG=envlog (diagnosis only, either back end): what f_80357060 reads and writes, every 300 presents.

   f_80357060 (x0 = its object O) reads three values the GPU summed into small buffers ([O+0x48] + [[O+0x40]]+4,
   [O+0x1f0] + [[O+0x1e8]]+4 at +0xc, [O+0x280] + [[O+0x278]]+4), folds each into a running average (O+0x32c, +0x34c,
   +0x36c), and writes the sky / ambient pass's 16-float uniform: the sun direction, and words 11-14 from the environment
   block L = [[[[0x84c9e740]]+0x1e8]+0xc98] (+0x9c0..0x9cc) times [[0x84c9e740]+0x1e8]+0xdac. This logs those inputs, the
   averages and L's words, so the two back ends can be compared. With HOS_DBG unset nothing
   here runs. */
#include "hos.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>

extern volatile LONG64 hos_present_count;

static int envlog_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *d = getenv("HOS_DBG");
        const char *p = d ? strstr(d, "envlog") : NULL;
        on = p && (p == d || p[-1] == ',') && (p[6] == 0 || p[6] == ',' || p[6] == ':');
    }
    return on;
}

static float gf(uint64_t a) {
    float f = 0;
    if (a && hos_guest_range(a, 4)) memcpy(&f, GP(a), 4);
    return f;
}
static uint64_t g64(uint64_t a) { return a && hos_guest_range(a, 8) ? G64(a) : 0; }
static uint32_t g32(uint64_t a) { return a && hos_guest_range(a, 4) ? G32(a) : 0; }

ENTRY_HOOK(0x80357060) {
    if (!envlog_on() || !(a->x[1] & 1)) return 0;
    static LONG64 last = -1;
    const LONG64 p = hos_present_count;
    if (p / 300 == last / 300) return 0;
    last = p;
    const uint64_t o = a->x[0];
    const uint64_t a1 = g64(o + 0x48) + g32(g64(g64(o + 0x40)) + 4);
    const uint64_t a2 = g64(o + 0x1f0) + g32(g64(g64(o + 0x1e8)) + 4);
    const uint64_t a3 = g64(o + 0x280) + g32(g64(g64(o + 0x278)) + 4);
    const uint64_t g = g64(g64(g64(0x84c9e740)) + 0x1e8), l = g64(g + 0xc98);
    hos_log("envlog: present %lld O %#llx flags %#x/%#x mode %u: in1 %g (at %#llx) in2 %g %g %g %g (at %#llx) in3 %g (at %#llx); "
            "averages %g %g %g; 0x308/0x30c/0x310 %g %g %g; 0x37c %g 0x380 %g 0x384 %g 0x388 %g",
            (long long)p, (unsigned long long)o, g32(o + 0x3a4), (unsigned)*(uint8_t *)GP(o + 0x3a0), g32(o + 0x398), gf(a1),
            (unsigned long long)a1, gf(a2), gf(a2 + 4), gf(a2 + 8), gf(a2 + 12), (unsigned long long)a2, gf(a3),
            (unsigned long long)a3, gf(o + 0x32c), gf(o + 0x34c), gf(o + 0x36c), gf(o + 0x308), gf(o + 0x30c), gf(o + 0x310),
            gf(o + 0x37c), gf(o + 0x380), gf(o + 0x384), gf(o + 0x388));
    {   /* (in2's buffer past the sums: 256 threads' {r, g, b, depth term} at +16, written by compute 435a0138b9013040) */
        int nonzero = 0;
        for (int i = 0; i < 256 * 4; i++) nonzero += g32(a2 + 16 + 4 * (uint64_t)i) != 0;
        hos_log("envlog: in2's threads: %d of 1024 words nonzero; thread 0 %g %g %g %g, thread 136 %g %g %g %g", nonzero, gf(a2 + 16),
                gf(a2 + 20), gf(a2 + 24), gf(a2 + 28), gf(a2 + 16 + 136 * 16), gf(a2 + 20 + 136 * 16), gf(a2 + 24 + 136 * 16),
                gf(a2 + 28 + 136 * 16));
    }
    {   /* (the 64^3 BC4 noise volume's storage: zero in direct mode) */
        int nonzero = 0;
        for (uint64_t i = 0; i < 0x20000; i += 8) nonzero += g64(0x1140bcb000 + i) != 0;
        hos_log("envlog: guest 0x1140bcb000+0x20000: %d of 16384 words nonzero", nonzero);
    }
    hos_log("envlog: G %#llx L %#llx: G+0xdac %g; L+0x9c0 %g %g %g %g; L+0x960 %g 0x980 %g 0x9a0 %g 0xaf0 %g; L+0x6c0 %g %g %g",
            (unsigned long long)g, (unsigned long long)l, gf(g + 0xdac), gf(l + 0x9c0), gf(l + 0x9c4), gf(l + 0x9c8), gf(l + 0x9cc),
            gf(l + 0x960), gf(l + 0x980), gf(l + 0x9a0), gf(l + 0xaf0), gf(l + 0x6c0), gf(l + 0x6c4), gf(l + 0x6c8));
    return 0;
}
