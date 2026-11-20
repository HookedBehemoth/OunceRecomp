/* The host's side of prepared assets for the direct back end, direct mode only (debug.noassets turns every patch
   here off, for a before/after).

   The store: prepared=<folder> (default the game folder's prepared/, which every ounce build makes): <folder>/tex is
   the texture store the back end maps (gpu_assets_open).

   Texture loads (agl::TextureData), each tied to its BNTX record and handed to the back end (gpu_asset_load) before the
   texture's nvnTextureInitialize, which then makes the image from the store:
     - loadImage_ (td, ctx, src) f_80210d50: the texture by its name, descriptor and place in its BNTX; the NVN texture
       is [td+0xd8]+0x10. The copy into the texture heap is skipped (x2 = 0: no memcpy or flush; the BRTI's init
       f_80210e90 still runs): the back end never reads texture storage.
     - rebind_ (td, base, res, ctx) f_815ec36c (compaction's move, Replace's swap): the object is initialized again from
       res: the same record.
     - the compaction step f_815f0820 (worker): its movers (f_815efb08, f_815efd34, f_815f064c) copy a moved texture's
       bytes only when byte [worker+0x1c11] is 1; it is cleared at every step, so compaction moves no texels.
   In-place files (a BNTX kept whole in memory): ui2d's layout textures (f_8088e790) and a font's BNTX (f_808aa478) note
   their file for this thread; at TextureImpl::Initialize f_80210fdc inside them, the texture is found in the store by
   its name, index and mip 0's offset in that file.
   The font glyph cache: each glyph the TextureCache plots (f_80767b20) goes to the atlas image as a region upload
   (gpu_texels) of its cell. */
#include "hos.h"
#include "patches.h"
#include "nvn_claims.h"
#include "rx_nvn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

int hos_gpu_pools(uint64_t (*out)[2], int max);
int nvn_tex_live_at(uint64_t gpu, nvn_tex *out);
extern const char *hos_romfs_dir;

static volatile int64_t n_loads, n_load_skipped, n_load_bytes, n_rebind, n_compaction, n_compaction_cleared,
    n_inplace, n_inplace_bad, n_glyphs,
    n_glyph_notex;


static int on_ = -1;
int assets_on(void) {
    if (on_ < 0) on_ = !setting_bool(SET_DBG_NOASSETS);
    return on_;
}

/* ---- the prepared folder ---- */
static char prepared[1024];
static const char *prepared_dir(void) {
    const char *e = setting_str(SET_PREPARED);
    if (*e) snprintf(prepared, sizeof prepared, "%s", e);
    else {   /* (the game folder's prepared/: ounce build makes it) */
        char exe[1024];
        const int n = os_exe_path(exe, sizeof exe);
        char *slash = n ? strrchr(exe, '/') : NULL;
        if (slash) *slash = 0;
        snprintf(prepared, sizeof prepared, "%s/prepared", slash ? exe : ".");
    }
    char real[1024];
    if (realpath(prepared, real)) snprintf(prepared, sizeof prepared, "%s", real);
    hos_log("assets: the prepared store: %s (prepared)", prepared);
    return prepared;
}

/* ---- the 600-present line ---- */
void assets_log(void) {
    char b[512];
    texdata_line(b, sizeof b);
    hos_log("assets: copies skipped: loads %lld of %lld (%lld MB), compaction steps %lld (copy flag "
            "cleared %lld); rebinds %lld; in-place textures %lld (not their file's %lld); glyph cells %lld (%lld with no "
            "texture)%s; %s", (long long)n_load_skipped,
            (long long)n_loads, (long long)(n_load_bytes >> 20),
            (long long)n_compaction, (long long)n_compaction_cleared,
            (long long)n_rebind, (long long)n_inplace, (long long)n_inplace_bad, (long long)n_glyphs, (long long)n_glyph_notex,
            assets_on() ? "" : " (debug.noassets= the patches are off)", b);
}

static volatile int32_t started;
static os_mutex start_lock = OS_MUTEX_INIT;
static int ready(void) {   /* (with the patches on, the store is open) */
    if (!assets_on()) return 0;
    if (!os_atomic_load(&started)) {   /* (once, and the others wait until it is done) */
        os_mutex_lock(&start_lock);
        if (!started) {
            char tex[1100];
            snprintf(tex, sizeof tex, "%s/tex", prepared_dir());
            rx_direct()->assets_open(tex);
            os_atomic_store(&started, 1);
        }
        os_mutex_unlock(&start_lock);
    }
    return 1;
}

/* the texture heap's pool, named to the asset path once (by the first load's slot) */
static void find_heap(uint64_t slot) {
    static volatile int32_t done;
    if (done || os_atomic_xchg(&done, 1)) return;
    uint64_t pools[1024][2];
    const int n = hos_gpu_pools(pools, 1024);
    for (int i = 0; i < n; i++)
        if (slot >= pools[i][0] && slot < pools[i][0] + pools[i][1]) {
            hos_log("assets: the texture heap: pool %#llx + %llu MB", (unsigned long long)pools[i][0],
                    (unsigned long long)(pools[i][1] >> 20));
            rx_direct()->asset_pool(pools[i][0], pools[i][0] + pools[i][1]);
            return;
        }
    hos_log("assets: the texture heap's pool not found (slot %#llx)", (unsigned long long)slot);
}

/* ---- BNTX helpers ---- */
static const char *bstr(uint64_t p, uint32_t *len) {
    if (!p || !hos_guest_range(p, 2)) return NULL;
    const uint16_t n = *(const uint16_t *)GP(p);
    if (!hos_guest_range(p + 2, n)) return NULL;
    *len = n;
    return (const char *)GP(p + 2);
}
static void key_of_brti(gpu_asset_key *k, uint64_t brti) {
    k->format = G32(brti + 0x1c), k->width = G32(brti + 0x24), k->height = G32(brti + 0x28), k->layers = G32(brti + 0x30);
    k->mips = *(const uint16_t *)GP(brti + 0x16), k->image_size = G32(brti + 0x50);
}

/* texdata_from_texture_info (agl::TextureDataMgr's TextureData from a TextureInfo): the game's code made the texture and
   its texels (DummyTexNotFound): its load is GPU_ASSET_CPU, and the texels go to the image after
   the load initialized the texture (cpu_texels) */
static _Thread_local int tl_cpu_made;
PATCH_FUNC(0x80576180, word(0xd503237f), body(0xab612c338ebd7a6c), aapcs, x0, assets_texture_info, orig) {
    tl_cpu_made++;
    const uint64_t r = guest_orig(orig);
    tl_cpu_made--;
    return r;
}
/* 1: the back end has the texture's texels; its src is not copied into the heap */
static int load_image(uint64_t td, uint64_t src) {
    if (!ready() || !hos_guest_range(td, 0x100)) return 0;
    const uint64_t brti = td + 0x58, gfx = G64(td + 0xd8);
    if (hos_guest_range(G64(td + 0x18), 8)) find_heap(G64(td + 0x18) + 0x10);
    if (!gfx || G32(brti) != 0x49545242u) return 0;   /* ('BRTI') */
    gpu_asset_key k;
    memset(&k, 0, sizeof k);
    k.tex = GP(gfx + 0x10);
    k.path = tl_cpu_made ? GPU_ASSET_CPU : GPU_ASSET_LOAD;
    key_of_brti(&k, brti);
    /* (its BNTX: the store's records of its name and descriptor say where mip 0 is in their file; the one whose file
       header is that far back from src is the one: gpu_asset_load) */
    uint32_t nl = 0;
    const char *nm = bstr(G64(brti + 0x60), &nl);
    if (nm) k.name = nm, k.name_len = nl;
    k.data = src && hos_guest_range(src, 1) ? GP(src) : NULL;
    rx_direct()->asset_load(&k);
    texdata_created(td);
    os_atomic_inc(&n_loads);
    if (!src) return 0;
    os_atomic_inc(&n_load_skipped);
    os_atomic_add(&n_load_bytes, G32(td + 0xa8));
    return 1;
}
/* TextureData::loadImage_(td, ctx, src): the game's load (orig), without src when the back end takes the texels (from
   the store, or for a texture the game's code made from src after the load: nvn_texels_cpu_made) */
PATCH_FUNC(0x80210d50, word(0xd503237f), body(0x9cc63809f6c28c83), aapcs, x0, assets_load_image,
           orig, as(x0, td), as(x1, ctx), as(x2, src)) {
    const uint64_t r = guest_orig(orig, td, ctx, load_image(td, src) ? 0 : src);   /* (x2 changed: x0-x2 passed) */
    if (tl_cpu_made && assets_on() && hos_guest_range(td, 0x100) && G64(td + 0xd8)) nvn_texels_cpu_made(GP(G64(td + 0xd8) + 0x10), src);
    return r;
}

/* TextureData::rebind_(td, base, res, ctx) */
PATCH_AT(0x815ec36c, word(0xd503237f), before, none, assets_rebind, as(x0, td), as(x2, res)) {
    if (!ready() || !hos_guest_range(td, 0x100)) return;
    const uint64_t gfx = G64(td + 0xd8);
    os_atomic_inc(&n_rebind);
    if (!gfx) return;
    /* (res: the BRTI the texture is made from now; in a file (a streamed texture's data), its mip 0 is in that file) */
    gpu_asset_key k;
    memset(&k, 0, sizeof k);
    k.tex = GP(gfx + 0x10);
    k.path = GPU_ASSET_REBIND;
    if (res && hos_guest_range(res, 0xa0) && G32(res) == 0x49545242u) {
        key_of_brti(&k, res);
        uint32_t nl = 0;
        const char *nm = bstr(G64(res + 0x60), &nl);
        const uint64_t mt = G64(res + 0x70);
        const uint64_t mip0 = hos_guest_range(mt, 8) ? G64(mt) : 0;
        if (nm) k.name = nm, k.name_len = nl;
        if (mip0 && hos_guest_range(mip0, 1)) k.data = GP(mip0);
    }
    rx_direct()->asset_load(&k);
}

/* the compaction step (worker): its movers copy only with [worker+0x1c11] == 1 */
PATCH_AT(0x815f0820, word(0xd503237f), before, none, assets_compaction, as(x0, w)) {
    if (!ready()) return;
    os_atomic_inc(&n_compaction);
    if (hos_guest_range(w + 0x1c11, 1) && *(uint8_t *)GP(w + 0x1c11) == 1) {
        *(uint8_t *)GP(w + 0x1c11) = 0;
        os_atomic_inc(&n_compaction_cleared);
    }
}

/* ---- in-place files: a BNTX the game keeps whole in memory, its textures initialized over the file's own pool ----
   Each caller of TextureImpl::Initialize that initializes an in-place file's texture notes the file for this thread
   while its body runs; the Initialize entry then hands the texture to the back end by its name, index and mip 0's
   offset in that file. */
static _Thread_local uint64_t tl_inplace;
static uint64_t run_inplace(GuestFn *orig, uint64_t bntx) {
    if (tl_inplace || !ready()) return guest_orig(orig);
    tl_inplace = bntx;
    const uint64_t r = guest_orig(orig);
    tl_inplace = 0;
    return r;
}
/* (a texture's BRTI +0x68 is its file's 'NX  ' header, the BNTX +0x20: the game reads the file's pool through it) */
static uint64_t bntx_of(uint64_t brti) {
    return hos_guest_range(brti, 0xa0) ? G64(brti + 0x68) - 0x20 : 0;
}
/* ui2d's texture by name in a layout's timg/__Combined.bntx (f_8043bf18, the resource accessor's lookup, tail-calls it
   through f_8043c0d0): (obj, device, BRTI) */
PATCH_FUNC(0x8088e790, word(0xd503237f), body(0x21fbddced3eded15), aapcs, x0, assets_ui2d_texture, orig, as(x2, brti)) {
    return run_inplace(orig, bntx_of(brti));
}
/* a font's BNTX in place (the bffnt's sheets, f_8057f61c): (obj, device, pool, offset), the file at [obj+8] */
PATCH_FUNC(0x808aa478, word(0xd503237f), body(0xba0dde2aaaa72996), aapcs, x0, assets_font_bntx, orig, as(x0, obj)) {
    nvn_claim_begin(NVN_CLAIM_TEXTURES, "font_bntx_texture_init");   /* (its pool holds the BNTX) */
    const uint64_t r = run_inplace(orig, hos_guest_range(obj, 0x10) ? G64(obj + 8) : 0);
    nvn_claim_end();
    return r;
}

static int index_in(uint64_t bntx, uint64_t brti) {
    const uint32_t n = G32(bntx + 0x24);
    const uint64_t tbl = G64(bntx + 0x28);
    for (uint32_t i = 0; i < n && hos_guest_range(tbl + 8 * i, 8); i++)
        if (G64(tbl + 8 * i) == brti) return (int)i;
    return -1;
}
/* nn::gfx TextureImpl<NVN>::Initialize(obj, device, info, pool, offset, size): info is the BRTI + 0x10, the NVN texture
   obj + 0x10 */
PATCH_AT(0x80210fdc, word(0xd503237f), before, none, assets_inplace_init, as(x0, obj), as(x2, info)) {
    const uint64_t bntx = tl_inplace;
    if (!bntx) return;
    const uint64_t brti = info - 0x10;
    int index = -1;
    if (hos_guest_range(bntx, 0x40) && G32(bntx) == 0x58544e42u && hos_guest_range(brti, 0xa0) && G32(brti) == 0x49545242u &&
        bntx_of(brti) == bntx)   /* ('BNTX', 'BRTI') */
        index = index_in(bntx, brti);
    if (index < 0) {
        if (os_atomic_inc(&n_inplace_bad) <= 5)
            hos_log("assets: in place: texture info %#llx is not a texture of the BNTX %#llx noted", (unsigned long long)info,
                    (unsigned long long)bntx);
        return;
    }
    gpu_asset_key k;
    memset(&k, 0, sizeof k);
    k.tex = GP(obj + 0x10);
    k.path = GPU_ASSET_INPLACE;
    key_of_brti(&k, brti);
    uint32_t nl = 0;
    const char *nm = bstr(G64(brti + 0x60), &nl);
    const uint64_t mt = G64(brti + 0x70);
    const uint64_t mip0 = hos_guest_range(mt, 8) ? G64(mt) : 0;
    if (nm && mip0 > bntx) k.name = nm, k.name_len = nl, k.index = (uint32_t)index, k.data_offset = (uint32_t)(mip0 - bntx);
    rx_direct()->asset_load(&k);
    os_atomic_inc(&n_inplace);
}

/* ---- the font glyph cache ----
   nn::font's TextureCache plot (f_80767b20, cache x19) writes each glyph into the linear R8 atlas (the pool's mapping
   x27, pitch [cache+0x28], [cache+0x2c] rows, stored bottom-up: row r of the cache is storage row rows - 1 - r) in its
   line's cell: column [node+0x40], cache rows from [node+0x42], x22 bytes wide (the glyph's width + 2); the rows below
   it, down to the line's height (the blr at 0x80768474), are cleared. When the plot unmaps the pool after the glyph
   (0x80768504), the cell's rows go to the atlas image. */
static _Thread_local uint32_t tl_line_h;
PATCH_AT(0x80768478, word(0x79403ee9), after, none, assets_glyph_line, as(x0, h)) { tl_line_h = (uint32_t)h; }
PATCH_AT(0x80768504, word(0xf94f4e60), after, none, assets_glyph_plotted, as(x19, cache), as(x22, w), as(x27, atlas),
         as(x28, node)) {
    if (!assets_on() || !hos_guest_range(cache + 0x28, 8) || !hos_guest_range(node + 0x40, 4)) return;
    const uint32_t pitch = G32(cache + 0x28), rows = G32(cache + 0x2c);
    const uint32_t x = *(const uint16_t *)GP(node + 0x40), y = *(const uint16_t *)GP(node + 0x42);
    nvn_tex t;
    if (!nvn_tex_live_at(atlas, &t) || t.d.width != pitch || t.d.height != rows) {
        if (os_atomic_inc(&n_glyph_notex) <= 5)
            hos_log("assets: glyph: no %ux%u texture at the atlas %#llx", pitch, rows, (unsigned long long)atlas);
        return;
    }
    uint32_t h = tl_line_h + 2, cw = (uint32_t)w;
    if (x >= pitch || y >= rows) return;
    if (x + cw > pitch) cw = pitch - x;
    if (y + h > rows) h = rows - y;
    const uint32_t top = rows - (y + h);   /* (the cell's first storage row) */
    NVNcopyRegion r = {(int)x, (int)top, 0, (int)cw, (int)h, 1};
    rx_direct()->texels(GPU_TEXELS_GLYPH, &t, 0, 0, &r, GP(atlas + (uint64_t)top * pitch + x), pitch, 0);
    os_atomic_inc(&n_glyphs);
}

/* agl::TextureDataMgr::InitializeHeap (f_80577140): its pools hold texture storage only: claimed as such
   (runtime/nvn_claims.h), so nothing backs them on the GPU */
PATCH_FUNC(0x80577140, word(0xd503237f), body(0xd76e3c5e7f45b5b0), aapcs, x0, assets_heap_init, orig) {
    nvn_claim_begin(NVN_CLAIM_TEXTURES, "agl::TextureDataMgr::InitializeHeap");
    const uint64_t r = guest_orig(orig);
    nvn_claim_end();
    return r;
}
