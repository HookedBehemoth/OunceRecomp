/* The game's side of the runtime's hook points (runtime/game_hooks.h): each hook calls the patches that take part
   there. */
#include "patches.h"
#include "rx_nvn.h"
#include "os.h"

void rt_slots_texture_init(uint64_t obj, uint64_t gpu, const nvn_texdesc *d, int dta);
void rt_slots_frame(void);
extern volatile int64_t nvn_texture_inits_dta;   /* (rt_slots.c) */

void game_frame(void) {
    rt_slots_frame();
    if (rx_direct()) direct_patches_frame();   /* (the patches' lines) */
}

/* (agl's DynamicTextureAllocator::alloc, f_8005ed00 .. its end: the transient render targets' textures) */
static int dta_caller(uint64_t lr) { return lr >= 0x8005ed00ull && lr < 0x80062b6cull; }

void game_texture_initializing(uint64_t obj, uint64_t gpu, const void *desc, uint64_t lr) {
    if (dta_caller(lr)) os_atomic_inc(&nvn_texture_inits_dta);
    /* (rt_slots.c: a texture in the transient slots, from any caller: its slot's bytes go to the back end first) */
    rt_slots_texture_init(obj, gpu, (const nvn_texdesc *)desc, dta_caller(lr));
}

void game_texture_initialized(const void *nvn_texture) {
    /* (assets.c: a terrain tile given before its texture) */
    if (direct_assets_tiles_pending || direct_assets_tilelog) direct_assets_texture_init(nvn_texture);
}

/* agl::utl::DepthBufferEx's coarse occlusion grid (f_800998a0 -> f_80365f00): two 0x3980-byte slots the GPU
   writes a frame each, with the frame's camera and a GPU-written frame stamp at +0x1540. The game takes the slot stamped
   frame-1, else the other one stamped frame-2, else culls without it that frame: it never needs the GPU done, and on the
   console the invalidate is a cache op. Here the invalidate waits for the submission writing the slot back, ~8 ms every
   frame on a job worker on the frame's critical path (the replay is a frame behind the game). No wait for any of its four
   invalidates: lr 0x803665d4 and 0x80366168 (the current slot, before and after the copy), 0x8036635c (the other slot),
   and the tail call's (lr the caller's, 0x80099d84): the reader marks its own run (its PATCH_FUNC below runs its body
   with this thread noted), so the invalidate (game_invalidate_unneeded) is told by who asks, not by its return address. The exposure meter
   (f_80357060, read and zeroed) keeps its wait. */
static _Thread_local int tl_grid_reader;
/* report_reader_a: the grid reader's body (orig), this thread noted */
PATCH_FUNC(0x80365f00, word(0xd503237f), body(0x7f2d99cdf2bfd4bf), aapcs, x0, nvn_grid_reader, orig) {
    if (tl_grid_reader) return guest_orig(orig);
    tl_grid_reader = 1;
    const uint64_t r = guest_orig(orig);
    tl_grid_reader = 0;
    return r;
}
int game_invalidate_unneeded(uint64_t size) { return tl_grid_reader && size == 0x3980; }

/* ---- the benchmark (runtime/bench.c) ---- */
/* the engine's worker threads wait for work in sead's message-queue receive (f_834a3060; w1 0: a blocking receive),
   one receive a message taken */
PATCH_AT(0x834a3060, word(0xd503237f), before, none, bench_msg_taken, w1) {
    if (!w1) hos_bench_message();
}
int game_textures_settled(void) {
    const uint64_t mgr = G64(0x84ca9bf0);   /* (agl::TextureDataMgr; +0x338 its Replace thread's "work pending") */
    return mgr && !*(const uint8_t *)GP(mgr + 0x338);
}

/* ---- code pointers (runtime/code_ptrs.c) ---- */
/* __nnmusl_init_dso's range check of nnMain (0x850565b0) reads a code pointer the lift made a host pointer */
PATCH_AT(0x850565b0, word(0xf947158c), after, x12, hos_code_guest_value, as(x12, v)) {   /* ldr x12, [x12, #0xe28] */
    return hos_code_ptr_as_guest(v);
}
