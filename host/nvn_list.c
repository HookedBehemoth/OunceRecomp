/* (native-vk layer 1, stage S1) Command lists by reference, HOS_NVN_LIST_REF=1: see nvn_list.h for the design and the
   lifetime rules. Used by host/nvn_native.c only; no guest code and no renderer state here, so tests/list_ref_test.c
   builds this file as it is. */
#include "nvn_list.h"
#include "../gpu/render/rx_nvn.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define list_yield() SwitchToThread()
#else
#include <sched.h>
#define list_yield() sched_yield()
#endif

static int check_on;
static void (*say)(const char *fmt, ...);
static void (*fatal)(const char *fmt, ...);
static void say_none(const char *fmt, ...) { (void)fmt; }
static void fatal_abort(const char *fmt, ...) { (void)fmt; abort(); }

void nvn_list_init(int check, void (*log)(const char *fmt, ...), void (*die)(const char *fmt, ...)) {
    check_on = check;
    say = log ? log : say_none;
    fatal = die ? die : fatal_abort;
}
int nvn_list_check_on(void) { return check_on; }

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

static volatile uint64_t st_made, st_live, st_live_bytes, st_calls, st_calls_expanded, st_released, st_freed, st_clones,
    st_conflicts, st_in_place, st_grown, st_shrunk, st_swept, st_check_handles, st_check_bytes, st_check_mismatch,
    st_check_early, st_check_changed, st_copied;
#define COUNT(v, n) __atomic_add_fetch(&(v), (uint64_t)(n), __ATOMIC_RELAXED)
void nvn_list_get_stats(nvn_list_stats *s) {
#define G(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)
    s->made = G(st_made), s->live = G(st_live), s->live_bytes = G(st_live_bytes), s->calls = G(st_calls);
    s->calls_expanded = G(st_calls_expanded), s->released = G(st_released), s->freed = G(st_freed);
    s->clones = G(st_clones), s->conflicts = G(st_conflicts), s->resolved_in_place = G(st_in_place);
    s->grown = G(st_grown), s->shrunk = G(st_shrunk), s->swept = G(st_swept), s->copied = G(st_copied);
    s->check_handles = G(st_check_handles), s->check_bytes = G(st_check_bytes), s->check_mismatch = G(st_check_mismatch);
    s->check_early_free = G(st_check_early), s->check_changed = G(st_check_changed);
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
    if (c == LCLS_HEAP) return malloc(total);
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
    return malloc((size_t)1 << c);
}
static void blk_put(void *b, uint32_t c) {
    if (!b) return;
    if (c == LCLS_HEAP) {
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

/* ---- records ---- */
static nvn_op_hdr op_at(const uint8_t *p) {
    nvn_op_hdr h;
    memcpy(&h, p, sizeof h);
    return h;
}
static int is_deferred(const uint8_t *op) {
    const nvn_op_hdr h = op_at(op);
    if (h.op == NOP_BIND_TEX || h.op == NOP_BIND_IMG || h.op == NOP_BIND_SAMP) {
        nvn_p_bind_handles b;
        memcpy(&b, op + 8, offsetof(nvn_p_bind_handles, h));
        return b.src != 0;
    }
    if (h.op == NOP_BIND_BUFS) {
        nvn_p_bind_bufs b;
        memcpy(&b, op + 8, offsetof(nvn_p_bind_bufs, r));
        return b.src != 0;
    }
    return 0;
}
static nvn_p_call *call_at(const nvn_lrec *r, uint32_t off) { return (nvn_p_call *)(void *)((uint8_t *)(uintptr_t)nvn_lrec_begin(r) + off + 8); }
static nvn_lrec *hdr_of(const uint8_t *begin) { return (nvn_lrec *)(uintptr_t)begin - 1; }

/* (check) a hash of the bytes, the deferred arrays left out (each submit writes them) */
static uint64_t rec_hash(const nvn_lrec *r) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = nvn_lrec_begin(r), *end = p + r->bytes;
    while (p < end) {
        const nvn_op_hdr o = op_at(p);
        size_t n = o.size;
        if (is_deferred(p)) n = 8 + (o.op == NOP_BIND_BUFS ? offsetof(nvn_p_bind_bufs, r) : offsetof(nvn_p_bind_handles, h));
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
        if (!o.size) break;
        p += o.size;
    }
    return h;
}

/* the records from begin to its END with every callee's records copied in (their BEGIN and END left out); top: this
   one's BEGIN and END too. Returns the bytes written */
static size_t expand_into(const uint8_t *p, uint8_t *out, int top) {
    uint8_t *o = out;
    for (;;) {
        const nvn_op_hdr h = op_at(p);
        if (h.op == NOP_CALL) {
            nvn_p_call c;
            memcpy(&c, p + 8, sizeof c);
            if (c.target) o += expand_into(c.target, o, 0);
        } else if ((h.op != NOP_BEGIN && h.op != NOP_END) || top) {
            memcpy(o, p, h.size);
            o += h.size;
        }
        if (h.op == NOP_END || !h.size) return (size_t)(o - out);
        p += h.size;
    }
}
void nvn_list_expand(const nvn_lrec *r, uint8_t *out) { expand_into(nvn_lrec_begin(r), out, 1); }

/* the deferred binds' arrays from the game: compare (write 0) or write (1). Returns 1 when they were all equal */
static int def_rw(nvn_lrec *r, int write, void *(*guest)(uint64_t a, uint64_t n)) {
    if (!r->first_def) return 1;
    uint8_t *p = (uint8_t *)(uintptr_t)nvn_lrec_begin(r) + r->first_def, *last = (uint8_t *)(uintptr_t)nvn_lrec_begin(r) + r->last_def;
    int same = 1;
    for (;;) {
        const nvn_op_hdr h = op_at(p);
        if (h.op == NOP_BIND_TEX || h.op == NOP_BIND_IMG || h.op == NOP_BIND_SAMP) {   /* (as resolve_deferred) */
            nvn_p_bind_handles *b = (nvn_p_bind_handles *)(void *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                const void *g = guest(b->src, 8 * (size_t)b->n);
                if (memcmp(b->h, g, 8 * (size_t)b->n)) {
                    same = 0;
                    if (write) memcpy(b->h, g, 8 * (size_t)b->n);
                }
            }
        }
        if (h.op == NOP_BIND_BUFS) {
            nvn_p_bind_bufs *b = (nvn_p_bind_bufs *)(void *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 16) {
                const void *g = guest(b->src, sizeof(NVNbufferRange) * (size_t)b->n);
                if (memcmp(b->r, g, sizeof(NVNbufferRange) * (size_t)b->n)) {
                    same = 0;
                    if (write) memcpy(b->r, g, sizeof(NVNbufferRange) * (size_t)b->n);
                }
            }
        }
        if (p >= last || h.op == NOP_END || !h.size) return same;
        p += h.size;
    }
}
/* (check) the default path's resolve_deferred, on a copy */
static void resolve_flat(uint8_t *p, void *(*guest)(uint64_t a, uint64_t n)) {
    for (;;) {
        const nvn_op_hdr h = op_at(p);
        if (h.size < 8 || (h.size & 7)) fatal("nvn: list check: a corrupt copy (op %u size %u)", h.op, h.size);
        if (h.op == NOP_END) return;
        if (h.op == NOP_BIND_TEX || h.op == NOP_BIND_IMG || h.op == NOP_BIND_SAMP) {
            nvn_p_bind_handles *b = (nvn_p_bind_handles *)(void *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) memcpy(b->h, guest(b->src, 8 * (size_t)b->n), 8 * (size_t)b->n);
        }
        if (h.op == NOP_BIND_BUFS) {
            nvn_p_bind_bufs *b = (nvn_p_bind_bufs *)(void *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 16) memcpy(b->r, guest(b->src, sizeof(NVNbufferRange) * (size_t)b->n), sizeof(NVNbufferRange) * (size_t)b->n);
        }
        p += h.size;
    }
}

static void rec_free_storage(nvn_lrec *r) {
    COUNT(st_freed, 1);
    if (!(r->flags & NVN_LREC_PRIVATE)) __atomic_sub_fetch(&st_live, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&st_live_bytes, r->cap, __ATOMIC_RELAXED);
    free(r->shadow);
    const uint32_t cls = r->cls;
    if (check_on) memset(r, 0, sizeof *r + (size_t)(r->bytes ? r->bytes : 8));   /* (a walk into freed bytes meets END: op 0) */
    else r->magic = 0;
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
        if (check_on) {
            if (x->reached > done && done != UINT64_MAX) {   /* (a submit not replayed yet walks it: kept, never freed) */
                const uint64_t k = COUNT(st_check_early, 1);
                if (k <= 20) say("nvn: list check: recording %p freed while submit %llu walks it (replayed: %llu): kept",
                                 (void *)x, (unsigned long long)x->reached, (unsigned long long)done);
                continue;
            }
            if (x->hash && rec_hash(x) != x->hash) {
                const uint64_t k = COUNT(st_check_changed, 1);
                if (k <= 20) say("nvn: list check: recording %p (%llu bytes) changed after EndRecording", (void *)x, (unsigned long long)x->bytes);
            }
        }
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
void nvn_list_reserve(nvn_lw *w, size_t need) {
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
void nvn_list_note_deferred(nvn_lw *w, const uint8_t *op) {
    if (!w->rec || op < w->seg || op >= w->cur) return;
    const uint32_t off = (uint32_t)(op - w->seg);
    if (!w->first_def) w->first_def = off;
    w->last_def = off;
    w->flags |= NVN_LREC_OWN_DEF;
}
int nvn_list_call(nvn_lw *w, uint64_t handle, uint64_t *flat) {
    nvn_lrec *r = nvn_list_lookup(handle);
    if (!r) return 0;
    *flat = r->flat;
    if (r->flat <= 16) return 1;   /* (BEGIN and END only: nothing, as the default path copies nothing) */
    if (!w->rec) nvn_list_open(w, w->key);
    if (r->depth + 1 > NVN_LIST_MAX_DEPTH) {   /* (deeper than the replay's stack: its records copied in, as by default) */
        const size_t n = (size_t)r->flat - 16;
        nvn_list_reserve(w, n);
        const uint32_t at = (uint32_t)(w->cur - w->seg);
        expand_into(nvn_lrec_begin(r), w->cur, 0);
        w->cur += n;
        if (r->flags & (NVN_LREC_OWN_DEF | NVN_LREC_SUB_DEF)) {   /* (its deferred binds are this one's own now) */
            for (const uint8_t *p = w->seg + at; p < w->cur; p += op_at(p).size) {
                if (!is_deferred(p)) continue;
                if (!w->first_def) w->first_def = (uint32_t)(p - w->seg);
                w->last_def = (uint32_t)(p - w->seg);
                w->flags |= NVN_LREC_OWN_DEF;
            }
        }
        COUNT(st_calls_expanded, 1);
        return 1;
    }
    nvn_list_reserve(w, 8 + sizeof(nvn_p_call));
    const uint32_t off = (uint32_t)(w->cur - w->seg);
    const nvn_op_hdr h = {NOP_CALL, 8 + sizeof(nvn_p_call)};
    const nvn_p_call c = {nvn_lrec_begin(r), 0, (uint32_t)r->bytes};
    memcpy(w->cur, &h, 8);
    memcpy(w->cur + 8, &c, sizeof c);
    w->cur += h.size;
    if (w->last_call) ((nvn_p_call *)(void *)(w->seg + w->last_call + 8))->next = off;
    else w->first_call = off;
    w->last_call = off;
    __atomic_add_fetch(&r->refs, 1, __ATOMIC_ACQ_REL);
    if (r->depth + 1 > w->depth) w->depth = r->depth + 1;
    if (r->flags & (NVN_LREC_OWN_DEF | NVN_LREC_SUB_DEF)) w->flags |= NVN_LREC_SUB_DEF;
    w->flat_extra += r->flat - 16 - h.size;
    COUNT(st_calls, 1);
    return 1;
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
    if (w->key) {
        lhint *h = &tl_hints[hint_at(w->key)];
        /* (the biggest of the last ones, let down by an eighth a recording: a job's command buffer on a worker's stack
           records lists of every size at one address; the last size alone left a third of them growing) */
        const uint64_t was = h->key == w->key ? h->bytes - h->bytes / 8 : 0;
        h->key = w->key, h->bytes = bytes > was ? bytes : was;
    }
    if (check_on) {
        r->hash = rec_hash(r);
        r->shadow = (uint8_t *)malloc((size_t)r->flat);
        if (!r->shadow) fatal("nvn: list check: a copy of %llu bytes", (unsigned long long)r->flat);
        nvn_list_expand(r, r->shadow);
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

/* ---- submits ---- */
typedef struct { nvn_lrec **v; size_t n, cap; } lvec;
static void lvec_push(lvec *v, nvn_lrec *r) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 256;
        v->v = (nvn_lrec **)realloc(v->v, v->cap * sizeof *v->v);
        if (!v->v) fatal("nvn: list submit");
    }
    v->v[v->n++] = r;
}
static _Thread_local lvec tl_touched, tl_clones, tl_reached;

/* a private copy of r for this submit: its callees referenced again, not filed, no handle */
static nvn_lrec *rec_clone(const nvn_lrec *r) {
    nvn_lrec *c = rec_alloc((size_t)r->bytes);
    const uint64_t cap = c->cap;
    const uint32_t cls = c->cls;
    memcpy(c, r, sizeof *c);
    memcpy(c + 1, r + 1, (size_t)r->bytes);
    c->cap = cap, c->cls = cls;
    c->refs = 1;   /* (this submit's: let go after it, through the dead list) */
    c->id = 0;
    c->ctl = ~0ull;
    c->next = NULL;
    c->flags = (r->flags & ~NVN_LREC_RELEASED) | NVN_LREC_PRIVATE;
    c->def_busy = 0, c->def_after = 0, c->reached = 0, c->visit = 0;
    c->shadow = NULL;
    c->hash = 0;   /* (its call targets change: no check of its bytes) */
    for (uint32_t off = c->first_call; off; off = call_at(c, off)->next)
        __atomic_add_fetch(&hdr_of(call_at(c, off)->target)->refs, 1, __ATOMIC_ACQ_REL);
    lvec_push(&tl_clones, c);
    COUNT(st_clones, 1);
    return c;
}
/* the recording this submit walks for r: r itself, its deferred binds read in place, or (r or one of its callees read
   by a submit the replay is not past, with other values now) a private copy */
static nvn_lrec *resolve(nvn_lrec *r, uint64_t serial, uint64_t done, void *(*guest)(uint64_t a, uint64_t n)) {
    if (!(r->flags & (NVN_LREC_OWN_DEF | NVN_LREC_SUB_DEF))) return r;
    /* (a callee many lists call: once a submit. Only "r itself" is kept, so another thread's submit overwriting the
       stamp makes this one only do it again) */
    if (__atomic_load_n(&r->visit, __ATOMIC_ACQUIRE) == serial) return r;
    nvn_lrec *u = r;
    if (r->flags & NVN_LREC_OWN_DEF) {
        const int busy = __atomic_add_fetch(&r->def_busy, 1, __ATOMIC_ACQ_REL) > 1;
        lvec_push(&tl_touched, r);
        const int pending = busy || __atomic_load_n(&r->def_after, __ATOMIC_ACQUIRE) > done;
        if (!pending) {
            def_rw(r, 1, guest);
            COUNT(st_in_place, 1);
        } else if (!def_rw(r, 0, guest)) {
            u = rec_clone(r);
            def_rw(u, 1, guest);
            COUNT(st_conflicts, 1);
        }
    }
    if (r->flags & NVN_LREC_SUB_DEF) {
        for (uint32_t off = r->first_call; off; off = call_at(r, off)->next) {
            nvn_lrec *t = hdr_of(call_at(r, off)->target);
            nvn_lrec *v = resolve(t, serial, done, guest);
            if (v == t) continue;
            if (u == r) u = rec_clone(r);
            nvn_p_call *c = call_at(u, off);   /* (the same place in the copy) */
            __atomic_add_fetch(&v->refs, 1, __ATOMIC_ACQ_REL);
            c->target = nvn_lrec_begin(v);
            rec_unref(t, UINT64_MAX);   /* (the copy's reference on t: r holds one too, so t stays) */
        }
    }
    if (u == r) __atomic_store_n(&r->visit, serial, __ATOMIC_RELEASE);
    return u;
}
/* (check) every recording this submit walks, for the free's check */
static void note_reached(nvn_lrec *r) {
    lvec_push(&tl_reached, r);
    for (uint32_t off = r->first_call; off; off = call_at(r, off)->next) note_reached(hdr_of(call_at(r, off)->target));
}
/* (check) the walk the replay makes from u against the default path's copy, op by op. Returns 0 on a mismatch */
static int cmp_walk(const uint8_t *p, int top, const uint8_t **s, const uint8_t *s_end, uint64_t *ops) {
    for (;;) {
        const nvn_op_hdr h = op_at(p);
        if (h.op == NOP_CALL) {
            nvn_p_call c;
            memcpy(&c, p + 8, sizeof c);
            if (c.target && !cmp_walk(c.target, 0, s, s_end, ops)) return 0;
        } else if ((h.op != NOP_BEGIN && h.op != NOP_END) || top) {
            if (*s + h.size > s_end || memcmp(p, *s, h.size)) {
                const uint64_t k = COUNT(st_check_mismatch, 1);
                if (k <= 20) {
                    const nvn_op_hdr sh = *s + 8 <= s_end ? op_at(*s) : (nvn_op_hdr){~0u, 0};
                    say("nvn: list check: op %llu differs: the reference walk has op %u (%u bytes), the copy op %u (%u bytes)",
                        (unsigned long long)*ops, h.op, h.size, sh.op, sh.size);
                }
                return 0;
            }
            *s += h.size;
            (*ops)++;
        }
        if (h.op == NOP_END) return 1;
        if (!h.size) return 0;
        p += h.size;
    }
}
static uint64_t submit_serial;
int nvn_list_submit(const uint64_t *in, uint64_t *out, int n, uint64_t done, void *(*guest)(uint64_t a, uint64_t n)) {
    tl_touched.n = tl_clones.n = tl_reached.n = 0;
    const uint64_t serial = __atomic_add_fetch(&submit_serial, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++) {
        nvn_lrec *r = nvn_list_lookup(in[i]);
        if (!r) {
            nvn_list_submitted(0);   /* (what was touched let go: the caller dies) */
            return i + 1;
        }
        nvn_lrec *u = resolve(r, serial, done, guest);
        out[i] = (uint64_t)(uintptr_t)nvn_lrec_begin(u);
        if (check_on && r->shadow) {
            resolve_flat(r->shadow, guest);
            const uint8_t *s = r->shadow;
            uint64_t ops = 0;
            if (cmp_walk(nvn_lrec_begin(u), 1, &s, r->shadow + r->flat, &ops) && s != r->shadow + r->flat) {
                const uint64_t k = COUNT(st_check_mismatch, 1);
                if (k <= 20) say("nvn: list check: the copy has %lld bytes more than the reference walk", (long long)(r->shadow + r->flat - s));
            }
            COUNT(st_check_handles, 1);
            COUNT(st_check_bytes, r->flat);
            note_reached(u);
        }
    }
    return 0;
}
void nvn_list_submitted(uint64_t after) {
    for (size_t i = 0; i < tl_touched.n; i++) {
        nvn_lrec *r = tl_touched.v[i];
        uint64_t was = __atomic_load_n(&r->def_after, __ATOMIC_RELAXED);
        while (was < after && !__atomic_compare_exchange_n(&r->def_after, &was, after, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
        __atomic_sub_fetch(&r->def_busy, 1, __ATOMIC_ACQ_REL);
    }
    for (size_t i = 0; i < tl_reached.n; i++) {
        nvn_lrec *r = tl_reached.v[i];
        if (r->reached < after) r->reached = after;
    }
    dead_push(tl_clones.v, tl_clones.n, after);
    tl_touched.n = tl_clones.n = tl_reached.n = 0;
}
