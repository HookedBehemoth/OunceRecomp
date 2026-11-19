/* Game patch: agl's NDSR upscaler keeps its stage, renders at ratio 1.0, and its network passes go to the host's upscaler.

   The main model scene makes its NDSR stage (gsys::ModelSceneConfigParam's CreateNdsr: Main True, UI False;
   scene_stages_init, f_80ea3890): a graphics scene's +0x1d58 layer holds it at +0x2f8, and the game drives it with no
   check that it exists (f_80ea3470, f_80ea3380). The stage reads its networks' objects as well (their kind, +0x997,
   in f_80675040 and pass_smaa_output; their layer configs in f_80271a60), so agl_resource_init builds them as it does.

   The render ratio: each network's ratio table (ndsr_network_build: out/in sizes of its layer configs, 16 floats and a
   count at +0x40) is cut to the one ratio 1.0, so the scene renders at the output's size. The networks are of three
   kinds (f_80675290): kind 1 runs only above ratio 1.0, kinds 2 and 3 run at every ratio (f_80675040 sets the stage
   entry's network flag +0x18 for them), and the stage takes the kind its request asks for (f_80ea36f0, f_835187c0).

   The upscale: pass_smaa_output's three network evaluations (f_80271a60, f_81502210, f_815022c0, by network kind) are
   replaced by the host's upscaler (runtime/upscale.h), which gets the SMAA-resolved colour ("ndsr::smaa_output",
   RGB9E5F), the output image and their sizes. None is written yet: the network flag reads clear at its two readers
   (f_80271420, f_83518ac0), so the frame takes the game's own path without a network, SMAA (pass_smaa_edge) straight
   into the output. With an upscaler, those two rows go and the ratio table may keep ratios below 1.0.

   Check that it applies: the line "ndsr: network <n> (kind <k>, byte ..): <r> ratios, cut to 1.0" for each network at boot. */
#include "patches.h"
#include "upscale.h"

#define NDSR_KINDS 0x83e7fcd8ull   /* (f_80675290: a network's kind, by its byte +0x997 >> 4) */

/* ndsr_network_build(ratios, ...): the network; its ratio table entry (x0) cut to 1.0 */
PATCH_FUNC(0x80f718a0, word(0xd503237f), body(0xc9996e5e09eb70cd), aapcs, x0, ndsr_network_ratios, orig,
           as(x0, ratios)) {
    const uint64_t net = guest_orig(orig, ratios);
    static int n;
    const uint8_t kb = *(const uint8_t *)GP(net + 0x997);
    hos_log("ndsr: network %d (kind %u, byte %#x): %u ratios, cut to 1.0", n++, kb < 0x30 ? G32(NDSR_KINDS + (kb >> 4) * 4) : 99u,
            kb, *(const uint32_t *)GP(ratios + 0x40));
    *(float *)GP(ratios) = 1.0f;
    *(uint32_t *)GP(ratios + 0x40) = 1;
    return net;
}

/* pass_smaa_output's argument block of its network call (x2): the sizes, the colour's and the output's NVNtexture and
   pool ids; the stage entry's parameter at +0x48 (f_80271a60) or +0x4c (f_815022c0) */
typedef struct {
    uint64_t zero;
    uint32_t in_w, in_h;
    uint32_t view_w, view_h;   /* (pass_smaa_output's x6) */
    uint32_t out_w, out_h;
    uint32_t kind, kind_b;
    uint64_t src_tex;
    uint32_t src_id, pad0;
    uint64_t dst_tex;
    uint32_t dst_id, pad1;
    float param_a, param_c;
} ndsr_block;
_Static_assert(__builtin_offsetof(ndsr_block, in_w) == 0x08 && __builtin_offsetof(ndsr_block, out_w) == 0x18 &&
                   __builtin_offsetof(ndsr_block, src_tex) == 0x28 && __builtin_offsetof(ndsr_block, src_id) == 0x30 &&
                   __builtin_offsetof(ndsr_block, dst_tex) == 0x38 && __builtin_offsetof(ndsr_block, dst_id) == 0x40 &&
                   __builtin_offsetof(ndsr_block, param_a) == 0x48 && __builtin_offsetof(ndsr_block, param_c) == 0x4c,
               "pass_smaa_output's block");

/* site: 0 f_80271a60, 1 f_81502210, 2 f_815022c0; cmd x1, the block x2 */
PATCH_AT(0x8027176c, word(0x940000bd), instead, none, ndsr_network_run, as(k(0), site), as(x1, cmd), as(x2, block)) {   /* bl f_80271a60 */
    const ndsr_block *b = GP(block);
    const hos_upscale u = {cmd, b->src_tex, b->src_id, b->in_w, b->in_h, b->dst_tex, b->dst_id, b->out_w, b->out_h,
                           site == 0 ? b->param_a : site == 2 ? b->param_c : 0.0f};
    hos_upscale_record(&u);
}
PATCH_AT_SITE(0x80271968, word(0x944a422a), instead, none, ndsr_network_run, k(1), x1, x2);   /* bl f_81502210 */
PATCH_AT_SITE(0x80271a08, word(0x944a422e), instead, none, ndsr_network_run, k(2), x1, x2);   /* bl f_815022c0 */

/* the stage entry's network flag (+0x18) at its two readers: clear, SMAA into the output without a network */
PATCH_AT(0x80271448, word(0x39406108), after, w8, ndsr_network_flag) {   /* ldrb w8, [x8, #24] (f_80271420) */
    return 0;
}
PATCH_AT_SITE(0x83518ad8, word(0x39406108), after, w8, ndsr_network_flag);   /* ldrb w8, [x8, #24] (f_83518ac0) */
