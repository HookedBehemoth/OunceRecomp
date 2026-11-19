/* Game patch: agl's NDSR upscaler keeps its stage, renders at ratio 1.0, and its network pass goes to the host's upscaler.

   The model scene makes its NDSR stage when its resolved config's CreateNdsr (+0xe10) is set (scene_stages_init at
   0x804c4578: a 0x20-byte stage at +0x2b0, its network holder at +0x18). The game reads that stage with no check that it
   exists (ndsr_render_scale, f_80442ccc, when the config's +0x930 is set), so the game's own setting stays, and agl's
   resource set-up builds agl_ndsr.pck's network into the holder (ndsr_network_load).

   The render ratio: the holder's ratio table (ndsr_network_load: 1.0, then the network's ratios; up to 10 floats at +0,
   their count at +0x24) is cut to the one ratio 1.0. The stage's ratio index (+0x10) then stays 0, and its pass
   (ndsr_stage_pass, f_80345a80) takes the game's own path without the network: SMAA (f_81625678) straight into the
   output, at the output's size.

   The upscale: that pass's network evaluation (f_80345f30) is replaced by the host's upscaler (runtime/upscale.h),
   which gets the SMAA-resolved colour ("ndsr::smaa_output"), the output image and their sizes. At ratio 1.0 it is not
   reached.

   Check that it applies: the line "ndsr: network: <n> ratios, cut to 1.0" at boot. */
#include "patches.h"
#include "upscale.h"

/* ndsr_network_load(holder, ...): the network built, the holder's ratio table cut to 1.0 */
PATCH_FUNC(0x8055ab00, word(0xd503237f), body(0xb525de25aa1cba9f), aapcs, none, ndsr_network_ratios, orig,
           as(x0, holder), as(x1, args)) {
    guest_orig(orig, holder, args);
    hos_log("ndsr: network: %u ratios, cut to 1.0", *(const uint32_t *)GP(holder + 0x24));
    *(float *)GP(holder) = 1.0f;
    *(uint32_t *)GP(holder + 0x24) = 1;
}

/* ndsr_stage_pass's argument block of the network evaluation (x2): the sizes, the colour's and the output's
   NVNtexture and pool ids */
typedef struct {
    uint64_t zero;
    uint32_t in_w, in_h;
    uint32_t view_w, view_h;   /* (the pass's x6) */
    uint32_t out_w, out_h;
    uint32_t kind, index;
    uint64_t src_tex;
    uint32_t src_id, pad0;
    uint64_t dst_tex;
    uint32_t dst_id, pad1;
} ndsr_block;
_Static_assert(__builtin_offsetof(ndsr_block, out_w) == 0x18 && __builtin_offsetof(ndsr_block, src_tex) == 0x28 &&
                   __builtin_offsetof(ndsr_block, dst_tex) == 0x38 && __builtin_offsetof(ndsr_block, dst_id) == 0x40,
               "ndsr_stage_pass's block");

PATCH_AT(0x80345d84, word(0x9400006b), instead, none, ndsr_network_run, as(x1, cmd), as(x2, block)) {   /* bl f_80345f30 */
    const ndsr_block *b = GP(block);
    const hos_upscale u = {cmd, b->src_tex, b->src_id, b->in_w, b->in_h, b->dst_tex, b->dst_id, b->out_w, b->out_h, 0.0f};
    hos_upscale_record(&u);
}
