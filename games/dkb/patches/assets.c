/* The host's side of prepared assets for the direct back end, direct mode only (HOS_DBG=noassets turns every patch
   here off, for a before/after).

   The store: HOS_PREPARED=<folder> (default the game folder's prepared/, else <romfs>/../../prepared): <folder>/tex is
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
   Packaged texture data (nvnTextureBuilderSetPackagedTextureData) is not copied into the storage (nvn_native.c).
   Not covered yet: in-place BNTX and ui2d resource files, and the font glyph cache (their texels stay unwritten). */
#include "hos.h"
#include "patches.h"
#include "rx_nvn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

int hos_gpu_pools(uint64_t (*out)[2], int max);
extern const char *hos_romfs_dir;

static volatile int64_t n_loads, n_load_skipped, n_load_bytes, n_rebind, n_compaction, n_compaction_cleared, n_pkg,
    n_pkg_bytes;

static int dbg_has(const char *name) {
    const char *d = getenv("HOS_DBG");
    if (!d) return 0;
    const size_t n = strlen(name);
    for (const char *p = d; (p = strstr(p, name)); p += n)
        if ((p == d || p[-1] == ',') && (p[n] == 0 || p[n] == ',' || p[n] == ':')) return 1;
    return 0;
}

static int on_ = -1;
int assets_on(void) {
    if (on_ < 0) on_ = rx_direct() && !dbg_has("noassets");
    return on_;
}

/* ---- the prepared folder ---- */
static char prepared[1024];
static const char *prepared_dir(void) {
    const char *e = getenv("HOS_PREPARED");
    if (e && *e) snprintf(prepared, sizeof prepared, "%s", e);
    else {   /* (the game folder's prepared/, then beside the romfs) */
        char exe[1024], probe[1100];
        const int n = os_exe_path(exe, sizeof exe);
        char *slash = n ? strrchr(exe, '/') : NULL;
        if (slash) {
            *slash = 0;
            snprintf(probe, sizeof probe, "%s/prepared/tex/index.bin", exe);
            if (os_path_kind(probe) != OS_PATH_NONE) snprintf(prepared, sizeof prepared, "%s/prepared", exe);
        }
        if (!prepared[0]) snprintf(prepared, sizeof prepared, "%s/../../prepared", hos_romfs_dir ? hos_romfs_dir : "romfs");
    }
    char real[1024];
    if (realpath(prepared, real)) snprintf(prepared, sizeof prepared, "%s", real);
    hos_log("assets: the prepared store: %s (HOS_PREPARED)", prepared);
    return prepared;
}

/* ---- the 600-present line ---- */
void assets_log(void) {
    char b[512];
    texdata_line(b, sizeof b);
    hos_log("assets: copies skipped: loads %lld of %lld (%lld MB), packaged %lld (%lld MB), compaction steps %lld (copy flag "
            "cleared %lld); rebinds %lld%s; %s", (long long)n_load_skipped, (long long)n_loads, (long long)(n_load_bytes >> 20),
            (long long)n_pkg, (long long)(n_pkg_bytes >> 20), (long long)n_compaction, (long long)n_compaction_cleared,
            (long long)n_rebind, assets_on() ? "" : " (HOS_DBG=noassets: the patches are off)", b);
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

/* 1: the back end has the texture's texels; its src is not copied into the heap */
static int load_image(uint64_t td, uint64_t src) {
    if (!ready() || !hos_guest_range(td, 0x100)) return 0;
    const uint64_t brti = td + 0x58, gfx = G64(td + 0xd8);
    if (hos_guest_range(G64(td + 0x18), 8)) find_heap(G64(td + 0x18) + 0x10);
    if (!gfx || G32(brti) != 0x49545242u) return 0;   /* ('BRTI') */
    gpu_asset_key k;
    memset(&k, 0, sizeof k);
    k.tex = GP(gfx + 0x10);
    k.path = GPU_ASSET_LOAD;
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
/* TextureData::loadImage_(td, ctx, src): the game's load (orig), without src when the back end takes the texels */
PATCH_FUNC(0x80210d50, word(0xd503237f), body(0x9cc63809f6c28c83), aapcs, x0, assets_load_image,
           orig, as(x0, td), as(x1, ctx), as(x2, src)) {
    return guest_orig(orig, td, ctx, load_image(td, src) ? 0 : src);   /* (x2 changed: x0-x2 passed) */
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

int game_skips_packaged_texels(uint64_t bytes) {
    if (!assets_on()) return 0;
    os_atomic_inc(&n_pkg);
    os_atomic_add(&n_pkg_bytes, (int64_t)bytes);
    return 1;
}

/* The texture heap's pools are made inside agl::TextureDataMgr::InitializeHeap (assets_heap_init f_80577140), through
   agl's pool wrapper f_80577b30: named to the back end at their creation, before layer 0 binds pools, so that it leaves
   them unbound (it takes every texel of them from the store and never reads them). */
static _Thread_local int tl_heap_init;
PATCH_FUNC(0x80577140, word(0xd503237f), body(0xd76e3c5e7f45b5b0), aapcs, x0, assets_heap_init, orig) {
    if (!assets_on()) return guest_orig(orig);
    tl_heap_init = 1;
    const uint64_t r = guest_orig(orig);
    tl_heap_init = 0;
    return r;
}
/* (in f_80577b30, after ldr x8, [x8] before its blr to nvnMemoryPoolInitialize at 0x80577be0) the next pool this thread
   makes is agl's wrapper's */
static _Thread_local int tl_agl_pool;
PATCH_AT(0x80577bdc, word(0xf9400108), after, none, assets_agl_pool) { tl_agl_pool = 1; }
void game_pool_made(uint64_t gpu, uint64_t size) {
    const int agl = tl_agl_pool;
    tl_agl_pool = 0;
    if (!agl || !tl_heap_init || !assets_on()) return;
    hos_log("assets: the texture data pool made: %#llx + %llu MB (agl::TextureDataMgr)", (unsigned long long)gpu,
            (unsigned long long)(size >> 20));
    rx_direct()->texture_data_pool(gpu, gpu + size);
}
