/* agl::TextureDataMgr's own texture events, hooked (direct mode only; docs/direct/texture-streaming.md 7).
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
   What frees VRAM is the game's own destroy: f_8079bbe0 -> f_8079c220 finalizes the texture's NVN object (an alias at its
   release, f_80517d80, likewise), and that finalize releases the texture's hold on its image; an image (shared by every
   texture of its content) is destroyed, with its views and heap entries, once its last texture is gone and the frames
   that may still read it have completed (direct_image.cpp, image_sweep). Released to the LRU cache is not a free: the
   game may take the texture back without a load, so its image stays. The back end has no eviction policy of its own (the
   user: no own eviction, no async upload). */
#include "hos.h"
#include "direct.h"
#include "../gpu/api/rx_nvn.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
void f_807981f0(Ctx *), f_80797240(Ctx *);   /* (lifted functions the host calls by name: a code pointer is a host function pointer) */

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
PATCH_AT(0x8079aa20, word(0xd503237f), before, none, direct_texdata_evictable, as(x0, slot), as(x1, evictable)) {
    if (!rx_direct() || !direct_assets_on()) return;
    const uint64_t td = slot - 0x18;
    const int to_cache = (evictable & 1) != 0;
    if (!hos_guest_range(td, 0x1d0)) return;
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
    if (!known) {
        InterlockedIncrement64(&n_unknown);
        return;
    }
    InterlockedIncrement64(to_cache ? &n_to_cache : &n_from_cache);
    const nvn_tex *t = tex_of(td);
    if (changed && t && rx_direct()->texture_cached) rx_direct()->texture_cached(t, to_cache);
}

/* f_8079bbe0(mgr, td): destroyed (its NVN texture is finalized inside, and released as any finalize) */
PATCH_AT(0x8079bbe0, word(0xd503237f), before, none, direct_texdata_destroyed, as(x1, td)) {
    if (!rx_direct() || !direct_assets_on()) return;
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
}

/* f_807981f0(td, ctx): a move (compaction apply, each alias after its head). Its body runs from here, with the td's NVN
   object noted for this thread: nvn_native.c then keeps the object's serial through the rebind's Finalize + Initialize
   and tells the back end nothing (its image and registration follow the object). */
static _Thread_local const void *tl_moving;
const void *direct_texdata_moving(void) { return tl_moving; }
PATCH_FUNC(0x807981f0, word(0xd503237f), body(0xd8e8d96c2812b6c3), aapcs, x0, direct_texdata_move,
           orig, as(x0, td), as(x1, ctx), x2, x3, x4, x5, x6, x7) {
    if (!rx_direct() || !direct_assets_on()) return guest_orig(orig, td, ctx, x2, x3, x4, x5, x6, x7);
    const nvn_tex *t = hos_guest_range(td, 0x1d0) ? tex_of(td) : NULL;
    tl_moving = t && t->magic == NVN_MAGIC('T', 'E', 'X', 0) && t->serial ? (const void *)t : NULL;
    const uint64_t r = guest_orig(orig, td, ctx, x2, x3, x4, x5, x6, x7);
    tl_moving = NULL;
    InterlockedIncrement64(&n_moves);
    return r;
}

void direct_texdata_line(char *out, size_t cap) {
    snprintf(out, cap, "texture events: created %lld, aliases %lld, moved %lld, released to the cache %lld, taken back %lld, "
             "destroyed %lld; in the cache now %lld (unknown tds %lld, table full %lld)", (long long)n_created, (long long)n_aliases,
             (long long)n_moves, (long long)n_to_cache, (long long)n_from_cache, (long long)n_destroyed, (long long)n_cached_now,
             (long long)n_unknown, (long long)n_full);
}

/* (game patch) The cache eviction driven by free VRAM, not by the texture heap's bytes. agl's per-frame update
   f_80797240(mgr) destroys cached (count 0) textures, through its own f_8079bbe0, while the heap's free bytes
   ([mgr+0x40]+0x1ee8) are under mgr+0x340 (TextureCacheRejectionThreshold, 120 MB) and compaction is idle
   (docs/direct/texture-streaming.md 2.6). The heap's bytes say nothing about VRAM here (the store's BC images are 1-2x the
   console's bytes, the console's texels are never copied). So for that one call the threshold is raised by what free
   VRAM lacks of kVramMargin: the game then evicts LRU textures until its heap has that many more bytes free (or nothing
   is cached), in its own order and through its own destroy; the value is put back after the call, so the game's own
   heap-pressure path (and its allocation-failure eviction, f_83537760) is unchanged. Free VRAM is the back end's
   figure (hsd_vram_free: the driver's budget minus usage, so other processes count, plus images already waiting to be
   destroyed, which are freed within a few presents). The back end has no policy: it reports the number and frees what
   the game destroys.
   kVramMargin, 1.5 GB: what a scene change allocates before the game releases the old scene's textures (the scene route,
   m6f_s5_d: the device-local usage rose by 0.6-1.4 GB across a load, store images 0.6 GB and the new scene's targets),
   so that a load that starts at the margin does not exhaust VRAM. */
#define kVramMargin (1536ll << 20)
static volatile LONG64 n_vram_evicted, n_vram_frames, last_free_vram = -1, last_line;
PATCH_FUNC(0x80797240, word(0xd503237f), body(0x99d8ac1665d1c349), aapcs, x0, direct_texdata_update,
           orig, as(x0, mgr), x1, x2, x3, x4, x5, x6, x7) {
    if (!rx_direct() || !direct_assets_on() || !rx_direct()->vram_free) return guest_orig(orig, mgr, x1, x2, x3, x4, x5, x6, x7);
    if (!hos_guest_range(mgr + 0x340, 8) || !hos_guest_range(G64(mgr + 0x40) + 0x1ee8, 8)) return guest_orig(orig, mgr, x1, x2, x3, x4, x5, x6, x7);
    const int64_t vf = rx_direct()->vram_free();
    last_free_vram = vf;
    const uint64_t game_thr = G64(mgr + 0x340), heap_free = G64(G64(mgr + 0x40) + 0x1ee8);
    int raised = 0;
    if (vf >= 0 && vf < kVramMargin && n_cached_now > 0) {
        const uint64_t want = heap_free + (uint64_t)(kVramMargin - vf);
        if (want > game_thr) G64(mgr + 0x340) = want, raised = 1;
    }
    const LONG64 d0 = n_destroyed;
    const uint64_t r = guest_orig(orig, mgr, x1, x2, x3, x4, x5, x6, x7);
    if (raised) {
        G64(mgr + 0x340) = game_thr;
        InterlockedAdd64(&n_vram_evicted, n_destroyed - d0);
        InterlockedIncrement64(&n_vram_frames);
    }
    if (hos_present_count / 600 != last_line / 600) {
        last_line = hos_present_count;
        hos_log("assets: VRAM eviction patch: free VRAM %lld MB (margin %lld MB), textures in agl's cache %lld, evicted by the "
                "patch %lld in %lld frames", (long long)(vf >> 20), (long long)(kVramMargin >> 20), (long long)n_cached_now,
                (long long)n_vram_evicted, (long long)n_vram_frames);
    }
    return r;
}
