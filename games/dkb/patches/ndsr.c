/* Game patch: the game renders without agl's NDSR upscaler, and its network is never built.

   The model scene's resolved config holds CreateNdsr at +0xe10 (the node at +0xdf8, its value +0x18, as
   gsys::ModelSceneConfigParam's in OoT). Three places read it: the scene's pipeline set-up (f_804c4258 at 0x804c4578),
   which makes the NDSR stage when it is set; that stage's teardown (f_806bf9d4 at 0x806bf9fc); and dynres_setup
   (f_80499f5c at 0x8049a13c), which marks the frame as upscaled. A row after each load makes it read False, the setting
   of a scene the game makes without the stage.

   agl's resource set-up (f_8055a8fc) makes the network holder (resource +0x78, zeroed: no network, a ratio table of 0
   entries) and then builds agl_ndsr.pck's network into it (f_8055ab00, its call at 0x8055aab4). The call is not made:
   the holder stays empty, which the dynamic resolution controller's readers of the table (f_803adb98, f_803ae0f0) take
   as no upscale ratios. */
#include "patches.h"

PATCH_AT(0x804c4578, word(0x39784109), after, none, ndsr_off_setup, io(w9)) {   /* ldrb w9, [x8, #0xe10] */
    io->w9 = 0;
}
PATCH_AT(0x806bf9fc, word(0x39784108), after, none, ndsr_off_teardown, io(w8)) {   /* ldrb w8, [x8, #0xe10] */
    io->w8 = 0;
}
PATCH_AT(0x8049a13c, word(0x39784329), after, none, ndsr_off_dynres, io(w9)) {   /* ldrb w9, [x25, #0xe10] */
    io->w9 = 0;
}

PATCH_AT(0x8055aab4, word(0x94000013), instead, none, ndsr_network_build) {   /* bl f_8055ab00 */
}
