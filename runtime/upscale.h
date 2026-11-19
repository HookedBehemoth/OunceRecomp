/* The upscaler: which one takes the game's frame from its render size to the output (OoT and DKB: agl's NDSR stage,
   games/<game>/patches/ndsr.c), chosen while the game runs. The game's own network is one choice; the others are the
   host's, recorded into the game's command buffer in the network's place. The game hands over what its network gets:
   the anti-aliased colour, the image to fill and their sizes; no depth, motion vectors or jitter. At ratio 1.0 no
   upscaler runs: the game's SMAA writes the output itself. Above a 3D scale of 1.0 the host's filters take the frame
   down to the output. */
#pragma once
#include <stdint.h>

typedef enum hos_upscaler {
    HOS_UPSCALER_GAME,          /* the game's NDSR network */
    HOS_UPSCALER_BILINEAR,
    HOS_UPSCALER_CATMULL_ROM,   /* bicubic, clamped to the nearest texels' range */
    HOS_UPSCALER_FSR3,          /* (not written: hos_upscaler_set refuses these; temporal, see docs/direct/render-targets.md) */
    HOS_UPSCALER_XESS,
    HOS_UPSCALER_DLSS,
    HOS_UPSCALER_KINDS,
} hos_upscaler;

/* the choice, any thread, from the next frame on; 0: not an upscaler this build has (the choice stays) */
int hos_upscaler_set(hos_upscaler kind);
hos_upscaler hos_upscaler_get(void);
/* whether kind can take the frame to the output at this 3D scale: the game's network upscales only (scale <= 1.0); the
   settings offer only what it allows, and a game patch meeting another pairing stops the game */
int hos_upscaler_allows(hos_upscaler kind, float scale);
const char *hos_upscaler_name(hos_upscaler kind);

typedef struct hos_upscale {
    uint64_t cmd;                    /* the guest NVNcommandBuffer the game records the frame into */
    uint32_t src_id, src_w, src_h;   /* the colour: its texture pool id, its size */
    uint32_t dst_id, dst_w, dst_h;   /* the image to fill: its image pool id, its size */
    float sharpness;                 /* the game's per-ratio parameter (OoT: the NDSR stage entry's +0xc) */
} hos_upscale;

/* the host's upscaler (hos_upscaler_get() is not HOS_UPSCALER_GAME) records the upscale into u->cmd */
void hos_upscale_record(const hos_upscale *u);
