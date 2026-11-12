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
    hos_log("gridlog: present %lld obj %#llx flags %#x: since the start %lld calls, grid taken %lld (of the frame before %lld); "
            "frame counter %u, slot stamps %u %u (slots %#llx %#llx, index %u)",
            (long long)p, (unsigned long long)o, f, (long long)calls, (long long)taken, (long long)fresh, g32(o + 0x18),
            g32(s0 + 0x1540), g32(s1 + 0x1540), (unsigned long long)s0, (unsigned long long)s1, (unsigned)*(uint8_t *)GP(o + 0x13));
    return 0;
}
