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
/* nn::gfx MemoryPool for in-place files: BFRES vertex and index data (0x24), shader binaries (0x64), ui2d (0x21) */
PATCH_FUNC(0x80792e30, word(0xd503237f), body(0xba09850595f0fa65), aapcs, x0, pools_gfx, orig) {
    CLAIM_RULE("gfx_memory_pool_init");
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
