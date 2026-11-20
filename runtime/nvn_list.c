/* (native-vk layer 1, stage S1) Command lists by reference, HOS_NVN_LIST_REF=1: see nvn_list.h for the design and the
   lifetime rules. Used by runtime/nvn_native.c only; no guest code and no renderer state here, so tests/list_ref_test.c
   builds this file as it is. */
#include "nvn_list.h"
#include "hos_ledger.h"
#include "rx_nvn.h"
#include <stdlib.h>
#include <string.h>
#include "os.h"
#define list_yield() os_yield()

static void (*say)(const char *fmt, ...);
static void (*fatal)(const char *fmt, ...);
static void say_none(const char *fmt, ...) { (void)fmt; }
static void fatal_abort(const char *fmt, ...) { (void)fmt; abort(); }

void nvn_list_init(void (*log)(const char *fmt, ...), void (*die)(const char *fmt, ...)) {
    say = log ? log : say_none;
    fatal = die ? die : fatal_abort;
}
static void (*user_released)(nvn_lrec *r), (*user_freed)(nvn_lrec *r);
void nvn_list_set_user_hooks(void (*released)(nvn_lrec *r), void (*freed)(nvn_lrec *r)) {
    user_released = released, user_freed = freed;
}

/* ---- small locks: held for a few loads and stores (a bucket chain, a free list, the dead list's head) ---- */
static void spin_lock(volatile int32_t *l) {
    for (int n = 0; __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE);) {
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
            if (++n < 64) {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
            } else {
                list_yield();
            }
        }
    }
}
static void spin_unlock(volatile int32_t *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

static volatile uint64_t st_made, st_live, st_live_bytes, st_calls, st_released, st_freed, st_grown, st_shrunk, st_swept,
    st_copied;
#define COUNT(v, n) __atomic_add_fetch(&(v), (uint64_t)(n), __ATOMIC_RELAXED)
void nvn_list_get_stats(nvn_list_stats *s) {
#define G(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)
    s->made = G(st_made), s->live = G(st_live), s->live_bytes = G(st_live_bytes), s->calls = G(st_calls);
    s->released = G(st_released), s->freed = G(st_freed);
    s->grown = G(st_grown), s->shrunk = G(st_shrunk), s->swept = G(st_swept), s->copied = G(st_copied);
#undef G
}

/* ---- allocations: power-of-two classes from 1 KiB, a free list per class and thread, the rest shared ---- */
#define LCLS_MIN 10u
#define LCLS_MAX 27u        /* 128 MiB; bigger ones from the heap as they are */
#define LCLS_HEAP 0xffu
typedef struct { void *head; uint32_t n; } lcache;
static _Thread_local lcache tl_cache[LCLS_MAX + 1];
static void *g_cache[LCLS_MAX + 1];
static uint32_t g_cache_n[LCLS_MAX + 1];
static volatile int32_t g_cache_lock;
static uint32_t cls_of(size_t total) {
    uint32_t c = LCLS_MIN;
    while (c <= LCLS_MAX && ((size_t)1 << c) < total) c++;
    return c <= LCLS_MAX ? c : LCLS_HEAP;
}
static uint32_t tl_keep(uint32_t c) { const uint32_t k = (uint32_t)((4u << 20) >> c); return k < 4 ? 4 : k; }   /* ~4 MiB a class */
static uint32_t g_keep(uint32_t c) { const uint32_t k = (uint32_t)((64u << 20) >> c); return k < 2 ? 2 : k; }  /* ~64 MiB */
static void *blk_get(size_t total, uint32_t *cls) {
    const uint32_t c = cls_of(total);
    *cls = c;
    if (c == LCLS_HEAP) return hos_ledger_took(HL_NVN_LISTS, malloc(total));
    lcache *t = &tl_cache[c];
    if (!t->head && __atomic_load_n(&g_cache[c], __ATOMIC_RELAXED)) {   /* (a peek without the lock) */   /* (half of what this thread keeps, from the shared list) */
        spin_lock(&g_cache_lock);
        for (uint32_t k = tl_keep(c) / 2 + 1; k && g_cache[c]; k--) {
            void *b = g_cache[c];
            __atomic_store_n(&g_cache[c], *(void **)b, __ATOMIC_RELAXED), g_cache_n[c]--;
            *(void **)b = t->head, t->head = b, t->n++;
        }
        spin_unlock(&g_cache_lock);
    }
    if (t->head) {
        void *b = t->head;
        t->head = *(void **)b, t->n--;
        return b;
    }
    void *b = malloc((size_t)1 << c);
    if (b) hos_ledger_add(HL_NVN_LISTS, (int64_t)1 << c);
    return b;
}
static void blk_put(void *b, uint32_t c) {
    if (!b) return;
    if (c == LCLS_HEAP) {
        hos_ledger_frees(HL_NVN_LISTS, b);
        free(b);
        return;
    }
    lcache *t = &tl_cache[c];
    *(void **)b = t->head, t->head = b, t->n++;
    if (t->n <= tl_keep(c)) return;
    /* (over: half of them to the shared list, what it does not keep to the heap) */
    void *give = NULL;
    for (uint32_t k = t->n / 2; k; k--) {
        void *x = t->head;
        t->head = *(void **)x, t->n--;
        *(void **)x = give, give = x;
    }
    spin_lock(&g_cache_lock);
    while (give && g_cache_n[c] < g_keep(c)) {
        void *x = give;
        give = *(void **)x;
        *(void **)x = g_cache[c], __atomic_store_n(&g_cache[c], x, __ATOMIC_RELAXED), g_cache_n[c]++;
    }
    spin_unlock(&g_cache_lock);
    while (give) {
        void *x = give;
        give = *(void **)x;
        free(x);
        hos_ledger_add(HL_NVN_LISTS, -((int64_t)1 << c));
    }
}

/* ---- handles: (generation << 32) | slot id. Slots in chunks never freed: a lookup needs no lock ---- */
typedef struct { uint64_t gen; nvn_lrec *rec; } lslot;
#define LID_CHUNK_BITS 12u
#define LID_CHUNKS 4096u    /* 16M handles at once */
static lslot *lid_chunks[LID_CHUNKS];
static uint32_t lid_next = 1;   /* (slot 0: none) */
static volatile int32_t lid_lock;
static uint32_t *lid_free;
static size_t lid_nfree, lid_cap;
static _Thread_local uint32_t tl_ids[256];
static _Thread_local uint32_t tl_nids;
static lslot *lid_slot(uint32_t id) {
    if (!id || (id >> LID_CHUNK_BITS) >= LID_CHUNKS) return NULL;
    lslot *k = __atomic_load_n(&lid_chunks[id >> LID_CHUNK_BITS], __ATOMIC_ACQUIRE);
    return k ? &k[id & ((1u << LID_CHUNK_BITS) - 1)] : NULL;
}
static uint32_t lid_take(void) {
    if (!tl_nids) {
        spin_lock(&lid_lock);
        while (lid_nfree && tl_nids < 64) tl_ids[tl_nids++] = lid_free[--lid_nfree];
        if (!tl_nids) {   /* (64 new ones) */
            const uint32_t first = lid_next;
            if (((first + 64) >> LID_CHUNK_BITS) >= LID_CHUNKS) {
                spin_unlock(&lid_lock);
                fatal("nvn: list handles: more than %u recordings at once", LID_CHUNKS << LID_CHUNK_BITS);
            }
            lid_next += 64;
            for (uint32_t i = 0; i < 64; i++) {
                const uint32_t id = first + 63 - i, ch = id >> LID_CHUNK_BITS;
                if (!lid_chunks[ch]) {
                    lslot *k = (lslot *)calloc((size_t)1 << LID_CHUNK_BITS, sizeof *k);
                    if (!k) fatal("nvn: list handle table");
                    for (uint32_t j = 0; j < (1u << LID_CHUNK_BITS); j++) k[j].gen = 1;
                    __atomic_store_n(&lid_chunks[ch], k, __ATOMIC_RELEASE);
                }
                tl_ids[tl_nids++] = id;
            }
        }
        spin_unlock(&lid_lock);
    }
    return tl_ids[--tl_nids];
}
static void lid_give(uint32_t id) {
    if (tl_nids < 256) {
        tl_ids[tl_nids++] = id;
        return;
    }
    spin_lock(&lid_lock);
    while (tl_nids > 128) {
        if (lid_nfree == lid_cap) {
            lid_cap = lid_cap ? lid_cap * 2 : 4096;
            lid_free = (uint32_t *)realloc(lid_free, lid_cap * sizeof *lid_free);
            if (!lid_free) fatal("nvn: list handle table");
        }
        lid_free[lid_nfree++] = tl_ids[--tl_nids];
    }
    spin_unlock(&lid_lock);
    tl_ids[tl_nids++] = id;
}
nvn_lrec *nvn_list_lookup(uint64_t handle) {
    lslot *s = lid_slot((uint32_t)handle);
    const uint32_t want = (uint32_t)(handle >> 32);
    if (!s || !want) return NULL;
    const uint64_t g = __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE);
    nvn_lrec *r = __atomic_load_n(&s->rec, __ATOMIC_ACQUIRE);
    if ((uint32_t)g != want || !r || __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) != g) return NULL;
    return r;
}
static uint64_t lid_publish(nvn_lrec *r) {
    const uint32_t id = lid_take();
    lslot *s = lid_slot(id);
    r->id = id;
    __atomic_store_n(&s->rec, r, __ATOMIC_RELEASE);
    return (__atomic_load_n(&s->gen, __ATOMIC_RELAXED) << 32) | id;
}
static void lid_retire(nvn_lrec *r) {   /* (its handle stops working now) */
    lslot *s = lid_slot(r->id);
    if (!s) return;
    __atomic_store_n(&s->rec, NULL, __ATOMIC_RELEASE);
    uint64_t g = __atomic_load_n(&s->gen, __ATOMIC_RELAXED) + 1;
    if (!(uint32_t)g) g++;   /* (generation 0 is no handle's) */
    __atomic_store_n(&s->gen, (uint64_t)(uint32_t)g, __ATOMIC_RELEASE);
    lid_give(r->id);
    r->id = 0;
}

/* ---- the control index: buckets of 64 KiB blocks of the control memory position, 256 buckets to a lock ---- */
#define LIDX_SHIFT 16u
#define LIDX_BUCKETS 65536u
#define LIDX_PER_LOCK 256u
static nvn_lrec *lidx[LIDX_BUCKETS];
static volatile int32_t lidx_lock[LIDX_BUCKETS / LIDX_PER_LOCK];
static unsigned lidx_bucket(uint64_t ctl) { return (unsigned)(ctl >> LIDX_SHIFT) & (LIDX_BUCKETS - 1); }
static void lidx_add(nvn_lrec *r) {
    const unsigned b = lidx_bucket(r->ctl);
    volatile int32_t *l = &lidx_lock[b / LIDX_PER_LOCK];
    spin_lock(l);
    r->next = lidx[b];
    lidx[b] = r;
    spin_unlock(l);
}

/* ---- the dead list: released recordings and private copies, oldest first, freed once the replay is past them ---- */
typedef struct { nvn_lrec *r; uint64_t after; } ldead;
static ldead *dead;
static size_t dead_cap, dead_head, dead_n;   /* (a ring) */
static volatile int32_t dead_lock;
static uint64_t dead_first = UINT64_MAX;     /* (the oldest's after: read without the lock) */
static void dead_push(nvn_lrec **rs, size_t n, uint64_t after) {
    if (!n) return;
    spin_lock(&dead_lock);
    if (dead_n + n > dead_cap) {
        size_t cap = dead_cap ? dead_cap : 1024;
        while (cap < dead_n + n) cap *= 2;
        ldead *d = (ldead *)malloc(cap * sizeof *d);
        if (!d) fatal("nvn: list dead list");
        for (size_t i = 0; i < dead_n; i++) d[i] = dead[(dead_head + i) % dead_cap];
        free(dead);
        dead = d, dead_cap = cap, dead_head = 0;
    }
    for (size_t i = 0; i < n; i++) dead[(dead_head + dead_n++) % dead_cap] = (ldead){rs[i], after};
    __atomic_store_n(&dead_first, dead[dead_head].after, __ATOMIC_RELEASE);
    spin_unlock(&dead_lock);
}

static nvn_p_call *call_at(const nvn_lrec *r, uint32_t off) { return (nvn_p_call *)(void *)((uint8_t *)(uintptr_t)nvn_lrec_begin(r) + off + 8); }
static nvn_lrec *hdr_of(const uint8_t *begin) { return (nvn_lrec *)(uintptr_t)begin - 1; }




static void rec_free_storage(nvn_lrec *r) {
    COUNT(st_freed, 1);
    if (!(r->flags & NVN_LREC_PRIVATE)) __atomic_sub_fetch(&st_live, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&st_live_bytes, r->cap, __ATOMIC_RELAXED);
    if (r->user && user_freed) user_freed(r);
    const uint32_t cls = r->cls;
    r->magic = 0;
    blk_put(r, cls);
}
/* one reference let go; at 0 its bytes go, and its references on its callees (done: the replay's progress, for the check) */
static void rec_unref(nvn_lrec *r, uint64_t done) {
    nvn_lrec *work = NULL;   /* (freed ones to walk, chained through next: no recursion) */
    if (__atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL)) return;
    r->next = NULL, work = r;
    while (work) {
        nvn_lrec *x = work;
        work = x->next;
        if (x->magic != NVN_LREC_MAGIC) fatal("nvn: list: a recording freed twice (%p)", (void *)x);
        for (uint32_t off = x->first_call; off;) {
            const nvn_p_call *c = call_at(x, off);
            nvn_lrec *t = hdr_of(c->target);
            off = c->next;
            if (!__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL)) t->next = work, work = t;
        }
        rec_free_storage(x);
    }
}

void nvn_list_drain(uint64_t done) {
    if (__atomic_load_n(&dead_first, __ATOMIC_ACQUIRE) > done) return;
    nvn_lrec *ready[256];
    size_t n = 0;
    spin_lock(&dead_lock);
    while (dead_n && n < 256 && dead[dead_head].after <= done) {
        ready[n++] = dead[dead_head].r;
        dead_head = (dead_head + 1) % dead_cap, dead_n--;
    }
    __atomic_store_n(&dead_first, dead_n ? dead[dead_head].after : UINT64_MAX, __ATOMIC_RELEASE);
    spin_unlock(&dead_lock);
    for (size_t i = 0; i < n; i++) rec_unref(ready[i], done);
}

/* ---- recording ---- */
#define HINTS 256u
typedef struct { uint64_t key, bytes; } lhint;
static _Thread_local lhint tl_hints[HINTS];
static unsigned hint_at(uint64_t key) { return (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 56) & (HINTS - 1); }

static nvn_lrec *rec_alloc(size_t cap) {
    uint32_t cls;
    nvn_lrec *r = (nvn_lrec *)blk_get(sizeof(nvn_lrec) + cap, &cls);
    if (!r) fatal("nvn: list: a recording of %zu bytes", cap);
    memset(r, 0, sizeof *r);
    r->magic = NVN_LREC_MAGIC;
    r->cls = cls;
    r->cap = (cls == LCLS_HEAP ? sizeof(nvn_lrec) + cap : (size_t)1 << cls) - sizeof(nvn_lrec);
    r->ctl = ~0ull;
    COUNT(st_live_bytes, r->cap);
    return r;
}
static void w_reset(nvn_lw *w) {
    w->cur = w->seg;
    w->last_call = w->first_call = w->first_def = w->last_def = 0;
    w->flags = w->depth = 0;
    w->flat_extra = 0;
    w->user = NULL;
}
/* the open recording's calls up to cur let go (dropped records: their references on the callees) */
static void w_drop_calls(nvn_lw *w) {
    for (uint32_t off = w->first_call; off;) {
        const nvn_p_call *c = (const nvn_p_call *)(const void *)(w->seg + off + 8);
        off = c->next;
        rec_unref(hdr_of(c->target), UINT64_MAX);
    }
    w->first_call = w->last_call = 0;
}
void nvn_list_open(nvn_lw *w, uint64_t key) {
    if (w->rec) {
        w_drop_calls(w);
        w_reset(w);
        w->key = key;
        return;
    }
    const lhint *h = &tl_hints[hint_at(key)];
    const uint64_t guess = key && h->key == key ? h->bytes : 0;
    size_t cap = (size_t)(guess + guess / 8) + 256;
    if (cap < 2048 - sizeof(nvn_lrec)) cap = 2048 - sizeof(nvn_lrec);
    w->rec = rec_alloc(cap);
    w->seg = (uint8_t *)(w->rec + 1);
    w->end = w->seg + w->rec->cap;
    w->key = key;
    w_reset(w);
}
/* room for need bytes at cur (and NVN_LIST_SLACK): grows the open recording, opening one when none is */
static void nvn_list_reserve(nvn_lw *w, size_t need) {
    if (!w->rec) nvn_list_open(w, w->key);
    if (need + NVN_LIST_SLACK <= (size_t)(w->end - w->cur)) return;
    const size_t used = (size_t)(w->cur - w->seg);
    size_t cap = (size_t)w->rec->cap * 2;
    while (cap < used + need + NVN_LIST_SLACK) cap *= 2;
    nvn_lrec *n = rec_alloc(cap - sizeof(nvn_lrec));
    memcpy(n + 1, w->seg, used);
    COUNT(st_copied, used);
    nvn_lrec *old = w->rec;
    __atomic_sub_fetch(&st_live_bytes, old->cap, __ATOMIC_RELAXED);
    blk_put(old, old->cls);
    w->rec = n;
    w->seg = (uint8_t *)(n + 1);
    w->cur = w->seg + used;
    w->end = w->seg + n->cap;
    COUNT(st_grown, 1);
}
nvn_lrec *nvn_list_end(nvn_lw *w, uint64_t ctl, int marked, uint64_t *handle) {
    if (!w->rec) nvn_list_reserve(w, 0);
    nvn_lrec *r = w->rec;
    const uint64_t bytes = (uint64_t)(w->cur - w->seg);
    if (r->cap >= 8192 && bytes * 4 < r->cap) {   /* (a guess far too big: into one that fits, once) */
        nvn_lrec *n = rec_alloc((size_t)bytes);
        memcpy(n + 1, w->seg, (size_t)bytes);
        COUNT(st_copied, bytes);
        __atomic_sub_fetch(&st_live_bytes, r->cap, __ATOMIC_RELAXED);
        blk_put(r, r->cls);
        r = n;
        COUNT(st_shrunk, 1);
    }
    r->bytes = bytes;
    r->flat = bytes + w->flat_extra;
    r->first_call = w->first_call, r->first_def = w->first_def, r->last_def = w->last_def;
    r->flags = w->flags | (marked ? NVN_LREC_MARKED : 0), r->depth = w->depth;
    r->refs = 1;
    r->ctl = ctl;
    r->user = w->user, w->user = NULL;
    if (w->key) {
        lhint *h = &tl_hints[hint_at(w->key)];
        /* (the biggest of the last ones, let down by an eighth a recording: a job's command buffer on a worker's stack
           records lists of every size at one address; the last size alone left a third of them growing) */
        const uint64_t was = h->key == w->key ? h->bytes - h->bytes / 8 : 0;
        h->key = w->key, h->bytes = bytes > was ? bytes : was;
    }
    w->rec = NULL;
    w->seg = w->cur = w->end = NULL;
    *handle = lid_publish(r);
    if (ctl != ~0ull) lidx_add(r);
    COUNT(st_made, 1);
    COUNT(st_live, 1);
    return r;
}
void nvn_list_drop(nvn_lw *w) {
    if (!w->rec) return;
    w_drop_calls(w);
    __atomic_sub_fetch(&st_live_bytes, w->rec->cap, __ATOMIC_RELAXED);
    blk_put(w->rec, w->rec->cls);
    w->rec = NULL;
    w->seg = w->cur = w->end = NULL;
}

/* ---- releases ---- */
static void release_list(nvn_lrec *list, uint64_t after) {
    nvn_lrec *batch[256];
    size_t n = 0;
    while (list) {
        nvn_lrec *r = list;
        list = r->next;
        lid_retire(r);
        r->flags |= NVN_LREC_RELEASED;
        if (r->user && user_released) user_released(r);
        r->after = after;
        r->next = NULL;
        batch[n++] = r;
        COUNT(st_released, 1);
        if (n == 256) dead_push(batch, n, after), n = 0;
    }
    dead_push(batch, n, after);
}
void nvn_list_release(uint64_t lo, uint64_t hi, uint64_t after) {
    if (hi <= lo) return;
    const uint64_t blocks = ((hi - 1) >> LIDX_SHIFT) - (lo >> LIDX_SHIFT) + 1;
    const uint64_t steps = blocks > LIDX_BUCKETS ? LIDX_BUCKETS : blocks;
    nvn_lrec *out = NULL;
    volatile int32_t *held = NULL;
    for (uint64_t k = 0; k < steps; k++) {
        const unsigned b = blocks > LIDX_BUCKETS ? (unsigned)k : lidx_bucket(((lo >> LIDX_SHIFT) + k) << LIDX_SHIFT);
        if (!__atomic_load_n(&lidx[b], __ATOMIC_RELAXED)) continue;
        volatile int32_t *l = &lidx_lock[b / LIDX_PER_LOCK];
        if (l != held) {
            if (held) spin_unlock(held);
            spin_lock(l);
            held = l;
        }
        for (nvn_lrec **pn = &lidx[b]; *pn;) {
            nvn_lrec *r = *pn;
            if (r->ctl < lo || r->ctl >= hi) {
                pn = &r->next;
                continue;
            }
            *pn = r->next;
            r->next = out, out = r;
        }
    }
    if (held) spin_unlock(held);
    release_list(out, after);
}
size_t nvn_list_sweep(uint64_t mark, uint64_t after, void *(*guest)(uint64_t a, uint64_t n)) {
    nvn_lrec *out = NULL;
    size_t swept = 0;
    for (unsigned l = 0; l < LIDX_BUCKETS / LIDX_PER_LOCK; l++) {
        spin_lock(&lidx_lock[l]);
        for (unsigned b = l * LIDX_PER_LOCK; b < (l + 1) * LIDX_PER_LOCK; b++) {
            for (nvn_lrec **pn = &lidx[b]; *pn;) {
                nvn_lrec *r = *pn;
                if (!(r->flags & NVN_LREC_MARKED)) {
                    pn = &r->next;
                    continue;
                }
                const lslot *s = lid_slot(r->id);
                const uint64_t h = s ? (s->gen << 32) | r->id : 0;
                uint64_t m[2] = {0, 0};
                const void *at = guest(r->ctl, sizeof m);
                if (at) memcpy(m, at, sizeof m);
                if (!at || (m[0] == mark && m[1] == h)) {
                    pn = &r->next;
                    continue;
                }
                *pn = r->next;
                r->next = out, out = r;
                swept++;
            }
        }
        spin_unlock(&lidx_lock[l]);
    }
    COUNT(st_swept, swept);
    release_list(out, after);
    return swept;
}

