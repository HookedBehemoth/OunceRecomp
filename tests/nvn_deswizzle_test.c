/* host/nvn_layout.c's nvn_tex_level0_linear against a byte-by-byte block-linear address, for the shapes the game's
   patches deswizzle (the sky tables: 2D and 3D RGBA16F; terrain tiles: 260 x 260 arrays, BC and plain) and odd widths */
#include "../gpu/api/rx_nvn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t addr(uint64_t x, uint64_t y, uint64_t z, uint64_t gx, uint64_t by, unsigned th, unsigned td) {
    const uint64_t block = 512ull << (th + td);
    const uint64_t b = ((z >> td) * by + (y >> (3 + th))) * gx + (x >> 6);
    return b * block + (z & ((1u << td) - 1)) * (512ull << th) + ((y >> 3) & ((1u << th) - 1)) * 512 +
           ((x & 63) >> 5) * 256 + ((y & 7) >> 2) * 128 + ((x & 31) >> 4) * 64 + ((y & 3) >> 1) * 32 + (y & 1) * 16 + (x & 15);
}

static int check(const char *name, int target, int format, uint32_t w, uint32_t h, uint32_t d) {
    nvn_texdesc t;
    memset(&t, 0, sizeof t);
    t.target = target, t.format = format, t.width = w, t.height = h, t.depth = d, t.levels = 1;
    nvn_texlayout L;
    nvn_tex_layout(&t, &L);
    unsigned th, td;
    nvn_tex_level_tiles(&t, 0, &th, &td);
    int bw, bh;
    const int bpb = nvn_fmt_bpb(format, &bw, &bh);
    const uint64_t wb = (w + bw - 1) / bw * bpb, rows = (h + bh - 1) / bh, slices = target == 2 ? d : 1;
    const uint64_t gx = (wb + 63) / 64, by = ((rows + 7) / 8 + (1u << th) - 1) >> th;
    const uint64_t lin = wb * rows * slices * L.layers;
    uint8_t *src = malloc(L.size), *want = malloc(lin), *got = malloc(lin);
    for (uint64_t i = 0; i < L.size; i++) src[i] = (uint8_t)(i * 2654435761u >> 13);
    uint8_t *o = want;
    for (uint32_t l = 0; l < L.layers; l++)
        for (uint64_t z = 0; z < slices; z++)
            for (uint64_t y = 0; y < rows; y++)
                for (uint64_t x = 0; x < wb; x++) *o++ = src[l * L.layer_size + addr(x, y, z, gx, by, th, td)];
    const uint64_t n = nvn_tex_level0_linear(&t, src, L.size, got);
    const int ok = n == lin && !memcmp(got, want, lin) && !nvn_tex_level0_linear(&t, src, L.size - 1, got);
    printf("%s %s: %ux%ux%u format %#x, block height %u depth %u, %llu bytes -> %llu\n", ok ? "ok  " : "FAIL", name, w, h, d, format,
           th, td, (unsigned long long)L.size, (unsigned long long)n);
    free(src), free(want), free(got);
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= check("sky 2D", 1, 0x29 /* RGBA16F */, 256, 64, 1);
    ok &= check("sky 3D", 2, 0x29, 256, 32, 16);
    ok &= check("sky 2D small", 1, 0x29, 64, 64, 1);
    ok &= check("tile RGBA8 array", 4, 0x25, 260, 260, 3);
    ok &= check("tile R8", 1, 0x01, 260, 260, 1);
    ok &= check("tile BC1 array", 4, 0x42, 260, 260, 2);
    ok &= check("tile BC3", 1, 0x45, 260, 260, 1);
    ok &= check("odd width", 1, 0x0d, 37, 19, 1);
    return ok ? 0 : 1;
}
