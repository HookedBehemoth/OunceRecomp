/* Pool claims (runtime/nvn_claims.h, docs/direct/pool-claims.md): what each memory pool the game makes holds, claimed
   at the engine function that makes it. A pool no claim covers is an error in the NVN layer. The texture heap's claim is
   assets.c's (agl::TextureDataMgr::InitializeHeap). A rule names a kind only for the flags its evidence covers; any
   other pool is unclaimed, an error. */
#include "hos.h"
#include "patches.h"
#include "nvn_claims.h"

#define CLAIM(fn, kind)                       \
    nvn_claim_begin(kind, fn);                \
    const uint64_t r = guest_orig(orig);      \
    nvn_claim_end();                          \
    return r
/* by access alone: a pool the CPU cannot access (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS without a CPU flag) is GPU-only,
   VRAM; one the GPU cannot access (GPU_NO_ACCESS) is the host's; any other is unclaimed */
static int by_access(uint32_t flags, uint64_t size) {
    (void)size;
    if ((flags & 0x7) == 0x1 && !(flags & 0x308)) return NVN_CLAIM_VRAM;
    return (flags & 0x8) && !(flags & 0x30) ? NVN_CLAIM_HOST : NVN_CLAIM_NONE;
}
/* the gsys GPU heap's pools: its general pools (0x22: uniform blocks, reports, CPU-written texels) and the object blocks
   (0x24) no maker below claims are Upload, the CPU_NO_ACCESS ones VRAM. The census of the object blocks no maker
   claims (Kokiri and Market: agl's DTA objects in dta_create, the texture-data makers, small per-system blocks) finds
   each written by the CPU at its making and read by binds, or never used: none read by the CPU, none written by GPU
   work but its maker's (docs/direct/pool-claims.md, "OoT's gsys object blocks") */
static int gsys_rule(uint32_t flags, uint64_t size) {
    return (flags & 0xff) == 0x22 || (flags & 0xff) == 0x24 ? NVN_CLAIM_UPLOAD : by_access(flags, size);
}
/* nn::gfx in-place files' pools: their vertex and index data (0x24) is written before the GPU first uses it, then
   never again: load once. f_80f6a898's 8 MB pool (0xa2: agl's, through f_80f6a55c) is written by the CPU now and then,
   a few pages bound: Upload */
static int gfx_rule(uint32_t flags, uint64_t size) {
    return (flags & 0xff) == 0x24 ? NVN_CLAIM_LOAD_ONCE : (flags & 0xff) == 0xa2 ? NVN_CLAIM_UPLOAD : by_access(flags, size);
}
#define CLAIM_RULE(fn)                        \
    nvn_claim_begin_rule(by_access, fn);  \
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
/* NDSR's networks' pool (0x64): its programs, and the weights the GPU reads (16-byte reads at +0x70a00 in Kokiri), written
   and read by the CPU only inside ndsr_network_setup, before any use (debug.poolcensus): VRAM, the scope's end copying
   what the set-up wrote */
PATCH_FUNC(0x80f71a00, word(0xd503237f), body(0xe6b4bb892a0f8c6d), aapcs, x0, pools_ndsr, orig) {
    CLAIM("ndsr_network_setup", NVN_CLAIM_VRAM);
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
/* gsys_vfx_mgr_init: NintendoWare vfx3's pools, through gfx_memory_pool_init: six dynamic buffers (0x24, 2 MB each), its
   resource pool (0x22, 88 MB) the CPU writes every frame, and a 0.12 MB pool (0xa4, unused in Kokiri): Upload; the
   GPU-only ones VRAM */
static int vfx_rule(uint32_t flags, uint64_t size) {
    const uint32_t f = flags & 0xff;
    return f == 0x24 || f == 0x22 || f == 0xa4 ? NVN_CLAIM_UPLOAD : by_access(flags, size);
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
   exposure_light_readback reads a float of every frame, and the light block the CPU fills every frame. The sums' resets
   and their reader are gpu_readers.c's */
PATCH_FUNC(0x80c54290, word(0xd503237f), body(0x0513d52b2b7b7e43), aapcs, x0, pools_exposure, orig) {
    CLAIM("exposure_light_buffers_create", NVN_CLAIM_READBACK);
}
/* render_light_visibility_buffer: a light's 0x40 counter buffer (a pool of 0x2000 each, 29 in Kokiri) that
   light_visibility_pass's atomic adds write and render_light_visibility_readback reads every frame (gpu_readers.c) */
PATCH_FUNC(0x831e5dd0, word(0xd503237f), body(0xf29cda637aa262e9), aapcs, x0, pools_light_visibility, orig) {
    CLAIM("render_light_visibility_buffer", NVN_CLAIM_READBACK);
}
/* ---- the gsys heap's object blocks (ubo_object_block_alloc, flags 0x24 and 0x12), claimed by their makers. Evidence: a
   Kokiri survey (nvn.log_pools, debug.claimuse) of each pool's binds, its page changes, and the CPU's invalidates of it
   (a CPU-cached pool the CPU invalidates is one whose GPU writes it reads). The makers still on layer 0 and why are in
   docs/direct/pool-claims.md ("OoT's gsys object blocks") ----
   CPU readers of GPU results, each invalidated every frame: f_80366600's 0x37000 block (the depth grid's, in the
   allocation table), f_80c54a20 (0x4000, texdata_model_procedural) and f_80ece560 (small result blocks): Readback */
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
   Object a's lists, instance buffers, indirect arguments and result lists are the GPU's: VRAM. Its lists' resets and the
   counts the CPU reads are cull_lists.c's, which names the records it reads once the object is made */
void cull_lists_made(uint64_t obj);
PATCH_FUNC(0x80c58a80, word(0xd503237f), body(0xd48e5979b49a3213), aapcs, x0, pools_cull_object_a, orig, as(x0, obj)) {
    nvn_claim_begin(NVN_CLAIM_VRAM, "cull_gpu_object_a_create");
    const uint64_t r = guest_orig(orig);
    nvn_claim_end();
    cull_lists_made(obj);
    return r;
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
   them through stored addresses, not binds (one of the two pools is never bound). The count's flush (0x80365b04) is a
   reported write; the records' memcpy (0x80365b64, never reached in Kokiri) is reported: its destination as the function
   computes it, list + layout[+0x4c], x20 the list's buffer object */
PATCH_AT(0x80365b04, word(0xd63f0100), before, none, pools_veg_flush) { nvn_claim_flush_written(); }
PATCH_AT(0x80365b64, word(0x94e3f203), after, none, pools_veg_records, as(x20, buf), as(x23, list), as(x24, count)) {
    const uint64_t dst = list + G32(G64(G64(buf + 0x10)) + 0x4c);
    nvn_claim_write(dst, (uint64_t)(uint32_t)count << 6);
}
/* f_80c576b0's two lists (0x20000 each, made with cull_gpu_objects_create's object b): the GPU appends to them and
   actor_records_read_a reads their counts once their submission completed, then zeroes them: Readback, the zeroing made
   on the GPU (cull_lists.c) */
PATCH_FUNC(0x80c576b0, word(0xd503237f), body(0x006c33e34ee6fc7f), aapcs, x0, pools_80c576b0, orig) {
    CLAIM("f_80c576b0", NVN_CLAIM_READBACK);
}

/* ---- gsys::GeometryCullingMgr (gpu_result_block_a_create, docs/direct/pool-users.md): per view, an array of the units'
   GPU addresses that the calc jobs write each frame (cull_geometry_store_address, the only CPU write) and a draw binds as
   storage (its program declares the binding written, so the grains are the GPU's): Readback, each store reported; the block
   the GPU writes through those addresses (ubo_object_block_alloc_b at 0x80ecdd24): VRAM. The first view's allocation makes
   the small pool other ubo objects share: exposure_light_readback's 0x90 exposure record lives there (gpu_readers.c) */
PATCH_AT(0x834bfb24, word(0xf9000008), after, none, pools_geocull_address, as(x0, at)) { nvn_claim_write(at, 8); }

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
/* vfx_particle_stream_write (the emitters' stream buffers each frame, ModuleSystemWorker): three memcpys into the resource
   pool (0x812606b4: the dirty range [+0x140, +0x144) of [+0x1d8]; 0x81260840: [+0x1f8]'s dirty records or all of it;
   0x81260898: count [+0xd0] x 16 bytes) and a 0x1d0-byte record at x24 (a memset of 0x140, then its fields, the last at
   0x81260e6c). Their grains are partly GPU-written (debug.poolcensus): each reported */
static _Thread_local uint64_t vfx_dst, vfx_n;
PATCH_AT(0x812606b4, word(0x94a8072f), before, none, pools_vfx_stream_a, as(x0, dst), as(x2, n)) { vfx_dst = dst, vfx_n = (uint32_t)n; }
PATCH_AT(0x812606b4, word(0x94a8072f), after, none, pools_vfx_stream_a_end) { nvn_claim_write(vfx_dst, vfx_n); }
PATCH_AT(0x81260838, word(0x14000002), before, none, pools_vfx_stream_b, as(x0, dst), as(x2, n)) { vfx_dst = dst, vfx_n = n; }
PATCH_AT(0x8126083c, word(0xa95f8a81), after, none, pools_vfx_stream_b_all, as(x0, dst), as(x2, n)) { vfx_dst = dst, vfx_n = n; }
PATCH_AT(0x81260840, word(0x94a806cc), after, none, pools_vfx_stream_b_end) { nvn_claim_write(vfx_dst, vfx_n); }
PATCH_AT(0x81260898, word(0x94a806b6), after, none, pools_vfx_stream_c, as(x0, dst), as(x20, emitter)) {
    const int32_t n = (int32_t)G32(emitter + 0xd0);
    if (n > 0) nvn_claim_write(dst, (uint64_t)n << 4);
}
PATCH_AT(0x81260e6c, word(0x4d008100), after, none, pools_vfx_stream_record, as(x24, rec)) { nvn_claim_write(rec, 0x1d0); }
PATCH_AT(0x81268558, word(0x94a7e786), after, none, pools_vfx_copy, as(x0, dst), as(x19, o)) {
    const uint64_t n = G32(o + 0x1d0) >= 2 ? G64(o + 0x1b8) * (uint64_t)((int32_t)G32(o + 0x1cc) - (int32_t)G32(o + 0x1c8) + 1)
                                           : G64(o + 0x1b0);
    nvn_claim_write(dst, n);
}

/* ---- the depth grid: agl::utl::DepthBufferEx (depth_grid_slots_create) and the query object (depth_grid_buffers_create),
   claimed at their ubo_object_pool_alloc calls in the allocation table (docs/direct/pool-users.md). The grid's per-frame
   constants (0x390 x2) and the query constants (0x80 x4, x2) are Upload; the two grid slots (0x3970: the GPU writes the
   grid, report_reader_a reads the newest slot) Readback, with the CPU's per-frame camera copy into them (f_80365b70's memcpy of 0x2420 bytes at
   +0x1550, from job_calc_runner) reported; the 0x5b40 block VRAM. The query records (5000 x 0x90, zeroed) and the per-id
   state (5000 x 16, 1e6 each) are VRAM: the GPU's after their fill at creation, which follows their allocations inside
   depth_grid_buffers_create (the scope below). The query's read block (0x80366788) is Readback */
/* depth_grid_buffers_create fills its records (memset, 0x8036672c) and state (1e6, 0x80366814) after their allocations:
   a scope over the whole function, naming no kind, ends the maker's writes there */
PATCH_FUNC(0x80366600, word(0xd503237f), body(0x54947c02491b4cf1), aapcs, x0, pools_query_object, orig) {
    CLAIM("depth_grid_buffers_create", NVN_CLAIM_NONE);
}
PATCH_AT(0x80365c00, word(0x94e3f1dc), after, none, pools_grid_camera, as(x0, dst)) { nvn_claim_write(dst, 0x2420); }

/* job_list_build_b's 16.5 MB scratch (two pools through ubo_object_pool_alloc at 0x80c7dee4): GPU work reads 1.3 GB and
   writes 7.8 MB of it a present; the CPU only zeroes it once after making it (f_80c58d70's memset, reported): VRAM */
PATCH_FUNC(0x80c58d70, word(0xd503237f), body(0xec736dd16a039ab2), aapcs, x0, pools_job_scratch_zero, orig, as(x0, dims),
           as(x1, obj)) {
    /* (the memset's range as the function computes it: [obj + 0x18] + layout[+4], 8 x the product of the dims' four
       sizes) */
    const uint64_t dst = G64(obj + 0x18) + G32(G64(G64(obj + 0x10)) + 4);
    const int32_t n = (int32_t)(G32(dims + 0x10) * G32(dims + 0x14) * G32(dims + 0x18) * (*(uint8_t *)GP(dims + 0x24)) * 2u);
    const uint64_t r = guest_orig(orig);
    if (n > 0) nvn_claim_write(dst, (uint64_t)n * 4);
    return r;
}

/* ---- the probe module's calc blocks (gpu_calc_block_create, from probe_calc_block_init: 150 MB, and f_80c5c450 beside
   probe_bake_load: 0x2000; docs/direct/pool-users.md, "The probe module"). Never bound: shaders reach them through
   stored addresses: VRAM. gpu_calc_block_create fills its five ubo objects after making the pool, inside the claim's
   scope (the scope's end copies them). Later writers:
   - probe_calc_stream_write's memcpy of count ([x19 + 0x78]) x 16 bytes (about 26 pages a present) and
     probe_calc_fields_write's six 8-byte fields at the f_80c5c450 block's current half, each flushed by its writer (the
     tail call at 0x806ad940, 0x806ad0b0): those flushes are reported writes;
   - the BakeProbe thread's probe_bake_records_write: count ([x3 + 0x30]) records of 0x80 bytes at [x0 + 0x300] + 0x80 x
     [x2 + 8], and count ([x3 + 0x20]) of 0x20 at [x0 + 0x270] + 0x20 x [x2], reported: its flush comes before the
     records and covers the whole 156 MB block */
PATCH_AT(0x806accec, word(0x9416be2d), before, none, pools_probe_calc) { nvn_claim_begin(NVN_CLAIM_VRAM, "probe_calc_block_init calc block"); }
PATCH_AT(0x806accf0, word(0x36000420), before, none, pools_probe_calc_end) { nvn_claim_end(); }
PATCH_AT(0x80c5c55c, word(0x94000011), before, none, pools_probe_bake_calc) { nvn_claim_begin(NVN_CLAIM_VRAM, "f_80c5c450 calc block"); }
PATCH_AT(0x80c5c560, word(0xa94b4ff4), before, none, pools_probe_bake_calc_end) { nvn_claim_end(); }
PATCH_AT(0x806ad940, word(0xd61f0060), before, none, pools_probe_stream_flush) { nvn_claim_flush_written(); }
PATCH_AT(0x806ad0b0, word(0xd63f0100), before, none, pools_probe_fields_flush) { nvn_claim_flush_written(); }
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
/* terrain: terrain_streams_create's streams (written before their first use, then bound): Load once; the tile
   cache terrain_tile_load fills and reads (agl_memory_pool_create_b in terrain_tera_mgr_init) and f_80f6dc20's 202 MB pool
   are never bound: Host */
PATCH_AT(0x809e3990, word(0x97fc7af4), before, none, pools_terrain_streams) { nvn_claim_begin(NVN_CLAIM_LOAD_ONCE, "terrain_streams_create"); }
PATCH_AT(0x809e3994, word(0xf9400261), before, none, pools_terrain_streams_end) { nvn_claim_end(); }
PATCH_AT(0x809e3684, word(0x94162633), before, none, pools_terrain_tiles) { nvn_claim_begin(NVN_CLAIM_HOST, "terrain_tera_mgr_init tiles"); }
PATCH_AT(0x809e3688, word(0xf9402260), before, none, pools_terrain_tiles_end) { nvn_claim_end(); }
PATCH_AT(0x80f6dc5c, word(0x97fffb5d), before, none, pools_80f6dc20) { nvn_claim_begin(NVN_CLAIM_HOST, "f_80f6dc20 pool"); }
PATCH_AT(0x80f6dc60, word(0x39448268), before, none, pools_80f6dc20_end) { nvn_claim_end(); }
/* Writes after the claims' scopes that shaders reach through stored addresses, each flushed by the writer itself: those
   flushes are reported writes (a census in Kokiri: every write inside the flush that follows it, no grain the GPU writes).
   - cull_scene_blocks_init (f_80c62260) fills each block it just made, a 16-byte record per entry, and flushes the
     records (0x80c63174, 0x80c6323c), once a scene;
   - f_80c63e70 (called by f_80c62260 on SceneLowPriority) fills f_80c64650's block (count x 4 bytes, never bound in
     Kokiri) and flushes it (0x80c64234);
   - cull_address_block_create (f_80c64650) stores an 8-byte GPU address into the one-word block it just allocated, in a
     heap pool another maker grew (cull_task_block_fill's), and flushes it (0x80c64af4) */
PATCH_AT(0x80c63174, word(0xd63f0100), before, none, pools_80c62260_flush_a) { nvn_claim_flush_written(); }
PATCH_AT(0x80c6323c, word(0xd63f0100), before, none, pools_80c62260_flush_b) { nvn_claim_flush_written(); }
PATCH_AT(0x80c64234, word(0xd63f0100), before, none, pools_80c63e70_flush) { nvn_claim_flush_written(); }
PATCH_AT(0x80c64af4, word(0xd63f0100), before, none, pools_80c64650_flush) { nvn_claim_flush_written(); }
/* texdata_storage_block's: GPU scratch the CPU fills only when it makes it: VRAM */
PATCH_AT(0x80dd81d0, word(0x9400002c), before, none, pools_texdata_storage) { nvn_claim_begin(NVN_CLAIM_VRAM, "texdata_storage_block"); }
PATCH_AT(0x80dd81d4, word(0xf9400282), before, none, pools_texdata_storage_end) { nvn_claim_end(); }
/* f_80cdf2f0's third block, texdata_cpu_lut's table, is never bound: Host. The agl/gfx general pool (f_80f6a950, 0x22),
   CPU-written and bound: Upload */
PATCH_AT(0x80cdfc10, word(0x97f41a40), before, none, pools_80cdf2f0_lut) { nvn_claim_begin(NVN_CLAIM_HOST, "f_80cdf2f0 lut"); }
PATCH_AT(0x80cdfc14, word(0xf9401660), before, none, pools_80cdf2f0_lut_end) { nvn_claim_end(); }
PATCH_AT(0x80f6aa08, word(0x97e0a10a), before, none, pools_agl_general) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "f_80f6a950 general"); }
PATCH_AT(0x80f6aa0c, word(0x910003e0), before, none, pools_agl_general_end) { nvn_claim_end(); }
/* agl_driver_mgr_dl's third list (0x12, via f_80c51190): command memory: Host */
PATCH_AT(0x80c4d598, word(0x94000efe), before, none, pools_agl_driver_dl_c) { nvn_claim_begin(NVN_CLAIM_HOST, "agl_driver_mgr_dl lists"); }
PATCH_AT(0x80c4d59c, word(0xa95b4ff4), before, none, pools_agl_driver_dl_c_end) { nvn_claim_end(); }
/* the probe module's storage block (storage_block_create in probe_storage_init, 5.5 MB): draws bind it whole, read only;
   bake::ModuleMgr writes 32-byte records into it through f_80c4e880 (probe_storage_records_write) and flushes them
   (0x80c4e9d0): VRAM, that flush a reported write, so the whole-pool binds cost nothing */
PATCH_AT(0x80b167c4, word(0x9404dea3), before, none, pools_probe_storage) { nvn_claim_begin(NVN_CLAIM_VRAM, "probe_storage_init storage"); }
PATCH_AT(0x80b167c4, word(0x9404dea3), after, none, pools_probe_storage_end) { nvn_claim_end(); }   /* (0x80b167c8 is a branch target) */
PATCH_AT(0x80c4e9d0, word(0xd63f0100), before, none, pools_probe_records_flush) { nvn_claim_flush_written(); }

/* ---- the allocation table: the gsys object blocks claimed at their allocators' calls. ubo_object_pool_alloc,
   ubo_object_block_alloc and ubo_object_block_alloc_b run inside the claim of the row whose return address is the call's
   (x30 at their entry); a call no row names takes the claims around it. A row is a BL's return address and the BL's
   word, checked against the image at the first allocation of a run ---- */
static const struct { uint64_t ret; uint32_t bl; int kind; const char *by; } alloc_sites[] = {
    /* depth_grid_buffers_create's read block: invalidated and read every frame */
    {0x8036678c, 0x9406eb8a, NVN_CLAIM_READBACK, "f_80366600 read block"},
    /* the query constants; the query records and per-id state, filled inside the maker's scope */
    {0x80366698, 0x9406ebc7, NVN_CLAIM_UPLOAD, "depth_grid_buffers_create constants"},
    {0x803666d4, 0x9406ebb8, NVN_CLAIM_VRAM, "depth_grid_buffers_create records"},
    {0x803667c8, 0x9406eb7b, NVN_CLAIM_VRAM, "depth_grid_buffers_create state"},
    /* DepthBufferEx: the per-frame constants, the two grid slots report_reader_a reads, the 0x5b40 block */
    {0x80c5a8e0, 0x97e31b35, NVN_CLAIM_UPLOAD, "depth_grid_slots_create constants"},
    {0x80c5aa2c, 0x97e31ae2, NVN_CLAIM_READBACK, "depth_grid_slots_create slots"},
    {0x80c5aa9c, 0x97e31ac6, NVN_CLAIM_READBACK, "depth_grid_slots_create slots"},
    {0x80c5ab28, 0x97e31aa3, NVN_CLAIM_VRAM, "depth_grid_slots_create block"},
    /* culling object b's two result blocks, read by f_80586fd0 once their submission completed */
    {0x80c597e4, 0x97e31f74, NVN_CLAIM_READBACK, "cull_gpu_object_b_create results"},
    {0x80c59828, 0x97e31f63, NVN_CLAIM_READBACK, "cull_gpu_object_b_create results"},
    /* GeometryCullingMgr: the views' address arrays (their pool also holds the exposure record); the block the GPU writes */
    {0x80ecdcc4, 0x97d94e3c, NVN_CLAIM_READBACK, "GeometryCullingMgr views"},
    {0x80ecdd28, 0x97d952fb, NVN_CLAIM_VRAM, "GeometryCullingMgr block"},
    /* job_list_build_b's 16.5 MB scratch: GPU work's, the CPU's one zeroing reported */
    {0x80c7dee8, 0x97e28db3, NVN_CLAIM_VRAM, "job_list_build_b scratch"},
    /* the save picture (texdata_serialize_jpg): a copy the CPU waits for, then encodes */
    {0x80bd1e4c, 0x97e54082, NVN_CLAIM_READBACK, "texdata_serialize_jpg picture"},
    /* texdata_init_jpg's work buffer: never bound */
    {0x809ea320, 0x97ecdf4d, NVN_CLAIM_HOST, "texdata_init_jpg buffer"},
    /* cull_address_block_create's block: written later (f_80c63e70's fill), bound as storage in Market */
    {0x80c648a4, 0x97e2f344, NVN_CLAIM_UPLOAD, "f_80c64650 block"},
    /* cull_scene_blocks_init's blocks: GPU scratch the CPU fills only when it makes them */
    {0x80c62a3c, 0x97e2fade, NVN_CLAIM_VRAM, "f_80c62260 blocks"},
    {0x80c62f8c, 0x97e2f98a, NVN_CLAIM_VRAM, "f_80c62260 blocks"},
    /* CPU-written blocks binds read: f_80cdf2f0's two (texdata_model_procedural's), f_80c5ea90's */
    {0x80cdfcf4, 0x97e10630, NVN_CLAIM_UPLOAD, "f_80cdf2f0 blocks"},
    {0x80cdfdcc, 0x97e105fa, NVN_CLAIM_UPLOAD, "f_80cdf2f0 blocks"},
    {0x80c5ee24, 0x97e309e4, NVN_CLAIM_UPLOAD, "f_80c5ea90 block"},
    /* f_82c24db0's block: written before its first use, never again (Kokiri and Market) */
    {0x82c2d6d0, 0x9763cfb9, NVN_CLAIM_LOAD_ONCE, "f_82c24db0 block"},
};
enum { ALLOC_SITES = sizeof alloc_sites / sizeof *alloc_sites };
static void alloc_sites_check(void) {
    static const uint64_t allocators[] = {0x805215b0, 0x80522050, 0x80522910};
    for (int i = 0; i < ALLOC_SITES; i++) {
        const uint64_t at = alloc_sites[i].ret - 4;
        const uint32_t w = G32(at);
        const uint64_t to = at + (uint64_t)((int64_t)((int32_t)(w << 6) >> 6) * 4);   /* (a BL's target) */
        int ok = w == alloc_sites[i].bl && (w >> 26) == 0x25;
        if (ok) ok = to == allocators[0] || to == allocators[1] || to == allocators[2];
        if (!ok)
            hos_fatal(NULL, "pools: the allocation table's row %#llx (%s): the word %#x is not its BL %#x of a ubo allocator",
                      (unsigned long long)at, alloc_sites[i].by, w, alloc_sites[i].bl);
    }
}
static uint64_t alloc_claimed(GuestFn *orig, uint64_t lr) {
    static int checked;
    if (!__atomic_load_n(&checked, __ATOMIC_ACQUIRE)) {
        alloc_sites_check();
        __atomic_store_n(&checked, 1, __ATOMIC_RELEASE);
    }
    for (int i = 0; i < ALLOC_SITES; i++)
        if (alloc_sites[i].ret == lr) {
            nvn_claim_begin(alloc_sites[i].kind, alloc_sites[i].by);
            const uint64_t r = guest_orig(orig);
            nvn_claim_end();
            return r;
        }
    return guest_orig(orig);
}
PATCH_FUNC(0x805215b0, word(0xd503237f), body(0x1cfe9a3b476f777c), aapcs, x0, pools_ubo_object_pool_alloc, orig, as(x30, lr)) {
    return alloc_claimed(orig, lr);
}
PATCH_FUNC(0x80522050, word(0xd503237f), body(0x258ada0e4edabbfa), aapcs, x0, pools_ubo_object_block_alloc, orig, as(x30, lr)) {
    return alloc_claimed(orig, lr);
}
PATCH_FUNC(0x80522910, word(0xd503237f), body(0x59910b0ba0473726), aapcs, x0, pools_ubo_object_block_alloc_b, orig,
           as(x30, lr)) {
    return alloc_claimed(orig, lr);
}
