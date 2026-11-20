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

/* ---- the GPU culling objects (docs/direct/pool-users.md, "The GPU culling lists"; debug.poolcensus in Kokiri and Market).
   Object a's lists, instance buffers, indirect arguments and result lists are the GPU's: VRAM. The CPU's resets of their
   first records are made on the GPU and the counts it reads are copied out of their records alone (mesh_lists.c,
   mesh_args.c) */
PATCH_FUNC(0x80c58a80, word(0xd503237f), body(0xd48e5979b49a3213), aapcs, x0, pools_cull_object_a, orig) {
    CLAIM("cull_gpu_object_a_create", NVN_CLAIM_VRAM);
}
/* object b (the vegetation culling) is claimed by its makers, not as a whole: its construction also allocates from a
   general heap (f_80c59c30), which may grow that heap by a pool other systems share (claimed VRAM with the object, the
   grass went). Its CPU-written task records and arguments (f_80c5f870's and f_80c5da90's pools: the CPU writes them each
   frame, binds only read them) are Upload; its two small result blocks (ubo_object_pool_alloc at 0x80c597e0 and
   0x80c59824) that f_80586fd0 reads once their submission completed Readback */
PATCH_FUNC(0x80c5f870, word(0xd503237f), body(0x98f5d55e007fed32), aapcs, x0, pools_80c5f870, orig) {
    CLAIM("f_80c5f870", NVN_CLAIM_UPLOAD);
}
/* cull_task_block_create (f_80c5da90) is a generic block maker, claimed at its calls. Object b's two (its task records and
   the gather programs' data, 0x12000 x2 each) stay on layer 0: as Upload the grass goes, as VRAM the leaves too, with
   cull_veg_task_records_write's writes reported (below) and whole-block reports after the maker; the check then finds its
   storage reads differing from guest memory from the first frames, which suggests GPU writes through stored addresses that
   the CPU reads. Open. f_80cb2a40's two (CPU-written, bound read) the same: as Upload the grass goes */
PATCH_AT(0x80c5960c, word(0x94001121), before, none, pools_veg_tasks) { nvn_claim_begin(NVN_CLAIM_LAYER0, "cull_gpu_object_b_create tasks"); }
PATCH_AT(0x80c59610, word(0xb945aa88), before, none, pools_veg_tasks_end) { nvn_claim_end(); }
PATCH_AT(0x80c59670, word(0x94001108), before, none, pools_veg_tasks_b) { nvn_claim_begin(NVN_CLAIM_LAYER0, "cull_gpu_object_b_create tasks"); }
PATCH_AT(0x80c59674, word(0xb945f288), before, none, pools_veg_tasks_b_end) { nvn_claim_end(); }
PATCH_AT(0x80cb2b20, word(0x97feabdc), before, none, pools_cb2a40_tasks) { nvn_claim_begin(NVN_CLAIM_LAYER0, "f_80cb2a40 tasks"); }
PATCH_AT(0x80cb2b24, word(0xb9401e68), before, none, pools_cb2a40_tasks_end) { nvn_claim_end(); }
PATCH_AT(0x80cb2ff4, word(0x97feaaa7), before, none, pools_cb2a40_tasks_b) { nvn_claim_begin(NVN_CLAIM_LAYER0, "f_80cb2a40 tasks"); }
PATCH_AT(0x80cb2ff8, word(0xb9401e68), before, none, pools_cb2a40_tasks_b_end) { nvn_claim_end(); }
/* f_80365950 (cull_veg_task_records_write, from f_80586fd0 on the main thread) writes f_80c5da90's task records each frame: the
   count at the list's first record (`str w24, [x23, x8]`) and count x 0x40 bytes of records (a memcpy). Shaders reach
   them through stored addresses, not binds (one of the two pools is never bound): reported */
PATCH_AT(0x803659fc, word(0xb8286af8), after, none, pools_veg_count, as(x23, list), as(x8, off)) { nvn_claim_write(list + off, 4); }
/* (after the memcpy: its destination as the function computes it, list + layout[+0x4c], x20 the list's buffer object) */
PATCH_AT(0x80365b64, word(0x94e3f203), after, none, pools_veg_records, as(x20, buf), as(x23, list), as(x24, count)) {
    const uint64_t dst = list + G32(G64(G64(buf + 0x10)) + 0x4c);
    nvn_claim_write(dst, (uint64_t)(uint32_t)count << 6);
}
PATCH_AT(0x80c597e0, word(0x97e31f74), before, none, pools_cull_b_results) { nvn_claim_begin(NVN_CLAIM_READBACK, "cull_gpu_object_b_create results"); }
PATCH_AT(0x80c597e4, word(0xb9406688), before, none, pools_cull_b_results_end) { nvn_claim_end(); }
PATCH_AT(0x80c59824, word(0x97e31f63), before, none, pools_cull_b_results_b) { nvn_claim_begin(NVN_CLAIM_READBACK, "cull_gpu_object_b_create results"); }
PATCH_AT(0x80c59828, word(0xb9406688), before, none, pools_cull_b_results_b_end) { nvn_claim_end(); }
/* f_80c576b0's two lists (0x20000 each, made with cull_gpu_objects_create's object b): the GPU appends to them and
   actor_records_read_a reads their counts once their submission completed, then zeroes them (`str wzr, [x21, x8]`, eleven
   stores): Readback, the zeroing made on the GPU */
PATCH_FUNC(0x80c576b0, word(0xd503237f), body(0x006c33e34ee6fc7f), aapcs, x0, pools_80c576b0, orig) {
    CLAIM("f_80c576b0", NVN_CLAIM_LAYER0);
}
PATCH_AT(0x82c1dcc8, word(0xb8286abf), after, none, pools_actor_count_zeroed, as(x21, list), as(x8, off)) { nvn_claim_fill(list + off, 4, 0); }
PATCH_AT_SITE(0x82c1dcfc, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);
PATCH_AT_SITE(0x82c1dd30, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);
PATCH_AT_SITE(0x82c1dd64, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);
PATCH_AT_SITE(0x82c1dd98, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);
PATCH_AT_SITE(0x82c1ddcc, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);
PATCH_AT_SITE(0x82c1de00, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);
PATCH_AT_SITE(0x82c1de34, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);
PATCH_AT_SITE(0x82c1de64, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);
PATCH_AT_SITE(0x82c1dea8, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);
PATCH_AT_SITE(0x82c1ded8, word(0xb8286abf), after, none, pools_actor_count_zeroed, x21, x8);

/* ---- gsys::GeometryCullingMgr (gpu_result_block_a_create, docs/direct/pool-users.md): per view, an array of the units'
   GPU addresses that the calc jobs write each frame (cull_geometry_store_address, the only CPU write) and a draw binds as
   storage (its program declares the binding written, so the grains are the GPU's): Upload, each store reported; the block
   the GPU writes through those addresses (ubo_object_block_alloc_b at 0x80ecdd24): VRAM. The first view's allocation makes
   the small pool other ubo objects share: exposure_light_readback's 0x90 exposure record lives there, its CPU writes
   reported below */
PATCH_AT(0x80ecdcc0, word(0x97d94e3c), before, none, pools_geocull_views) { nvn_claim_begin(NVN_CLAIM_READBACK, "GeometryCullingMgr views"); }
PATCH_AT(0x80ecdcc4, word(0xb9400aa8), before, none, pools_geocull_views_end) { nvn_claim_end(); }
PATCH_AT(0x80ecdd24, word(0x97d952fb), before, none, pools_geocull_block) { nvn_claim_begin(NVN_CLAIM_VRAM, "GeometryCullingMgr block"); }
PATCH_AT(0x80ecdd28, word(0xd10083a0), before, none, pools_geocull_block_end) { nvn_claim_end(); }
PATCH_AT(0x834bfb24, word(0xf9000008), after, none, pools_geocull_address, as(x0, at)) { nvn_claim_write(at, 8); }
/* exposure_light_readback's exposure record: 0x30 bytes of stores (0x80357608..0x80357618) then a 0x60-byte memset, at
   [x19 + 0xd8] + layout[+4] ([[x19 + 0xd0]] + 4): reported after the memset */
PATCH_AT(0x8035761c, word(0x94e42b51), after, none, pools_exposure_record, as(x19, obj), as(x22, base)) {
    nvn_claim_write(base + G32(G64(G64(obj + 0xd0)) + 4), 0x90);
}

/* ---- NintendoWare Vfx3's resource pool (gsys_vfx_mgr_init, Upload): particle attribute buffers the draws bind as storage
   their programs write (the grains are the GPU's), which the CPU also writes (debug.poolcensus, Kokiri): reported.
   - the emitter plugins' per-particle setters (called through vtable slots from vfx_particle_init_callbacks), one per type:
     float, vec2, vec3, vec4, mat4, each storing into base + stride x index + offset when the value changed (and widening the
     buffer's dirty index range at +0x20);
   - f_812efb80's fill of a new buffer (memset 0x6f, x24 the buffer, x23 its size);
   - f_81267bc0's copy of a CPU buffer's dirty range (or all of it) into the pool (BaseProcCreateAndDeleteThread) */
PATCH_AT(0x812f0ed0, word(0xbc286920), after, none, pools_vfx_set1, as(x9, rec), as(x8, off)) { nvn_claim_write(rec + off, 4); }
PATCH_AT(0x812f0f30, word(0x2d000500), after, none, pools_vfx_set2, as(x8, at)) { nvn_claim_write(at, 8); }
PATCH_AT(0x812f0f98, word(0xbd000902), after, none, pools_vfx_set3, as(x8, at)) { nvn_claim_write(at, 12); }
PATCH_AT(0x812f1000, word(0x2d010d02), after, none, pools_vfx_set4, as(x8, at)) { nvn_claim_write(at, 16); }
PATCH_AT(0x812f1100, word(0x2d075d16), after, none, pools_vfx_set16, as(x8, at)) { nvn_claim_write(at, 0x40); }
PATCH_AT(0x812efbd8, word(0x94a5c9e2), after, none, pools_vfx_fill, as(x24, buf), as(x23, size)) { nvn_claim_write(buf, size); }
PATCH_AT(0x81268558, word(0x94a7e786), after, none, pools_vfx_copy, as(x0, dst), as(x19, o)) {
    const uint64_t n = G32(o + 0x1d0) >= 2 ? G64(o + 0x1b8) * (uint64_t)((int32_t)G32(o + 0x1cc) - (int32_t)G32(o + 0x1c8) + 1)
                                           : G64(o + 0x1b0);
    nvn_claim_write(dst, n);
}
