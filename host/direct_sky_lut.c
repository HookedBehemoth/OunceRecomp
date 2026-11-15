/* Game patch, direct mode only: the sky / ambient LUTs.

   What the game does: at boot (BootupBackgroundWorkThread) and at each scene load (SceneLowPriority), f_80846070(set,
   src, n0, src + n0, n1, src + n0 + n1, n2) memcpys the three parts of one file into the storage of the set's three
   RGBA16F textures: 256x64 (2D), 256x32x16 (3D) and 64x64 (2D), [set+0x238], [set+0x330], [set+0x428] (+ their pool's
   mapping). It is called for two sets (0x80845ecc, 0x80845efc), each only when n0 + n1 + n2 is the file's size. The file
   is res/Master_Field.skybin in Env/GameScene.Nin_Ounce_NVN2.genvb.zs (a SARC: the blob is the member's bytes, no header),
   the console's block-linear storage as it is. Found by this file's diagnosis (run m4e_cold_k: src 0x7fe31ba480 sits
   0x128480 into the decompressed archive, the member's offset) after texwatch named the writer.

   Why it is changed: the direct back end has no block-linear code and never reads texture storage, so the LUT pass read
   zeros (black) and the scene's ambient light followed. nx2/prepare_skybin.py writes the archive with the member linear
   (same sizes) into the prepared overlay (direct_assets_overlay(): work/prepared/terrain), which nn_fs.c serves in direct
   mode. Here (an At row, nx2/lift2/probes.py DIRECT), before each of the three memcpys (x0 the storage, x1 the part, x2 its size), the part goes to its
   texture's image (hsd_texels, a region of the whole level) and the memcpy copies nothing. Converted is told from not by
   where the archive was read: the patch uploads only when the overlay served it (direct_sky_lut_opened, from
   direct_assets_overlay_opened); otherwise the textures get the placeholder, counted. The part sizes are checked
   against the textures' linear sizes; a mismatch leaves the site alone, logged.
   Check that it applies: the 600-present line "direct patch sky lut" (uploads 6 at the boot, 6 more each scene load). */
#include "direct.h"
#include "../gpu/api/rx_nvn.h"
#include <string.h>
#include <windows.h>

int nvn_tex_live_at(uint64_t gpu, nvn_tex *out);
int nvn_fmt_bpb(int format, int *bw, int *bh);

static const char kFile[] = "Env/GameScene.Nin_Ounce_NVN2.genvb.zs";
static volatile LONG converted;   /* the archive was read from the prepared overlay */
static struct { volatile LONG64 uploads, bytes, unconverted, badsite, notex; } n;

void direct_sky_lut_opened(const char *path) {
    const size_t a = strlen(path), b = sizeof kFile - 1;
    if (a >= b && !strcmp(path + a - b, kFile) && (a == b || path[a - b - 1] == '/' || path[a - b - 1] == '\\')) {
        InterlockedExchange(&converted, 1);
        hos_log("direct patch sky lut: the converted archive from the overlay: %s", path);
    }
}

/* f_80846070: after each `x0 = the storage's CPU address` (the line before each of its three memcpys): x0 the storage,
   x1 the part, x2 its size. Returns the size the memcpy copies: 0 when the part went to the texture */
uint64_t direct_sky_lut_part(uint64_t dst, uint64_t src, uint64_t size) {
    if (!direct_assets_on()) return size;
    nvn_tex t;
    if (!dst || !nvn_tex_live_at(dst, &t)) {
        if (InterlockedIncrement64(&n.notex) <= 5)
            hos_log("direct patch sky lut: no texture at %#llx (%llu bytes from %#llx): left alone", (unsigned long long)dst,
                    (unsigned long long)size, (unsigned long long)src);
        return size;
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(t.d.format, &bw, &bh);
    const uint32_t d = t.d.target == 2 /* 3D */ ? (t.d.depth ? t.d.depth : 1) : 1;
    const uint64_t linear = (uint64_t)t.d.width * t.d.height * d * (uint64_t)bpb;
    if (bpb <= 0 || bw != 1 || bh != 1 || (t.d.target != 1 && t.d.target != 2) || size != linear ||
        !hos_guest_range(src, size)) {
        if (InterlockedIncrement64(&n.badsite) <= 5)
            hos_log("direct patch sky lut: texture %#llx (%ux%ux%u, format %#x, target %d) and a part of %llu bytes: left alone",
                    (unsigned long long)t.gpu, t.d.width, t.d.height, t.d.depth, t.d.format, t.d.target,
                    (unsigned long long)size);
        return size;
    }
    NVNcopyRegion r = {0, 0, 0, (int)t.d.width, (int)t.d.height, (int)d};
    if (converted) {
        rx_direct()->texels(HSD_TEXELS_FILE, &t, 0, 0, &r, GP(src), 0, 0);
        InterlockedIncrement64(&n.uploads);
        InterlockedAdd64(&n.bytes, (LONG64)size);
    } else {
        rx_direct()->texels(HSD_TEXELS_FILE, &t, 0, 0, &r, NULL, 0, 0);
        if (InterlockedIncrement64(&n.unconverted) <= 3)
            hos_log("direct patch sky lut: %s was not read from the prepared overlay (nx2/prepare_skybin.py): texture %#llx "
                    "is the placeholder", kFile, (unsigned long long)t.gpu);
    }
    return 0;   /* (the memcpy copies nothing: the storage stays unwritten) */
}

void direct_sky_lut_log(double frames) {
    (void)frames;
    hos_log("direct patch sky lut (f_80846070's memcpys of res/Master_Field.skybin -> uploads): %lld uploads (%.1f MB), "
            "%lld placeholders (file not converted), %lld sites left alone (%lld with no texture) since the start",
            (long long)n.uploads, n.bytes / 1048576.0, (long long)n.unconverted, (long long)n.badsite, (long long)n.notex);
}
