/* (native-vk A-rt) The host's side of prepared assets for the direct back end (docs/direct/a-rt-report.md; the hooks are
   the ones traced in work/assets/native/asset-path.md 3.4 and 7.3, on branch agent/assets1). Direct mode only
   (RX_BACKEND=direct; HOS_DBG=noassets turns every patch here off, for a before/after): with RX_BACKEND unset nothing here
   runs and the game is as before.

   The store: HOS_PREPARED=<folder> (default <the executable's folder>/../../prepared, i.e. work/prepared beside a build in
   work/sa/<build>, else <romfs>/../../prepared): <folder>/tex is the texture store the back end maps (hsd_assets_open),
   <folder>/terrain the converted terrain tiles, served as a second file overlay root after HOS_FS_OVERLAY's (nn_fs.c).

   Texture loads (agl::TextureData), each tied to its BNTX record and handed to the back end (hsd_asset_load) before the
   texture's nvnTextureInitialize, which then makes the image from the store:
     - f_80798690 loadImage_(td, ctx, src): the BNTX is found back from src (its data in the decompressed file), the
       texture by its name, descriptor and place there; the NVN texture is [td+0xd8]+0x10. The copy into the asset heap
       (pool 127) is skipped (x2 = 0: no memcpy, memmove or flush; f_807987d0 still runs): the back end never reads
       texture storage, so for a texture with no record too.
     - f_80798b60 (ui2d) and f_80792bd0 (in-place BNTX) note their file (x0 / x1) for this thread while they run; at
       f_80798920 (nn::gfx TextureImpl::Initialize(obj, device, info, ...)) inside them, info - 0x10 is the file's own
       BRTI and obj + 0x10 the NVN texture.
     - f_807982f0 rebind_(td, ...) (compaction's move, Replace's swap): the object is initialized again: the same record.
     - f_807da480, the compaction step: the movers copy a moved texture's bytes only when byte [worker+0x1f21] is 1 (and
       no copier object is set); it is cleared at every step, so compaction moves no texels (nothing reads them).
     - f_83537d90 replaceLoad_: counted (its copy is not skipped; none was seen in A0's trace).
   Game patches (texels no NVN call carries), as region uploads into the texture's image (hsd_texels):
     - terrain: f_80c17040+0x544, the memcpy of a tile record's GPU blob B (x1, x2 bytes) into the tile texture's storage
       (x0): a converted file's B is linear (prepare_terrain.py), recognised by its size, and uploaded instead; an
       unconverted one is the placeholder, counted; the memcpy copies nothing either way.
     - the font glyph cache: f_8152f540 (the shelf packer: cache, &x, &y, w, h) notes the cell for this thread; after
       f_81539300 (nn::font's plot) has written it into the atlas ([cache+0xf0], pitch [cache+0x140]), the cell's rows
       x..x+w+4, y..y+h+3 go to the atlas image (docs/direct/m3-patch-sites.md 6).
   Packaged texture data (nvnTextureBuilderSetPackagedTextureData) is not copied into the storage (nvn_native.c).
   A line every 10 s: the asset heap's resident pages and the guest memory's (mincore), the copies skipped. */
#include "hos.h"
#include "direct.h"
#include "../gpu/render/rx_nvn.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>

uint8_t *hos_guest_base(void);
uint64_t hos_guest_size(void);
int hos_gpu_pools(uint64_t (*out)[2], int max);
uint64_t nvn_gpu_guest(uint64_t gpu);
int nvn_tex_live_at(uint64_t gpu, nvn_tex *out);
int nvn_fmt_bpb(int format, int *bw, int *bh);
extern const char *hos_romfs_dir;

static volatile LONG64 n_loads, n_load_skipped, n_load_bytes, n_inplace, n_rebind, n_compaction, n_compaction_cleared,
    n_replace, n_pkg, n_pkg_bytes, n_tiles, n_tile_bytes, n_tiles_unconv, n_tile_notex, n_tile_badsite, n_glyphs,
    n_glyph_notex, n_overlay, n_overlay_bytes, n_overlay_raw;

static int dbg_has(const char *name) {
    const char *d = getenv("HOS_DBG");
    if (!d) return 0;
    const size_t n = strlen(name);
    for (const char *p = d; (p = strstr(p, name)); p += n)
        if ((p == d || p[-1] == ',') && (p[n] == 0 || p[n] == ',' || p[n] == ':')) return 1;
    return 0;
}

static int on_ = -1;
int direct_assets_on(void) {
    if (on_ < 0) on_ = rx_direct() && !dbg_has("noassets");
    return on_;
}

/* ---- the prepared folder ---- */
static char prepared[1024];
static const char *prepared_dir(void) {
    static int done;
    if (done) return prepared[0] ? prepared : NULL;
    done = 1;
    struct stat st;
    const char *e = getenv("HOS_PREPARED");
    if (e && *e) snprintf(prepared, sizeof prepared, "%s", e);
    else {
        char exe[1024];
        const DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = n ? strrchr(exe, '/') : NULL;
        if (slash) {
            *slash = 0;
            snprintf(prepared, sizeof prepared, "%s/../../prepared", exe);
        }
        char probe[1100];
        snprintf(probe, sizeof probe, "%s/tex/index.bin", prepared);
        if (!slash || stat(probe, &st)) snprintf(prepared, sizeof prepared, "%s/../../prepared", hos_romfs_dir ? hos_romfs_dir : "romfs");
    }
    char real[1024];
    if (realpath(prepared, real)) snprintf(prepared, sizeof prepared, "%s", real);
    hos_log("assets: the prepared store: %s (HOS_PREPARED)", prepared);
    return prepared;
}

/* ---- the asset heap (pool 127) and the memory line ---- */
static uint64_t heap_lo, heap_hi;
static void *mem_line(void *arg) {
    (void)arg;
    const long pg = sysconf(_SC_PAGESIZE);
    for (;;) {
        Sleep(10000);
        uint64_t heap_res = 0, guest_res = 0;
        const uint64_t gsize = hos_guest_size();
        unsigned char *v = (unsigned char *)malloc((size_t)(gsize / (uint64_t)pg) + 1);
        if (!v) continue;
        if (!mincore(hos_guest_base(), (size_t)gsize, v))
            for (uint64_t i = 0; i < gsize / (uint64_t)pg; i++) guest_res += v[i] & 1;
        if (heap_hi && heap_hi <= gsize) {
            const uint64_t lo = heap_lo & ~(uint64_t)(pg - 1);
            if (!mincore(hos_guest_base() + lo, (size_t)(heap_hi - lo), v))
                for (uint64_t i = 0; i < (heap_hi - lo) / (uint64_t)pg; i++) heap_res += v[i] & 1;
        }
        free(v);
        hos_log("assets: resident: the asset heap %llu of %llu MB, guest memory %llu of %llu MB; copies skipped: loads %lld of %lld "
                "(%lld MB), packaged %lld (%lld MB), compaction steps %lld (copy flag cleared %lld); in-place textures %lld, rebinds "
                "%lld, replaced %lld; terrain tiles %lld uploaded (%lld MB), %lld not converted, %lld with no texture, %lld odd "
                "sites; overlay files %lld (%lld MB, %lld MB decompressed); glyph cells %lld (%lld with no texture)%s",
                (unsigned long long)(heap_res * (uint64_t)pg >> 20), (unsigned long long)((heap_hi - heap_lo) >> 20),
                (unsigned long long)(guest_res * (uint64_t)pg >> 20), (unsigned long long)(gsize >> 20), (long long)n_load_skipped,
                (long long)n_loads, (long long)(n_load_bytes >> 20), (long long)n_pkg, (long long)(n_pkg_bytes >> 20),
                (long long)n_compaction, (long long)n_compaction_cleared, (long long)n_inplace, (long long)n_rebind,
                (long long)n_replace, (long long)n_tiles, (long long)(n_tile_bytes >> 20), (long long)n_tiles_unconv,
                (long long)n_tile_notex, (long long)n_tile_badsite, (long long)n_overlay, (long long)(n_overlay_bytes >> 20),
                (long long)(n_overlay_raw >> 20), (long long)n_glyphs, (long long)n_glyph_notex,
                direct_assets_on() ? "" : " (HOS_DBG=noassets: the patches are off)");
    }
    return NULL;
}

static pthread_once_t start_once = PTHREAD_ONCE_INIT;
static int store_ok;
static void start(void) {
    pthread_t t;
    if (!pthread_create(&t, NULL, mem_line, NULL)) pthread_detach(t);
    if (!direct_assets_on()) return;
    const char *d = prepared_dir();
    char tex[1100];
    snprintf(tex, sizeof tex, "%s/tex", d ? d : ".");
    store_ok = rx_direct()->assets_open(tex) > 0;
}
static int ready(void) {   /* (direct mode: the memory line runs, and with the patches on, the store is open) */
    if (!rx_direct()) return 0;
    pthread_once(&start_once, start);
    return direct_assets_on();
}

static void find_heap(uint64_t slot) {
    static volatile LONG done;
    if (done || InterlockedExchange(&done, 1)) return;
    uint64_t pools[1024][2];
    const int n = hos_gpu_pools(pools, 1024);
    for (int i = 0; i < n; i++)
        if (slot >= pools[i][0] && slot < pools[i][0] + pools[i][1]) {
            heap_lo = nvn_gpu_guest(pools[i][0]), heap_hi = heap_lo + pools[i][1];
            hos_log("assets: the asset heap: pool %#llx + %llu MB (%s)", (unsigned long long)pools[i][0],
                    (unsigned long long)(pools[i][1] >> 20), pools[i][1] == 0x98000000ull ? "pool 127's size" : "not pool 127's size");
            if (direct_assets_on()) rx_direct()->asset_pool(pools[i][0], pools[i][0] + pools[i][1]);
            return;
        }
    hos_log("assets: the asset heap's pool not found (slot %#llx)", (unsigned long long)slot);
}

/* ---- BNTX helpers (relocated by the loader: absolute pointers, else offsets in the file) ---- */
static const char *bstr(uint64_t p, uint32_t *len) {
    if (!p || !hos_guest_range(p, 2)) return NULL;
    const uint16_t n = *(const uint16_t *)GP(p);
    if (!hos_guest_range(p + 2, n)) return NULL;
    *len = n;
    return (const char *)GP(p + 2);
}
#define REL(bntx, p) ((p) < (bntx) ? (p) + (bntx) : (p))
static void key_of_brti(hsd_asset_key *k, uint64_t brti) {
    k->format = G32(brti + 0x1c), k->width = G32(brti + 0x24), k->height = G32(brti + 0x28), k->layers = G32(brti + 0x30);
    k->mips = *(const uint16_t *)GP(brti + 0x16), k->image_size = G32(brti + 0x50);
}
/* the BRTI's index in its BNTX (by its table entry, or by its name when brti is a copy); -1: not there */
static int index_in(uint64_t bntx, uint64_t brti, const char *nm, uint32_t nl, uint64_t *file_brti) {
    const uint32_t n = G32(bntx + 0x24);
    const uint64_t tbl = REL(bntx, G64(bntx + 0x28));
    for (uint32_t i = 0; i < n && i < 8192 && hos_guest_range(tbl + 8 * i, 8); i++) {
        const uint64_t b = REL(bntx, G64(tbl + 8 * i));
        if (b == brti) return *file_brti = b, (int)i;
        uint32_t fl = 0;
        const char *fn = nm && hos_guest_range(b, 0xa0) ? bstr(REL(bntx, G64(b + 0x60)), &fl) : NULL;
        if (fn && fl == nl && !memcmp(fn, nm, nl)) return *file_brti = b, (int)i;
    }
    return -1;
}

/* TextureData::loadImage_(td, ctx, src) */
ENTRY_HOOK(0x80798690) {
    if (!rx_direct()) return 0;
    const int on = ready();
    const uint64_t td = ARG(0), src = ARG(2);
    if (!hos_guest_range(td, 0x100)) return 0;
    const uint64_t brti = td + 0x58, gfx = G64(td + 0xd8);
    const uint64_t slot = hos_guest_range(G64(td + 0x18), 8) ? G64(td + 0x18) + 0x10 : 0;
    if (slot) find_heap(slot);   /* (with the patches off too: the before/after line) */
    if (!on) return 0;
    if (!gfx || G32(brti) != 0x49545242u) return 0;   /* ('BRTI') */
    hsd_asset_key k;
    memset(&k, 0, sizeof k);
    k.tex = GP(gfx + 0x10);
    k.path = HSD_ASSET_LOAD;
    key_of_brti(&k, brti);
    /* (the BNTX: src is the texture's data in the decompressed file, at a multiple of 512 from its start; up to 64 MB back) */
    uint64_t bntx = 0;
    if (src && !(src & 0x1ff))
        for (uint64_t a = src - 0x200; a + (64ull << 20) > src && a >= 0x200; a -= 0x200)
            if (hos_guest_range(a, 16) && G32(a) == 0x58544e42u && G32(a + 8) == 0x00040101u) { bntx = a; break; }
    uint32_t nl = 0;
    const char *nm = bstr(G64(brti + 0x60), &nl);
    uint64_t file_brti = 0;
    const int index = bntx && nm ? index_in(bntx, 0, nm, nl, &file_brti) : -1;
    if (index >= 0) {
        k.name = nm, k.name_len = nl, k.index = (uint32_t)index, k.data_offset = (uint32_t)(src - bntx);
    }
    rx_direct()->asset_load(&k);
    InterlockedIncrement64(&n_loads);
    if (src) {   /* (no copy into the heap: the back end makes the image from the store, or the placeholder) */
        InterlockedIncrement64(&n_load_skipped);
        InterlockedAdd64(&n_load_bytes, G32(td + 0xa8));
        c->x[2] = 0;
    }
    return 0;
}

/* TextureData::rebind_(td, newBase, res, ctx) */
ENTRY_HOOK(0x807982f0) {
    if (!ready()) return 0;
    const uint64_t td = ARG(0);
    if (!hos_guest_range(td, 0x100)) return 0;
    const uint64_t gfx = G64(td + 0xd8);
    InterlockedIncrement64(&n_rebind);
    if (gfx) rx_direct()->asset_rebind(GP(gfx + 0x10));
    return 0;
}

/* in-place files: the file, for this thread while its textures are initialized */
static _Thread_local uint64_t tl_inplace;
static _Thread_local int tl_in_body;
static int run_inplace(Ctx *c, uint64_t addr, uint64_t file) {
    if (tl_in_body || !ready()) return 0;
    A64Fn f = a64_lookup(addr);
    if (!f) return 0;
    const uint64_t was = tl_inplace;
    tl_inplace = file;
    tl_in_body = 1;
    f(c);   /* (the lifted body: the hook sees it again and lets it run) */
    tl_in_body = 0;
    tl_inplace = was;
    return 1;
}
ENTRY_HOOK(0x80798b60) { return run_inplace(c, 0x80798b60ull, ARG(0)); }   /* ResTextureFile in place (ui2d) */
ENTRY_HOOK(0x80792bd0) { return run_inplace(c, 0x80792bd0ull, ARG(1)); }   /* in-place BNTX */

/* nn::gfx TextureImpl<NVN>::Initialize(obj, device, info, pool, offset, size): an in-place file's texture */
ENTRY_HOOK(0x80798920) {
    const uint64_t bntx = tl_inplace;
    if (!bntx || !direct_assets_on()) return 0;
    const uint64_t brti = ARG(2) - 0x10, obj = ARG(0);
    if (!obj || !hos_guest_range(brti, 0xa0) || G32(brti) != 0x49545242u || !hos_guest_range(bntx, 0x40) ||
        G32(bntx) != 0x58544e42u)
        return 0;
    hsd_asset_key k;
    memset(&k, 0, sizeof k);
    k.tex = GP(obj + 0x10);
    k.path = HSD_ASSET_INPLACE;
    key_of_brti(&k, brti);
    uint32_t nl = 0;
    const char *nm = bstr(REL(bntx, G64(brti + 0x60)), &nl);
    uint64_t file_brti = 0;
    const int index = index_in(bntx, brti, nm, nl, &file_brti);
    const uint64_t mt = REL(bntx, G64(brti + 0x70));
    const uint64_t mip0 = hos_guest_range(mt, 8) ? REL(bntx, G64(mt)) : 0;
    if (index >= 0 && nm && mip0 > bntx) k.name = nm, k.name_len = nl, k.index = (uint32_t)index, k.data_offset = (uint32_t)(mip0 - bntx);
    rx_direct()->asset_load(&k);
    InterlockedIncrement64(&n_inplace);
    return 0;
}

/* the compaction step (worker): its movers copy only with [worker+0x1f21] == 1 */
ENTRY_HOOK(0x807da480) {
    if (!ready()) return 0;
    const uint64_t w = ARG(0);
    InterlockedIncrement64(&n_compaction);
    if (hos_guest_range(w + 0x1f21, 1) && *(uint8_t *)GP(w + 0x1f21) == 1) {
        *(uint8_t *)GP(w + 0x1f21) = 0;
        if (InterlockedIncrement64(&n_compaction_cleared) == 1)
            hos_log("assets: compaction (worker %#llx): its texel copies are off (byte +0x1f21 was 1)", (unsigned long long)w);
    }
    return 0;
}

/* TextureDataMgr replaceLoad_(replacer, td, src, size, align): counted */
ENTRY_HOOK(0x83537d90) {
    if (!ready()) return 0;
    if (InterlockedIncrement64(&n_replace) <= 10)
        hos_log("assets: a Replace load (td %#llx, %llu bytes): its copy into the heap is not skipped", (unsigned long long)ARG(1),
                (unsigned long long)ARG(3));
    return 0;
}

int direct_assets_skip_packaged(uint64_t bytes) {
    if (!rx_direct() || !direct_assets_on()) return 0;
    InterlockedIncrement64(&n_pkg);
    InterlockedAdd64(&n_pkg_bytes, (LONG64)bytes);
    return 1;
}

/* ---- the terrain overlay ---- */
const char *direct_assets_overlay(void) {
    if (!rx_direct() || !direct_assets_on()) return NULL;
    static char root[1100];
    const char *d = prepared_dir();
    struct stat st;
    snprintf(root, sizeof root, "%s/terrain", d ? d : ".");
    if (!d || stat(root, &st)) {
        hos_log("assets: no converted terrain tiles at %s: the game's own files (their tiles the placeholder)", root);
        return NULL;
    }
    hos_log("assets: the converted terrain tiles are a file overlay: %s", root);
    return root;
}
void direct_assets_overlay_opened(const char *path) {
    InterlockedIncrement64(&n_overlay);
    FILE *f = fopen(path, "rb");
    unsigned char h[20];
    if (f) {
        if (fread(h, 1, 20, f) == 20 && !memcmp(h, "TARB", 4)) {
            uint32_t raw, z;
            memcpy(&raw, h + 12, 4), memcpy(&z, h + 16, 4);
            InterlockedAdd64(&n_overlay_raw, raw);
            InterlockedAdd64(&n_overlay_bytes, z);
        }
        fclose(f);
    }
}

/* f_80c17040+0x544: memcpy(x0 the tile texture's storage, x1 the record's blob B, x2 its size), x11 the record */
SPLICE_AFTER(0x80c17040, "(c->z) ? (uint64_t)0 : c->x[8]; c->x[1] = _r;") {
    if (!direct_assets_on()) return;
    const uint64_t rec = c->x[11], dst = c->x[0], src = c->x[1];
    const uint32_t n = (uint32_t)c->x[2];
    if (!hos_guest_range(rec + 0x2c, 8) || src != rec + 0x2c + (uint64_t)(int64_t)(int32_t)G32(rec + 0x2c) || n != G32(rec + 0x30)) {
        if (InterlockedIncrement64(&n_tile_badsite) <= 5)
            hos_log("assets: terrain: the memcpy site with x1 %#llx x2 %#x, not record %#llx's blob: left alone",
                    (unsigned long long)src, n, (unsigned long long)rec);
        return;
    }
    nvn_tex t;
    if (!nvn_tex_live_at(dst, &t)) {
        if (InterlockedIncrement64(&n_tile_notex) <= 5)
            hos_log("assets: terrain: no texture at %#llx (a tile of %u bytes): left alone", (unsigned long long)dst, n);
        return;
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(t.d.format, &bw, &bh);
    const uint32_t layers = t.d.target == NVN_TEXTURE_TARGET_2D_ARRAY ? (t.d.depth ? t.d.depth : 1) : 1;
    const uint64_t linear = (uint64_t)((t.d.width + bw - 1) / bw) * ((t.d.height + bh - 1) / bh) * (uint64_t)bpb * layers;
    NVNcopyRegion r = {0, 0, 0, (int)t.d.width, (int)t.d.height, (int)layers};
    if (bpb > 0 && n == linear) {
        rx_direct()->texels(HSD_TEXELS_TERRAIN, &t, 0, 0, &r, GP(src), 0, 0);
        InterlockedIncrement64(&n_tiles);
        InterlockedAdd64(&n_tile_bytes, n);
    } else {
        rx_direct()->texels(HSD_TEXELS_TERRAIN, &t, 0, 0, &r, NULL, 0, 0);
        InterlockedIncrement64(&n_tiles_unconv);
    }
    c->x[2] = 0;   /* (the memcpy copies nothing: the storage stays unwritten) */
}

/* ---- the font glyph cache ---- */
static _Thread_local struct { int in_pack, in_plot, have; uint32_t x, y, w, h; } gl;
ENTRY_HOOK(0x8152f540) {   /* the shelf packer (cache, &x, &y, w, h) */
    if (gl.in_pack || !gl.in_plot || !direct_assets_on()) return 0;
    static A64Fn f;
    if (!f) f = a64_lookup(0x8152f540ull);
    if (!f) return 0;
    const uint64_t px = ARG(1), py = ARG(2);
    const uint32_t w = (uint32_t)ARG(3), h = (uint32_t)ARG(4);
    gl.in_pack = 1;
    f(c);
    gl.in_pack = 0;
    if (hos_guest_range(px, 4) && hos_guest_range(py, 4)) gl.x = G32(px), gl.y = G32(py), gl.w = w, gl.h = h, gl.have = 1;
    return 1;
}
ENTRY_HOOK(0x81539300) {   /* nn::font's plot (cache, glyph, ...) */
    if (gl.in_plot || !ready()) return 0;
    static A64Fn f;
    if (!f) f = a64_lookup(0x81539300ull);
    if (!f) return 0;
    const uint64_t cache = ARG(0);
    gl.have = 0;
    gl.in_plot = 1;
    f(c);
    gl.in_plot = 0;
    if (!gl.have || !hos_guest_range(cache + 0xf0, 0x58)) return 1;
    const uint64_t storage = G64(cache + 0xf0);
    const uint32_t stride = G32(cache + 0x140);
    nvn_tex t;
    if (!storage || !stride || !nvn_tex_live_at(storage, &t)) {
        if (InterlockedIncrement64(&n_glyph_notex) <= 5)
            hos_log("assets: glyph: no texture at %#llx (stride %u)", (unsigned long long)storage, stride);
        return 1;
    }
    const uint32_t x = gl.x, y = gl.y;
    if (x >= t.d.width || y >= t.d.height) return 1;
    uint32_t w = gl.w + 5, h = gl.h + 4;   /* (the cell's memsets: columns x..x+w+4, rows y..y+h+3) */
    if (x + w > t.d.width) w = t.d.width - x;
    if (y + h > t.d.height) h = t.d.height - y;
    NVNcopyRegion r = {(int)x, (int)y, 0, (int)w, (int)h, 1};
    rx_direct()->texels(HSD_TEXELS_GLYPH, &t, 0, 0, &r, GP(storage + (uint64_t)y * stride + x), stride, 0);
    InterlockedIncrement64(&n_glyphs);
    return 1;
}
