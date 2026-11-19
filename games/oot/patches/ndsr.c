/* Game patch: agl's NDSR upscaler runs as the game ships it; the host only logs each network's ratio table.

   The main model scene makes its NDSR stage (gsys::ModelSceneConfigParam's CreateNdsr: Main True, UI False;
   scene_stages_init, f_80ea3890): a graphics scene's +0x1d58 layer holds it at +0x2f8, and the game drives it with no
   check that it exists (f_80ea3470, f_80ea3380).

   The networks are of three kinds (f_80675290): kind 1 runs only above ratio 1.0, kinds 2 and 3 run at every ratio
   (f_80675040 sets the stage entry's network flag +0x18 for them), and the stage takes the kind its request asks for
   (f_80ea36f0, f_835187c0). Each network's ratio table (ndsr_network_build: out/in sizes of its layer configs, 16
   floats and a count at +0x40) gives the ratios the 3D scale is taken to. pass_smaa_output resolves SMAA into
   "ndsr::smaa_output" (RGB9E5F) at the render size, and the network (f_80271a60, f_81502210, f_815022c0, by kind) takes
   it to the output.

   Check that it applies: the line "ndsr: network <n> (kind <k>, byte ..): <r> ratios: ..." for each network at boot. */
#include "patches.h"

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
