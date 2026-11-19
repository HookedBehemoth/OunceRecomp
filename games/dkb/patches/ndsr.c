/* Game patch: agl's NDSR upscaler keeps its stage and its ratio table; the host chooses the upscaler (runtime/upscale.h).

   The model scene makes its NDSR stage when its resolved config's CreateNdsr (+0xe10) is set (scene_stages_init at
   0x804c4578: a 0x20-byte stage at +0x2b0, its network holder at +0x18). The game reads that stage with no check that it
   exists (ndsr_render_scale, f_80442ccc, when the config's +0x930 is set), and agl's resource set-up builds
   agl_ndsr.pck's network into the holder (ndsr_network_load).

   The holder's ratio table (ndsr_network_load: 1.0, then the network's ratios; up to 10 floats at +0, their count at
   +0x24) gives the stage's ratio index (+0x10). At index 0 the pass (ndsr_stage_pass, f_80345a80) resolves SMAA
   (f_81625678) straight into the output, and no upscaler runs; above it, into "ndsr::smaa_output" at the render size,
   and the network evaluation (f_80345f30) runs the game's network or the host's upscaler, by hos_upscaler_get() at each
   frame.

   Check that it applies: the line "ndsr: network: <n> ratios: ..." at boot; with a host upscaler, "upscale: <name>,
   <in> to <out>" once. */
#include "patches.h"
#include "upscale.h"

/* ndsr_network_load(holder, ...): the network built, its ratio table logged */
PATCH_FUNC(0x8055ab00, word(0xd503237f), body(0xb525de25aa1cba9f), aapcs, none, ndsr_network_ratios, orig,
           as(x0, holder), as(x1, args)) {
    guest_orig(orig, holder, args);
    const uint32_t count = *(const uint32_t *)GP(holder + 0x24);
    const float *r = GP(holder);
    hos_log("ndsr: network: %u ratios: %.3f %.3f %.3f %.3f %.3f", count, r[0], count > 1 ? r[1] : 0.0f,
            count > 2 ? r[2] : 0.0f, count > 3 ? r[3] : 0.0f, count > 4 ? r[4] : 0.0f);
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
_Static_assert(__builtin_offsetof(ndsr_block, out_w) == 0x18 && __builtin_offsetof(ndsr_block, src_id) == 0x30 &&
                   __builtin_offsetof(ndsr_block, dst_id) == 0x40,
               "ndsr_stage_pass's block");

void f_80345f30(Ctx *);   /* (the network's evaluation, called by name) */

/* the network evaluation (net x0, cmd x1, the block x2): the game's, or the host's upscaler */
PATCH_AT(0x80345d84, word(0x9400006b), instead, none, ndsr_network_run, guest(0x80345f30), sp, x0, x1, x2) {   /* bl f_80345f30 */
    if (hos_upscaler_get() == HOS_UPSCALER_GAME) {
        guest_call(g, sp, CODE_PTR(f_80345f30), x0, x1, x2, 0, 0);
        return;
    }
    const ndsr_block *b = GP(x2);
    const hos_upscale u = {x1, b->src_id, b->in_w, b->in_h, b->dst_id, b->out_w, b->out_h, 0.0f};
    hos_upscale_record(&u);
}
