/* Game patch: the game renders without agl's NDSR upscaler, and its networks are never built.

   gsys::ModelSceneConfigParam's CreateNdsr (Main: True, UI: False in romfs gsys/ModelSceneConfigParam) decides whether a
   model scene makes its NDSR stage (f_80ec3c70 at 0x80ec3f28: the resolved config's byte +0xf20). Its getter
   f_811ef5b0 (the resource's value +0x588, inherited through +0x10 when +0x618 is clear) feeds that byte: here it reads
   False for every scene, the UI scene's setting, so the main scene takes the same path.

   agl's resource set-up (f_80f71360) looks up agl_ndsr.pck, agl_ndsr_vr.pck and agl_ndsr_vr_light.pck in the agl
   resource archive and builds a network from each it finds; a name the archive lacks (-1) leaves that network out. An
   instead row on each lookup answers -1, so no network is built and no pool is made for one. */
#include "patches.h"
#include <string.h>

PATCH_FUNC(0x811ef5b0, word(0x39586008), body(0xa076490c8db65903), aapcs, x0, ndsr_create_param, orig, as(x0, param)) {
    const uint64_t value = guest_orig(orig, param);
    *(uint8_t *)GP(value) = 0;
    return value;
}

/* (the lookup's name: x1 the address of a slot holding it) */
PATCH_AT(0x80f713f8, word(0xd63f0100), instead, w0, ndsr_network_lookup, as(x1, name_slot)) {   /* blr x8 */
    const char *name = GP(*(const uint64_t *)GP(name_slot));
    if (strcmp(name, "agl_ndsr.pck") && strcmp(name, "agl_ndsr_vr.pck") && strcmp(name, "agl_ndsr_vr_light.pck"))
        hos_fatal(NULL, "ndsr: f_80f71360 looks up %.40s, not an NDSR network", name);
    return 0xffffffffu;
}
PATCH_AT_SITE(0x80f71460, word(0xd63f0100), instead, w0, ndsr_network_lookup, x1);   /* blr x8 */
PATCH_AT_SITE(0x80f714d0, word(0xd63f0100), instead, w0, ndsr_network_lookup, x1);   /* blr x8 */
