/* (native-vk M6f) agl::TextureDataMgr's own texture events, hooked (direct mode only; docs/direct/texture-streaming.md 7).
   One table keyed by the TextureData (td), written only by these hooks:
     - created with data, or as a UnifyTex alias: f_80798690 loadImage_ (direct_assets.c calls direct_texdata_created; an
       alias's target is the head of its chain, direct_tex_dedup.c);
     - moved by compaction (the frame update f_80797da0 -> f_807981f0(td, ctx) for each moved texture and each alias):
       the rebind's nvnTextureFinalize + nvnTextureInitialize of the td's own NVN object reach the back end as nothing at
       all: the object keeps its serial, so its image (keyed by its record or plain image, never by storage) and its
       registration follow it (nvn_native.c asks direct_texdata_moving);
     - released to the LRU cache / taken back from it: f_8079aa20(&td->slot, 1 / 0), called from the release f_80517d80 and
       the acquire paths; the back end is told (hsd_texture_cached), which counts the cached textures' images;
     - destroyed: f_8079bbe0(mgr, td) (eviction, or a lost load race): its nvnTextureFinalize follows and releases as before.
   Nothing here evicts (the user: eviction and async upload are far-future). With RX_BACKEND unset nothing here runs. */
#include "hos.h"
#include "direct.h"
#include "../gpu/render/rx_nvn.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

extern volatile LONG64 hos_present_count;

#define TD_N (1 << 15)   /* (TexatureHandleNum is 10,240 textures; aliases have tds of their own too) */
enum { TD_LIVE = 1, TD_CACHED = 2, TD_ALIAS = 3 };
static struct { uint64_t td; uint8_t state; } tds[TD_N];   /* (open addressing; a destroyed td's entry stays as a tombstone, state 0) */
static SRWLOCK lock = SRWLOCK_INIT;
static volatile LONG64 n_created, n_aliases, n_moves, n_to_cache, n_from_cache, n_destroyed, n_cached_now, n_full, n_unknown;

static uint32_t td_at(uint64_t td) { return (uint32_t)((td >> 3) * 0x9e3779b1u) & (TD_N - 1); }
static int td_slot(uint64_t td, int make) {   /* (lock held exclusively when make) */
    uint32_t i = td_at(td);
    for (int k = 0; k < TD_N; k++, i = (i + 1) & (TD_N - 1)) {
        if (tds[i].td == td) return (int)i;
        if (!tds[i].td) {
            if (!make) return -1;
            tds[i].td = td;
            return (int)i;
        }
    }
    return -1;
}
static const nvn_tex *tex_of(uint64_t td) {
    const uint64_t gfx = hos_guest_range(td + 0xd8, 8) ? G64(td + 0xd8) : 0;
    return gfx && hos_guest_range(gfx + 0x10, sizeof(nvn_tex)) ? (const nvn_tex *)GP(gfx + 0x10) : NULL;
}

void direct_texdata_created(uint64_t td, int alias) {
    AcquireSRWLockExclusive(&lock);
    const int i = td_slot(td, 1);
    if (i >= 0) {
        if (tds[i].state == TD_CACHED) InterlockedDecrement64(&n_cached_now);
        tds[i].state = alias ? TD_ALIAS : TD_LIVE;
    } else InterlockedIncrement64(&n_full);
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement64(alias ? &n_aliases : &n_created);
}

/* f_8079aa20(&td->slot, evictable): released to the LRU cache (1), taken back from it (0) */
ENTRY_HOOK(0x8079aa20) {
    if (!rx_direct() || !direct_assets_on()) return 0;
    const uint64_t td = ARG(0) - 0x18;
    const int to_cache = (ARG(1) & 1) != 0;
    if (!hos_guest_range(td, 0x1d0)) return 0;
    int known = 0, changed = 0;
    AcquireSRWLockExclusive(&lock);
    const int i = td_slot(td, 0);
    if (i >= 0 && tds[i].state) {
        known = 1;
        const uint8_t want = to_cache ? TD_CACHED : TD_LIVE;
        if (tds[i].state != want && tds[i].state != TD_ALIAS) {
            changed = 1;
            tds[i].state = want;
            if (to_cache) InterlockedIncrement64(&n_cached_now);
            else InterlockedDecrement64(&n_cached_now);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    if (!known) return InterlockedIncrement64(&n_unknown), 0;
    InterlockedIncrement64(to_cache ? &n_to_cache : &n_from_cache);
    const nvn_tex *t = tex_of(td);
    if (changed && t && rx_direct()->texture_cached) rx_direct()->texture_cached(t, to_cache);
    return 0;
}

/* f_8079bbe0(mgr, td): destroyed (its NVN texture is finalized inside, and released as any finalize) */
ENTRY_HOOK(0x8079bbe0) {
    if (!rx_direct() || !direct_assets_on()) return 0;
    const uint64_t td = ARG(1);
    int was_cached = 0;
    AcquireSRWLockExclusive(&lock);
    const int i = td_slot(td, 0);
    if (i >= 0) {
        was_cached = tds[i].state == TD_CACHED;
        if (was_cached) InterlockedDecrement64(&n_cached_now);
        tds[i].state = 0;
    }
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement64(&n_destroyed);
    const nvn_tex *t = tex_of(td);
    if (was_cached && t && rx_direct()->texture_cached) rx_direct()->texture_cached(t, 0);
    return 0;
}

/* f_807981f0(td, ctx): a move (compaction apply, each alias after its head). Its body runs from here, with the td's NVN
   object noted for this thread: nvn_native.c then keeps the object's serial through the rebind's Finalize + Initialize
   and tells the back end nothing (its image and registration follow the object). */
static _Thread_local const void *tl_moving;
static _Thread_local int tl_in_move;
const void *direct_texdata_moving(void) { return tl_moving; }
ENTRY_HOOK(0x807981f0) {
    if (tl_in_move || !rx_direct() || !direct_assets_on()) return 0;
    static A64Fn body;
    if (!body) body = a64_lookup(0x807981f0ull);
    if (!body) return 0;
    const uint64_t td = ARG(0);
    const nvn_tex *t = hos_guest_range(td, 0x1d0) ? tex_of(td) : NULL;
    tl_moving = t && t->magic == NVN_MAGIC('T', 'E', 'X', 0) && t->serial ? (const void *)t : NULL;
    tl_in_move = 1;
    body(c);
    tl_in_move = 0;
    tl_moving = NULL;
    InterlockedIncrement64(&n_moves);
    return 1;
}

void direct_texdata_line(char *out, size_t cap) {
    snprintf(out, cap, "texture events (M6f): created %lld, aliases %lld, moved %lld, released to the cache %lld, taken back %lld, "
             "destroyed %lld; in the cache now %lld (unknown tds %lld, table full %lld)", (long long)n_created, (long long)n_aliases,
             (long long)n_moves, (long long)n_to_cache, (long long)n_from_cache, (long long)n_destroyed, (long long)n_cached_now,
             (long long)n_unknown, (long long)n_full);
}
