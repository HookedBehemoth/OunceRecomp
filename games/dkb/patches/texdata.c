/* agl::TextureDataMgr's own texture events, hooked (direct mode only; docs/direct/texture-streaming.md).
   One table keyed by the TextureData (td), written only by these hooks:
     - created: loadImage_ (assets.c calls texdata_created);
     - moved by compaction (f_815ec5e8(td, base, res, ctx), which initializes the td's NVN object again, through rebind_
       when it is live): the rebind's nvnTextureFinalize + nvnTextureInitialize of the same NVN object reach the back end
       as nothing at all: the object keeps its serial, so its image and registration follow it (nvn_native.c asks
       game_texture_moving);
     - released to the LRU cache / taken back from it: texheap_mark_evictable f_815f0934(heap, [td+0x18], flag), which
       finds the td's heap block by its address; At rows before its calls that have the td in a register (texdata_destroy's
       and two others' loops over a td's alias chain, and f_802104d4's) name the td; the back end is told
       (gpu_texture_cached), which counts the cached textures' images;
     - destroyed: texdata_evict f_806b73ac(mgr, td) (eviction, or a lost load race): its nvnTextureFinalize follows.
   The back end has no eviction policy of its own; what frees VRAM is the game's own destroy. */
#include "hos.h"
#include "patches.h"
#include "rx_nvn.h"
#include <stdio.h>
#include <string.h>
#include "os.h"

extern volatile int64_t hos_present_count;

#define TD_N (1 << 15)
enum { TD_LIVE = 1, TD_CACHED = 2 };
static struct { uint64_t td; uint8_t state; } tds[TD_N];   /* (open addressing; a destroyed td's entry stays as a tombstone, state 0) */
static os_mutex lock = OS_MUTEX_INIT;
static volatile int64_t n_created, n_moves, n_to_cache, n_from_cache, n_destroyed, n_cached_now, n_full, n_unknown;

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

void texdata_created(uint64_t td) {
    os_mutex_lock(&lock);
    const int i = td_slot(td, 1);
    if (i >= 0) {
        if (tds[i].state == TD_CACHED) os_atomic_dec(&n_cached_now);
        tds[i].state = TD_LIVE;
    } else os_atomic_inc(&n_full);
    os_mutex_unlock(&lock);
    os_atomic_inc(&n_created);
}

/* texheap_mark_evictable's callers: the td (its register p, less back), released to the LRU cache (flag 1) or taken
   back from it (0) */
PATCH_AT(0x806b7774, word(0x943ce470), before, none, texdata_evictable, as(x22, p), as(k(0), back), as(w2, flag)) {
    if (!assets_on()) return;
    const uint64_t td = p - back;
    const int to_cache = (flag & 1) != 0;
    if (!hos_guest_range(td, 0x1d0)) return;
    int known = 0, changed = 0;
    os_mutex_lock(&lock);
    const int i = td_slot(td, 0);
    if (i >= 0 && tds[i].state) {
        known = 1;
        const uint8_t want = to_cache ? TD_CACHED : TD_LIVE;
        if (tds[i].state != want) {
            changed = 1;
            tds[i].state = want;
            if (to_cache) os_atomic_inc(&n_cached_now);
            else os_atomic_dec(&n_cached_now);
        }
    }
    os_mutex_unlock(&lock);
    if (!known) {
        os_atomic_inc(&n_unknown);
        return;
    }
    os_atomic_inc(to_cache ? &n_to_cache : &n_from_cache);
    const nvn_tex *t = tex_of(td);
    if (changed && t && rx_direct()->texture_cached) rx_direct()->texture_cached(t, to_cache);
}
PATCH_AT_SITE(0x815ec24c, word(0x940011ba), before, none, texdata_evictable, x22, k(0), w2);
PATCH_AT_SITE(0x815ecd28, word(0x94000f03), before, none, texdata_evictable, x19, k(0), w2);
PATCH_AT_SITE(0x8021061c, word(0x944f80c6), before, none, texdata_evictable, x24, k(0x18), w2);   /* (x24: &td->slot) */

/* texdata_evict(mgr, td): destroyed (its NVN texture is finalized inside, and released as any finalize) */
PATCH_AT(0x806b73ac, word(0xd503237f), before, none, texdata_destroyed, as(x1, td)) {
    if (!assets_on()) return;
    int was_cached = 0;
    os_mutex_lock(&lock);
    const int i = td_slot(td, 0);
    if (i >= 0) {
        was_cached = tds[i].state == TD_CACHED;
        if (was_cached) os_atomic_dec(&n_cached_now);
        tds[i].state = 0;
    }
    os_mutex_unlock(&lock);
    os_atomic_inc(&n_destroyed);
    const nvn_tex *t = tex_of(td);
    if (was_cached && t && rx_direct()->texture_cached) rx_direct()->texture_cached(t, 0);
}

/* f_815ec5e8(td, base, res, ctx): a move. Its body runs from here, with the td's NVN object noted for this thread:
   nvn_native.c then keeps the object's serial through the rebind's Finalize + Initialize and tells the back end
   nothing (its image and registration follow the object). */
static _Thread_local const void *tl_moving;
const void *game_texture_moving(void) { return tl_moving; }
PATCH_FUNC(0x815ec5e8, word(0xd503237f), body(0x380f2bb47fa420fe), aapcs, x0, texdata_move, orig, as(x0, td)) {
    if (!assets_on()) return guest_orig(orig);
    const nvn_tex *t = hos_guest_range(td, 0x1d0) ? tex_of(td) : NULL;
    tl_moving = t && t->magic == NVN_MAGIC('T', 'E', 'X', 0) && t->serial ? (const void *)t : NULL;
    const uint64_t r = guest_orig(orig);
    tl_moving = NULL;
    os_atomic_inc(&n_moves);
    return r;
}

void texdata_line(char *out, size_t cap) {
    snprintf(out, cap, "texture events: created %lld, moved %lld, released to the cache %lld, taken back %lld, destroyed %lld; "
             "in the cache now %lld (unknown tds %lld, table full %lld)", (long long)n_created, (long long)n_moves,
             (long long)n_to_cache, (long long)n_from_cache, (long long)n_destroyed, (long long)n_cached_now,
             (long long)n_unknown, (long long)n_full);
}

/* The cache eviction driven by free VRAM, not by the texture heap's bytes. agl's per-frame update texdata_update
   f_8033e9e4(mgr) destroys cached textures, through texdata_evict, while the heap's free bytes ([mgr+0x40]+0x1bd8) are
   under mgr+0x340 (the rejection threshold) and compaction is idle. The heap's bytes say nothing about VRAM here (the
   console's texels are never copied). So for that one call the threshold is raised by what free VRAM lacks of
   kVramMargin: the game then evicts LRU textures in its own order and through its own destroy; the value is put back
   after the call, so the game's own heap-pressure path is unchanged. Free VRAM is the back end's figure (gpu_vram_free).
   kVramMargin, 1.5 GB: what a scene change allocates before the game releases the old scene's textures (OoT's figure). */
#define kVramMargin (1536ll << 20)
static volatile int64_t n_vram_evicted, n_vram_frames;
PATCH_FUNC(0x8033e9e4, word(0xd503237f), body(0x0b7e88588f9ed8de), aapcs, x0, texdata_update, orig, as(x0, mgr)) {
    if (!assets_on() || !rx_direct()->vram_free) return guest_orig(orig);
    if (!hos_guest_range(mgr + 0x340, 8) || !hos_guest_range(G64(mgr + 0x40) + 0x1bd8, 8)) return guest_orig(orig);
    const int64_t vf = rx_direct()->vram_free();
    const uint64_t game_thr = G64(mgr + 0x340), heap_free = G64(G64(mgr + 0x40) + 0x1bd8);
    int raised = 0;
    if (vf >= 0 && vf < kVramMargin && n_cached_now > 0) {
        const uint64_t want = heap_free + (uint64_t)(kVramMargin - vf);
        if (want > game_thr) G64(mgr + 0x340) = want, raised = 1;
    }
    const int64_t d0 = n_destroyed;
    const uint64_t r = guest_orig(orig);
    if (raised) {
        G64(mgr + 0x340) = game_thr;
        os_atomic_add(&n_vram_evicted, n_destroyed - d0);
        os_atomic_inc(&n_vram_frames);
    }
    return r;
}
void texdata_vram_line(char *out, size_t cap) {
    snprintf(out, cap, "VRAM eviction patch: margin %lld MB, textures in agl's cache %lld, evicted by the patch %lld in %lld "
             "frames", (long long)(kVramMargin >> 20), (long long)n_cached_now, (long long)n_vram_evicted, (long long)n_vram_frames);
}
