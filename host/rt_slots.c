/* (native-vk) Stable slots for the game's transient render targets (agl::utl::DynamicTextureAllocator).
   Analysis and evidence: work/assets/native/rt-alloc.md; measurements: work/assets/native/rt-slots-impl.md.

   The game's allocator (DTA, the instance at guest 0x84ea1b80) keeps one pool record with 7 arenas (0xe8 bytes each)
   over the SAME 512 MB of its virtual pool: one arena per lane (the worker thread recording the pass; lane 6 for
   persistent allocations), each a first-fit free list. So a target's offset depends on which worker recorded its pass
   and on everything that lane allocated before it in the frame, and lanes alias each other. Every frame ~42 targets
   land somewhere else: new texture objects, new descriptors, new images for the renderer.

   Here the arenas keep all of the game's bookkeeping (budget, peak, collision checks, the per-range history that
   decides its reuse barriers), but the memory a transient texture uses (block +0x38, the offset that goes to
   nvnTextureBuilderSetStorage and into the entry) becomes a slot of our own in the top S bytes of the pool, which the
   arenas are shrunk away from at first use. A slot belongs to one family (caller lr, name, format, type, depth,
   levels) and one live occurrence of it, so a logical target lands at the same address every frame and never shares
   memory with another one: the renderer finds the same (address, shape) each frame and keeps its image. (The game's
   own registration compare at alloc never hits on its own: the entry it picks has no registration. Parking, below,
   keeps the entry with its target, so the ~42 initializations a frame drop to ~2.)

     ENTRY_HOOK(0x8005ed00)       DTA::alloc(dta, ctx, desc, flags): note lr/name/shape for this thread (flags bit 0:
                                  transient); the first call shrinks the arenas. Returns 0 (the lifted body runs).
     SPLICE_AFTER_EACH(0x8005ed00) the two lines (0x8005fb74, 0x80061814) that store block +0x38 = the aligned offset:
                                  x23 block, x10 offset, x8 the block's pool. Both are rewritten to the slot.
     ENTRY_HOOK(0x802737a0)       DTA::free(dta, entry): the slot of entry +0xc0's block is free again.

   Persistent allocations (f_80062c50, from the top of the free block) do not set the note and are left to the game.
   Any mismatch (arenas not fresh, a pool smaller than the slot range, an arena offset inside it, a second pool record)
   turns the slots off for new allocations: those take the game's offsets again, which lie below the slot range.
   HOS_GUEST_RT_ALLOC=1: the game's allocator untouched (A/B). RX_RT_SLOTS_MB: S, default 320 (of 512; the arenas
   keep 192 MB, the fullest seen used 111). Parking and the shadow size classes: below.

   The transient pool (direct mode; docs/direct/shadow-pool-report.md). The slot range is one device allocation in the
   back end, made when the range is first known (hsd_transient_pool), and every texture initialized in a slot gets an
   image bound in it at the slot's offset: the targets cost that constant and nothing else, whatever the game's free
   entries still name. For that
   - a slot is sized by what the device's image of its target needs (device_need: the back end's answer for the
     target's NVN format and shape), where that is more than the block the game's arena gave;
   - each slot's bytes go to the back end before a texture is initialized in it (rt_slots_texture_init);
   - a slot's memory goes to another image (another shape of its family, or another slot made over it after an
     eviction) only REUSE_AFTER presents after its last use, so the lists that drew or sampled the last one are sent
     (place's eviction, take's reuse of a slot by another shape);
   - each entry the allocator gives out is told to the back end by its own texture object (alloc_gave: the entry's
     descriptor id -> agl's record -> the object), which claims the range for its image if another has had it since.
   The game's free tells the back end nothing: the memory is the pool's, and a freed target's image is only an object.
   HOS_RT_SLOTS_LOG=1: every slot made, with its name, and why the allocator's first initializations after 1000
   presents were not skipped. Addresses and offsets are HOS_GAME_VERSION 100's (the only one);
   the splice's anchor stops the build on a lift without it. */
#include "hos.h"
#include "../gpu/render/rx_nvn.h"
#include "direct.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
uint64_t hos_call_guest(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);   /* hos_hostcall.c */

#define DTA_NRECS 0x2a48        /* u32 pool records */
#define DTA_RECS 0x2a50         /* u64 -> array of pool record pointers */
#define REC_ARENAS 0x50         /* pool record: u64 -> arenas[REC_NARENAS] of 0xe8 */
#define REC_NARENAS 0x48
#define AR_END 0x28             /* arena: u64 end (= first block offset + size at init) */
#define AR_BYTES 0x30           /* u64 bytes in use */
#define AR_USED 0x68            /* u32 blocks in use */
#define AR_FREE_HEAD 0x70       /* u64 first free-list node */
#define AR_FREE_N 0x80          /* u32 free blocks */
#define AR_NODE_OFF 0x84        /* s32 node offset in a block */
#define BLK_OFF 0x10            /* block: bookkeeping offset */
#define BLK_SIZE 0x30           /* u64 size the arena gave it */
#define ENT_BLOCK 0xc0          /* TextureData entry: u64 its block */
#define SP_ENTRY 40             /* alloc's frame: the entry, from 0x80060308 on (before: the descriptor cache's candidate) */
#define SP_ALIGN 192            /* alloc's frame: s32 the TextureData's storage alignment */

/* HOS_NATIVE_TEXALLOC=1 (experiment, off): the game's lane arenas stop being a budget. A transient target's memory is a
   slot of ours; its arena block is bookkeeping only (the range history, the game's counters), so the six lane arenas
   (0-5) get a free block of 1 TB of bookkeeping offsets, the persistent arena (the last: lane 6, persistent targets
   take real memory from the top of its free block) keeps its real 192 MB below the slots, and the arenas' spare block
   descriptors (+0x88, which a split takes: none left writes near null, 0x80061068) are topped up from guest memory
   of ours. No alloc can then find no block (the 0x8005fd14 path that gave block 0). A target the slots cannot place
   takes a parked entry's slot (that entry is released later), or keeps the game's offset only when it lies in real
   memory below the slots. work/assets/native/cores.md ("A native texture allocator") */
#define HUGE_ARENA (1ull << 40)
static int native_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_NATIVE_TEXALLOC") && atoi(getenv("HOS_NATIVE_TEXALLOC")) > 0;
    return v;
}
static int off_(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_GUEST_RT_ALLOC") && atoi(getenv("HOS_GUEST_RT_ALLOC")) != 0;
    return v;
}
/* Direct mode: the slots lie after the pool's end (offsets past its size: nvn_native.c leaves those GPU addresses
   unused after every virtual pool), the arenas keep the whole pool for their bookkeeping, and the range is the back
   end's transient pool: SLOTS_MB_DIRECT of device memory, the constant the transient targets cost
   (docs/direct/shadow-pool-report.md, "The constant"). Another back end: the top 320 MB of the pool, as before */
#define SLOTS_MB_DIRECT 512
uint64_t nvn_virtual_pool_tail(void);   /* nvn_native.c */
static int after_pool(void) { return rx_direct() && nvn_virtual_pool_tail() != 0; }
static uint64_t slots_bytes(void) {
    static uint64_t v;
    if (!v) {
        const char *e = getenv("RX_RT_SLOTS_MB");
        const int def = after_pool() ? SLOTS_MB_DIRECT : 320;
        const int mb = e ? atoi(e) : def;
        v = (uint64_t)(mb > 0 ? mb : def) << 20;
        if (after_pool() && v > nvn_virtual_pool_tail()) v = nvn_virtual_pool_tail();
    }
    return v;
}
/* Parking (HOS_RT_PARK=0: off): a transient entry the game frees is kept whole (entry, TextureData and texture object,
   registration, descriptor slot, arena block, memory slot) and handed back by the next alloc with the same caller and
   descriptor, so the game neither re-initializes nor re-registers it. Released for real (the game's own free) when its
   key goes HOS_RT_PARK_FRAMES presents without an alloc (default 6: passes that run every few frames still find
   theirs). */
static int park_on(void) {
    static int v = -1;
    if (v < 0) v = !(getenv("HOS_RT_PARK") && atoi(getenv("HOS_RT_PARK")) == 0);
    return v;
}
static uint64_t park_frames(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_RT_PARK_FRAMES") ? atoi(getenv("HOS_RT_PARK_FRAMES")) : 6;
    return (uint64_t)(v > 0 ? v : 1);
}
/* Local-light shadow maps (f_802640a0, the alloc at lr 0x80264130, "local_shadow_depth") are sized by the light's
   distance: square 384..2432, a new size most frames while a light moves. They get one of a few size classes (the
   next class up, never a smaller map than the game asked for), with hysteresis per light (the shadow object, noted at
   f_802640a0's entry): a light keeps its class until the size wanted passes it, or falls to 90% of the class below. So
   a light keeps one shape for long: parking hands its entry back, and the back end keeps one image for it. Safe
   because the pass takes its viewport and scissor from the texture (f_802640a0 reads entry +0x40 width / +0x42 height,
   not its own size at +0x4e8) and the projection maps the light's frustum to the whole map; only the shadow's texel
   density rises (by less than the step between two classes, at most 1.5 a side). HOS_RT_QUANT=0: off (the game's sizes) */
static int shadow_classes_on(void) {
    static int v = -1;
    if (v < 0) v = !(getenv("HOS_RT_QUANT") && atoi(getenv("HOS_RT_QUANT")) == 0);
    return v;
}
#define LR_LOCAL_SHADOW 0x80264130ull
#define REUSE_AFTER 3   /* presents after a slot's last use before another image may take its memory */
static int log_slots(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_RT_SLOTS_LOG") != NULL;
    return v;
}

typedef struct {
    uint64_t lr, name, dta;
    uint32_t fmt, type, w, h, d, mips, flags, arg;   /* arg: alloc's flags argument */
    uint8_t desc[0x38];                              /* the whole descriptor (parking's key) */
    int valid;
} Cur;
static _Thread_local Cur cur;
static _Thread_local uint64_t alloc_sp, alloc_block;   /* (debug: the splice's frame, for the texture init after it) */

/* A slot belongs to a family (caller lr, name, format, type, depth, levels): one logical target. Its size may change
   (dynamic resolution, local shadows sized by distance), so a family's slot is reused by any of its shapes it holds. */
typedef struct {
    uint64_t fam, off, cap, block;   /* block: the arena block now in it (0: free) */
    uint64_t last_frame, name, lr;
    uint32_t occ, w, h, fmt;
    Cur key;   /* who placed the block in it now (parking keys the entry by it) */
} Slot;
#define MAX_SLOTS 4096
static Slot slots[MAX_SLOTS];
static int nslots;
static uint64_t lo, hi, used, peak;   /* the slot range [lo, hi) of pool offsets; bytes in slots */
static volatile uint64_t frame;
static SRWLOCK mu = SRWLOCK_INIT;
static volatile int state = -1;   /* -1 not tried, 0 off (never or backed off), 1 on */
static uint64_t dta_seen;   /* (the DTA the allocs come for: rt_slots_dump) */
static volatile uint64_t dta_pool_gpu;   /* the DTA pool's GPU address: a slot's storage is this + its offset */
static uint32_t nrecs0, narenas;
static uint64_t arenas0;   /* (the log: the fullest arena) */
static uint64_t pools[16];   /* the virtual pools the slots were checked against */
static int npools;

static struct {   /* counters (under mu, or read racily for the log) */
    uint64_t calls, transient, spliced, unspliced, stale, frees, freed_slot, unseen_free, made, evicted, game_offset,
        same_shape, other_shape, parked, handed_back, released, park_full, quantized, drained, blockless, stolen,
        topped, aliased, moved, evicted_young, class_up, class_down, class_kept, given, given_noid, given_other,
        slot_told, slot_untold, sized, sized_up;
} n;
static uint64_t last_n_calls, last_n_spliced, last_n_inits, last_n_dta_inits, last_n_made, last_frame_log;
extern volatile LONG64 nvn_texture_inits, nvn_texture_inits_dta;   /* (nvn_native.c) */

static void back_off(const char *why, uint64_t a, uint64_t b) {
    if (state == 1) hos_log("rt slots: off from now (%s: %#llx %#llx); new targets take the game's offsets", why,
                            (unsigned long long)a, (unsigned long long)b);
    state = 0;
}

static uint64_t fam_of(const Cur *k) {   /* FNV-1a over the identity: who, which name, which kind */
    uint64_t h = 1469598103934665603ULL;
    const uint64_t v[6] = {k->lr, k->name, k->fmt, k->type, k->d, k->mips};
    for (int i = 0; i < 6; i++) { h ^= v[i]; h *= 1099511628211ULL; }
    return h;
}

/* once, before the first allocation: every arena of pool record 0 still holds its one initial free block covering the
   pool; take S off its top (block size and arena end), so the game's own placements stay below lo */
static void shrink_arenas(uint64_t dta) {
    state = 0;
    const uint32_t nr = *(uint32_t *)GP(dta + DTA_NRECS);
    const uint64_t recs = G64(dta + DTA_RECS);
    if (nr != 1 || !hos_guest_range(recs, 8) || !hos_guest_range(G64(recs), 0x58)) {
        hos_log("rt slots: %u pool records at %#llx, off", nr, (unsigned long long)recs);
        return;
    }
    const uint64_t rec = G64(recs);
    const uint64_t ar = G64(rec + REC_ARENAS);
    const uint32_t na = G32(rec + REC_NARENAS);
    uint64_t base = 0, size = 0;
    if (na == 0 || na > 16 || !hos_guest_range(ar, 0xe8ull * na)) { hos_log("rt slots: %u arenas, off", na); return; }
    for (uint32_t i = 0; i < na; i++) {
        const uint64_t a = ar + (uint64_t)i * 0xe8;
        const uint64_t b = G64(a + AR_FREE_HEAD) - (uint64_t)(int64_t)(int32_t)G32(a + AR_NODE_OFF);
        if (G32(a + AR_FREE_N) != 1 || G32(a + AR_USED) != 0 || !hos_guest_range(b, 0xb0)) {
            hos_log("rt slots: arena %u not fresh (%u free, %u used), off", i, G32(a + AR_FREE_N), G32(a + AR_USED));
            return;
        }
        if (i == 0) base = G64(b + BLK_OFF), size = G64(b + BLK_SIZE);
        else if (G64(b + BLK_OFF) != base || G64(b + BLK_SIZE) != size) {
            hos_log("rt slots: arenas differ (%#llx+%#llx), off", (unsigned long long)G64(b + BLK_OFF),
                    (unsigned long long)G64(b + BLK_SIZE));
            return;
        }
        if (G64(a + AR_END) != base + size) { hos_log("rt slots: arena %u end %#llx, off", i, (unsigned long long)G64(a + AR_END)); return; }
    }
    if (after_pool() && base == 0) {   /* the slots after the pool's end: the arenas are left as they are */
        lo = size, hi = size + slots_bytes();
        nrecs0 = nr;
        arenas0 = ar, narenas = na;
        state = 1;
        hos_log("rt slots: %u arenas of %llu MB (the game's), slots after the pool at +%#llx..+%#llx", na,
                (unsigned long long)(size >> 20), (unsigned long long)lo, (unsigned long long)hi);
        return;
    }
    if (base != 0 || size < slots_bytes() + (128ull << 20)) {   /* (the arenas keep at least 128 MB) */
        hos_log("rt slots: arenas at %#llx+%llu MB, off", (unsigned long long)base, (unsigned long long)(size >> 20));
        return;
    }
    for (uint32_t i = 0; i < na; i++) {
        const uint64_t a = ar + (uint64_t)i * 0xe8;
        const uint64_t b = G64(a + AR_FREE_HEAD) - (uint64_t)(int64_t)(int32_t)G32(a + AR_NODE_OFF);
        const int lane_arena = native_on() && i + 1 < na;   /* (the last arena is the persistent lane's) */
        G64(b + BLK_SIZE) = lane_arena ? HUGE_ARENA - base : size - slots_bytes();
        G64(a + AR_END) = lane_arena ? HUGE_ARENA : base + size - slots_bytes();
    }
    lo = size - slots_bytes();
    hi = size;
    nrecs0 = nr;
    arenas0 = ar, narenas = na;
    state = 1;
    if (native_on()) hos_log("rt slots: native: arenas 0-%u of 1 TB of bookkeeping, arena %u (persistent) real", na - 2, na - 1);
    hos_log("rt slots: %u arenas of %llu MB (were %llu), slots at +%#llx..+%#llx", na,
            (unsigned long long)((size - slots_bytes()) >> 20), (unsigned long long)(size >> 20), (unsigned long long)lo,
            (unsigned long long)hi);
}

static int check_on(void);   /* (HOS_RT_CHECK, below) */
static void check_arenas(const char *what, Ctx *c, uint64_t dta, uint64_t e);
typedef struct { Cur key; uint64_t entry, block, frame; } Parked;
static uint64_t steal_q[64];   /* (native) entries whose slots were taken: released at the next sweep (under mu) */
static int nsteal_q;
#define MAX_PARKED 512
static Parked parked[MAX_PARKED];
static int nparked;
static uint64_t last_sweep;
static _Thread_local int releasing;   /* (the game's free called from here: not parked again) */

static int same_key(const Cur *a, const Cur *b) {
    return a->lr == b->lr && a->dta == b->dta && a->arg == b->arg && !memcmp(a->desc, b->desc, sizeof a->desc);
}
/* a parked entry of this key (the oldest parked first: instances keep their order), out of the list; 0: none */
/* The arena an entry's block came from: pool record +0x13a, its lane's (+0x108) arena index +0x281 (the free's own
   lookup, f_802737a0 0x802737cc..). 0: not known */
static uint64_t arena_of(uint64_t dta, uint64_t e) {
    const uint64_t lane = G64(e + 0x108);
    const uint32_t ri = *(uint8_t *)GP(e + 0x13a);
    if (!hos_guest_range(lane, 0x288) || ri >= *(uint32_t *)GP(dta + DTA_NRECS)) return 0;
    const uint64_t rec = G64(G64(dta + DTA_RECS) + 8ull * ri);
    const uint32_t ai = *(uint8_t *)GP(lane + 0x281), na = G32(rec + REC_NARENAS);
    return G64(rec + REC_ARENAS) + (uint64_t)(ai < na ? ai : 0) * 0xe8;
}
/* Parked and handed-back entries keep the block of the lane that first allocated them, and hand-backs cross lanes
   (a pass records on whichever worker runs it): one lane's arena then holds the blocks of targets every lane uses,
   while the arenas are shrunk to 192 MB of bookkeeping. On Hyrule Field one arena reached 174 MB with a 17 MB hole,
   the next large alloc on that lane found no block, and the game's alloc went on with block 0 (0x8005fd14: on the
   console its 512 MB never run out): the later free of that entry linked a node at null into the arena's lists, and
   f_802737a0 looped or crashed in them (work/assets/native/cores.md). So an entry whose arena is fuller than
   HOS_RT_PARK_FILL percent (default 40) of its end, or has fewer than 16 spare list nodes (+0x98: an alloc that
   splits a block with none left writes near null, 0x80061068), is not handed back: it is released, which gives
   its bookkeeping back, and the caller's own lane allocates. */
static uint32_t park_fill(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_RT_PARK_FILL") ? atoi(getenv("HOS_RT_PARK_FILL")) : 40;
    return (uint32_t)(v > 0 ? v : 100);
}
static int arena_tight(uint64_t ar) {
    return ar && (G64(ar + AR_BYTES) * 100 > G64(ar + AR_END) * park_fill() || (int32_t)G32(ar + 0x98) < 16);
}
static uint64_t unpark(const Cur *k, uint64_t *drain) {
    uint64_t e = 0;
    AcquireSRWLockExclusive(&mu);
    for (int i = 0; i < nparked; i++)
        if (same_key(&parked[i].key, k)) {
            e = parked[i].entry;
            if (arena_tight(arena_of(k->dta, e))) {   /* (released by the caller, outside mu) */
                *drain = e;
                e = 0;
                n.drained++;
            } else {
                for (int j = 0; j < nslots; j++)
                    if (slots[j].block == parked[i].block) { slots[j].last_frame = frame; break; }
                n.handed_back++;
            }
            memmove(&parked[i], &parked[i + 1], (size_t)(nparked - i - 1) * sizeof *parked);
            nparked--;
            break;
        }
    ReleaseSRWLockExclusive(&mu);
    return e;
}
/* the game's free for entries parked longer than HOS_RT_PARK_FRAMES presents: once a present, from the first free of
   the game's after it (the context the game frees in: a pass's thread at a function entry, so calling guest code
   keeps the guest's ABI; caller-saved registers restored), never while this thread holds one of the DTA's mutexes
   (a free from an alloc hook deadlocked once: sead::MainThread held +0x29e8/+0x2a08 for good, f_hf_new1) */
void imp__ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE(Ctx *c);   /* (nn_os_sync.c) */
static int holds(Ctx *c, uint64_t m) {
    const uint64_t x0 = c->x[0];
    c->x[0] = m;
    imp__ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE(c);
    const int r = c->x[0] != 0;
    c->x[0] = x0;
    return r;
}
static void release_now(Ctx *c, uint64_t dta, uint64_t entry) {
    uint64_t x[19];
    memcpy(x, c->x, sizeof x);
    const uint64_t lr = c->x[30];
    releasing = 1;
    hos_call_guest(c, 0x802737a0ull, dta, entry, 0, 0);
    releasing = 0;
    if (check_on()) check_arenas("after the release of", c, dta, entry);
    memcpy(c->x, x, sizeof x);
    c->x[30] = lr;
}
static void sweep(Ctx *c, uint64_t dta) {
    const uint64_t f = frame;
    if (__atomic_load_n(&last_sweep, __ATOMIC_RELAXED) == f) return;
    if (holds(c, dta + 0x29e8) || holds(c, dta + 0x2a08)) return;
    uint64_t go[MAX_PARKED + 64][2];
    int ngo = 0;
    AcquireSRWLockExclusive(&mu);
    for (int i = 0; i < nsteal_q; i++) go[ngo][0] = dta, go[ngo][1] = steal_q[i], ngo++;
    nsteal_q = 0;
    if (last_sweep != f) {
        last_sweep = f;
        int k = 0;
        for (int i = 0; i < nparked; i++) {
            if (parked[i].frame + park_frames() <= f) {
                go[ngo][0] = parked[i].key.dta, go[ngo][1] = parked[i].entry, ngo++;
            } else {
                parked[k++] = parked[i];
            }
        }
        nparked = k;
    }
    ReleaseSRWLockExclusive(&mu);
    for (int i = 0; i < ngo; i++) release_now(c, go[i][0], go[i][1]);
    if (ngo) __atomic_add_fetch(&n.released, (uint64_t)ngo, __ATOMIC_RELAXED);
}

void imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(Ctx *c);     /* (nn_os_sync.c) */
void imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(Ctx *c);
static void call_mutex(Ctx *c, void (*f)(Ctx *), uint64_t m) {   /* (c->x[0] kept) */
    const uint64_t x0 = c->x[0];
    c->x[0] = m;
    f(c);
    c->x[0] = x0;
}
/* a sead list (circular: node +0 previous, +8 next; the sentinel its own when empty; count at sentinel +0x10): 0 when
   every node's previous is the one before it and count nodes lead back to the sentinel; else where it is not (1 + k:
   a previous link, 1000: no way back, 2000 + k: another count, 3000 + k: a link out of guest memory) */
static int list_bad(uint64_t sentinel) {
    const uint32_t count = G32(sentinel + 0x10);
    uint64_t prev = sentinel, p = G64(sentinel + 8);
    for (uint32_t k = 0; k <= count + 64; k++) {
        if (p == sentinel) return k == count && G64(sentinel) == prev ? 0 : 2000 + (int)k;
        if (!hos_guest_range(p, 16)) return 3000 + (int)k;
        if (G64(p) != prev) return 1 + (int)k;
        prev = p;
        p = G64(p + 8);
    }
    return 1000;
}
/* HOS_RT_CHECK=1 (debugging, off; one static test when off): at every alloc and free hook and after each release, the
   arenas' used, free and spare lists, under the arena mutex (+0x2a08, taken when this thread holds neither DTA mutex),
   the last 256 hook events kept; at the first broken list they and the arenas go to the log, and a free of an entry
   without a block logs that entry's own last events. How the Hyrule Field freeze was found (cores.md). Costly when on */
static int check_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_RT_CHECK") && atoi(getenv("HOS_RT_CHECK")) > 0;
    return v;
}
typedef struct { int64_t ns; const char *what, *thread; uint64_t e, lr, blk; } Ev;
static Ev evs[256];
static volatile LONG nev;
#define EH 8192
static struct { uint64_t e; Ev v[8]; int n; } eh[EH];   /* (per entry, its last 8 events) */
static SRWLOCK eh_lock = SRWLOCK_INIT;
static uint32_t eh_slot(uint64_t e) {   /* (eh_lock held) */
    uint32_t i = (uint32_t)((e >> 7) * 0x9e3779b1u) & (EH - 1);
    for (int k = 0; k < EH && eh[i].e && eh[i].e != e; k++) i = (i + 1) & (EH - 1);
    return i;
}
static void ev_log(const Ev *v) {
    hos_log("rt check:   %.6f %s %s entry %#llx block %#llx lr %#llx", v->ns / 1e9, v->thread, v->what,
            (unsigned long long)v->e, (unsigned long long)v->blk, (unsigned long long)v->lr);
}
static void ev(const char *what, Ctx *c, uint64_t e) {
    const Ev v = {hos_now_ns(), what, hos_thread_name(hos_self()), e, c->x[30],
                  e && hos_guest_range(e, 0xc8) ? G64(e + ENT_BLOCK) : 0};
    evs[(InterlockedIncrement(&nev) - 1) & 255] = v;
    if (!e) return;
    AcquireSRWLockExclusive(&eh_lock);
    const uint32_t i = eh_slot(e);
    eh[i].e = e;
    eh[i].v[eh[i].n++ & 7] = v;
    ReleaseSRWLockExclusive(&eh_lock);
    if (!v.blk && !strcmp(what, "free of")) {
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 10) {
            hos_log("rt check: free of entry %#llx without a block (+0xba %#x); its last events:", (unsigned long long)e,
                    *(uint8_t *)GP(e + 0xba));
            AcquireSRWLockShared(&eh_lock);
            const uint32_t k = eh_slot(e);
            for (int j = eh[k].n > 8 ? eh[k].n - 8 : 0; j < eh[k].n; j++) ev_log(&eh[k].v[j & 7]);
            ReleaseSRWLockShared(&eh_lock);
        }
    }
}
static void check_arenas(const char *what, Ctx *c, uint64_t dta, uint64_t e) {
    if (!check_on() || state < 0) return;
    ev(what, c, e);
    if (holds(c, dta + 0x29e8) || holds(c, dta + 0x2a08)) return;
    static volatile LONG told;
    static const int lists[3] = {0x58, 0x70, 0x88};
    call_mutex(c, imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE, dta + 0x2a08);
    const uint64_t recs = G64(dta + DTA_RECS);
    for (uint32_t r = 0; r < *(uint32_t *)GP(dta + DTA_NRECS) && r < 4; r++) {
        const uint64_t rec = G64(recs + 8ull * r), ar = G64(rec + REC_ARENAS);
        for (uint32_t i = 0; i < G32(rec + REC_NARENAS) && i < 16; i++)
            for (int l = 0; l < 2; l++) {   /* (+0x88 is a stack, not a circular list) */
                const uint64_t sn = ar + (uint64_t)i * 0xe8 + (uint64_t)lists[l];
                const int bad = list_bad(sn);
                if (!bad || InterlockedIncrement(&told) > 20) continue;
                if (told == 1) {
                    hos_log("rt check: the first broken list; the hook events before it:");
                    const LONG end = nev;
                    for (LONG k = end > 256 ? end - 256 : 0; k < end; k++) ev_log(&evs[k & 255]);
                }
                hos_log("rt check: at %s entry %#llx: record %u arena %u list +%#x broken at %d (count %u, %llu MB used); "
                        "lr %#llx thread %s", what, (unsigned long long)e, r, i, lists[l], bad, G32(sn + 0x10),
                        (unsigned long long)(G64(ar + (uint64_t)i * 0xe8 + AR_BYTES) >> 20), (unsigned long long)c->x[30],
                        hos_thread_name(hos_self()));
            }
    }
    call_mutex(c, imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE, dta + 0x2a08);
}
   /* (native) a context whose lane byte this thread changed, and its own */
static _Thread_local uint64_t lane_ctx;   /* (native) a context whose lane byte this thread changed, and its own */
static _Thread_local uint8_t lane_was;
static void restore_lane(void) {
    if (lane_ctx) *(uint8_t *)GP(lane_ctx + 0x186) = lane_was, lane_ctx = 0;
}
/* (native) the caller's lane arena keeps at least 32 spare block descriptors (+0x88: a stack, node at descriptor +
   [arena +0x9c], count +0x98, pushed as the game's free does): 128 zeroed 0xb0-byte descriptors of ours at a time, under
   the arena mutex (+0x2a08), at an alloc's entry (this thread holds neither DTA mutex there; checked) */
static void top_up(Ctx *c, uint64_t dta, uint64_t ctx) {
    if (!hos_guest_range(ctx, 0x188) || state != 1) return;
    const uint64_t lane = dta + 8 + (uint64_t)*(uint8_t *)GP(ctx + 0x186) * 0x288;
    const uint64_t rec = G64(G64(dta + DTA_RECS));
    const uint32_t ai = *(uint8_t *)GP(lane + 0x281), na = G32(rec + REC_NARENAS);
    const uint64_t ar = G64(rec + REC_ARENAS) + (uint64_t)(ai < na ? ai : 0) * 0xe8;
    if ((int32_t)G32(ar + 0x98) >= 32 || holds(c, dta + 0x29e8) || holds(c, dta + 0x2a08)) return;
    call_mutex(c, imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE, dta + 0x2a08);
    if ((int32_t)G32(ar + 0x98) < 32) {
        const uint64_t mem = hos_sys_alloc(128 * 0xb0, 0x10);
        const int64_t off = (int32_t)G32(ar + 0x9c);
        for (int i = 0; mem && i < 128; i++) {
            const uint64_t node = mem + (uint64_t)i * 0xb0 + (uint64_t)off, head = G64(ar + 0x88);
            G64(node) = head;
            G64(node + 8) = ar + 0x88;
            if (head) G64(head + 8) = node;
            G64(ar + 0x88) = node;
            G32(ar + 0x98) = G32(ar + 0x98) + 1;
        }
        if (mem) __atomic_add_fetch(&n.topped, 128, __ATOMIC_RELAXED);
    }
    call_mutex(c, imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE, dta + 0x2a08);
}

static int alloc_pre(Ctx *c);
static _Thread_local uint64_t shadow_obj;   /* (f_802640a0's shadow object, for the alloc it makes) */
static uint32_t shadow_size(uint64_t obj, uint32_t w);
static void alloc_gave(uint64_t entry);
static _Thread_local int in_alloc;
/* In direct mode the alloc's body runs from here, so the entry it gives is seen (alloc_gave); RX_BACKEND unset: as
   before, the body runs after the hook */
ENTRY_HOOK(0x8005ed00) {   /* (dta, ctx, desc, flags) */
    if (in_alloc) return in_alloc = 0, 0;   /* (the lifted body, called below: the flag is taken by its first entry, so a nested call is hooked as any) */
    const int handled = alloc_pre(c);
    if (off_() || !rx_direct()) return handled;
    if (!handled) {
        static A64Fn body;
        if (!body) body = a64_lookup(0x8005ed00ull);
        if (!body) return 0;
        in_alloc = 1;
        body(c);
        in_alloc = 0;
    }
    alloc_gave(c->x[0]);
    return 1;
}
static int alloc_pre(Ctx *c) {
    if (off_()) return 0;
    alloc_sp = 0;   /* (M6f: the splice of this call sets it) */
    dta_seen = ARG(0);
    __atomic_add_fetch(&n.calls, 1, __ATOMIC_RELAXED);
    if (cur.valid) __atomic_add_fetch(&n.stale, 1, __ATOMIC_RELAXED);   /* the last transient call never reached the splice */
    cur.valid = 0;
    if (state < 0) {
        AcquireSRWLockExclusive(&mu);
        if (state < 0) shrink_arenas(ARG(0));
        ReleaseSRWLockExclusive(&mu);
    }
    if (check_on()) check_arenas("alloc", c, ARG(0), 0);
    if (native_on()) {
        restore_lane();
        top_up(c, ARG(0), ARG(1));
        if (!(ARG(3) & 1) && hos_guest_range(ARG(1), 0x188) && state == 1) {   /* a persistent target takes real memory
                                                                                  from the top of its lane's free block */
            const uint64_t rec = G64(G64(ARG(0) + DTA_RECS));
            const uint64_t lane = ARG(0) + 8 + (uint64_t)*(uint8_t *)GP(ARG(1) + 0x186) * 0x288;
            const uint32_t ai = *(uint8_t *)GP(lane + 0x281);
            static volatile LONG told;
            if (ai + 1 < G32(rec + REC_NARENAS)) {
                /* (sead::MainThread's context is lane 0: agl::RenderDisplay's targets, 0x809ed1a4, 0x80792318) the alloc
                   reads the lane once, at its entry (0x8005ed30): the context says the persistent lane for this call
                   and gets its own back at the next hook of this thread (the splice, or the next alloc or free) */
                restore_lane();
                lane_ctx = ARG(1), lane_was = *(uint8_t *)GP(ARG(1) + 0x186);
                *(uint8_t *)GP(ARG(1) + 0x186) = (uint8_t)(G32(rec + REC_NARENAS) - 1);
                if (InterlockedIncrement(&told) <= 10)
                    hos_log("rt slots: native: a persistent alloc on lane %u (lr %#llx) moved to the persistent lane",
                            lane_was, (unsigned long long)c->x[30]);
                n.moved++;
            }
        }
    }
    if (!(ARG(3) & 1)) return 0;   /* persistent: the game's */
    __atomic_add_fetch(&n.transient, 1, __ATOMIC_RELAXED);
    const uint64_t d = ARG(2);
    if (state != 1 || !hos_guest_range(d, 0x38)) return 0;
    if (c->x[30] == LR_LOCAL_SHADOW && shadow_classes_on()) {   /* (the descriptor is the caller's, on its stack, read only here) */
        const uint32_t w = G32(d + 0x10), h = G32(d + 0x14);
        if (w == h && w >= 16 && w <= 4096) {
            const uint32_t to = shadow_size(shadow_obj, w);
            G32(d + 0x10) = G32(d + 0x14) = to;
            if (to != w) __atomic_add_fetch(&n.quantized, 1, __ATOMIC_RELAXED);
        }
        shadow_obj = 0;
    }
    Cur k = {c->x[30], G64(d), ARG(0), G32(d + 8), G32(d + 0xc), G32(d + 0x10), G32(d + 0x14), G32(d + 0x18),
             G32(d + 0x1c), G32(d + 0x30), (uint32_t)ARG(3), {0}, 1};
    memcpy(k.desc, GP(d), sizeof k.desc);
    if (park_on()) {
        uint64_t drain = 0;
        const uint64_t e = unpark(&k, &drain);
        if (drain) {   /* its arena is tight: back to the game now (this thread holds neither DTA mutex at an alloc's
                          entry, as sweep checks), the caller's own lane allocates below */
            if (!holds(c, ARG(0) + 0x29e8) && !holds(c, ARG(0) + 0x2a08)) {
                release_now(c, ARG(0), drain);
                __atomic_add_fetch(&n.released, 1, __ATOMIC_RELAXED);
            } else {   /* (not expected) parked again: the sweep releases it */
                AcquireSRWLockExclusive(&mu);
                if (nparked < MAX_PARKED) parked[nparked++] = (Parked){k, drain, G64(drain + ENT_BLOCK), 0};
                ReleaseSRWLockExclusive(&mu);
            }
        }
        if (e) {   /* the same target as last frame: its entry, whole */
            RET(e);
            return 1;
        }
    }
    cur = k;
    return 0;
}

/* What the device's image of a transient target needs, for its slot (the back end binds the image in the slot: RADV
   rounds sizes to its tiles, a depth image carries its HTILE, the alignment is 64 KB; e.g. 960x540 R11G11B10 is 2.0 MB
   on the console's layout and 2.6 MB here). The allocator's descriptor has agl's format and type: the NVN format is
   the game's own table (0x83f4848c, u32 per agl format, 0x5f of them: f_800634b0 finds a TextureData's format in it),
   the target from the type (agl_target: as seen at the textures' initializations, which check both, below).
   0: not known (no back end, a format outside the table) */
#define AGL_FORMATS 0x5f
#define AGL_FORMAT_TABLE 0x83f4848cull
static int agl_target[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1};   /* (agl's type is the NVN target: seen for 2D
                                                                                      and 2D array) */
static uint64_t device_need(const Cur *k, uint64_t *align) {
    const hsd_fns *f = rx_direct();
    *align = 0;
    if (!f || !f->image_need || k->fmt >= AGL_FORMATS || k->type >= 16 || agl_target[k->type] < 0 ||
        !hos_guest_range(AGL_FORMAT_TABLE, 4 * AGL_FORMATS))
        return 0;
    nvn_texdesc d = {0};
    d.target = agl_target[k->type];
    d.format = (int)G32(AGL_FORMAT_TABLE + 4ull * k->fmt);
    d.samples = 1;
    d.width = k->w, d.height = k->h, d.depth = k->d, d.levels = k->mips;
    return f->image_need(&d, align);
}

static int by_off(const void *a, const void *b) {
    const uint64_t x = **(const uint64_t *const *)a, y = **(const uint64_t *const *)b;
    return x < y ? -1 : x > y;
}
/* Large slots (the shadow maps and cascades, full-resolution targets) are placed from the top of the range down, the
   rest from the bottom up: the many small targets that come and go with the dynamic resolution then do not cut up the
   room the 16-24 MB maps need (with one end, the scene route placed 48 maps outside the slots for want of one gap) */
#define BIG_SLOT (8ull << 20)
/* an aligned gap of cap bytes between the slots (the lowest; for a large slot the highest); idle slots (free 8 presents; with none of those left, free
   REUSE_AFTER presents) go, least recently used first, until one is found. ~0: none */
static uint64_t place(uint64_t cap, uint64_t align) {
    static const uint64_t *order[MAX_SLOTS];
    uint64_t idle = 8;   /* (then REUSE_AFTER, when no slot that old is left: the range is full) */
    for (;;) {
        for (int i = 0; i < nslots; i++) order[i] = &slots[i].off;
        qsort(order, (size_t)nslots, sizeof *order, by_off);
        uint64_t at = lo, best = ~0ull;
        for (int i = 0; i <= nslots; i++) {
            at = (at + align - 1) & ~(align - 1);
            const uint64_t end = i < nslots ? *order[i] : hi;
            if (at + cap <= end) {
                if (cap < BIG_SLOT) return at;
                best = (end - cap) & ~(align - 1);   /* (>= at: at is aligned) */
            }
            if (i < nslots) {
                const Slot *s = (const Slot *)((const char *)order[i] - offsetof(Slot, off));
                if (s->off + s->cap > at) at = s->off + s->cap;
            }
        }
        if (best != ~0ull) return best;
        int lru = -1;
        for (int i = 0; i < nslots; i++)
            if (!slots[i].block && slots[i].last_frame + idle <= frame && (lru < 0 || slots[i].last_frame < slots[lru].last_frame))
                lru = i;
        if (lru < 0 && idle > REUSE_AFTER) {
            idle = REUSE_AFTER;
            continue;
        }
        if (lru < 0) return ~0ull;
        if (idle < 8) n.evicted_young++;
        used -= slots[lru].cap;
        slots[lru] = slots[--nslots];
        n.evicted++;
    }
}

/* no room: the slot of a parked entry that fits (the smallest) and was parked REUSE_AFTER presents ago or more (its
   target was not asked for since: the lists that used it are sent), its entry out of the parked list and released at
   the next sweep (its block's slot is the new block's from now: its later free frees no slot). Under mu. NULL: none */
static Slot *steal(uint64_t need, uint64_t align, uint64_t block) {
    int bi = -1, bs = -1;
    for (int i = 0; i < nparked && nsteal_q < 64; i++)
        for (int j = 0; j < nslots; j++)
            if (parked[i].frame + REUSE_AFTER <= frame && slots[j].block == parked[i].block && slots[j].cap >= need &&
                !(slots[j].off & (align - 1)) &&
                (bs < 0 || slots[j].cap < slots[bs].cap))
                bi = i, bs = j;
    if (bs < 0) return NULL;
    steal_q[nsteal_q++] = parked[bi].entry;
    memmove(&parked[bi], &parked[bi + 1], (size_t)(nparked - bi - 1) * sizeof *parked);
    nparked--;
    Slot *t = &slots[bs];
    t->fam = fam_of(&cur);
    t->name = cur.name, t->lr = cur.lr, t->fmt = cur.fmt;
    t->occ = 0;
    t->w = cur.w, t->h = cur.h;
    t->block = block;
    t->last_frame = frame;
    t->key = cur;
    n.stolen++;
    return t;
}
/* the slot for a block of this family: a free one of the family that holds it (the same shape first, the lowest
   occurrence: the same address for the same target each frame), else a new one. NULL: no room */
static Slot *take(uint64_t fam, uint64_t need, uint64_t align, uint64_t block) {
    if (align < 0x10000) align = 0x10000;   /* 64 KB: any NVN alignment, and room for the renderer's placement */
    Slot *best = NULL;
    int best_rank = 9, family = 0;
    uint32_t occ = 0;
    for (int i = 0; i < nslots; i++) {
        Slot *s = &slots[i];
        if (s->block == block) {   /* the block came back without a free we saw */
            s->block = 0;
            n.unseen_free++;
        }
        if (s->fam != fam) continue;
        family++;
        if (s->block) { occ++; continue; }
        if (s->cap < need || (s->off & (align - 1))) continue;
        const int same = s->w == cur.w && s->h == cur.h;
        if (!same && s->last_frame + REUSE_AFTER > frame) continue;   /* (another shape is another image over this memory) */
        const int rank = same ? 1 : 2;
        if (rank < best_rank || (rank == best_rank && (rank == 2 ? s->cap < best->cap : s->occ < best->occ)))
            best = s, best_rank = rank;
    }
    if (best) {
        if (best->w == cur.w && best->h == cur.h) n.same_shape++;
        else n.other_shape++;
    } else {
        /* a family seen before (its size moves, or more of it live at once) gets a quarter more room; not the local
           shadows, whose sizes are classes */
        const uint64_t cap = ((family && cur.lr != LR_LOCAL_SHADOW ? need + need / 4 : need) + 0xffff) & ~0xffffULL;
        const uint64_t at = nslots < MAX_SLOTS ? place(cap, align) : ~0ull;
        if (at == ~0ull) return steal(need, align, block);
        best = &slots[nslots++];
        *best = (Slot){fam, at, cap, 0, 0, cur.name, cur.lr, occ, cur.w, cur.h, cur.fmt};
        used += cap;
        if (used > peak) peak = used;
        n.made++;
        if (log_slots()) {
            const char *nm = hos_guest_range(cur.name, 1) ? (const char *)GP(cur.name) : "?";
            hos_log("rt slots: new %.60s %ux%ux%u fmt %u occ %u lr %#llx at +%#llx (%llu KB; %d slots, %llu MB)", nm,
                    cur.w, cur.h, cur.d, cur.fmt, occ, (unsigned long long)cur.lr, (unsigned long long)at,
                    (unsigned long long)(cap >> 10), nslots, (unsigned long long)(used >> 20));
        }
    }
    best->occ = occ;
    best->w = cur.w, best->h = cur.h;
    best->block = block;
    best->last_frame = frame;
    best->key = cur;
    return best;
}

static int pool_ok(uint64_t pool) {   /* a virtual pool that holds the slot range */
    for (int i = 0; i < npools; i++)
        if (pools[i] == pool) return 1;
    const nvn_pool *p = hos_guest_range(pool, sizeof(nvn_pool));
    if (!p || p->magic != NVN_MAGIC('P', 'O', 'O', 'L') || p->mem || (after_pool() ? p->size != lo : p->size < hi) || npools == 16) {
        back_off("the block's pool", pool, p ? p->size : 0);
        return 0;
    }
    pools[npools++] = pool;
    hos_log("rt slots: pool %#llx (GPU %#llx, %llu MB, flags %#x)", (unsigned long long)pool, (unsigned long long)p->gpu,
            (unsigned long long)(p->size >> 20), p->flags);
    if (!dta_pool_gpu) {   /* the slot range's storage addresses: the back end's transient pool, allocated now */
        dta_pool_gpu = p->gpu;
        const hsd_fns *d = rx_direct();
        if (d && d->transient_pool && !d->transient_pool(p->gpu + lo, p->gpu + hi))
            hos_log("rt slots: the back end made no transient pool for %#llx..%#llx", (unsigned long long)(p->gpu + lo),
                    (unsigned long long)(p->gpu + hi));
    } else if (npools > 1) {
        hos_log("rt slots: a second pool: its targets are not in the transient pool");
    }
    return 1;
}

/* (lifted f_8005ed00, after each "block +0x38 = aligned offset"): x23 block, x10 offset, x8 the pool. The next lifted
   line adds the TextureData's base to x10; everything after (entry +0, the registration compare, SetStorage, views
   made later from the entry) reads the offset from x10 or block +0x38. */
SPLICE_AFTER_EACH(0x8005ed00, "c->x[23] + 56,c->x[10]);") {
    if (lane_ctx) {   /* (native) a persistent alloc's block: real memory, the lane given back */
        static volatile LONG told;
        if (c->x[10] + G64(c->x[23] + BLK_SIZE) > lo && InterlockedIncrement(&told) <= 10)
            hos_log("rt slots: native: a persistent block at %#llx is not below the slots", (unsigned long long)c->x[10]);
        restore_lane();
    }
    alloc_sp = c->x[31], alloc_block = c->x[23];   /* (M6f: persistent allocations too: the entry's texture, below) */
    if (!cur.valid) {
        if (!off_()) __atomic_add_fetch(&n.unspliced, 1, __ATOMIC_RELAXED);
        return;
    }
    const Cur k = cur;
    cur.valid = 0;
    const uint64_t block = c->x[23], game = c->x[10];
    const uint64_t align = (uint64_t)(int64_t)(int32_t)G32(c->x[31] + SP_ALIGN);
    uint64_t dev_align = 0;
    const uint64_t dev_need = device_need(&k, &dev_align);
    AcquireSRWLockExclusive(&mu);
    n.spliced++;
    Slot *s = NULL;
    if (state == 1) {
        const uint64_t game_need = G64(block + BLK_SIZE);
        const uint64_t need = dev_need > game_need ? dev_need : game_need;   /* (the slot holds the device's image) */
        n.sized++, n.sized_up += dev_need > game_need;
        if (!native_on() && game + game_need > lo) back_off("an arena offset in the slot range", game, game_need);
        else if (*(uint32_t *)GP(k.dta + DTA_NRECS) != nrecs0) back_off("a second pool record", nrecs0, 0);
        else if (align & (align - 1) || align > 0x1000000 || dev_align > 0x10000) back_off("alignment", align, dev_align);
        else if (pool_ok(c->x[8])) {
            cur = k;   /* (take reads the shape and name from it) */
            s = take(fam_of(&k), need, align, block);
            cur.valid = 0;
        }
    }
    if (!s) n.game_offset++;
    if (!s && log_slots() && state == 1) {   /* (HOS_RT_SLOTS_LOG: what held the range when a target found no slot) */
        static int told;
        if (told++ < 6) {
            hos_log("rt slots: no slot for %ux%u (%llu KB, lr %#llx) at present %llu; the slots (offset MB, KB, size, state):", k.w, k.h,
                    (unsigned long long)(G64(block + BLK_SIZE) >> 10), (unsigned long long)k.lr, (unsigned long long)frame);
            char b[900];
            int o = 0;
            for (int i = 0; i < nslots; i++) {
                int pk = -1;
                for (int j = 0; j < nparked; j++)
                    if (slots[i].block && parked[j].block == slots[i].block) pk = (int)(frame - parked[j].frame);
                o += snprintf(b + o, sizeof b - (size_t)o, " %.1f:%llu:%ux%u:%s%d", (double)(slots[i].off - lo) / 1048576.0,
                              (unsigned long long)(slots[i].cap >> 10), slots[i].w, slots[i].h,
                              !slots[i].block ? "free" : pk >= 0 ? "parked" : "live", !slots[i].block || pk < 0 ? (int)(frame - slots[i].last_frame) : pk);
                if (o > 800 || i == nslots - 1) hos_log("rt slots:  %s", b), o = 0;
            }
        }
    }
    if (!s && native_on() && state == 1 && c->x[10] + G64(block + BLK_SIZE) > lo) {
        /* (native) no slot and a bookkeeping offset that is not memory: the slot range's start, aliased (logged; the
           parked entries and idle slots were already tried) */
        static volatile LONG told_alias;
        if (InterlockedIncrement(&told_alias) <= 10)
            hos_log("rt slots: native: no slot for %u x %u (%llu KB) and no memory at its offset %#llx: aliased at +%#llx",
                    k.w, k.h, (unsigned long long)(G64(block + BLK_SIZE) >> 10), (unsigned long long)c->x[10],
                    (unsigned long long)lo);
        n.aliased++;
        c->x[10] = lo;
        G64(block + 0x38) = lo;
    }
    static int told;
    if (log_slots() && frame > 1500 && told < 60) {   /* (what still needs placing in steady state) */
        told++;
        const char *nm = hos_guest_range(k.name, 1) ? (const char *)GP(k.name) : "?";
        hos_log("rt slots: placed %.60s %ux%ux%u fmt %u lr %#llx at %s%#llx", nm, k.w, k.h, k.d, k.fmt,
                (unsigned long long)k.lr, s ? "+" : "game's +", (unsigned long long)(s ? s->off : game));
    }
    ReleaseSRWLockExclusive(&mu);
    if (!s) {   /* (outside the slots: an image with memory of its own, which the transient pool was to end) */
        const hsd_fns *d = rx_direct();
        if (d && d->host_event) {
            char w[160];
            snprintf(w, sizeof w, "%ux%ux%u agl format %u lr %#llx, %llu KB, at the game's offset %#llx", k.w, k.h, k.d, k.fmt,
                     (unsigned long long)k.lr, (unsigned long long)(G64(block + BLK_SIZE) >> 10), (unsigned long long)game);
            d->host_event("a transient target placed outside the slots", w);
        }
        return;
    }
    c->x[10] = s->off;
    G64(block + 0x38) = s->off;
}

/* The entry an alloc gave (a new one, one from the allocator's free lists, or a parked one handed back), told to the
   back end by its own texture object: the entry's descriptor id (+0x68) names a record of agl's descriptor manager
   (*(*0x84ca2d60): records of 0x28 at +0x9f8, +0x9f0 of them, the first id at u16 +0xa62), whose +0x18 is the holder of
   the texture object registered under that id (f_800634b0: the object is the manager's, initialized over the entry's
   storage). Its image claims its memory in the transient pool if another image has had it since. The address lookup
   this replaces (the live texture at the block's storage) named another object after a reuse: the back end then let
   go of maps drawn in that very present (docs/direct/shadow-maps.md 5.3). An entry with no id yet is registered by
   its caller, and that initialization claims. */
static void alloc_gave(uint64_t entry) {
    const hsd_fns *d = rx_direct();
    if (!entry || !d || !d->target_given || !hos_guest_range(entry, 0x180)) return;
    const int32_t id = (int32_t)G32(entry + 0x68);
    const uint64_t mgr = hos_guest_range(0x84ca2d60ull, 8) && hos_guest_range(G64(0x84ca2d60ull), 8) ? G64(G64(0x84ca2d60ull)) : 0;
    const nvn_tex *t = NULL;
    if (id >= 0 && hos_guest_range(mgr, 0xa68)) {
        const uint32_t ri = (uint32_t)id - *(uint16_t *)GP(mgr + 0xa62);
        const uint64_t rec = G64(mgr + 0x9f8) + (uint64_t)ri * 0x28;
        if (ri < G32(mgr + 0x9f0) && hos_guest_range(rec, 0x28) && (int32_t)G32(rec) > 0) {
            const uint64_t holder = G64(rec + 0x18);
            const uint64_t obj = hos_guest_range(holder, 8) ? G64(holder) : 0;
            if (hos_guest_range(obj, sizeof(nvn_tex))) t = (const nvn_tex *)GP(obj);
        }
    }
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0) || !t->serial) {
        __atomic_add_fetch(&n.given_noid, 1, __ATOMIC_RELAXED);
        return;
    }
    const uint64_t block = G64(entry + ENT_BLOCK);
    if (!block || !dta_pool_gpu || !hos_guest_range(block + 0x38, 8) || t->gpu != dta_pool_gpu + G64(block + 0x38)) {
        __atomic_add_fetch(&n.given_other, 1, __ATOMIC_RELAXED);   /* (not registered over this block yet, or another pool) */
        return;
    }
    __atomic_add_fetch(&n.given, 1, __ATOMIC_RELAXED);
    d->target_given(t);
}

/* the local-shadow size classes, and each shadow object's class now (the hysteresis) */
static const uint32_t shadow_class[] = {256, 384, 512, 640, 768, 1024, 1280, 1536, 2048, 2432};
#define N_CLASS ((int)(sizeof shadow_class / sizeof *shadow_class))
static uint32_t class_of(uint32_t w) {
    for (int i = 0; i < N_CLASS; i++)
        if (w <= shadow_class[i]) return shadow_class[i];
    return (w + 127) / 128 * 128;
}
#define LS_TAB 2048
static struct { uint64_t obj; uint32_t cls; } ls_tab[LS_TAB];
static SRWLOCK ls_lock = SRWLOCK_INIT;
ENTRY_HOOK(0x802640a0) {   /* (shadow object = light + 0x100, ctx, flags): noted; the lifted body runs */
    shadow_obj = ARG(0);
    return 0;
}
/* the size a light's map gets for the size w it wants */
static uint32_t shadow_size(uint64_t obj, uint32_t w) {
    uint32_t want = class_of(w);
    if (!obj) return want;
    AcquireSRWLockExclusive(&ls_lock);
    uint32_t i = (uint32_t)((obj >> 4) * 0x9e3779b1u) & (LS_TAB - 1);
    int k = 0;
    for (; k < LS_TAB && ls_tab[i].obj && ls_tab[i].obj != obj; k++) i = (i + 1) & (LS_TAB - 1);
    if (k < LS_TAB) {
        const uint32_t now = ls_tab[i].obj ? ls_tab[i].cls : 0;
        if (now && w <= now && want != now) {
            uint32_t below = 0;   /* the class under the light's */
            for (int j = 0; j < N_CLASS; j++)
                if (shadow_class[j] < now) below = shadow_class[j];
            if (below && (uint64_t)w * 10 > (uint64_t)below * 9) want = now, n.class_kept++;
            else n.class_down++;
        } else if (now && want != now) {
            n.class_up++;
        }
        ls_tab[i].obj = obj, ls_tab[i].cls = want;
    }
    ReleaseSRWLockExclusive(&ls_lock);
    return want;
}

ENTRY_HOOK(0x802737a0) {   /* (dta, entry) */
    if (off_()) return 0;
    const uint64_t e = ARG(1);
    if (!hos_guest_range(e, 0x180)) return 0;
    restore_lane();
    if (check_on()) check_arenas("free of", c, ARG(0), e);
    const uint64_t block = G64(e + ENT_BLOCK);
    if (!block && (*(uint8_t *)GP(e + 0xba) & 3)) {
        /* an entry the game's alloc gave no block (its arena was full): its free would read block 0 and link a node at
           null into the arena's lists. Left as it is (the entry is not reused) */
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 10)
            hos_log("rt slots: free of entry %#llx without a block (an arena ran out), skipped; lr %#llx",
                    (unsigned long long)e, (unsigned long long)c->x[30]);
        __atomic_add_fetch(&n.blockless, 1, __ATOMIC_RELAXED);
        return 1;
    }
    if (park_on() && !releasing) sweep(c, ARG(0));
    int park = 0;
    AcquireSRWLockExclusive(&mu);
    n.frees++;
    if (block)
        for (int i = 0; i < nslots; i++)
            if (slots[i].block == block) {
                if (park_on() && !releasing && state == 1 && slots[i].key.valid && slots[i].key.dta == ARG(0) &&
                    !G64(block + 0x88)) {   /* (not a block with an on-free delegate, +0x78..+0x90) */
                    if (nparked < MAX_PARKED) {   /* kept whole: the slot stays with the block */
                        parked[nparked++] = (Parked){slots[i].key, e, block, frame};
                        slots[i].last_frame = frame;
                        n.parked++;
                        park = 1;
                        break;
                    }
                    n.park_full++;
                }
                slots[i].block = 0;
                slots[i].last_frame = frame;
                n.freed_slot++;
                break;
            }
    ReleaseSRWLockExclusive(&mu);
    return park;   /* 1: the game's free does not run (the back end is told nothing either way: the memory is the pool's) */
}

/* (nvn_native.c) the allocator initializes texture object obj: the same address and shape as that object's last
   initialization (the game's registration cache missed for another reason), or not */
#define TMAP 4096
static struct { uint64_t obj, gpu; uint32_t w, h, fmt; } tmap[TMAP];
static uint64_t init_same, init_moved, init_new;
static uint64_t held_peak, held_peak_block;   /* (rt_slots_frame: bytes held at a present, the most since the start / the last line) */
static volatile LONG64 n_desc_other;   /* (textures in a slot whose NVN format or target is not what device_need took) */
void rt_slots_texture_init(uint64_t obj, uint64_t gpu, const nvn_texdesc *td, int dta) {
    const uint32_t w = td->width, h = td->height, fmt = (uint32_t)td->format;
    const hsd_fns *d = rx_direct();
    const uint64_t base = dta_pool_gpu;
    if (d && d->transient_slot && state == 1 && base && gpu >= base + lo && gpu < base + hi) {
        /* a texture in the slot range: the bytes of the slot that starts there (0: none does), before its image is bound */
        uint64_t cap = 0;
        Cur k = {0};
        AcquireSRWLockShared(&mu);
        for (int i = 0; i < nslots; i++)
            if (slots[i].off == gpu - base) { cap = slots[i].cap, k = slots[i].key; break; }
        ReleaseSRWLockShared(&mu);
        __atomic_add_fetch(cap ? &n.slot_told : &n.slot_untold, 1, __ATOMIC_RELAXED);
        if (cap && k.valid && k.w == w && k.h == h && k.fmt < AGL_FORMATS && k.type < 16) {
            /* the slot was sized for the format and target device_need took: checked against the texture's own */
            const int tf = (int)G32(AGL_FORMAT_TABLE + 4ull * k.fmt);
            if (tf != td->format || agl_target[k.type] != td->target) {
                if (InterlockedIncrement64(&n_desc_other) <= 10)
                    hos_log("rt slots: a target of agl format %u type %u is NVN format %#x target %d, not %#x / %d as its slot was "
                            "sized for", k.fmt, k.type, td->format, td->target, tf, agl_target[k.type]);
                if (agl_target[k.type] != td->target) agl_target[k.type] = td->target;   /* (as seen, from here on) */
            }
        }
        d->transient_slot(gpu, cap);
    }
    if (!dta || !log_slots()) return;   /* (debugging: a lock per initialization) */
    AcquireSRWLockExclusive(&mu);
    uint32_t i = (uint32_t)((obj >> 3) * 0x9e3779b1u) & (TMAP - 1);
    for (int k = 0; k < TMAP && tmap[i].obj && tmap[i].obj != obj; k++) i = (i + 1) & (TMAP - 1);
    if (!tmap[i].obj) init_new++;
    else if (tmap[i].gpu == gpu && tmap[i].w == w && tmap[i].h == h && tmap[i].fmt == fmt) init_same++;
    else init_moved++;
    tmap[i].obj = obj, tmap[i].gpu = gpu, tmap[i].w = w, tmap[i].h = h, tmap[i].fmt = fmt;
    static int told;
    if (log_slots() && frame > 1000 && told < 40 && alloc_sp) {   /* why the game's registration cache missed */
        told++;
        const uint64_t e = G64(alloc_sp + SP_ENTRY), b = alloc_block;
        hos_log("rt slots: init obj %#llx gpu %#llx %ux%u: entry %#llx (block %#llx, ours %#llx) reg %#llx/%#llx/%#x vs "
                "block %#llx/%#llx/%#x; e+0x64 %#x b+0xa0 %#x b+0x68 %#llx", (unsigned long long)obj,
                (unsigned long long)gpu, w, h, (unsigned long long)e, (unsigned long long)(e ? G64(e + ENT_BLOCK) : 0),
                (unsigned long long)b, (unsigned long long)(e ? G64(e + 128) : 0),
                (unsigned long long)(e ? G64(e + 112) : 0), e ? G32(e + 120) : 0, (unsigned long long)G64(b + 72),
                (unsigned long long)G64(b + 56), G32(b + 64), e ? *(uint8_t *)GP(e + 100) : 0, *(uint8_t *)GP(b + 160),
                (unsigned long long)G64(b + 104));
    }
    ReleaseSRWLockExclusive(&mu);
}

/* (HOS_DUMP_FILE) the DTA's lists, checked: each arena's used (+0x58), free (+0x70) and spare-node (+0x88) lists with
   its bytes in use, the 256 descriptor buckets (+0x11d0) and the free-entry LRU (+0x29d0) */
void rt_slots_dump(void) {
    const uint64_t dta = dta_seen;
    if (!dta || state < 0) return;
    static const int lists[3] = {0x58, 0x70, 0x88};
    const uint64_t recs = G64(dta + DTA_RECS);
    for (uint32_t r = 0; r < *(uint32_t *)GP(dta + DTA_NRECS) && r < 4; r++) {
        const uint64_t rec = G64(recs + 8ull * r), ar = G64(rec + REC_ARENAS);
        for (uint32_t i = 0; i < G32(rec + REC_NARENAS) && i < 16; i++) {
            const uint64_t a = ar + (uint64_t)i * 0xe8;
            hos_log("rt dump: record %u arena %u: %llu MB used; used list %u (%d), free list %u (%d), spare nodes %u (%d)",
                    r, i, (unsigned long long)(G64(a + AR_BYTES) >> 20), G32(a + lists[0] + 0x10), list_bad(a + lists[0]),
                    G32(a + lists[1] + 0x10), list_bad(a + lists[1]), G32(a + lists[2] + 0x10), list_bad(a + lists[2]));
        }
    }
    for (int i = -1; i < 256; i++) {
        const uint64_t b = i < 0 ? dta + 0x29d0 : dta + 0x11d0 + (uint64_t)i * 0x18;
        const int r = list_bad(b);
        if (r || i < 0) hos_log("rt dump: %s %d: %u (%d)", i < 0 ? "LRU" : "bucket", i, G32(b + 0x10), r);
    }
}

/* (nvn_native.c, at each present) one line every 600 presents */
void rt_slots_frame(void) {
    const uint64_t f = __atomic_add_fetch(&frame, 1, __ATOMIC_RELAXED);
    if (state == 1) {   /* the bytes no other target may take now: slots with a block, or used within REUSE_AFTER presents */
        uint64_t held = 0;
        AcquireSRWLockShared(&mu);
        for (int i = 0; i < nslots; i++)
            if (slots[i].block || slots[i].last_frame + REUSE_AFTER > f) held += slots[i].cap;
        ReleaseSRWLockShared(&mu);
        if (held > held_peak) held_peak = held;
        if (held > held_peak_block) held_peak_block = held;
    }
    if (f % 600 || (off_() && !log_slots()) || (state < 0 && !off_())) return;
    AcquireSRWLockShared(&mu);
    int busy = 0;
    for (int i = 0; i < nslots; i++) busy += slots[i].block != 0;
    const uint64_t inits = (uint64_t)nvn_texture_inits, dta_inits = (uint64_t)nvn_texture_inits_dta;
    const double fr = (double)(f - last_frame_log);
    uint64_t full = 0;
    for (uint32_t i = 0; i < narenas; i++)
        if (G64(arenas0 + (uint64_t)i * 0xe8 + AR_BYTES) > full) full = G64(arenas0 + (uint64_t)i * 0xe8 + AR_BYTES);
    hos_log("rt slots: %s, %d slots (%d busy), %llu MB (peak %llu) of %llu, fullest arena %llu MB; a frame: %.1f allocs, %.1f placed, %.2f slots "
            "made, %.1f texture inits (%.1f by the allocator) | all: %llu calls (%llu transient), %llu spliced (%llu made, "
            "%llu evicted, %llu game's; reused %llu same shape, %llu other), %llu splices without a "
            "note (persistent), %llu notes without a splice; %llu frees (%llu of slots, %llu unseen); allocator inits: %llu "
            "as before, %llu moved, %llu new; parking %s: %d parked, %llu parked in all, %llu handed back, %llu released, "
            "%llu over the limit, %llu released for a tight arena; %llu shadow sizes rounded; %llu frees without a block; "
            "native: %llu slots taken from parked entries, %llu aliased, %llu spare descriptors added",
            state == 1 ? "on" : "off", nslots, busy, (unsigned long long)(used >> 20), (unsigned long long)(peak >> 20),
            (unsigned long long)((hi - lo) >> 20), (unsigned long long)(full >> 20), (n.calls - last_n_calls) / fr, (n.spliced - last_n_spliced) / fr,
            (n.made - last_n_made) / fr, (inits - last_n_inits) / fr, (dta_inits - last_n_dta_inits) / fr,
            (unsigned long long)n.calls, (unsigned long long)n.transient, (unsigned long long)n.spliced,
            (unsigned long long)n.made, (unsigned long long)n.evicted, (unsigned long long)n.game_offset,
            (unsigned long long)n.same_shape, (unsigned long long)n.other_shape,
            (unsigned long long)n.unspliced, (unsigned long long)n.stale, (unsigned long long)n.frees,
            (unsigned long long)n.freed_slot, (unsigned long long)n.unseen_free,
            (unsigned long long)init_same, (unsigned long long)init_moved, (unsigned long long)init_new,
            park_on() ? "on" : "off", nparked, (unsigned long long)n.parked, (unsigned long long)n.handed_back,
            (unsigned long long)n.released, (unsigned long long)n.park_full, (unsigned long long)n.drained,
            (unsigned long long)n.quantized, (unsigned long long)n.blockless, (unsigned long long)n.stolen,
            (unsigned long long)n.aliased, (unsigned long long)n.topped);
    hos_log("rt slots: transient pool: %llu MB of %llu held at a present at most since the last line (%llu since the start: slots "
            "in use, parked, or used within %d presents); %llu slots evicted before 8 idle presents, %llu targets placed outside the slots; shadow "
            "classes: %llu sizes changed, %llu up, %llu down, %llu kept by hysteresis; targets given to the back end by their "
            "entry's object %llu (%llu entries with no registered object yet, %llu with an object over other storage), slots told "
            "%llu (%llu textures in the range at no slot), slots sized %llu (%llu by the device's need, larger than the "
            "game's; %lld textures of another format or target than their slot was sized for)",
            (unsigned long long)(held_peak_block >> 20), (unsigned long long)((hi - lo) >> 20), (unsigned long long)(held_peak >> 20),
            REUSE_AFTER, (unsigned long long)n.evicted_young, (unsigned long long)n.game_offset, (unsigned long long)n.quantized,
            (unsigned long long)n.class_up, (unsigned long long)n.class_down, (unsigned long long)n.class_kept,
            (unsigned long long)n.given, (unsigned long long)n.given_noid, (unsigned long long)n.given_other,
            (unsigned long long)n.slot_told, (unsigned long long)n.slot_untold, (unsigned long long)n.sized,
            (unsigned long long)n.sized_up, (long long)n_desc_other);
    last_n_calls = n.calls, last_n_spliced = n.spliced, last_n_inits = inits, last_n_dta_inits = dta_inits;
    last_n_made = n.made, last_frame_log = f;
    held_peak_block = 0;
    ReleaseSRWLockShared(&mu);
}
