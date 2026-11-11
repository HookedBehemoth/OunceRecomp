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
   own registration compare at alloc does not save the nvnTextureInitialize: the entry's last registration is zero by
   then, so the ~42 initializations a frame stay; work/assets/native/rt-slots-impl.md.)

     ENTRY_HOOK(0x8005ed00)       DTA::alloc(dta, ctx, desc, flags): note lr/name/shape for this thread (flags bit 0:
                                  transient); the first call shrinks the arenas. Returns 0 (the lifted body runs).
     SPLICE_AFTER_EACH(0x8005ed00) the two lines (0x8005fb74, 0x80061814) that store block +0x38 = the aligned offset:
                                  x23 block, x10 offset, x8 the block's pool. Both are rewritten to the slot.
     ENTRY_HOOK(0x802737a0)       DTA::free(dta, entry): the slot of entry +0xc0's block is free again.

   Persistent allocations (f_80062c50, from the top of the free block) do not set the note and are left to the game.
   Any mismatch (arenas not fresh, a pool smaller than the slot range, an arena offset inside it, a second pool record)
   turns the slots off for new allocations: those take the game's offsets again, which lie below the slot range.
   HOS_GUEST_RT_ALLOC=1: the game's allocator untouched (A/B). RX_RT_SLOTS_MB: S, default 256 (of 512).
   HOS_RT_SLOTS_LOG=1: every slot made, with its name, and why the allocator's first initializations after 1000
   presents were not skipped. Addresses and offsets are HOS_GAME_VERSION 100's (the only one);
   the splice's anchor stops the build on a lift without it. */
#include "hos.h"
#include "../gpu/render/rx_nvn.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define DTA_NRECS 0x2a48        /* u32 pool records */
#define DTA_RECS 0x2a50         /* u64 -> array of pool record pointers */
#define REC_ARENAS 0x50         /* pool record: u64 -> arenas[REC_NARENAS] of 0xe8 */
#define REC_NARENAS 0x48
#define AR_END 0x28             /* arena: u64 end (= first block offset + size at init) */
#define AR_USED 0x68            /* u32 blocks in use */
#define AR_FREE_HEAD 0x70       /* u64 first free-list node */
#define AR_FREE_N 0x80          /* u32 free blocks */
#define AR_NODE_OFF 0x84        /* s32 node offset in a block */
#define BLK_OFF 0x10            /* block: bookkeeping offset */
#define BLK_SIZE 0x30           /* u64 size the arena gave it */
#define ENT_BLOCK 0xc0          /* TextureData entry: u64 its block */
#define SP_ENTRY 40             /* alloc's frame: the entry, from 0x80060308 on (before: the descriptor cache's candidate) */
#define SP_ALIGN 192            /* alloc's frame: s32 the TextureData's storage alignment */

static int off_(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_GUEST_RT_ALLOC") && atoi(getenv("HOS_GUEST_RT_ALLOC")) != 0;
    return v;
}
static uint64_t slots_bytes(void) {
    static uint64_t v;
    if (!v) {
        const char *e = getenv("RX_RT_SLOTS_MB");
        const int mb = e ? atoi(e) : 256;
        v = (uint64_t)(mb > 0 ? mb : 256) << 20;
    }
    return v;
}
static int log_slots(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_RT_SLOTS_LOG") != NULL;
    return v;
}

typedef struct { uint64_t lr, name, dta; uint32_t fmt, type, w, h, d, mips, flags; int valid; } Cur;
static _Thread_local Cur cur;
static _Thread_local uint64_t alloc_sp, alloc_block;   /* (debug: the splice's frame, for the texture init after it) */

/* A slot belongs to a family (caller lr, name, format, type, depth, levels): one logical target. Its size may change
   (dynamic resolution, local shadows sized by distance), so a family's slot is reused by any of its shapes it holds. */
typedef struct {
    uint64_t fam, off, cap, block;   /* block: the arena block now in it (0: free) */
    uint64_t last_frame, name, lr;
    uint32_t occ, w, h, fmt;
} Slot;
#define MAX_SLOTS 4096
static Slot slots[MAX_SLOTS];
static int nslots;
static uint64_t lo, hi, used, peak;   /* the slot range [lo, hi) of pool offsets; bytes in slots */
static volatile uint64_t frame;
static SRWLOCK mu = SRWLOCK_INIT;
static volatile int state = -1;   /* -1 not tried, 0 off (never or backed off), 1 on */
static uint32_t nrecs0;
static uint64_t pools[16];   /* the virtual pools the slots were checked against */
static int npools;

static struct {   /* counters (under mu, or read racily for the log) */
    uint64_t calls, transient, spliced, unspliced, stale, frees, freed_slot, unseen_free, made, evicted, game_offset,
        same_shape, other_shape;
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
    if (base != 0 || size < 2 * slots_bytes()) {
        hos_log("rt slots: arenas at %#llx+%llu MB, off", (unsigned long long)base, (unsigned long long)(size >> 20));
        return;
    }
    for (uint32_t i = 0; i < na; i++) {
        const uint64_t a = ar + (uint64_t)i * 0xe8;
        const uint64_t b = G64(a + AR_FREE_HEAD) - (uint64_t)(int64_t)(int32_t)G32(a + AR_NODE_OFF);
        G64(b + BLK_SIZE) = size - slots_bytes();
        G64(a + AR_END) = base + size - slots_bytes();
    }
    lo = size - slots_bytes();
    hi = size;
    nrecs0 = nr;
    state = 1;
    hos_log("rt slots: %u arenas of %llu MB (were %llu), slots at +%#llx..+%#llx", na,
            (unsigned long long)((size - slots_bytes()) >> 20), (unsigned long long)(size >> 20), (unsigned long long)lo,
            (unsigned long long)hi);
}

ENTRY_HOOK(0x8005ed00) {   /* (dta, ctx, desc, flags) */
    if (off_()) return 0;
    __atomic_add_fetch(&n.calls, 1, __ATOMIC_RELAXED);
    if (cur.valid) __atomic_add_fetch(&n.stale, 1, __ATOMIC_RELAXED);   /* the last transient call never reached the splice */
    cur.valid = 0;
    if (state < 0) {
        AcquireSRWLockExclusive(&mu);
        if (state < 0) shrink_arenas(ARG(0));
        ReleaseSRWLockExclusive(&mu);
    }
    if (!(ARG(3) & 1)) return 0;   /* persistent: the game's */
    __atomic_add_fetch(&n.transient, 1, __ATOMIC_RELAXED);
    const uint64_t d = ARG(2);
    if (state != 1 || !hos_guest_range(d, 0x38)) return 0;
    cur = (Cur){c->x[30], G64(d), ARG(0), G32(d + 8), G32(d + 0xc), G32(d + 0x10), G32(d + 0x14), G32(d + 0x18),
                G32(d + 0x1c), G32(d + 0x30), 1};
    return 0;
}

static int by_off(const void *a, const void *b) {
    const uint64_t x = **(const uint64_t *const *)a, y = **(const uint64_t *const *)b;
    return x < y ? -1 : x > y;
}
/* the lowest aligned gap of cap bytes between the slots; idle slots (120 frames free) go, least recently used first,
   until one is found. ~0: none */
static uint64_t place(uint64_t cap, uint64_t align) {
    static const uint64_t *order[MAX_SLOTS];
    for (;;) {
        for (int i = 0; i < nslots; i++) order[i] = &slots[i].off;
        qsort(order, (size_t)nslots, sizeof *order, by_off);
        uint64_t at = lo;
        for (int i = 0; i <= nslots; i++) {
            at = (at + align - 1) & ~(align - 1);
            const uint64_t end = i < nslots ? *order[i] : hi;
            if (at + cap <= end) return at;
            if (i < nslots) {
                const Slot *s = (const Slot *)((const char *)order[i] - offsetof(Slot, off));
                if (s->off + s->cap > at) at = s->off + s->cap;
            }
        }
        int lru = -1;
        for (int i = 0; i < nslots; i++)
            if (!slots[i].block && slots[i].last_frame + 120 <= frame && (lru < 0 || slots[i].last_frame < slots[lru].last_frame))
                lru = i;
        if (lru < 0) return ~0ull;
        used -= slots[lru].cap;
        slots[lru] = slots[--nslots];
        n.evicted++;
    }
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
        const int rank = same ? 1 : 2;
        if (rank < best_rank || (rank == best_rank && (rank == 2 ? s->cap < best->cap : s->occ < best->occ)))
            best = s, best_rank = rank;
    }
    if (best) {
        if (best->w == cur.w && best->h == cur.h) n.same_shape++;
        else n.other_shape++;
    } else {
        /* a family seen before (its size moves, or more of it live at once) gets a quarter more room */
        const uint64_t cap = ((family ? need + need / 4 : need) + 0xffff) & ~0xffffULL;
        const uint64_t at = nslots < MAX_SLOTS ? place(cap, align) : ~0ull;
        if (at == ~0ull) return NULL;
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
    return best;
}

static int pool_ok(uint64_t pool) {   /* a virtual pool that holds the slot range */
    for (int i = 0; i < npools; i++)
        if (pools[i] == pool) return 1;
    const nvn_pool *p = hos_guest_range(pool, sizeof(nvn_pool));
    if (!p || p->magic != NVN_MAGIC('P', 'O', 'O', 'L') || p->mem || p->size < hi || npools == 16) {
        back_off("the block's pool", pool, p ? p->size : 0);
        return 0;
    }
    pools[npools++] = pool;
    hos_log("rt slots: pool %#llx (GPU %#llx, %llu MB, flags %#x)", (unsigned long long)pool, (unsigned long long)p->gpu,
            (unsigned long long)(p->size >> 20), p->flags);
    return 1;
}

/* (lifted f_8005ed00, after each "block +0x38 = aligned offset"): x23 block, x10 offset, x8 the pool. The next lifted
   line adds the TextureData's base to x10; everything after (entry +0, the registration compare, SetStorage, views
   made later from the entry) reads the offset from x10 or block +0x38. */
SPLICE_AFTER_EACH(0x8005ed00, "c->x[23] + 56,c->x[10]);") {
    if (!cur.valid) {
        if (!off_()) __atomic_add_fetch(&n.unspliced, 1, __ATOMIC_RELAXED);
        return;
    }
    const Cur k = cur;
    cur.valid = 0;
    const uint64_t block = c->x[23], game = c->x[10];
    alloc_sp = c->x[31], alloc_block = block;
    const uint64_t align = (uint64_t)(int64_t)(int32_t)G32(c->x[31] + SP_ALIGN);
    AcquireSRWLockExclusive(&mu);
    n.spliced++;
    Slot *s = NULL;
    if (state == 1) {
        const uint64_t need = G64(block + BLK_SIZE);
        if (game + need > lo) back_off("an arena offset in the slot range", game, need);
        else if (*(uint32_t *)GP(k.dta + DTA_NRECS) != nrecs0) back_off("a second pool record", nrecs0, 0);
        else if (align & (align - 1) || align > 0x1000000) back_off("alignment", align, 0);
        else if (pool_ok(c->x[8])) {
            cur = k;   /* (take reads the shape and name from it) */
            s = take(fam_of(&k), need, align, block);
            cur.valid = 0;
        }
    }
    if (!s) n.game_offset++;
    ReleaseSRWLockExclusive(&mu);
    if (!s) return;
    c->x[10] = s->off;
    G64(block + 0x38) = s->off;
}

ENTRY_HOOK(0x802737a0) {   /* (dta, entry) */
    if (off_()) return 0;
    const uint64_t e = ARG(1);
    if (!hos_guest_range(e, 0x180)) return 0;
    const uint64_t block = G64(e + ENT_BLOCK);
    AcquireSRWLockExclusive(&mu);
    n.frees++;
    if (block)
        for (int i = 0; i < nslots; i++)
            if (slots[i].block == block) {
                slots[i].block = 0;
                slots[i].last_frame = frame;
                n.freed_slot++;
                break;
            }
    ReleaseSRWLockExclusive(&mu);
    return 0;
}

/* (nvn_native.c) the allocator initializes texture object obj: the same address and shape as that object's last
   initialization (the game's registration cache missed for another reason), or not */
#define TMAP 4096
static struct { uint64_t obj, gpu; uint32_t w, h, fmt; } tmap[TMAP];
static uint64_t init_same, init_moved, init_new;
void rt_slots_texture_init(uint64_t obj, uint64_t gpu, uint32_t w, uint32_t h, uint32_t fmt) {
    if (!log_slots()) return;   /* (debugging: a lock per initialization) */
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

/* (nvn_native.c, at each present) one line every 600 presents */
void rt_slots_frame(void) {
    const uint64_t f = __atomic_add_fetch(&frame, 1, __ATOMIC_RELAXED);
    if (f % 600 || (off_() && !log_slots()) || (state < 0 && !off_())) return;
    AcquireSRWLockShared(&mu);
    int busy = 0;
    for (int i = 0; i < nslots; i++) busy += slots[i].block != 0;
    const uint64_t inits = (uint64_t)nvn_texture_inits, dta_inits = (uint64_t)nvn_texture_inits_dta;
    const double fr = (double)(f - last_frame_log);
    hos_log("rt slots: %s, %d slots (%d busy), %llu MB (peak %llu) of %llu; a frame: %.1f allocs, %.1f placed, %.2f slots "
            "made, %.1f texture inits (%.1f by the allocator) | all: %llu calls (%llu transient), %llu spliced (%llu made, "
            "%llu evicted, %llu game's; reused %llu same shape, %llu other), %llu splices without a "
            "note (persistent), %llu notes without a splice; %llu frees (%llu of slots, %llu unseen); allocator inits: %llu "
            "as before, %llu moved, %llu new",
            state == 1 ? "on" : "off", nslots, busy, (unsigned long long)(used >> 20), (unsigned long long)(peak >> 20),
            (unsigned long long)((hi - lo) >> 20), (n.calls - last_n_calls) / fr, (n.spliced - last_n_spliced) / fr,
            (n.made - last_n_made) / fr, (inits - last_n_inits) / fr, (dta_inits - last_n_dta_inits) / fr,
            (unsigned long long)n.calls, (unsigned long long)n.transient, (unsigned long long)n.spliced,
            (unsigned long long)n.made, (unsigned long long)n.evicted, (unsigned long long)n.game_offset,
            (unsigned long long)n.same_shape, (unsigned long long)n.other_shape,
            (unsigned long long)n.unspliced, (unsigned long long)n.stale, (unsigned long long)n.frees,
            (unsigned long long)n.freed_slot, (unsigned long long)n.unseen_free,
            (unsigned long long)init_same, (unsigned long long)init_moved, (unsigned long long)init_new);
    last_n_calls = n.calls, last_n_spliced = n.spliced, last_n_inits = inits, last_n_dta_inits = dta_inits;
    last_n_made = n.made, last_frame_log = f;
    ReleaseSRWLockShared(&mu);
}
