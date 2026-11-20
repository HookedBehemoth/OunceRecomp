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
   (0x24) no maker below claims are Upload (as OoT's: the census finds each written by the CPU at its making and read by
   binds, or never used), the CPU_NO_ACCESS ones VRAM */
static int gsys_rule(uint32_t flags, uint64_t size) {
    return (flags & 0xff) == 0x22 || (flags & 0xff) == 0x24 ? NVN_CLAIM_UPLOAD : by_access(flags, size);
}
/* nn::gfx in-place files' pools: their vertex and index data (0x24) is written before the GPU first uses it, then
   never again: load once. The small 0x22 pools of f_808a54e0 and f_80877d2c (a page each, written at their making, some
   bound): Upload */
static int gfx_rule(uint32_t flags, uint64_t size) {
    return (flags & 0xff) == 0x24 ? NVN_CLAIM_LOAD_ONCE : (flags & 0xff) == 0x22 ? NVN_CLAIM_UPLOAD : by_access(flags, size);
}
#define CLAIM_RULE(fn)                        \
    nvn_claim_begin_rule(by_access, fn);  \
    const uint64_t r = guest_orig(orig);      \
    nvn_claim_end();                          \
    return r

/* agl::utl::DynamicTextureAllocator: one physical pool (flags 0x18c, 200 MB) and one virtual pool of its size, mapped
   whole once. Only render targets live there; the physical pool's memory holds the arenas' free-list nodes */
PATCH_FUNC(0x80737cac, word(0xd503237f), body(0x5141fda80a86a2e4), aapcs, x0, pools_dta_init, orig) {
    CLAIM("dta_init", NVN_CLAIM_TARGETS);
}
/* shader code (0x62, 0x64): translated by the host, never read by the GPU; f_8086a680: the in-place shader binaries
   (nn::gfx pools of flags 0x64), f_805ba7c0: the shader archives */
PATCH_FUNC(0x8086a680, word(0xd503237f), body(0xcfdce879f9d4dded), aapcs, x0, pools_shader_binary, orig) {
    CLAIM("f_8086a680", NVN_CLAIM_HOST);
}
PATCH_FUNC(0x805ba7c0, word(0xd503237f), body(0x1b1a418c6f5f5c4a), aapcs, x0, pools_shader_archive, orig) {
    CLAIM("f_805ba7c0", NVN_CLAIM_HOST);
}
/* ndsr_network_setup (f_8055d4e0, "SMAA_For_NDSR"): the networks' pool (0x64), its programs and the weights the GPU reads
   as storage, as OoT's: VRAM, written inside the set-up's scope */
PATCH_FUNC(0x8055d4e0, word(0xd503237f), body(0x8ab0de74b6d3e0da), aapcs, x0, pools_ndsr, orig) {
    CLAIM("ndsr_network_setup", NVN_CLAIM_VRAM);
}
/* in-place texture files and texture caches: layout_combined_bntx_load (ui2d, 0x21, 240 MB at the title),
   vfx2_texture_init_a (Vfx2's, 0x24), font_texture_cache_update (the glyph cache, 0x22); the fonts' BNTX is assets.c's
   assets_font_bntx */
PATCH_FUNC(0x8052f7b0, word(0xd503237f), body(0xf31b0e8c7b6eb98b), aapcs, x0, pools_layout_bntx, orig) {
    CLAIM("layout_combined_bntx_load", NVN_CLAIM_TEXTURES);
}
PATCH_FUNC(0x808d1060, word(0xd503237f), body(0x5a8f011a3e89f772), aapcs, x0, pools_vfx2_textures, orig) {
    CLAIM("vfx2_texture_init_a", NVN_CLAIM_TEXTURES);
}
PATCH_FUNC(0x805a1580, word(0xb9400048), body(0x6a630efbea9444ee), aapcs, x0, pools_glyph_cache, orig) {
    CLAIM("font_texture_cache_update", NVN_CLAIM_TEXTURES);
}
/* the gsys GPU heap (0x12, 0x22, 0x24, 0xa1) */
PATCH_FUNC(0x80198500, word(0xd503237f), body(0xb70196b1d65d6743), aapcs, x0, pools_gpu_heap, orig) {
    nvn_claim_begin_rule(gsys_rule, "f_80198500");
    const uint64_t r = guest_orig(orig);
    nvn_claim_end();
    return r;
}
/* agl's pool wrapper (the texture heap's inside its own claim) */
PATCH_FUNC(0x80577b30, word(0xd503237f), body(0xc69d2d96f5b1a11a), aapcs, x0, pools_agl, orig) {
    CLAIM_RULE("f_80577b30");
}
/* nn::gfx MemoryPool for in-place files */
PATCH_FUNC(0x8052f8a8, word(0xd503237f), body(0x716734f6519dbfb9), aapcs, x0, pools_gfx, orig) {
    nvn_claim_begin_rule(gfx_rule, "gfx_memory_pool_init");
    const uint64_t r = guest_orig(orig);
    nvn_claim_end();
    return r;
}
/* f_805e7a88 (sead's graphics system) makes the command memory (0x12, 16 MB; the host translates it, at 0x805e7dbc) and
   the shader scratch memory (0x21, GPU only, at 0x805e7fac), each call claimed; the display's buffers it makes through
   display_buffers_pool_init are that function's claim */
PATCH_AT(0x805e7dbc, word(0xd63f0100), before, none, pools_sead_commands) { nvn_claim_begin(NVN_CLAIM_HOST, "f_805e7a88 commands"); }
PATCH_AT(0x805e7dc0, word(0xb9004fff), before, none, pools_sead_commands_end) { nvn_claim_end(); }
PATCH_AT(0x805e7fac, word(0xd63f0100), before, none, pools_sead_scratch) { nvn_claim_begin(NVN_CLAIM_VRAM, "f_805e7a88 scratch"); }
PATCH_AT(0x805e7fb0, word(0x52800800), before, none, pools_sead_scratch_end) { nvn_claim_end(); }
/* f_805e6684: one pool (0xa1, 64 MB) holding two textures of the window's size at boot: render targets */
PATCH_FUNC(0x805e6684, word(0xd503237f), body(0xbe62a6f8ce137b87), aapcs, x0, pools_boot_targets, orig) {
    CLAIM("f_805e6684", NVN_CLAIM_TARGETS);
}
/* desc_pools_create: the texture and sampler descriptor pools' memory (0x22); the host translates the descriptors */
PATCH_FUNC(0x807a6234, word(0xd503237f), body(0x894b655fcae7f0aa), aapcs, x0, pools_desc, orig) {
    CLAIM("desc_pools_create", NVN_CLAIM_HOST);
}
/* f_807ac5e8 (a small drawer's set-up, through a vtable at 0x823dd138) makes a shader-code pool (0x62, its
   nvnMemoryPoolInitialize at 0x807ac6cc) and a data pool (0x22, at 0x807ac8b4) it carves into about twenty buffers,
   each mapped and filled by the CPU (nvnBufferMap, then f_807ad440 and its siblings), read as vertex and uniform data:
   Upload. Not reached by a title-menu run: claimed from the code, unverified */
PATCH_AT(0x807ac6cc, word(0xd63f0100), before, none, pools_807ac5e8_code) { nvn_claim_begin(NVN_CLAIM_HOST, "f_807ac5e8 code"); }
PATCH_AT(0x807ac6d0, word(0xb000e668), before, none, pools_807ac5e8_code_end) { nvn_claim_end(); }
PATCH_AT(0x807ac8b4, word(0xd63f0100), before, none, pools_807ac5e8_data) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "f_807ac5e8 data"); }
PATCH_AT(0x807ac8b8, word(0xf000e67a), before, none, pools_807ac5e8_data_end) { nvn_claim_end(); }
/* the display's buffers at boot (0x21, 95.6 MB): window textures, images the back end owns */
PATCH_FUNC(0x80802d48, word(0xd503237f), body(0xe94a043c888e1db2), aapcs, x0, pools_display, orig) {
    CLAIM("display_buffers_pool_init", NVN_CLAIM_TARGETS);
}

/* ---- the gsys object blocks and pools the census names (debug.poolcensus; DKB's headless run reaches only the title
   menus), each at the call that makes it ---- */
/* never bound, written by the CPU: f_80743ba8's (0x12, the Presentation Thread's) and f_80732960's (0x12, written and
   read by MainThread): Host. f_804ab060's (0x24, 0x40000: never bound at the title, read by f_8189f728) is written by GPU
   work as storage in play: Readback. f_807935d8's (0.8 MB, written about 104 pages a present on
   SceneGraphicsDrawer) is never bound at the title, but in play draws read it as vertex and storage data: Upload */
PATCH_AT(0x80743ed4, word(0x97e948bb), before, none, pools_80743ba8) { nvn_claim_begin(NVN_CLAIM_HOST, "f_80743ba8 block"); }
PATCH_AT(0x80743ed8, word(0xf9400268), before, none, pools_80743ba8_end) { nvn_claim_end(); }
PATCH_AT(0x807329c4, word(0x97e98dff), before, none, pools_80732960) { nvn_claim_begin(NVN_CLAIM_HOST, "f_80732960 block"); }
PATCH_AT(0x807329c8, word(0xb000ea48), before, none, pools_80732960_end) { nvn_claim_end(); }
PATCH_AT(0x804ab1c0, word(0x97f3ac00), before, none, pools_804ab060) { nvn_claim_begin(NVN_CLAIM_READBACK, "f_804ab060 block"); }
PATCH_AT(0x804ab1c4, word(0xf9400aa8), before, none, pools_804ab060_end) { nvn_claim_end(); }
PATCH_AT(0x80793688, word(0x9400010e), before, none, pools_807935d8) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "f_807935d8 block"); }
PATCH_AT(0x8079368c, word(0x294a2668), before, none, pools_807935d8_end) { nvn_claim_end(); }
/* f_805723e8's general pool (0x22, 14 MB, through gfx_memory_pool_init): CPU-written every frame, binds read it: Upload */
PATCH_AT(0x80572498, word(0x97fef504), before, none, pools_805723e8) { nvn_claim_begin(NVN_CLAIM_UPLOAD, "f_805723e8 general"); }
PATCH_AT(0x80572498, word(0x97fef504), after, none, pools_805723e8_end) { nvn_claim_end(); }   /* (0x8057249c is a branch target) */

/* ---- gsys::GeometryCullingMgr (cull_geometry_mgr_memory, as OoT's gpu_result_block_a_create; docs/direct/pool-users.md):
   per view, an array of the units' GPU addresses that cull_geometry_slot_alloc writes each frame (the address store
   inlined: `str x0, [x20], #8` at 0x8163d100, reported) and a draw binds as written storage: Readback (the views' small
   pool is shared by other ubo objects); the block the GPU writes through those addresses: VRAM */
PATCH_AT(0x804c0f18, word(0x94002280), before, none, pools_geocull_views) { nvn_claim_begin(NVN_CLAIM_READBACK, "GeometryCullingMgr views"); }
PATCH_AT(0x804c0f1c, word(0xb9400aa8), before, none, pools_geocull_views_end) { nvn_claim_end(); }
PATCH_AT(0x804c0f68, word(0x97f35496), before, none, pools_geocull_block) { nvn_claim_begin(NVN_CLAIM_VRAM, "GeometryCullingMgr block"); }
PATCH_AT(0x804c0f6c, word(0xd10083a0), before, none, pools_geocull_block_end) { nvn_claim_end(); }
PATCH_AT(0x8163d100, word(0xf8008680), after, none, pools_geocull_address, as(x20, next)) { nvn_claim_write(next - 8, 8); }

/* ---- makers a title-menu run does not reach, claimed from the code (unverified) ----
   f_8074431c: agl::lyr::RenderDLBuffer's block (ubo_object_block_alloc_b, 144 MB), display lists as OoT's
   agl_render_dl_buffer: command memory the host translates: Host */
PATCH_FUNC(0x8074431c, word(0xd503237f), body(0xd2d360b2bfbdff4b), aapcs, x0, pools_render_dl_buffer, orig) {
    CLAIM("f_8074431c render DL buffer", NVN_CLAIM_HOST);
}
/* f_808ec234: Vfx2's "NnVfx2GfxMemoryPool" (gfx_memory_pool_init, 0x22: 24 MB and 13 MB) and its "NnVfx2GfxBuffer":
   particle data the CPU writes each frame and GPU work writes too, which the CPU reads back (as f_808da640's, in play):
   Readback */
PATCH_FUNC(0x808ec234, word(0xd503237f), body(0x8e2badedb7caacc8), aapcs, x0, pools_vfx2_pool, orig) {
    CLAIM("f_808ec234 Vfx2 pool", NVN_CLAIM_READBACK);
}
/* f_808da640: Vfx2's other "NnVfx2GfxMemoryPool" (0x12, 27.5 MB, through gfx_memory_pool_init), its particle buffers: the
   CPU writes them, GPU work writes them as storage, and the CPU reads those writes (debug.poolcensus in the tunnel: about
   3 pages a present, through 0x80081634 and f_808efb20): Readback */
PATCH_FUNC(0x808da640, word(0xd503237f), body(0x7d376d12bfcde764), aapcs, x0, pools_vfx2_pool_b, orig) {
    CLAIM("f_808da640 Vfx2 pool", NVN_CLAIM_READBACK);
}
/* agl::driver::GraphicsDriverMgr's (f_80742da8) 0.5 MB block (0x22, ubo_object_block_alloc_b at 0x80743764): GPU results
   the CPU reads in flight and later (f_801474c0, f_803603d0, f_80360178): Readback */
PATCH_AT(0x80743764, word(0x97e94a97), before, none, pools_driver_results) { nvn_claim_begin(NVN_CLAIM_READBACK, "GraphicsDriverMgr results"); }
PATCH_AT(0x80743768, word(0xf946aaa8), before, none, pools_driver_results_end) { nvn_claim_end(); }
/* agl::driver::GraphicsDriverMgr's (f_80742da8) two display lists (0x12 through f_804e0908, 4 MB and 512 KB, as OoT's agl_driver_mgr_dl lists): never
   bound: Host */
PATCH_AT(0x8074363c, word(0x97f674b3), before, none, pools_driver_dl_a) { nvn_claim_begin(NVN_CLAIM_HOST, "f_80742da8 lists"); }
PATCH_AT(0x80743640, word(0xf9405be8), before, none, pools_driver_dl_a_end) { nvn_claim_end(); }
PATCH_AT(0x8074368c, word(0x97f6749f), before, none, pools_driver_dl_b) { nvn_claim_begin(NVN_CLAIM_HOST, "f_80742da8 lists"); }
PATCH_AT(0x80743690, word(0x52802d00), before, none, pools_driver_dl_b_end) { nvn_claim_end(); }
/* f_804a0944: a named block ("%s" or "Unknown", ubo_object_block_alloc_b; from f_803f4dbc, f_8040c21c, f_8049d9e4), its
   flags its caller's. In play (debug.poolcensus): 0x22 blocks the CPU writes every frame and binds read (Upload); a 450 MB
   0x24 block only GPU work writes, as storage, the CPU never touching it (VRAM); a 72 MB 0x12 block not used in the
   first four minutes (Upload, as CPU-written uncached memory: unverified) */
static int block_804a0944_rule(uint32_t flags, uint64_t size) {
    const uint32_t f = flags & 0xff;
    return f == 0x24 ? NVN_CLAIM_VRAM : f == 0x22 || f == 0x12 ? NVN_CLAIM_UPLOAD : by_access(flags, size);
}
PATCH_AT(0x804a0a98, word(0x97f3d5ca), before, none, pools_804a0944) { nvn_claim_begin_rule(block_804a0944_rule, "f_804a0944 block"); }
PATCH_AT(0x804a0a9c, word(0xf9400b28), before, none, pools_804a0944_end) { nvn_claim_end(); }
