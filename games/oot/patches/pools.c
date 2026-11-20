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
/* cull_task_block_create is a generic block maker, claimed at its calls (docs/direct/pool-users.md, "The vegetation task
   blocks"): object b's two (its task records, written each frame by cull_veg_task_records_write, reported below) and
   cull_task_block_fill's (filled once in that function, claimed over all of it): VRAM */
PATCH_AT(0x80c5960c, word(0x94001121), before, none, pools_veg_tasks) { nvn_claim_begin(NVN_CLAIM_VRAM, "cull_gpu_object_b_create tasks"); }
PATCH_AT(0x80c59610, word(0xb945aa88), before, none, pools_veg_tasks_end) { nvn_claim_end(); }
PATCH_AT(0x80c59670, word(0x94001108), before, none, pools_veg_tasks_b) { nvn_claim_begin(NVN_CLAIM_VRAM, "cull_gpu_object_b_create tasks"); }
PATCH_AT(0x80c59674, word(0xb945f288), before, none, pools_veg_tasks_b_end) { nvn_claim_end(); }
PATCH_FUNC(0x80cb2a40, word(0xd503237f), body(0x94458a45d56814cc), aapcs, x0, pools_task_block_fill, orig) {
    CLAIM("cull_task_block_fill", NVN_CLAIM_VRAM);
}
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
    CLAIM("f_80c576b0", NVN_CLAIM_READBACK);
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

/* ---- the depth grid: agl::utl::DepthBufferEx (depth_grid_slots_create) and the query object (depth_grid_buffers_create),
   claimed at their ubo_object_pool_alloc calls (docs/direct/pool-users.md). The grid's per-frame constants (0x390 x2) and
   the query constants (0x80 x4, x2) are Upload; the two grid slots (0x3970: the GPU writes the grid, report_reader_a reads
   the newest slot) Readback, with the CPU's per-frame camera copy into them (f_80365b70's memcpy of 0x2420 bytes at
   +0x1550, from job_calc_runner) reported; the 0x5b40 block VRAM. The query records (5000 x 0x90) and the per-id state
   (5000 x 16) as VRAM make the occlusion culling cull
   less (the foliage pass 0.9 -> 1.0 ms, rt9 0.15 -> 0.25 ms in Kokiri), so a CPU write after creation (an id's release or
   reuse, unattributed in the census) must reach them: on layer 0 until that writer is reported. The query's read block
   (0x80366788) is Readback above */
PATCH_AT(0x80c5a8dc, word(0x97e31b35), before, none, pools_grid_consts) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "depth_grid_slots_create constants"); }
PATCH_AT(0x80c5a8e0, word(0xd291c729), before, none, pools_grid_consts_end) { nvn_claim_end(); }
PATCH_AT(0x80c5aa28, word(0x97e31ae2), before, none, pools_grid_slot_a) { nvn_claim_begin(NVN_CLAIM_READBACK, "depth_grid_slots_create slots"); }
PATCH_AT(0x80c5aa2c, word(0xf955da68), before, none, pools_grid_slot_a_end) { nvn_claim_end(); }
PATCH_AT(0x80c5aa98, word(0x97e31ac6), before, none, pools_grid_slot_b) { nvn_claim_begin(NVN_CLAIM_READBACK, "depth_grid_slots_create slots"); }
PATCH_AT(0x80c5aa9c, word(0xf9562268), before, none, pools_grid_slot_b_end) { nvn_claim_end(); }
PATCH_AT(0x80c5ab24, word(0x97e31aa3), before, none, pools_grid_block) { nvn_claim_begin(NVN_CLAIM_VRAM, "depth_grid_slots_create block"); }
PATCH_AT(0x80c5ab28, word(0xf9559268), before, none, pools_grid_block_end) { nvn_claim_end(); }
PATCH_AT(0x80366694, word(0x9406ebc7), before, none, pools_query_consts) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "depth_grid_buffers_create constants"); }
PATCH_AT(0x80366698, word(0x91002260), before, none, pools_query_consts_end) { nvn_claim_end(); }
PATCH_AT(0x803666d0, word(0x9406ebb8), before, none, pools_query_records) { nvn_claim_begin(NVN_CLAIM_LAYER0, "depth_grid_buffers_create records"); }
PATCH_AT(0x803666d4, word(0xb9400688), before, none, pools_query_records_end) { nvn_claim_end(); }
PATCH_AT(0x803667c4, word(0x9406eb7b), before, none, pools_query_state) { nvn_claim_begin(NVN_CLAIM_LAYER0, "depth_grid_buffers_create state"); }
PATCH_AT(0x803667c8, word(0xb9400688), before, none, pools_query_state_end) { nvn_claim_end(); }
PATCH_AT(0x80365c00, word(0x94e3f1dc), after, none, pools_grid_camera, as(x0, dst)) { nvn_claim_write(dst, 0x2420); }

/* ---- the probe module's calc blocks (gpu_calc_block_create, from probe_calc_block_init: 150 MB, and f_80c5c450 beside
   probe_bake_load: 0x2000; docs/direct/pool-users.md, "The probe module"). Never bound: shaders reach them through
   stored addresses: VRAM. gpu_calc_block_create fills its five ubo objects after making the pool, inside the claim's
   scope (the scope's end copies them). Later writers, reported:
   - probe_calc_stream_write's memcpy of count ([x19 + 0x78]) x 16 bytes (about 26 pages a present);
   - the BakeProbe thread's probe_bake_records_write: count ([x3 + 0x30]) records of 0x80 bytes at [x0 + 0x300] + 0x80 x
     [x2 + 8], and count ([x3 + 0x20]) of 0x20 at [x0 + 0x270] + 0x20 x [x2];
   - probe_calc_fields_write's six 8-byte fields at the f_80c5c450 block's current half (x20, its last store at
     0x806acfdc) */
PATCH_AT(0x806accec, word(0x9416be2d), before, none, pools_probe_calc) { nvn_claim_begin(NVN_CLAIM_VRAM, "probe_calc_block_init calc block"); }
PATCH_AT(0x806accf0, word(0x36000420), before, none, pools_probe_calc_end) { nvn_claim_end(); }
PATCH_AT(0x80c5c55c, word(0x94000011), before, none, pools_probe_bake_calc) { nvn_claim_begin(NVN_CLAIM_VRAM, "f_80c5c450 calc block"); }
PATCH_AT(0x80c5c560, word(0xa94b4ff4), before, none, pools_probe_bake_calc_end) { nvn_claim_end(); }
PATCH_AT(0x806ad880, word(0x94d6d2bc), after, none, pools_probe_stream, as(x0, dst), as(x21, count)) { nvn_claim_write(dst, (uint64_t)count << 4); }
PATCH_AT(0x806acfdc, word(0xf9001288), after, none, pools_probe_fields, as(x20, at)) { nvn_claim_write(at, 0x28); }
PATCH_FUNC(0x80711820, word(0xd503237f), body(0x73bee5ec6ea59328), aapcs, x0, pools_bake_probe_records, orig, as(x0, obj),
           as(x2, idx), as(x3, counts)) {
    /* (the destinations as the function reads them at its start: both indexes are x2's words, +8 and +0 (it keeps x2 at
       [sp + 8] for the second loop)) */
    const uint64_t d1 = G64(obj + 0x300) + 0x80ull * G32(idx + 8), d2 = G64(obj + 0x270) + 0x20ull * G32(idx);
    const int32_t n1 = (int32_t)G32(counts + 0x30), n2 = (int32_t)G32(counts + 0x20);
    const uint64_t r = guest_orig(orig);
    if (n1 > 0) nvn_claim_write(d1, 0x80ull * (uint64_t)n1);
    if (n2 > 0) nvn_claim_write(d2, 0x20ull * (uint64_t)n2);
    return r;
}

/* ---- the rest of the gsys object blocks and agl pools the census names (debug.poolcensus, Kokiri and Market), each at the
   call that makes it ---- */
/* the save picture: agl::utl::TextureDataSerializerJPG's block (texdata_serialize_jpg <- ui_serialize_capture):
   texdata_copy_to_buffer copies the picture into it and waits; the JPEG encoder then reads it: Readback */
PATCH_AT(0x80bd1e48, word(0x97e54082), before, none, pools_jpg) { nvn_claim_begin(NVN_CLAIM_READBACK, "texdata_serialize_jpg picture"); }
PATCH_AT(0x80bd1e4c, word(0x36001180), before, none, pools_jpg_end) { nvn_claim_end(); }
/* texdata_init_jpg's work buffer: never bound: Host */
PATCH_AT(0x809ea31c, word(0x97ecdf4d), before, none, pools_jpg_init) { nvn_claim_begin(NVN_CLAIM_HOST, "texdata_init_jpg buffer"); }
PATCH_AT(0x809ea320, word(0x37000040), before, none, pools_jpg_init_end) { nvn_claim_end(); }
/* terrain: terrain_streams_create's streams (the CPU writes some pages a present, binds read them): Upload; the tile
   cache terrain_tile_load fills and reads (agl_memory_pool_create_b in terrain_tera_mgr_init) and f_80f6dc20's 202 MB pool
   are never bound: Host */
PATCH_AT(0x809e3990, word(0x97fc7af4), before, none, pools_terrain_streams) { nvn_claim_begin(NVN_CLAIM_LOAD_ONCE, "terrain_streams_create"); }
PATCH_AT(0x809e3994, word(0xf9400261), before, none, pools_terrain_streams_end) { nvn_claim_end(); }
PATCH_AT(0x809e3684, word(0x94162633), before, none, pools_terrain_tiles) { nvn_claim_begin(NVN_CLAIM_HOST, "terrain_tera_mgr_init tiles"); }
PATCH_AT(0x809e3688, word(0xf9402260), before, none, pools_terrain_tiles_end) { nvn_claim_end(); }
PATCH_AT(0x80f6dc5c, word(0x97fffb5d), before, none, pools_80f6dc20) { nvn_claim_begin(NVN_CLAIM_HOST, "f_80f6dc20 pool"); }
PATCH_AT(0x80f6dc60, word(0x39448268), before, none, pools_80f6dc20_end) { nvn_claim_end(); }
/* GPU scratch the CPU fills only when it makes it: f_80c62260's blocks, texdata_storage_block's: VRAM. f_80c64650's
   blocks the CPU writes later (f_80c63e70's fill, reported, and others: Market binds one as storage the CPU rewrites):
   Upload */
PATCH_AT(0x80c648a0, word(0x97e2f344), before, none, pools_80c64650) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "f_80c64650 block"); }
PATCH_AT(0x80c648a4, word(0xb940d688), before, none, pools_80c64650_end) { nvn_claim_end(); }
PATCH_AT(0x80c62a38, word(0x97e2fade), before, none, pools_80c62260_a) { nvn_claim_begin(NVN_CLAIM_VRAM, "f_80c62260 blocks"); }
PATCH_AT(0x80c62a3c, word(0xb9400f28), before, none, pools_80c62260_a_end) { nvn_claim_end(); }
PATCH_AT(0x80c62f88, word(0x97e2f98a), before, none, pools_80c62260_b) { nvn_claim_begin(NVN_CLAIM_VRAM, "f_80c62260 blocks"); }
PATCH_AT(0x80c62f8c, word(0xb9400f28), before, none, pools_80c62260_b_end) { nvn_claim_end(); }
/* cull_scene_blocks_init (f_80c62260) fills each block it just made, after the claim's scope: a 16-byte record per entry (stp xzr, xzr at
   0x80c62af0 in the first block; stp x23, x24 at 0x80c6301c in the second), w24 / w20 entries a stride apart. Each loop's
   records are gathered and reported where its two paths meet (0x80c62b0c, 0x80c63034), once a scene */
static _Thread_local uint64_t fill_lo, fill_hi;
static void fill_note(uint64_t at) {
    if (!fill_hi || at < fill_lo) fill_lo = at;
    if (at + 16 > fill_hi) fill_hi = at + 16;
}
static void fill_report(void) {
    if (fill_hi) nvn_claim_write(fill_lo, fill_hi - fill_lo);
    fill_lo = fill_hi = 0;
}
PATCH_AT(0x80c62af0, word(0xa9007d1f), after, none, pools_80c62260_fill_a, as(x8, at)) { fill_note(at); }
PATCH_AT(0x80c62b0c, word(0x1a9f3108), before, none, pools_80c62260_fill_a_end) { fill_report(); }
PATCH_AT(0x80c6301c, word(0xa9006117), after, none, pools_80c62260_fill_b, as(x8, at)) { fill_note(at); }
PATCH_AT(0x80c63034, word(0x1a9f3108), before, none, pools_80c62260_fill_b_end) { fill_report(); }
/* f_80c63e70 (called by f_80c62260 on SceneLowPriority) fills f_80c64650's block: memcpy of count (w23) x 4 bytes. Kokiri
   never binds that block (shaders reach it through stored addresses): reported */
/* cull_address_block_create (f_80c64650) stores an 8-byte GPU address (a 0x90 ubo object's +0x80 of [x20 + 0xe8]) into
   the one-word block it just allocated (ubo_object_pool_alloc at 0x80c649dc): shaders load it to find their data. That
   block lands in a heap pool another maker grew (cull_task_block_fill's), so it is reported */
PATCH_AT(0x80c64a28, word(0xf90002a8), after, none, pools_80c64650_address, as(x21, at)) { nvn_claim_write(at, 8); }
PATCH_AT(0x80c64244, word(0x94bff84b), after, none, pools_80c64650_fill, as(x0, dst), as(x23, count)) {
    if ((int32_t)count > 0) nvn_claim_write(dst, (uint64_t)(int32_t)count << 2);
}
PATCH_AT(0x80dd81d0, word(0x9400002c), before, none, pools_texdata_storage) { nvn_claim_begin(NVN_CLAIM_VRAM, "texdata_storage_block"); }
PATCH_AT(0x80dd81d4, word(0xf9400282), before, none, pools_texdata_storage_end) { nvn_claim_end(); }
/* CPU-written blocks binds read: f_80cdf2f0's two (texdata_model_procedural's), f_80c5ea90's, f_82c24db0's, and the
   agl/gfx general pool (f_80f6a950, 0x22): Upload. f_80cdf2f0's third, texdata_cpu_lut's table, is never bound: Host */
PATCH_AT(0x80cdfcf0, word(0x97e10630), before, none, pools_80cdf2f0_a) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "f_80cdf2f0 blocks"); }
PATCH_AT(0x80cdfcf4, word(0x39414ac8), before, none, pools_80cdf2f0_a_end) { nvn_claim_end(); }
PATCH_AT(0x80cdfdc8, word(0x97e105fa), before, none, pools_80cdf2f0_b) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "f_80cdf2f0 blocks"); }
PATCH_AT(0x80cdfdcc, word(0x12800008), before, none, pools_80cdf2f0_b_end) { nvn_claim_end(); }
PATCH_AT(0x80cdfc10, word(0x97f41a40), before, none, pools_80cdf2f0_lut) { nvn_claim_begin(NVN_CLAIM_HOST, "f_80cdf2f0 lut"); }
PATCH_AT(0x80cdfc14, word(0xf9401660), before, none, pools_80cdf2f0_lut_end) { nvn_claim_end(); }
PATCH_AT(0x80c5ee20, word(0x97e309e4), before, none, pools_80c5ea90) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "f_80c5ea90 block"); }
PATCH_AT(0x80c5ee24, word(0x91152260), before, none, pools_80c5ea90_end) { nvn_claim_end(); }
PATCH_AT(0x82c2d6cc, word(0x9763cfb9), before, none, pools_82c24db0) { nvn_claim_begin(NVN_CLAIM_LOAD_ONCE, "f_82c24db0 block"); }
PATCH_AT(0x82c2d6d0, word(0x9102a260), before, none, pools_82c24db0_end) { nvn_claim_end(); }
PATCH_AT(0x80f6aa08, word(0x97e0a10a), before, none, pools_agl_general) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "f_80f6a950 general"); }
PATCH_AT(0x80f6aa0c, word(0x910003e0), before, none, pools_agl_general_end) { nvn_claim_end(); }
/* agl_driver_mgr_dl's third list (0x12, via f_80c51190): command memory: Host */
PATCH_AT(0x80c4d598, word(0x94000efe), before, none, pools_agl_driver_dl_c) { nvn_claim_begin(NVN_CLAIM_HOST, "agl_driver_mgr_dl lists"); }
PATCH_AT(0x80c4d59c, word(0xa95b4ff4), before, none, pools_agl_driver_dl_c_end) { nvn_claim_end(); }
/* the probe module's storage block (storage_block_create in probe_storage_init, 5.5 MB): draws bind it whole, read only;
   bake::ModuleMgr writes 32-byte records into it through f_80c4e880 (count x 32 bytes at x21, its virtual's output): VRAM,
   the records reported, so the whole-pool binds cost nothing */
PATCH_AT(0x80b167c4, word(0x9404dea3), before, none, pools_probe_storage) { nvn_claim_begin(NVN_CLAIM_VRAM, "probe_storage_init storage"); }
PATCH_AT(0x80b167c4, word(0x9404dea3), after, none, pools_probe_storage_end) { nvn_claim_end(); }   /* (0x80b167c8 is a branch target) */
PATCH_AT(0x80c4e964, word(0xf9403a95), before, none, pools_probe_records, as(x21, at), as(x22, count)) {
    if ((int64_t)count > 0) nvn_claim_write(at, (uint64_t)count << 5);
}
