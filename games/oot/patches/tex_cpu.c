/* Texels the game's CPU writes into texture storage, handed to the back end at the writer (gpu_texels, GPU_TEXELS_CPU).
   The back end never reads texture storage, so each such writer is patched here; debug.texwatch finds the writers.

   - Two textures made by dta_alloc_b and cleared by the CPU right after: 1x1 NVN format 0x2e targets (f_80c553b0's
     memset at 0x80c558bc) and 1x1 format 0x25 ones (f_8093e7c0's memset at 0x8093ea3c, the tera manager's). The
     memset becomes the same fill, uploaded; the storage is not written.
   - texdata_cpu_lut: a 64x64 format 0x5 LUT the CPU computes into its texture's storage: uploaded from there before its
     copy_out.
   A write site whose destination is not a live texture's whole storage is an error (nvn_texels_fill). */
#include "hos.h"
#include "patches.h"
#include "rx_nvn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nvn_tex_live_at(uint64_t gpu, nvn_tex *out);
static bl_stats st;

PATCH_AT(0x80c558bc, word(0x94c032a9), instead, none, tex_cpu_clear_rgba32f, x0, x1, x2) { nvn_texels_fill("f_80c553b0", x0, (int)x1, x2); }
PATCH_AT(0x8093ea3c, word(0x94cc8e49), instead, none, tex_cpu_clear_tera, x0, x1, x2) { nvn_texels_fill("f_8093e7c0", x0, (int)x1, x2); }

/* texdata_cpu_lut, before its texdata_cpu_lut_copy_out(lut, src, 0): src is where the CPU wrote the LUT. When that is the
   texture's own storage (copy_out then only flushes), its bytes are uploaded; another buffer is copied by WriteTexels */
PATCH_AT(0x809e6abc, word(0x94000179), before, none, tex_cpu_lut, as(x1, src)) {
    nvn_tex t;
    if (!rx_direct() || !src || !nvn_tex_live_at(src, &t)) return;
    if (!hos_guest_range(src, t.L.size) || !nvn_texels_storage(GPU_TEXELS_CPU, &t, (const uint8_t *)GP(src), t.L.size, &st))
        nvn_texels_not_handed("texdata_cpu_lut", src, t.L.size, &t);
}
