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
   CPU_NO_ACCESS ones VRAM; the rest stay layer 0's until their claims move */
static int gsys_rule(uint32_t flags, uint64_t size) {
    return (flags & 0xff) == 0x22 ? NVN_CLAIM_UPLOAD : gpu_only_vram(flags, size);
}
#define CLAIM_RULE(fn)                        \
    nvn_claim_begin_rule(gpu_only_vram, fn);  \
    const uint64_t r = guest_orig(orig);      \
    nvn_claim_end();                          \
    return r

/* agl::utl::DynamicTextureAllocator: one physical pool (flags 0x18c, 200 MB) and one virtual pool of its size, mapped
   whole once. Only render targets live there; the physical pool's memory holds the arenas' free-list nodes */
PATCH_FUNC(0x80737cac, word(0xd503237f), body(0x5141fda80a86a2e4), aapcs, x0, pools_dta_init, orig) {
    CLAIM("dta_init", NVN_CLAIM_TARGETS);
}
/* shader code (0x62, 0x64): translated by the host, never read by the GPU; f_8086a680: the in-place shader binaries
   (nn::gfx pools of flags 0x64) */
PATCH_FUNC(0x8086a680, word(0xd503237f), body(0xcfdce879f9d4dded), aapcs, x0, pools_shader_binary, orig) {
    CLAIM("f_8086a680", NVN_CLAIM_HOST);
}
PATCH_FUNC(0x805ba7c0, word(0xd503237f), body(0x1b1a418c6f5f5c4a), aapcs, x0, pools_shader_archive, orig) {
    CLAIM("f_805ba7c0", NVN_CLAIM_HOST);
}
PATCH_FUNC(0x8055d4e0, word(0xd503237f), body(0x8ab0de74b6d3e0da), aapcs, x0, pools_shader_code, orig) {
    CLAIM("f_8055d4e0", NVN_CLAIM_HOST);
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
    CLAIM_RULE("gfx_memory_pool_init");
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
/* f_807ac5e8 makes a shader-code pool (0x62, its nvnMemoryPoolInitialize at 0x807ac6cc) and a data pool (0x22, at
   0x807ac8b4): each call claimed */
PATCH_AT(0x807ac6cc, word(0xd63f0100), before, none, pools_807ac5e8_code) { nvn_claim_begin(NVN_CLAIM_HOST, "f_807ac5e8 code"); }
PATCH_AT(0x807ac6d0, word(0xb000e668), before, none, pools_807ac5e8_code_end) { nvn_claim_end(); }
PATCH_AT(0x807ac8b4, word(0xd63f0100), before, none, pools_807ac5e8_data) { nvn_claim_begin(NVN_CLAIM_LAYER0, "f_807ac5e8 data"); }
PATCH_AT(0x807ac8b8, word(0xf000e67a), before, none, pools_807ac5e8_data_end) { nvn_claim_end(); }
/* the display's buffers at boot (0x21, 95.6 MB): window textures, images the back end owns */
PATCH_FUNC(0x80802d48, word(0xd503237f), body(0xe94a043c888e1db2), aapcs, x0, pools_display, orig) {
    CLAIM("display_buffers_pool_init", NVN_CLAIM_TARGETS);
}
