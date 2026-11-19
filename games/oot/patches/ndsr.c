/* Game patch: agl's NDSR upscaler keeps its stage and its ratio tables; the host chooses the upscaler (runtime/upscale.h).

   The main model scene makes its NDSR stage (gsys::ModelSceneConfigParam's CreateNdsr: Main True, UI False;
   scene_stages_init, f_80ea3890): a graphics scene's +0x1d58 layer holds it at +0x2f8, and the game drives it with no
   check that it exists (f_80ea3470, f_80ea3380).

   The networks are of three kinds (f_80675290): kind 1 runs only above ratio 1.0, kinds 2 and 3 run at every ratio
   (f_80675040 sets the stage entry's network flag +0x18 for them), and the stage takes the kind its request asks for
   (f_80ea36f0, f_835187c0). Each network's ratio table (ndsr_network_build: out/in sizes of its layer configs, 16
   floats and a count at +0x40) gives the ratios the 3D scale is taken to. With the flag, pass_smaa_output resolves SMAA
   into "ndsr::smaa_output" (RGB9E5F) at the render size, and a network (f_80271a60, f_81502210, f_815022c0, by kind)
   takes it to the output.

   - The network flag, at its two readers (f_80271420, f_83518ac0), is whether the render size differs from the
     output's: at the same size SMAA writes the output itself, and no upscaler runs. (At ratio 1.0 the kind 2 or 3
     network would run anyway, a 1:1 pass over SMAA's output at about 1.3 ms a frame at 2560 x 1440.)
   - pass_smaa_output's three network calls run the game's network or the host's upscaler, by hos_upscaler_get() at
     each frame.
   - Above a 3D scale of 1.0 the view renders at the output's size times the scale (the stage's ratios stop at 1.0, and
     scene_resolution_update cuts the scale to the output's size): the host's upscaler takes it down. The render
     targets' pool grows for the scale at boot (games/oot/patches/oot_probes.c).

   Check that it applies: the line "ndsr: network <n> (kind <k>, byte ..): <r> ratios: ..." for each network at boot;
   with a host upscaler, "upscale: <name>, <in> to <out>" once. */
#include "patches.h"
#include "upscale.h"

#define NDSR_KINDS 0x83e7fcd8ull   /* (f_80675290: a network's kind, by its byte +0x997 >> 4) */

/* ndsr_network_build(ratios, ...): the network; its ratio table entry (x0) logged */
PATCH_FUNC(0x80f718a0, word(0xd503237f), body(0xc9996e5e09eb70cd), aapcs, x0, ndsr_network_ratios, orig,
           as(x0, ratios)) {
    const uint64_t net = guest_orig(orig, ratios);
    static int n;
    const uint8_t kb = *(const uint8_t *)GP(net + 0x997);
    const uint32_t count = *(const uint32_t *)GP(ratios + 0x40);
    const float *r = GP(ratios);
    hos_log("ndsr: network %d (kind %u, byte %#x): %u ratios: %.3f %.3f %.3f %.3f %.3f %.3f", n++,
            kb < 0x30 ? G32(NDSR_KINDS + (kb >> 4) * 4) : 99u, kb, count, r[0], count > 1 ? r[1] : 0.0f,
            count > 2 ? r[2] : 0.0f, count > 3 ? r[3] : 0.0f, count > 4 ? r[4] : 0.0f, count > 5 ? r[5] : 0.0f);
    return net;
}

/* pass_smaa_output's argument block of its network call (x2): the sizes, the colour's and the output's NVNtexture and
   pool ids; the stage entry's parameter at +0x48 (f_80271a60) or +0x4c (f_815022c0) */
typedef struct {
    uint64_t zero;
    uint32_t in_w, in_h;       /* (pass_smaa_output's x3) */
    uint32_t view_w, view_h;   /* (its x6; 0 on the scaled path) */
    uint32_t out_w, out_h;     /* (its x5) */
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

void f_80271a60(Ctx *), f_81502210(Ctx *), f_815022c0(Ctx *), f_80675040(Ctx *);   /* (the networks' evaluations, called by name) */

/* one network call: the game's (net x0, cmd x1, the block x2; s0 as the call has it), or the host's upscaler */
static void network_run(GuestCall *g, uint64_t sp, uint64_t fn, uint64_t net, uint64_t cmd, uint64_t block, float param) {
    const ndsr_block *b = GP(block);
    if (hos_upscaler_get() == HOS_UPSCALER_GAME) {
        if (b->in_w > b->out_w || b->in_h > b->out_h)   /* (the settings never pair them: hos_upscaler_allows) */
            hos_fatal(NULL, "ndsr: the game's network asked to take %ux%u down to %ux%u", b->in_w, b->in_h, b->out_w, b->out_h);
        guest_call(g, sp, fn, net, cmd, block, 0, 0);
        return;
    }
    const hos_upscale u = {cmd, b->src_id, b->in_w, b->in_h, b->dst_id, b->out_w, b->out_h, param};
    hos_upscale_record(&u);
}
PATCH_AT(0x8027176c, word(0x940000bd), instead, none, ndsr_network_run_0, guest(0x80271a60), sp, x0, x1, x2) {   /* bl f_80271a60 */
    network_run(g, sp, CODE_PTR(f_80271a60), x0, x1, x2, ((const ndsr_block *)GP(x2))->param_a);
}
PATCH_AT(0x80271968, word(0x944a422a), instead, none, ndsr_network_run_1, guest(0x81502210), sp, x0, x1, x2) {   /* bl f_81502210 */
    network_run(g, sp, CODE_PTR(f_81502210), x0, x1, x2, 0.0f);
}
PATCH_AT(0x80271a08, word(0x944a422e), instead, none, ndsr_network_run_2, guest(0x815022c0), sp, x0, x1, x2) {   /* bl f_815022c0 */
    network_run(g, sp, CODE_PTR(f_815022c0), x0, x1, x2, ((const ndsr_block *)GP(x2))->param_c);
}

/* the stage entry's network flag (+0x18) at its two readers: set when the render size (x3) is not the output's (x5) */
PATCH_AT(0x80271448, word(0x39406108), after, w8, ndsr_network_flag, as(x3, in), as(x5, out)) {   /* ldrb w8, [x8, #24] (f_80271420) */
    return in != out;
}
PATCH_AT_SITE(0x83518ad8, word(0x39406108), after, w8, ndsr_network_flag, x3, x5);   /* ldrb w8, [x8, #24] (f_83518ac0) */

/* f_80674e70's render size from the stage (ndsr_stage_ratio_update(stage, the full size x1, 8, the 3D scale s0, s1)):
   above 1.0 the full size times the scale. The stage's ratios are 1.0 and up, so it takes the frame no larger than the
   output by itself; the flag rows above then send the larger frame through the upscaler, which takes it down */
PATCH_AT(0x80674f60, word(0x94000038), instead, x0, ndsr_render_size, guest(0x80675040), sp, x0, x1, x2, s0, s1) {   /* bl ndsr_stage_ratio_update */
    const uint64_t size = guest_call(g, sp, CODE_PTR(f_80675040), x0, x1, x2, 0, 0);
    if (s0 <= 1.0f && s1 <= 1.0f) return size;
    const uint32_t w = (uint32_t)((float)(uint32_t)x1 * s0 + 0.5f), h = (uint32_t)((float)(uint32_t)(x1 >> 32) * s1 + 0.5f);
    return (uint64_t)w | (uint64_t)h << 32;
}

/* scene_resolution_update's per-view scale (+0x5328 of each view): the 3D scale (+0xe5c) kept, not cut to the one that
   renders at the output's size (fcsel s1 = min(scale, 1 / the view's base scale)) */
PATCH_AT(0x831f3fec, word(0x1e204c21), after, s1, scene_scale_above_output, as(ld(x20, 0xe5c), bits)) {   /* fcsel s1, s1, s0, mi */
    float scale;
    const uint32_t b = (uint32_t)bits;
    __builtin_memcpy(&scale, &b, 4);
    return scale;
}
