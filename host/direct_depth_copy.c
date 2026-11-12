/* (native-vk M3a) Game patch, direct mode only: a barrier before and after the two depth copies of the 3D layer's list
   (docs/direct/m2a-report.md, "The hazards"; the validation layer's three hazards on them).

   f_80289590 (FogPrePass::HalfDepth, return 0x80289cc4) and f_80666d20 (gsys::RenderBuffer::SoftEdgeDepth, return
   0x806675dc) each copy a D32F depth target with nvnCommandBufferCopyTextureToTexture right after the pass that rendered it,
   in the list begun from 0x8007b0c8 (f_8007ad40), with an event signal and wait but no barrier: on the console the copy
   engine follows the 3D engine in order; Vulkan needs the copy to wait for the rendering's depth store (read after write),
   for the last pass that used the destination (write after write), and the next pass for the copy's read (write after
   read). A SPLICE after the line that loads the proc into x8 points those two calls at a host function, which records
   Barrier(ORDER_FRAGMENTS | INVALIDATE_TEXTURE), the copy, and the same barrier again, on the list's command buffer.

   Check that it applies: the 600-present line "direct patch depth copy" (copies a frame with their barriers, per site). */
#include "direct.h"
#include <string.h>
#include <windows.h>

uint64_t nvn_native_proc(const char *name);
uint64_t hos_hostcall(const char *name, void (*fn)(Ctx *, void *), void *user);

static uint64_t p_copy, p_barrier, s_copy;
static volatile LONG64 n_site[2], n_other;
#define BITS 0x12u   /* NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT */

static void barrier(Ctx *c, uint64_t cmd) {
    c->x[0] = cmd, c->x[1] = BITS;
    a64_icall(c, p_barrier);
}
static void copy_fn(Ctx *c, void *u) {   /* (cmd, src, src view, src region, dst, dst view, dst region, flags) */
    (void)u;
    const uint64_t lr = c->x[30];
    uint64_t a[8];
    memcpy(a, c->x, sizeof a);
    const int site = lr == 0x80289cc4ull ? 0 : lr == 0x806675dcull ? 1 : -1;
    if (site >= 0) barrier(c, a[0]), memcpy(c->x, a, sizeof a);
    c->x[30] = lr;
    a64_icall(c, p_copy);
    if (site >= 0) barrier(c, a[0]), InterlockedIncrement64(&n_site[site]);
    else InterlockedIncrement64(&n_other);
    c->x[30] = lr;
}
static int ready(void) {
    static volatile LONG state;
    if (state) return state == 1;
    if (!rx_direct()) return InterlockedExchange(&state, 2), 0;
    p_copy = nvn_native_proc("nvnCommandBufferCopyTextureToTexture");
    p_barrier = nvn_native_proc("nvnCommandBufferBarrier");
    s_copy = hos_hostcall("direct_depth_copy copy", copy_fn, NULL);
    InterlockedExchange(&state, p_copy && p_barrier && s_copy ? 1 : 2);
    return state == 1;
}
static void retarget(Ctx *c) {
    if (ready() && c->x[8] == p_copy) c->x[8] = s_copy;
}
SPLICE_AFTER_EACH(0x80289590, "c->x[8]=recomp_load64(c,c->x[8] + 0);") { retarget(c); }
SPLICE_AFTER_EACH(0x80666d20, "c->x[8]=recomp_load64(c,c->x[8] + 0);") { retarget(c); }

void direct_depth_copy_log(double frames) {
    static LONG64 last[3];
    const LONG64 now[3] = {n_site[0], n_site[1], n_other};
    hos_log("direct patch depth copy (f_80289590 0x80289cc4, f_80666d20 0x806675dc): a frame: copies with barriers around them "
            "%.2f, %.2f; other copies of those functions %.2f", (now[0] - last[0]) / frames, (now[1] - last[1]) / frames,
            (now[2] - last[2]) / frames);
    memcpy(last, now, sizeof last);
}
