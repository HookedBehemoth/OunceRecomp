/* Game patch, direct mode only: the sky / ambient LUTs.

   What the game does: at boot (BootupBackgroundWorkThread) and at each scene load (SceneLowPriority), f_80845d80 calls
   f_80846070(set, src, n0, src + n0, n1, src + n0 + n1, n2) for the two LUT sets f_80ddab00 made ([+0xc98], [+0xca0]),
   each only when n0 + n1 + n2 is the file's size. It memcpys the three parts of the file into the storage of the set's
   three RGBA16F textures: 256x64 (2D), 256x32x16 (3D) and 64x64 (2D), [set+0x238], [set+0x330], [set+0x428]. The file is
   res/Master_Field.skybin in Env/GameScene.Nin_Ounce_NVN2.genvb.zs (a SARC member, no header): the console's
   block-linear storage as it is. f_80dde4f0, the sets' setup, also calls f_80846070 when its argument block names a
   blob, but both of f_80ddab00's calls pass none (the block's +0x48 and +0x50 are stored zero).

   Why it is changed: the direct back end never reads texture storage, so the LUT pass read zeros (black) and the
   scene's ambient light followed. Here (an At row, a PATCH_AT declaration, runtime/patch.h), before each of the three
   memcpys (x0 the storage, x1 the part, x2 its size), the part's level 0 is deswizzled and goes to its texture's image,
   and the memcpy copies nothing. A part that is not its texture's storage size leaves the site alone, logged.
   Check that it applies: the 600-present line "direct patch sky lut" (uploads 6 at the boot, 6 more each scene load). */
#include "patches.h"
#include "rx_nvn.h"
#include "os.h"

int nvn_tex_live_at(uint64_t gpu, nvn_tex *out);

static bl_stats st;
static struct { volatile int64_t badsite, notex; } n;

/* f_80846070: after each `x0 = the storage's CPU address` (the line before each of its three memcpys): x0 the storage,
   x1 the part, x2 its size. Returns the size the memcpy copies: 0 when the part went to the texture */
PATCH_AT(0x808460e4, word(0x9a8803e0), after, x2, sky_lut_part, as(x0, dst), as(x1, src), as(x2, size)) {
    if (!assets_on()) return size;
    nvn_tex t;
    if (!dst || !nvn_tex_live_at(dst, &t)) {
        if (os_atomic_inc(&n.notex) <= 5)
            hos_log("direct patch sky lut: no texture at %#llx (%llu bytes from %#llx): left alone", (unsigned long long)dst,
                    (unsigned long long)size, (unsigned long long)src);
        return size;
    }
    if (!hos_guest_range(src, size) || !nvn_texels_storage(GPU_TEXELS_FILE, &t, GP(src), size, &st)) {
        if (os_atomic_inc(&n.badsite) <= 5)
            hos_log("direct patch sky lut: texture %#llx (%ux%ux%u, format %#x, target %d, storage %llu bytes) and a part of %llu "
                    "bytes: left alone", (unsigned long long)t.gpu, t.d.width, t.d.height, t.d.depth, t.d.format, t.d.target,
                    (unsigned long long)t.L.size, (unsigned long long)size);
        return size;
    }
    return 0;   /* (the memcpy copies nothing: the storage stays unwritten) */
}
PATCH_AT_SITE(0x80846114, word(0x9a8803e0), after, x2, sky_lut_part, x0, x1, x2);
PATCH_AT_SITE(0x80846144, word(0x9a8803e0), after, x2, sky_lut_part, x0, x1, x2);

void sky_lut_log(double frames) {
    (void)frames;
    hos_log("direct patch sky lut (f_80846070's memcpys of res/Master_Field.skybin -> uploads): %lld deswizzled (%.1f MB, "
            "%.3f ms, %.3f ms the longest), %lld sites left alone (%lld with no texture) since the start", (long long)st.uploads,
            st.bytes / 1048576.0, st.ns / 1e6, st.max_ns / 1e6, (long long)n.badsite, (long long)n.notex);
}
