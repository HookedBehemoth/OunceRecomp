/* Texels a game patch hands to the back end from bytes the CPU made in a texture's storage layout (terrain tiles, the sky
   tables, code-made default textures): level 0 made packed rows (a pitch-linear texture's rows as they are) and uploaded
   to the texture's image (gpu_texels). */
#pragma once
#include <stdint.h>

typedef struct { volatile int64_t uploads, bytes, ns, max_ns; } bl_stats;
/* tex: an nvn_tex; src: its whole storage's bytes (n, the storage's size); source: GPU_TEXELS_*; timed into st.
   0: n is not the storage's size or the layout is not one to deswizzle (nothing uploaded) */
int nvn_texels_storage(uint32_t source, const void *tex, const uint8_t *src, uint64_t n, bl_stats *st);
/* (agl's TextureInfo path, both games' assets.c) a texture the game's code made, its whole storage's bytes at guest src:
   uploaded to its image (GPU_TEXELS_CPU) after its load initialized it. One it cannot hand over (no data, more than one
   level, a layout not deswizzled) is a logged error and a strict event */
void nvn_texels_cpu_made(const void *tex, uint64_t src);
/* (game patches at a CPU fill of a texture's storage: memset(dst, c, n)) the fill uploaded to the texture's image instead
   of written; dst not a live texture's whole storage: an error (nvn_texels_not_handed) and the fill done in memory */
void nvn_texels_fill(const char *site, uint64_t dst, int c, uint64_t n);
/* a CPU texel write a patch could not hand over (tex: the texture there, or NULL): a logged error and a strict event */
void nvn_texels_not_handed(const char *site, uint64_t dst, uint64_t n, const void *tex);
