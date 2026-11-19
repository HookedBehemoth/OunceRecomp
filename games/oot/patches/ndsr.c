/* Game patch: agl's NDSR upscaler keeps its stage and the one network OoT runs; the host chooses the upscaler
   (runtime/upscale.h).

   The main model scene makes its NDSR stage (gsys::ModelSceneConfigParam's CreateNdsr: Main True, UI False;
   scene_stages_init, f_80ea3890): a graphics scene's +0x1d58 layer holds it at +0x2f8, and the game drives it with no
   check that it exists (f_80ea3470, f_80ea3380).

   agl's resource set-up builds a network from each of three packs it finds (ndsr_networks_load), into the next free
   slot: agl_ndsr.pck (kind 1, fixed ratios, "two step" upscale), agl_ndsr_vr.pck (kind 2) and agl_ndsr_vr_light.pck
   (kind 3). "vr" is a variable ratio, not a stereo view: the layer's parameters (f_80cf8cc0) name the kind 2 ratio
   "vr_upscale_ratio", "倍率 (dynamic ratio)", beside "vr_enable_dithering" and "vr_sharpening_amount". The stage takes
   the kind its request asks for (ndsr_stage_pick_network: 1 kind 2, 2 kind 3, else kind 1), and pass_smaa_output
   evaluates only that network. OoT's request is a constant: the gfx set-up (gfx_scene_ext_setup) writes request 2 and
   ratio 12/7 over the module's defaults (1, 1.5) before the first frame, whatever the mode, so the stage always runs
   the vr_light network. The other two packs are left out here (the lookup answers "not there"), and with them their
   compute programs (game.toml shaders_left_out); vr_light lands in slot 0.

   A kind 3 network takes the render size from the 3D scale at any ratio (ndsr_stage_ratio_update through
   ndsr_network_out_size), so every render scale up to 1.0 has the game's network. With the stage entry's network flag
   pass_smaa_output resolves SMAA into "ndsr::smaa_output" (RGB9E5F) at the render size, and the network
   (ndsr_network_run_kind3) takes it to the output.

   - The network flag, at its two readers (f_80271420, f_83518ac0), is whether the render size differs from the
     output's: at the same size SMAA writes the output itself, and no upscaler runs. (At ratio 1.0 the network would
     run anyway, a 1:1 pass over SMAA's output at about 1.3 ms a frame at 2560 x 1440.)
   - pass_smaa_output's network call runs the game's network or the host's upscaler, by hos_upscaler_get() at each
     frame.
   - Above a 3D scale of 1.0 the view renders at the output's size times the scale (the stage's ratios stop at 1.0, and
     scene_resolution_update cuts the scale to the output's size): the host's upscaler takes it down. The render
     targets' pool grows for the scale at boot (games/oot/patches/oot_probes.c).

   Check that it applies: one line "ndsr: network 0 (kind 3, byte 0x12): 7 ratios: ..." at boot; with a host upscaler,
   "upscale: <name>, <in> to <out>" once. */
#include "patches.h"
#include "upscale.h"
#include <string.h>

#define NDSR_KINDS 0x83e7fcd8ull   /* (f_80675290: a network's kind, by its byte +0x997 >> 4) */

/* ndsr_networks_load's lookups of agl_ndsr.pck and agl_ndsr_vr.pck in the agl archive (its vtable +0x48, the name's
   string at x1): -1, not there, so neither network is built */
PATCH_AT(0x80f713f8, word(0xd63f0100), instead, x0, ndsr_pack_left_out, as(x1, name_ref)) {   /* blr x8 */
    const char *name = GP(G64(name_ref));
    if (strcmp(name, "agl_ndsr.pck") && strcmp(name, "agl_ndsr_vr.pck"))
        hos_fatal(NULL, "ndsr: ndsr_networks_load looks up %s where a left-out pack was", name);
    return (uint64_t)-1;
}
PATCH_AT_SITE(0x80f71460, word(0xd63f0100), instead, x0, ndsr_pack_left_out, x1);   /* blr x8 */

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
   pool ids; the stage entry's parameter at +0x48 (ndsr_network_run_kind3; +0x4c is kind 2's) */
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

void f_80271a60(Ctx *), f_80675040(Ctx *);   /* (called by name) */

/* pass_smaa_output's kind 3 network call (net x0, cmd x1, the block x2): the game's network or the host's upscaler.
   (Its kind 1 and kind 2 calls have no network to run: those packs are left out.) */
PATCH_AT(0x8027176c, word(0x940000bd), instead, none, ndsr_network_run, guest(0x80271a60), sp, as(x0, net), as(x1, cmd),
         as(x2, block)) {   /* bl ndsr_network_run_kind3 */
    const ndsr_block *b = GP(block);
    if (hos_upscaler_get() == HOS_UPSCALER_GAME) {
        if (b->in_w > b->out_w || b->in_h > b->out_h)   /* (the settings never pair them: hos_upscaler_allows) */
            hos_fatal(NULL, "ndsr: the game's network asked to take %ux%u down to %ux%u", b->in_w, b->in_h, b->out_w, b->out_h);
        guest_call(g, sp, CODE_PTR(f_80271a60), net, cmd, block, 0, 0);
        return;
    }
    const hos_upscale u = {cmd, b->src_id, b->in_w, b->in_h, b->dst_id, b->out_w, b->out_h, b->param_a};
    hos_upscale_record(&u);
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
