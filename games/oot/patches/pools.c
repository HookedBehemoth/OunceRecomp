/* Pool claims (runtime/nvn_claims.h, docs/direct/pool-claims.md): what each memory pool the game makes holds, claimed
   at the engine function that makes it. A pool no claim covers is an error in the NVN layer. The texture heap's claim is
   assets.c's (agl::TextureDataMgr::InitializeHeap). Pools still on NVN_CLAIM_LAYER0 move to their own kinds as the
   back end's claim buffers come (the plan's steps 3 to 5). */
#include "hos.h"
#include "patches.h"
#include "nvn_claims.h"

#define CLAIM(fn, kind)                       \
    nvn_claim_begin(kind, fn);                \
    const uint64_t r = guest_orig(orig);      \
    nvn_claim_end();                          \
    return r
/* the pools the CPU cannot access (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS without a CPU flag) are GPU-only: VRAM; the rest
   stay layer 0's until their claims move */
static int gpu_only_vram(uint32_t flags, uint64_t size) {
    (void)size;
    return (flags & 0x7) == 0x1 && !(flags & 0x308) ? NVN_CLAIM_VRAM : NVN_CLAIM_LAYER0;
}
/* the gsys GPU heap's pools: its general pools (0x22: uniform blocks, reports, CPU-written texels) are Upload, the
   CPU_NO_ACCESS ones VRAM; ubo_object_block_alloc's object blocks (0x24, 0x12) are claimed by their makers below, and
   those no maker claims yet stay layer 0's */
static int gsys_rule(uint32_t flags, uint64_t size) {
    return (flags & 0xff) == 0x22 ? NVN_CLAIM_UPLOAD : gpu_only_vram(flags, size);
}
/* nn::gfx in-place files' pools: their vertex and index data (0x24) is written before the GPU first uses it, then
   never again: load once; the rest stay layer 0's until their claims move */
static int gfx_rule(uint32_t flags, uint64_t size) {
    return (flags & 0xff) == 0x24 ? NVN_CLAIM_LOAD_ONCE : gpu_only_vram(flags, size);
}
#define CLAIM_RULE(fn)                        \
    nvn_claim_begin_rule(gpu_only_vram, fn);  \
    const uint64_t r = guest_orig(orig);      \
    nvn_claim_end();                          \
    return r

/* agl::utl::DynamicTextureAllocator's pool record: one physical pool (flags 0x18c, 687 MB at 1080p) and one virtual
   pool of its size, mapped whole once. Only render targets live there; the physical pool's memory holds the arenas'
   free-list nodes, which the CPU writes, and no texels */
PATCH_FUNC(0x80c49380, word(0xd503237f), body(0x3b0566baade1f62c), aapcs, x0, pools_dta_record, orig) {
    CLAIM("dta_pool_record_create", NVN_CLAIM_TARGETS);
}
/* the shader archives' code (flags 0x62): translated by the host, never read by the GPU */
PATCH_FUNC(0x80b65f00, word(0xd503237f), body(0x1688e4429b5f5bb0), aapcs, x0, pools_sharcb, orig) {
    CLAIM("shader_sharcb_load", NVN_CLAIM_HOST);
}
/* NDSR's networks' pool (0x64): its programs, and data the GPU reads (16-byte reads at +0x70a00 in Kokiri) */
PATCH_FUNC(0x80f71a00, word(0xd503237f), body(0xe6b4bb892a0f8c6d), aapcs, x0, pools_ndsr, orig) {
    CLAIM("ndsr_network_setup", NVN_CLAIM_LAYER0);
}
/* the in-place shader binaries (nn::gfx pools of flags 0x64) */
PATCH_FUNC(0x812c3d60, word(0xd503237f), body(0x29df94e62f1665a4), aapcs, x0, pools_shader_binary, orig) {
    CLAIM("f_812c3d60", NVN_CLAIM_HOST);
}
/* the gsys GPU heap (0x12, 0x22, 0x24, 0xa1: uniform blocks, vertex streams, GPU scratch) */
PATCH_FUNC(0x809de920, word(0xd503237f), body(0x02d1932cc850ce16), aapcs, x0, pools_gpu_heap, orig) {
    nvn_claim_begin_rule(gsys_rule, "gpu_pool_create");
    const uint64_t r = guest_orig(orig);
    nvn_claim_end();
    return r;
}
/* agl's pool wrapper (the tera manager's pools; the texture heap's inside its own claim) */
PATCH_FUNC(0x80f6c9d0, word(0xd503237f), body(0x698a26829c189947), aapcs, x0, pools_agl, orig) {
    CLAIM_RULE("agl_memory_pool_create");
}
/* gsys_vfx_mgr_init: NintendoWare vfx3's pools, through gfx_memory_pool_init: six dynamic buffers (0x24, 2 MB each) and
   its resource pool (0x22, 88 MB) the CPU writes every frame: Upload; the GPU-only ones VRAM */
static int vfx_rule(uint32_t flags, uint64_t size) {
    return (flags & 0xff) == 0x24 || (flags & 0xff) == 0x22 ? NVN_CLAIM_UPLOAD : gpu_only_vram(flags, size);
}
PATCH_FUNC(0x8087b9b0, word(0xd503237f), body(0xd9c91fdd5e60c7e2), aapcs, x0, pools_vfx, orig) {
    nvn_claim_begin_rule(vfx_rule, "gsys_vfx_mgr_init");
    const uint64_t r = guest_orig(orig);
    nvn_claim_end();
    return r;
}
/* nn::gfx MemoryPool for in-place files: BFRES vertex and index data (0x24), shader binaries (0x64), ui2d (0x21) */
PATCH_FUNC(0x80792e30, word(0xd503237f), body(0xba09850595f0fa65), aapcs, x0, pools_gfx, orig) {
    nvn_claim_begin_rule(gfx_rule, "gfx_memory_pool_init");
    const uint64_t r = guest_orig(orig);
    nvn_claim_end();
    return r;
}
/* sead_device_queue_pools' pools, each claimed at its nvnMemoryPoolInitialize: the events' words (0x12, 4 KB; written by
   the host), the command memory (0x12; the host translates it) and the shader scratch memory (0x21, GPU only). The
   display's buffers it makes through display_buffers_pool_init are that function's claim */
PATCH_AT(0x80f692dc, word(0xd63f0100), before, none, pools_sead_events) { nvn_claim_begin(NVN_CLAIM_HOST, "sead_device_queue_pools events"); }
PATCH_AT(0x80f692e0, word(0xb001ea37), before, none, pools_sead_events_end) { nvn_claim_end(); }
PATCH_AT(0x80f69474, word(0xd63f0100), before, none, pools_sead_commands) { nvn_claim_begin(NVN_CLAIM_HOST, "sead_device_queue_pools commands"); }
PATCH_AT(0x80f69478, word(0xf9400348), before, none, pools_sead_commands_end) { nvn_claim_end(); }
PATCH_AT(0x80f6971c, word(0xd63f0100), before, none, pools_sead_scratch) { nvn_claim_begin(NVN_CLAIM_VRAM, "sead_device_queue_pools scratch"); }
PATCH_AT(0x80f69720, word(0xb94132c8), before, none, pools_sead_scratch_end) { nvn_claim_end(); }
/* the texture and sampler descriptor pools' memory (0x22): the host translates the descriptors */
PATCH_FUNC(0x80966440, word(0xd503237f), body(0x2abbf2d96dca7f84), aapcs, x0, pools_desc, orig) {
    CLAIM("desc_pools_create", NVN_CLAIM_HOST);
}
/* the display's buffers at boot (0x21, 95.6 MB): window textures, images the back end owns */
PATCH_FUNC(0x8114e4f0, word(0xd503237f), body(0xb3560c600e03da68), aapcs, x0, pools_display, orig) {
    CLAIM("display_buffers_pool_init", NVN_CLAIM_TARGETS);
}

/* ---- Readback claims: GPU results the CPU reads (docs/direct/pool-claims.md, "What the GPU writes and the CPU reads") ----
   exposure_light_buffers_create: the three light-sum buffers (0x1010 bytes each, a pool each; GPU atomic adds) that
   exposure_light_readback reads a float of every frame and zeroes, and the exposure and light blocks the CPU fills every
   frame. Its readback invalidates them (nvn.log_pools survey: 0.4 a frame each) */
PATCH_FUNC(0x80c54290, word(0xd503237f), body(0x0513d52b2b7b7e43), aapcs, x0, pools_exposure, orig) {
    CLAIM("exposure_light_buffers_create", NVN_CLAIM_READBACK);
}
/* render_light_visibility_buffer: an object's 0x40 counter buffer (a pool of 0x2000 each, 29 in Kokiri) that a program's
   atomic adds write and render_light_visibility_readback reads and zeroes every frame */
PATCH_FUNC(0x831e5dd0, word(0xd503237f), body(0xf29cda637aa262e9), aapcs, x0, pools_light_visibility, orig) {
    CLAIM("render_light_visibility_buffer", NVN_CLAIM_READBACK);
}
/* the readers' zeroing stores, reported: the next submission's GPU work adds to zero, and no copy out of an older sum lands
   over it. exposure_light_readback zeroes the +0x30 and +0x268 buffers' sum (str wzr, [base, off]) and the +0x1d8 buffer's
   16 bytes (stp xzr, xzr, [x20]); render_light_visibility_readback zeroes its float +0 */
PATCH_AT(0x803571f4, word(0xb8286abf), after, none, pools_zeroed_word, as(x21, base), as(x8, off)) { nvn_claim_write(base + off, 4); }
PATCH_AT(0x803577a4, word(0xb8286a9f), after, none, pools_zeroed_word_b, as(x20, base), as(x8, off)) { nvn_claim_write(base + off, 4); }
PATCH_AT(0x803574a8, word(0xa9007e9f), after, none, pools_zeroed_pair, as(x20, base)) { nvn_claim_write(base, 16); }
PATCH_AT(0x803541e4, word(0xb8286abf), after, none, pools_light_visibility_zeroed, as(x21, base), as(x8, off)) { nvn_claim_write(base + off, 4); }

/* ---- the gsys heap's object blocks (ubo_object_block_alloc, flags 0x24 and 0x12), claimed by their makers. Evidence: a
   Kokiri survey (nvn.log_pools, debug.claimuse) of each pool's binds, its page changes, and the CPU's invalidates of it
   (a CPU-cached pool the CPU invalidates is one whose GPU writes it reads). The makers still on layer 0 and why are in
   docs/direct/pool-claims.md ("OoT's gsys object blocks") ----
   CPU readers of GPU results, each invalidated every frame: f_80366600's 0x37000 block (the depth grid's), f_80c54a20
   (0x4000, texdata_model_procedural) and f_80ece560 (small result blocks): Readback */
PATCH_AT(0x80366788, word(0x9406eb8a), before, none, pools_80366600_read) { nvn_claim_begin(NVN_CLAIM_READBACK, "f_80366600 read block"); }
PATCH_AT(0x8036678c, word(0x91060260), before, none, pools_80366600_read_end) { nvn_claim_end(); }
PATCH_FUNC(0x80c54a20, word(0xd503237f), body(0x83adcb4a1ff38ae9), aapcs, x0, pools_80c54a20, orig) {
    CLAIM("f_80c54a20", NVN_CLAIM_READBACK);
}
PATCH_FUNC(0x80ece560, word(0xd503237f), body(0xac839e48a68e17da), aapcs, x0, pools_80ece560, orig) {
    CLAIM("f_80ece560", NVN_CLAIM_READBACK);
}
/* agl's display lists (0x12): agl_render_dl_buffer's (48 MB and 2.25 MB) and agl_driver_mgr_dl's two (4 MB, 512 KB),
   command memory the host translates, never bound: Host */
PATCH_FUNC(0x80c53800, word(0xd503237f), body(0x7d14f56496b891e4), aapcs, x0, pools_agl_dl, orig) {
    CLAIM("agl_render_dl_buffer", NVN_CLAIM_HOST);
}
PATCH_AT(0x80c4d350, word(0x97e35394), before, none, pools_agl_driver_dl) { nvn_claim_begin(NVN_CLAIM_HOST, "agl_driver_mgr_dl lists"); }
PATCH_AT(0x80c4d354, word(0xf9405be8), before, none, pools_agl_driver_dl_end) { nvn_claim_end(); }
PATCH_AT(0x80c4d3a0, word(0x97e35380), before, none, pools_agl_driver_dl_b) { nvn_claim_begin(NVN_CLAIM_HOST, "agl_driver_mgr_dl lists"); }
PATCH_AT(0x80c4d3a4, word(0x52802d00), before, none, pools_agl_driver_dl_b_end) { nvn_claim_end(); }

/* the GPU culling lists' counters the CPU zeroes after reading them: cull_gpu_lists_reset's and f_80361a00's appended
   counts (str wzr, [list, off]) and report_reader_b's records (stp xzr, xzr). They act once the lists leave layer 0 */
PATCH_AT(0x80361d8c, word(0xb8286a7f), after, none, pools_cull_count_zeroed, as(x19, list), as(x8, off)) { nvn_claim_write(list + off, 4); }
PATCH_AT_SITE(0x8036207c, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x803620e4, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x8036214c, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x803621b4, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x8036221c, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80362288, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x8036287c, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x803628e4, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x8036294c, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80362bb4, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80362c1c, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80362c84, word(0xb8286a7f), after, none, pools_cull_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80361b6c, word(0xb8286adf), after, none, pools_cull_count_zeroed, x22, x8);
PATCH_AT(0x8036321c, word(0xa9007d1f), after, none, pools_cull_record_zeroed, as(x8, rec)) { nvn_claim_write(rec, 16); }
PATCH_AT_SITE(0x80363244, word(0xa9007d1f), after, none, pools_cull_record_zeroed, x8);
PATCH_AT(0x8036326c, word(0xa9017d1f), after, none, pools_cull_record_zeroed_10, as(x8, rec)) { nvn_claim_write(rec + 0x10, 16); }
PATCH_AT(0x80363294, word(0xa9027d1f), after, none, pools_cull_record_zeroed_20, as(x8, rec)) { nvn_claim_write(rec + 0x20, 16); }
