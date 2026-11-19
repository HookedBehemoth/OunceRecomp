/* The upscaler (upscale.h). None is written yet: the back end's list op for it follows the barrier rework, and the game
   patches keep the render ratio at 1.0, so nothing asks for one. */
#include "upscale.h"
#include "hos.h"

void hos_upscale_record(const hos_upscale *u) {
    hos_fatal(NULL, "upscale: %ux%u (texture %u) to %ux%u (image %u) asked for, and no upscaler is written", u->src_w,
              u->src_h, u->src_id, u->dst_w, u->dst_h, u->dst_id);
}
