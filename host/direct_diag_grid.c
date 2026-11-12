/* (native-vk M6c) HOS_DBG=gridlog (diagnosis only, either back end): whether the game's CPU occlusion culling runs.

   agl::utl::DepthBufferEx's coarse depth grid (work/assets/native/readback.md): the GPU writes a 32x18 grid and the camera
   into one of two slots each frame, with a frame stamp; f_80365f00 takes a slot only if its stamp is the frame before (flags
   bits 0|1 at obj+0x10) or the one before that (bit 1 only); f_80822260 (x0 = the object) culls against it only when bit 1
   is set, and otherwise turns occlusion culling off for that frame (everything drawn). This counts, at f_80822260's entry,
   the frames with bit 1 (grid taken) and bit 0 (the frame before's), and logs them every 600 presents with the object's
   frame counter (obj+0x18) and both slots' stamps (slot = [obj+0x2bb0 + 0x90 i] + 0x1540). With HOS_DBG unset nothing here
   runs. */
#include "hos.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>

extern volatile LONG64 hos_present_count;

static int gridlog_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *d = getenv("HOS_DBG");
        const char *p = d ? strstr(d, "gridlog") : NULL;
        on = p && (p == d || p[-1] == ',') && (p[7] == 0 || p[7] == ',' || p[7] == ':');
    }
    return on;
}

static uint64_t g64(uint64_t a) { return a && hos_guest_range(a, 8) ? G64(a) : 0; }
static uint32_t g32(uint64_t a) { return a && hos_guest_range(a, 4) ? G32(a) : 0; }
static uint16_t g16(uint64_t a) { return a && hos_guest_range(a, 2) ? *(uint16_t *)GP(a) : 0; }

static volatile LONG64 calls, taken, fresh;

ENTRY_HOOK(0x80822260) {
    if (!gridlog_on()) return 0;
    const uint64_t o = c->x[0];
    const uint16_t f = g16(o + 0x10);
    InterlockedIncrement64(&calls);
    if (f & 2) InterlockedIncrement64(&taken);
    if (f & 1) InterlockedIncrement64(&fresh);
    static LONG64 last = -1;
    const LONG64 p = hos_present_count;
    if (p / 600 == last / 600) return 0;
    last = p;
    const uint64_t s0 = g64(o + 0x2bb0), s1 = g64(o + 0x2bb0 + 0x90);
    /* (the two slots' memory, at the pools readback.md names: stamp at +0x1540, 576 tile pairs (value, depth) from +0x240) */
    for (int k = 0; k < 2; k++) {
        const uint64_t b = k ? 0x113b628000ull : 0x113b623000ull;
        float lo = 1e30f, hi = -1e30f, sum = 0, vlo = 1e30f, vhi = -1e30f;
        for (int i = 0; i < 576; i++) {
            float v, dd;
            uint32_t a = g32(b + 0x240 + 8 * (uint64_t)i), z = g32(b + 0x244 + 8 * (uint64_t)i);
            memcpy(&v, &a, 4), memcpy(&dd, &z, 4);
            lo = dd < lo ? dd : lo, hi = dd > hi ? dd : hi, sum += dd;
            vlo = v < vlo ? v : vlo, vhi = v > vhi ? v : vhi;
        }
        float w[22];
        for (int i = 0; i < 16; i++) { uint32_t x = g32(b + 0x80 + 4 * (uint64_t)i); memcpy(&w[i], &x, 4); }
        for (int i = 0; i < 6; i++) { uint32_t x = g32(b + 0x1558 + 4 * (uint64_t)i); memcpy(&w[16 + i], &x, 4); }
        hos_log("gridlog: slot %d at %#llx: stamp %u dims %u %u; tile depth min %g max %g mean %g; tile value min %g max %g; "
                "+0x1558.. %g %g %g %g %g %g; projection %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", k,
                (unsigned long long)b, g32(b + 0x1540), g32(b + 0x1550), g32(b + 0x1554), lo, hi, sum / 576, vlo, vhi, w[16], w[17], w[18],
                w[19], w[20], w[21], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
    }
    {   /* (M6d) the grid compute's storage input 0x113b62d000 (s1): 576 16-byte tile records (f78bce74 sets them to 0xffffff),
           and from +0x4c00 the corner table 3237af3d fills with the main view's depth sampled at the 33x19 tile corners; the
           grid compute negates a tile's depth when all four of its corners are negative and one is -1 */
        const uint64_t s = 0x113b62d000ull;
        float lo = 1e30f, hi = -1e30f;
        int neg = 0, m1 = 0, zero = 0;
        for (int i = 0; i < 33 * 19; i++) {
            uint32_t a = g32(s + 0x4c00 + 4 * (uint64_t)i);
            float v;
            memcpy(&v, &a, 4);
            lo = v < lo ? v : lo, hi = v > hi ? v : hi;
            neg += v < 0, m1 += v == -1.0f, zero += v == 0.0f;
        }
        uint32_t rlo = 0xffffffffu, rhi = 0;
        for (int i = 0; i < 576 * 4; i++) {
            uint32_t a = g32(s + 4 * (uint64_t)i);
            rlo = a < rlo ? a : rlo, rhi = a > rhi ? a : rhi;
        }
        hos_log("gridlog: corners at %#llx+0x4c00 (627): min %g max %g, negative %d, -1 %d, zero %d; words %08x %08x %08x %08x %08x %08x %08x %08x; "
                "tile records min %#x max %#x", (unsigned long long)s, lo, hi, neg, m1, zero, g32(s + 0x4c00), g32(s + 0x4c04), g32(s + 0x4c08),
                g32(s + 0x4c0c), g32(s + 0x4c00 + 4 * 300), g32(s + 0x4c00 + 4 * 301), g32(s + 0x4c00 + 4 * 600), g32(s + 0x4c00 + 4 * 626), rlo, rhi);
    }
    for (int k = 0; k < 3; k++) {   /* (the grid compute's uniform buffers in the trace: words 768-799 and 0-7) */
        const uint64_t u = k == 0 ? 0x113b649000ull : k == 1 ? 0x113b65b000ull : 0x113b620000ull;
        hos_log("gridlog: ubo %#llx: +768.. %08x %08x %08x %08x %08x %08x %08x %08x; +0.. %08x %08x %08x %08x", (unsigned long long)u,
                g32(u + 768), g32(u + 772), g32(u + 776), g32(u + 780), g32(u + 784), g32(u + 788), g32(u + 792), g32(u + 796), g32(u), g32(u + 4),
                g32(u + 8), g32(u + 12));
    }
    hos_log("gridlog: present %lld obj %#llx flags %#x: since the start %lld calls, grid taken %lld (of the frame before %lld); "
            "frame counter %u, slot stamps %u %u (slots %#llx %#llx, index %u)",
            (long long)p, (unsigned long long)o, f, (long long)calls, (long long)taken, (long long)fresh, g32(o + 0x18),
            g32(s0 + 0x1540), g32(s1 + 0x1540), (unsigned long long)s0, (unsigned long long)s1, (unsigned)*(uint8_t *)GP(o + 0x13));
    return 0;
}
