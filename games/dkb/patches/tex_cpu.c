/* Texels the game's CPU writes into texture storage, handed to the back end at the writer (gpu_texels, GPU_TEXELS_CPU).
   The back end never reads texture storage, so each such writer is patched here; debug.texwatch finds the writers (the
   glyph cache's are assets.c's).

   - Textures made zeroed by the CPU: gpu_tex_storage_zeroed_make (a 64x64x18 cube array, NVN format 0x3f, 5 levels) and
     gpu_tex_storage_zeroed_make2 (1x1 format 0x2e targets), and f_80824864's 1x1 format 0x5 one. Each memset becomes
     the same fill, uploaded (nvn_texels_fill); the storage is not written. */
#include "hos.h"
#include "patches.h"

PATCH_AT(0x806c7090, word(0x94535028), instead, none, tex_cpu_zeroed, x0, x1, x2) {
    nvn_texels_fill("gpu_tex_storage_zeroed_make", x0, (int)x1, x2);
}
PATCH_AT(0x804dec0c, word(0x945af149), instead, none, tex_cpu_zeroed2, x0, x1, x2) {
    nvn_texels_fill("gpu_tex_storage_zeroed_make2", x0, (int)x1, x2);
}
PATCH_AT(0x80824abc, word(0x944dd99d), instead, x0, tex_cpu_fill_1x1, x0, x1, x2) {
    nvn_texels_fill("f_80824864", x0, (int)x1, x2);
    return x0;   /* (memset's result) */
}
