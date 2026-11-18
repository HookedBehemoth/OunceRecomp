/* Game patch, direct mode only: a barrier before and after the two depth copies of the 3D layer's list.

   f_80289590 (FogPrePass::HalfDepth, its call at 0x80289cc0) and f_80666d20 (gsys::RenderBuffer::SoftEdgeDepth, its call
   at 0x806675d8) each copy a D32F depth target with nvnCommandBufferCopyTextureToTexture right after the pass that
   rendered it, in the list begun from 0x8007b0c8 (f_8007ad40), with an event signal and wait but no barrier: on the
   console the copy engine follows the 3D engine in order; Vulkan needs the copy to wait for the rendering's depth store
   (read after write), for the last pass that used the destination (write after write), and the next pass for the copy's
   read (write after read). An instead row on each copy's BLR x8 (runtime/patch.h PATCH_AT) calls a host function with the
   proc and the copy's eight arguments in its place, which records Barrier(ORDER_FRAGMENTS | INVALIDATE_TEXTURE), the
   copy, and the same barrier again, on the list's command buffer; it asserts the proc is the copy.

   Check that it applies: the 600-present line "direct patch depth copy" (copies a frame with their barriers, per site). */
#include "patches.h"
#include <string.h>
#include "os.h"

uint64_t nvn_native_proc(const char *name);   /* nvn_native.c */

static volatile int64_t n_site[2];
#define BITS 0x12u   /* NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT */

/* the host's proc of name (nvn_native_proc, the same each time: a race only looks it up twice) */
static uint64_t proc_of(uint64_t *p, const char *name) {
    uint64_t v = __atomic_load_n(p, __ATOMIC_RELAXED);
    if (!v) __atomic_store_n(p, v = nvn_native_proc(name), __ATOMIC_RELAXED);
    return v;
}
static uint64_t p_copy, p_barrier;

/* the copy (cmd, src, src view, src region, dst, dst view, dst region, flags) with a barrier before and after it;
   site: 0 f_80289590's, 1 f_80666d20's */
PATCH_AT(0x80289cc0, word(0xd63f0100), instead, none, direct_depth_copy, guest(computed), sp, as(x8, proc),
         as(k(0), site), as(x0, cmd), x1, x2, x3, x4, x5, x6, x7) {
    const uint64_t copy = proc_of(&p_copy, "nvnCommandBufferCopyTextureToTexture");
    if (proc != copy)
        hos_fatal(NULL, "direct depth copy: the call of site %u is of %#llx, not nvnCommandBufferCopyTextureToTexture (%#llx)",
                  site, (unsigned long long)proc, (unsigned long long)copy);
    const uint64_t barrier = proc_of(&p_barrier, "nvnCommandBufferBarrier");
    guest_call(g, sp, barrier, cmd, BITS, 0, 0, 0);
    guest_call8(g, sp, proc, cmd, x1, x2, x3, x4, x5, x6, x7);
    guest_call(g, sp, barrier, cmd, BITS, 0, 0, 0);
    os_atomic_inc(&n_site[site]);
}
PATCH_AT_SITE(0x806675d8, word(0xd63f0100), instead, none, direct_depth_copy, guest(computed), sp, x8, k(1), x0, x1, x2,
              x3, x4, x5, x6, x7);

void direct_depth_copy_log(double frames) {
    static int64_t last[2];
    const int64_t now[2] = {n_site[0], n_site[1]};
    hos_log("direct patch depth copy (f_80289590 0x80289cc0, f_80666d20 0x806675d8): a frame: copies with barriers around "
            "them %.2f, %.2f", (now[0] - last[0]) / frames, (now[1] - last[1]) / frames);
    memcpy(last, now, sizeof last);
}
