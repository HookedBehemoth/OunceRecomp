/* The upscaler: a game's spatial upscale of its frame, recorded by the host in place of the game's own (OoT: agl's NDSR
   network, games/oot/patches/ndsr.c). The game hands over what its upscaler gets: the anti-aliased colour, the image to
   fill and their sizes; no depth, motion vectors or jitter. */
#pragma once
#include <stdint.h>

typedef struct hos_upscale {
    uint64_t cmd;                    /* the guest NVNcommandBuffer the game records the frame into */
    uint64_t src_tex;                /* the colour: a guest NVNtexture, its texture pool id, its size */
    uint32_t src_id, src_w, src_h;
    uint64_t dst_tex;                /* the image to fill: a guest NVNtexture, its image pool id, its size */
    uint32_t dst_id, dst_w, dst_h;
    float sharpness;                 /* the game's per-ratio parameter (OoT: the NDSR stage entry's +0xc) */
} hos_upscale;

/* records the upscale into u->cmd */
void hos_upscale_record(const hos_upscale *u);
