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

/* agl::utl::DynamicTextureAllocator: one physical pool (flags 0x18c, 200 MB) and one virtual pool of its size, mapped
   whole once. Only render targets live there; the physical pool's memory holds the arenas' free-list nodes */
PATCH_FUNC(0x80737cac, word(0xd503237f), body(0x5141fda80a86a2e4), aapcs, x0, pools_dta_init, orig) {
    CLAIM("dta_init", NVN_CLAIM_TARGETS);
}
/* shader code (0x62, 0x64): translated by the host, never read by the GPU */
PATCH_FUNC(0x805ba7c0, word(0xd503237f), body(0x1b1a418c6f5f5c4a), aapcs, x0, pools_shader_archive, orig) {
    CLAIM("f_805ba7c0", NVN_CLAIM_HOST);
}
PATCH_FUNC(0x8055d4e0, word(0xd503237f), body(0x8ab0de74b6d3e0da), aapcs, x0, pools_shader_code, orig) {
    CLAIM("f_8055d4e0", NVN_CLAIM_HOST);
}
/* the gsys GPU heap (0x12, 0x22, 0x24, 0xa1) */
PATCH_FUNC(0x80198500, word(0xd503237f), body(0xb70196b1d65d6743), aapcs, x0, pools_gpu_heap, orig) {
    CLAIM("f_80198500", NVN_CLAIM_LAYER0);
}
/* agl's pool wrapper (the texture heap's inside its own claim) */
PATCH_FUNC(0x80577b30, word(0xd503237f), body(0xc69d2d96f5b1a11a), aapcs, x0, pools_agl, orig) {
    CLAIM("f_80577b30", NVN_CLAIM_LAYER0);
}
/* nn::gfx MemoryPool for in-place files */
PATCH_FUNC(0x8052f8a8, word(0xd503237f), body(0x716734f6519dbfb9), aapcs, x0, pools_gfx, orig) {
    CLAIM("gfx_memory_pool_init", NVN_CLAIM_LAYER0);
}
/* sead's device and queue pools and their neighbours at boot (0x12, 0x21, 0xa1) */
PATCH_FUNC(0x805e7a88, word(0xd503237f), body(0xb4d112ce9270cf55), aapcs, x0, pools_sead, orig) {
    CLAIM("f_805e7a88", NVN_CLAIM_LAYER0);
}
PATCH_FUNC(0x805e6684, word(0xd503237f), body(0xbe62a6f8ce137b87), aapcs, x0, pools_boot_a1, orig) {
    CLAIM("f_805e6684", NVN_CLAIM_LAYER0);
}
/* other pool makers (0x22, 0x62) */
PATCH_FUNC(0x807a6234, word(0xd503237f), body(0x894b655fcae7f0aa), aapcs, x0, pools_807a6234, orig) {
    CLAIM("f_807a6234", NVN_CLAIM_LAYER0);
}
PATCH_FUNC(0x807ac5e8, word(0xd503237f), body(0xe9d853d0329ea395), aapcs, x0, pools_807ac5e8, orig) {
    CLAIM("f_807ac5e8", NVN_CLAIM_LAYER0);
}
/* the display's buffers at boot (0x21, 95.6 MB) */
PATCH_FUNC(0x80802d48, word(0xd503237f), body(0xe94a043c888e1db2), aapcs, x0, pools_display, orig) {
    CLAIM("f_80802d48", NVN_CLAIM_LAYER0);
}
