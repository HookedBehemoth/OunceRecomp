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

/* agl::utl::DynamicTextureAllocator's pool record: one physical pool (flags 0x18c, 687 MB at 1080p) and one virtual
   pool of its size, mapped whole once. Only render targets live there; the physical pool's memory holds the arenas'
   free-list nodes, which the CPU writes, and no texels */
PATCH_FUNC(0x80c49380, word(0xd503237f), body(0x3b0566baade1f62c), aapcs, x0, pools_dta_record, orig) {
    CLAIM("dta_pool_record_create", NVN_CLAIM_TARGETS);
}
/* the shader archives' code (flags 0x62) and NDSR's networks' (0x64): translated by the host, never read by the GPU */
PATCH_FUNC(0x80b65f00, word(0xd503237f), body(0x1688e4429b5f5bb0), aapcs, x0, pools_sharcb, orig) {
    CLAIM("shader_sharcb_load", NVN_CLAIM_HOST);
}
PATCH_FUNC(0x80f71a00, word(0xd503237f), body(0xe6b4bb892a0f8c6d), aapcs, x0, pools_ndsr, orig) {
    CLAIM("ndsr_network_setup", NVN_CLAIM_HOST);
}
/* the gsys GPU heap (0x12, 0x22, 0x24, 0xa1: uniform blocks, vertex streams, GPU scratch) */
PATCH_FUNC(0x809de920, word(0xd503237f), body(0x02d1932cc850ce16), aapcs, x0, pools_gpu_heap, orig) {
    CLAIM("gpu_pool_create", NVN_CLAIM_LAYER0);
}
/* agl's pool wrapper (the tera manager's pools; the texture heap's inside its own claim) */
PATCH_FUNC(0x80f6c9d0, word(0xd503237f), body(0x698a26829c189947), aapcs, x0, pools_agl, orig) {
    CLAIM("agl_memory_pool_create", NVN_CLAIM_LAYER0);
}
/* nn::gfx MemoryPool for in-place files: BFRES vertex and index data (0x24), shader binaries (0x64), ui2d (0x21) */
PATCH_FUNC(0x80792e30, word(0xd503237f), body(0xba09850595f0fa65), aapcs, x0, pools_gfx, orig) {
    CLAIM("gfx_memory_pool_init", NVN_CLAIM_LAYER0);
}
/* sead's device and queue pools (0x12, 0x21) */
PATCH_FUNC(0x80f69040, word(0xd503237f), body(0x64085c9cc9080b15), aapcs, x0, pools_sead, orig) {
    CLAIM("sead_device_queue_pools", NVN_CLAIM_LAYER0);
}
/* the texture and sampler descriptor pools' memory (0x22) */
PATCH_FUNC(0x80966440, word(0xd503237f), body(0x2abbf2d96dca7f84), aapcs, x0, pools_desc, orig) {
    CLAIM("desc_pools_create", NVN_CLAIM_LAYER0);
}
/* the display's buffers at boot (0x21, 95.6 MB) */
PATCH_FUNC(0x8114e4f0, word(0xd503237f), body(0xb3560c600e03da68), aapcs, x0, pools_display, orig) {
    CLAIM("f_8114e4f0", NVN_CLAIM_LAYER0);
}
