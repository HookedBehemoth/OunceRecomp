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
int nvn_tex_live_containing(uint64_t gpu, nvn_tex *out);
int nvn_fmt_bpb(int format, int *bw, int *bh);
extern const char *hos_romfs_dir;

static volatile LONG64 n_loads, n_load_skipped, n_load_bytes, n_inplace, n_rebind, n_compaction, n_compaction_cleared,
    n_replace, n_pkg, n_pkg_bytes, n_tiles, n_tile_bytes, n_tiles_unconv, n_tile_notex, n_tile_badsite, n_glyphs,
    n_glyph_notex, n_rebind_logged, n_tile_inside, n_tile_held, n_tile_held_max, n_tile_dropped, n_overlay, n_overlay_bytes, n_overlay_raw,
    n_tile_kept_given, n_tile_kept_other;

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
        {   /* (the guest memory's pages that exist: the memfd's blocks) */
            int hos_posix_guest_memfd(void);
            struct stat st;
            const int fd = hos_posix_guest_memfd();
            if (fd >= 0 && !fstat(fd, &st)) guest_res = (uint64_t)st.st_blocks * 512 / (uint64_t)pg;
        }
        unsigned char *v = heap_hi > heap_lo ? (unsigned char *)malloc((size_t)((heap_hi - heap_lo) / (uint64_t)pg) + 2) : NULL;
        if (v && heap_hi <= gsize) {
            const uint64_t lo = heap_lo & ~(uint64_t)(pg - 1);
            if (!mincore(hos_guest_base() + lo, (size_t)(heap_hi - lo), v))
                for (uint64_t i = 0; i < (heap_hi - lo) / (uint64_t)pg; i++) heap_res += v[i] & 1;
        }
        free(v);
        hos_log("assets: resident: the asset heap %llu of %llu MB, guest memory %llu of %llu MB; copies skipped: loads %lld of %lld "
                "(%lld MB), packaged %lld (%lld MB), compaction steps %lld (copy flag cleared %lld); in-place textures %lld, rebinds "
                "%lld, replaced %lld; terrain tiles %lld uploaded (%lld MB), %lld not converted, %lld with no texture, %lld odd "
                "sites, held for their texture %lld (storage addresses kept %lld, %lld dropped; given to textures initialized there %lld, "
                "a texture of another size there %lld); overlay opens %lld; glyph cells %lld (%lld with no texture)%s",
                (unsigned long long)(heap_res * (uint64_t)pg >> 20), (unsigned long long)((heap_hi - heap_lo) >> 20),
                (unsigned long long)(guest_res * (uint64_t)pg >> 20), (unsigned long long)(gsize >> 20), (long long)n_load_skipped,
                (long long)n_loads, (long long)(n_load_bytes >> 20), (long long)n_pkg, (long long)(n_pkg_bytes >> 20),
                (long long)n_compaction, (long long)n_compaction_cleared, (long long)n_inplace, (long long)n_rebind,
                (long long)n_replace, (long long)n_tiles, (long long)(n_tile_bytes >> 20), (long long)n_tiles_unconv,
                (long long)n_tile_notex, (long long)n_tile_badsite, (long long)n_tile_held, (long long)n_tile_held_max,
                (long long)n_tile_dropped, (long long)n_tile_kept_given, (long long)n_tile_kept_other, (long long)n_overlay, (long long)n_glyphs, (long long)n_glyph_notex,
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
    direct_assets_tilelog = dbg_has("tilelog");
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
    void direct_diag_nodata_load(Ctx *c);   /* (M4g: HOS_DBG=nodata, either back end) */
    direct_diag_nodata_load(c);
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
    /* (its BNTX: the store's records of its name and descriptor say where mip 0 is in their file; the one whose file
       header is that far back from src is the one: hsd_asset_load) */
    uint32_t nl = 0;
    const char *nm = bstr(G64(brti + 0x60), &nl);
    if (nm) k.name = nm, k.name_len = nl;
    k.data = src && hos_guest_range(src, 1) ? GP(src) : NULL;
    /* (M4g, M6f: a load with no data is an alias of another texture: its target's texels, direct_tex_dedup.c) */
    if (!src) direct_tex_alias(td, &k);
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
    const uint64_t gfx = G64(td + 0xd8), res = ARG(2);
    InterlockedIncrement64(&n_rebind);
    if (!gfx) return 0;
    /* (res: the BRTI the texture is made from now; in a file (a streamed texture's data), its mip 0 is in that file) */
    hsd_asset_key k;
    memset(&k, 0, sizeof k);
    k.tex = GP(gfx + 0x10);
    k.path = HSD_ASSET_REBIND;
    if (res && hos_guest_range(res, 0xa0) && G32(res) == 0x49545242u) {
        key_of_brti(&k, res);
        uint32_t nl = 0;
        const char *nm = bstr(G64(res + 0x60), &nl);
        const uint64_t mt = G64(res + 0x70);
        const uint64_t mip0 = hos_guest_range(mt, 8) ? G64(mt) : 0;
        if (nm) k.name = nm, k.name_len = nl;
        if (mip0 && hos_guest_range(mip0, 1)) k.data = GP(mip0);
        if (InterlockedIncrement64(&n_rebind_logged) <= 12)
            hos_log("assets: rebind td %#llx new base %#llx res %#llx '%.*s' mip 0 %#llx (in the heap: %d)", (unsigned long long)td,
                    (unsigned long long)ARG(1), (unsigned long long)res, nm ? (int)nl : 0, nm ? nm : "", (unsigned long long)mip0,
                    mip0 >= heap_lo && mip0 < heap_hi);
    }
    rx_direct()->asset_load(&k);
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

/* (M4f) The texture data pool (pool 127, 2.4 GB) is made by agl::TextureDataMgr, whose nvnMemoryPoolInitialize returns to
   0x80f6cb20 (HOS_NVN_LOG_POOLS=1, run m4f_pl_d: the only pool made from there). The direct back end takes every texel of
   it from the store and never reads it, so its layer 0 leaves it unbound: named here, at its creation, before layer 0
   binds pools (direct_pools.cpp layer0_pool_native). The manager makes more than one (Kokiri: 2432 MB at boot, 201 MB and
   2 MB at the scene load): each is named. find_heap still names the asset heap's pool (127) to the asset path, as before. */
void direct_assets_pool_made(uint64_t lr, uint64_t gpu, uint64_t size) {
    if (lr != 0x80f6cb20ull || !rx_direct() || !direct_assets_on()) return;
    hos_log("assets: the texture data pool made: %#llx + %llu MB (agl::TextureDataMgr, lr %#llx)", (unsigned long long)gpu,
            (unsigned long long)(size >> 20), (unsigned long long)lr);
    rx_direct()->texture_data_pool(gpu, gpu + size);
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
    direct_sky_lut_opened(path);
    const LONG64 k = InterlockedIncrement64(&n_overlay);
    if (k <= 20 || k % 100000 == 0) hos_log("assets: overlay open #%lld: %s", (long long)k, path);
}

/* ---- the tiles' bytes, kept by storage address (M6f) ----
   The memcpy below writes nothing (the direct back end never reads texture storage), so the bytes it would have left in
   the storage are kept here, by the storage's address, as the storage would keep them: given to the texture live there
   (if any), and to every texture initialized there later whose linear size they are, until another tile is copied
   there; the back end takes them only into an image nothing has written yet (HSD_TEXELS_TERRAIN_KEPT: an image of
   that storage and shape that still holds what was written into it holds the storage's content already). The game does
   initialize tile textures again over storage it does not copy into again (walking in Hyrule Field: the height and
   material tiles at present ~990, whose first textures it had finalized; their new images were empty, the floor at
   height 0 and the foliage over it lit from garbage: docs/direct/m6f-report.md 2). Before M6f the bytes were held only
   until the first texture there took them. */
#define TILES_HELD 4096   /* (distinct tile storage addresses; Hyrule Field uses ~150) */
volatile long direct_assets_tiles_pending;
volatile long direct_assets_tilelog;
static struct { uint64_t dst; uint8_t *bytes; uint32_t n; } held[TILES_HELD];   /* (open addressing; dst stays once seen) */
static SRWLOCK held_lock = SRWLOCK_INIT;
static uint32_t held_at(uint64_t dst) { return (uint32_t)((dst >> 12) * 0x9e3779b1u) & (TILES_HELD - 1); }
/* the slot of dst (made if new); -1: the table is full (counted as dropped) (held_lock exclusive) */
static int held_slot(uint64_t dst) {
    uint32_t i = held_at(dst);
    for (int k = 0; k < TILES_HELD; k++, i = (i + 1) & (TILES_HELD - 1)) {
        if (held[i].dst == dst) return (int)i;
        if (!held[i].dst) { held[i].dst = dst; return (int)i; }
    }
    return -1;
}
/* the bytes copied to dst now kept (the newer ones replace what was kept there) */
static int tile_keep(uint64_t dst, const uint8_t *src, uint32_t n) {
    uint8_t *b = (uint8_t *)malloc(n);
    if (!b) return 0;
    memcpy(b, src, n);
    AcquireSRWLockExclusive(&held_lock);
    const int i = held_slot(dst);
    if (i < 0) {
        ReleaseSRWLockExclusive(&held_lock);
        free(b);
        InterlockedIncrement64(&n_tile_dropped);
        return 0;
    }
    if (held[i].bytes) free(held[i].bytes);
    else {
        const long live = InterlockedIncrement(&direct_assets_tiles_pending);
        if (live > n_tile_held_max) n_tile_held_max = live;
    }
    held[i].bytes = b, held[i].n = n;
    ReleaseSRWLockExclusive(&held_lock);
    return 1;
}
static void tile_upload(const nvn_tex *t, const uint8_t *src, uint32_t n);
static void tile_upload_as(uint32_t source, const nvn_tex *tp, const uint8_t *src, uint32_t n);
static uint64_t tile_linear(const nvn_tex *t) {
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    const uint32_t layers = t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY ? (t->d.depth ? t->d.depth : 1) : 1;
    return bpb > 0 ? (uint64_t)((t->d.width + bw - 1) / bw) * ((t->d.height + bh - 1) / bh) * (uint64_t)bpb * layers : 0;
}
void direct_assets_texture_init(const void *tex) {
    const nvn_tex *t = (const nvn_tex *)tex;
    if (!t->gpu) return;
    if (direct_assets_tilelog && t->d.width == 260) {   /* (M6f) HOS_DBG=tilelog: tile-sized textures initialized */
        extern volatile LONG64 hos_present_count;
        hos_log("tilelog: present %lld init %#llx %ux%ux%u format %#x serial %llu", (long long)hos_present_count,
                (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.depth, t->d.format, (unsigned long long)t->serial);
    }
    if (!direct_assets_tiles_pending) return;
    const uint64_t want = tile_linear(t);
    int given = 0, other = 0;
    AcquireSRWLockShared(&held_lock);   /* (the upload under it: a copy to that storage replaces the bytes only exclusively) */
    for (uint32_t i = held_at(t->gpu), k = 0; k < TILES_HELD && held[i].dst; k++, i = (i + 1) & (TILES_HELD - 1))
        if (held[i].dst == t->gpu) {
            /* (a texture of another size there: its initialization writes no memory, so the bytes stay for a later one) */
            if (held[i].bytes && held[i].n == want) tile_upload_as(HSD_TEXELS_TERRAIN_KEPT, t, held[i].bytes, held[i].n), given = 1;
            else other = held[i].bytes != NULL;
            break;
        }
    ReleaseSRWLockShared(&held_lock);
    if (given) InterlockedIncrement64(&n_tile_kept_given);
    if (other) InterlockedIncrement64(&n_tile_kept_other);
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
    {   /* (M6f) HOS_DBG=tilelog (diagnosis): every tile memcpy, where it went */
        static int tl = -1;
        if (tl < 0) tl = dbg_has("tilelog");
        if (tl) {
            extern volatile LONG64 hos_present_count;
            nvn_tex lt;
            const int live = nvn_tex_live_at(dst, &lt) || nvn_tex_live_containing(dst, &lt);
            hos_log("tilelog: present %lld memcpy dst %#llx %u bytes: %s %#llx %ux%ux%u format %#x serial %llu", (long long)hos_present_count,
                    (unsigned long long)dst, n, live ? "live texture" : "no texture (held)", live ? (unsigned long long)lt.gpu : 0ull,
                    live ? lt.d.width : 0, live ? lt.d.height : 0, live ? lt.d.depth : 0, live ? lt.d.format : 0,
                    live ? (unsigned long long)lt.serial : 0ull);
        }
    }
    const int kept = tile_keep(dst, GP(src), n);   /* (M6f: what the storage holds now, for every texture made over it) */
    if (!nvn_tex_live_at(dst, &t) && !nvn_tex_live_containing(dst, &t)) {
        /* (its texture is initialized after the copy: the kept bytes go to it then, direct_assets_texture_init) */
        if (!kept) {
            if (InterlockedIncrement64(&n_tile_notex) <= 5)
                hos_log("assets: terrain: no texture at %#llx (a tile of %u bytes) and no room to keep it: left alone",
                        (unsigned long long)dst, n);
            return;
        }
        InterlockedIncrement64(&n_tile_held);
        c->x[2] = 0;
        return;
    }
    if (dst != t.gpu && InterlockedIncrement64(&n_tile_inside) <= 5)
        hos_log("assets: terrain: a tile at %#llx inside texture %#llx (%ux%ux%u, format %#x, target %d, %llu bytes, layer %llu bytes)",
                (unsigned long long)dst, (unsigned long long)t.gpu, t.d.width, t.d.height, t.d.depth, t.d.format, t.d.target,
                (unsigned long long)t.L.size, (unsigned long long)t.L.layer_size);
    tile_upload(&t, GP(src), n);
    c->x[2] = 0;   /* (the memcpy copies nothing: the storage stays unwritten) */
}

static void tile_upload_as(uint32_t source, const nvn_tex *tp, const uint8_t *src, uint32_t n);
static void tile_upload(const nvn_tex *tp, const uint8_t *src, uint32_t n) { tile_upload_as(HSD_TEXELS_TERRAIN, tp, src, n); }
static void tile_upload_as(uint32_t source, const nvn_tex *tp, const uint8_t *src, uint32_t n) {
    const nvn_tex t = *tp;
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(t.d.format, &bw, &bh);
    const uint32_t layers = t.d.target == NVN_TEXTURE_TARGET_2D_ARRAY ? (t.d.depth ? t.d.depth : 1) : 1;
    const uint64_t linear = (uint64_t)((t.d.width + bw - 1) / bw) * ((t.d.height + bh - 1) / bh) * (uint64_t)bpb * layers;
    NVNcopyRegion r = {0, 0, 0, (int)t.d.width, (int)t.d.height, (int)layers};
    if (bpb > 0 && n == linear) {
        rx_direct()->texels(source, &t, 0, 0, &r, src, 0, 0);
        InterlockedIncrement64(&n_tiles);
        InterlockedAdd64(&n_tile_bytes, n);
    } else {
        rx_direct()->texels(HSD_TEXELS_TERRAIN, &t, 0, 0, &r, NULL, 0, 0);
        InterlockedIncrement64(&n_tiles_unconv);
    }
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
