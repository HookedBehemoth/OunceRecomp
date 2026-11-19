/* Game patch: agl's NDSR upscaler runs as the game ships it; the host only logs its network's ratio table.

   The model scene makes its NDSR stage when its resolved config's CreateNdsr (+0xe10) is set (scene_stages_init at
   0x804c4578: a 0x20-byte stage at +0x2b0, its network holder at +0x18). The game reads that stage with no check that it
   exists (ndsr_render_scale, f_80442ccc, when the config's +0x930 is set), and agl's resource set-up builds
   agl_ndsr.pck's network into the holder (ndsr_network_load).

   The holder's ratio table (ndsr_network_load: 1.0, then the network's ratios; up to 10 floats at +0, their count at
   +0x24) gives the stage's ratio index (+0x10). At index 0 the pass (ndsr_stage_pass, f_80345a80) resolves SMAA
   (f_81625678) straight into the output; above it, into "ndsr::smaa_output" at the render size, and the network
   (f_80345f30) takes it to the output.

   Check that it applies: the line "ndsr: network: <n> ratios: ..." at boot. */
#include "patches.h"

/* ndsr_network_load(holder, ...): the network built, its ratio table logged */
PATCH_FUNC(0x8055ab00, word(0xd503237f), body(0xb525de25aa1cba9f), aapcs, none, ndsr_network_ratios, orig,
           as(x0, holder), as(x1, args)) {
    guest_orig(orig, holder, args);
    const uint32_t count = *(const uint32_t *)GP(holder + 0x24);
    const float *r = GP(holder);
    hos_log("ndsr: network: %u ratios: %.3f %.3f %.3f %.3f %.3f", count, r[0], count > 1 ? r[1] : 0.0f,
            count > 2 ? r[2] : 0.0f, count > 3 ? r[3] : 0.0f, count > 4 ? r[4] : 0.0f);
}
