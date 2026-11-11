/* (native-vk engine cut E2) HOS_ENG_LISTS: host packets for the game's load-time command lists. See eng_lists.h and
   gpu/render/nvn_pkt.h for the design and the lifetime rules; host/nvn_native.c calls in. */
#include "eng_lists.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define eng_yield() SwitchToThread()
#else
#include <sched.h>
#define eng_yield() sched_yield()
#endif

static int on_mode;
static FILE *dump_f;
static void (*say)(const char *fmt, ...);
static int64_t (*clock_ns)(void);
static void say_none(const char *fmt, ...) { (void)fmt; }
static int64_t clock_none(void) { return 0; }

static eng_lists_stats st;
#define ADD(f, v) __atomic_fetch_add(&st.f, (uint64_t)(v), __ATOMIC_RELAXED)
#define SUB(f, v) __atomic_fetch_sub(&st.f, (uint64_t)(v), __ATOMIC_RELAXED)

static void spin_lock(volatile int32_t *l) {
    for (int n = 0; __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE);)
        while (__atomic_load_n(l, __ATOMIC_RELAXED))
            if (++n > 64) eng_yield();
}
static void spin_unlock(volatile int32_t *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

void eng_lists_init(int on, const char *dump, void (*log)(const char *fmt, ...), int64_t (*now_ns)(void)) {
    on_mode = on;
    say = log ? log : say_none;
    clock_ns = now_ns ? now_ns : clock_none;
    if (dump && *dump && on) {
        dump_f = fopen(dump, "w");
        if (!dump_f) say("eng: lists: cannot write %s", dump);
    }
}
int eng_lists_on(void) { return on_mode; }

/* ---- classification: where the game began the recording (guest return addresses after BeginRecording, this build of
   the game; work/assets/native/engine-materials.md section 1.1, confirmed by the trace's lrs) ---- */
int eng_lists_kind(uint64_t lr) {
    switch (lr) {
    case 0x804582acull: return NVN_PK_VERTEX;            /* f_804581b0 <- f_80457e60 */
    case 0x80293b14ull: return NVN_PK_DRAW;              /* f_80293890 <- f_804566e0 */
    case 0x806006d8ull: return NVN_PK_SHAPE;             /* f_806002e0 <- f_8009bb80 (0x800a2d9c) */
    case 0x80445314ull: return NVN_PK_MATERIAL;          /* f_80445020 <- f_80443000 */
    case 0x8044ea20ull: return NVN_PK_VERTEX_SCRATCH;    /* f_8044e8e0 <- f_8044e020 */
    case 0x80293460ull: return NVN_PK_DRAW_SCRATCH;      /* f_80293190 <- f_8044cb30 */
    default: return NVN_PK_NONE;
    }
}
void eng_lists_begun(int kind) { if (kind > 0 && kind < NVN_PK_KINDS) ADD(begun[kind], 1); }

/* ---- packet memory: size classes, never given back (a stale NOP_PKT reads a packet, never freed memory) ---- */
#define PCLS_MIN 7    /* 128 bytes */
#define PCLS_MAX 17   /* 128 KiB */
static nvn_pkt *pfree[PCLS_MAX + 1];
static volatile int32_t pfree_lock;
static uint32_t serial_next = 1;
static nvn_pkt *pkt_get(size_t n, unsigned *cls) {
    unsigned c = PCLS_MIN;
    while (c <= PCLS_MAX && ((size_t)1 << c) < n) c++;
    if (c > PCLS_MAX) return NULL;
    *cls = c;
    spin_lock(&pfree_lock);
    nvn_pkt *k = pfree[c];
    if (k) pfree[c] = k->next;
    spin_unlock(&pfree_lock);
    if (!k) {
        k = (nvn_pkt *)calloc(1, (size_t)1 << c);
        if (!k) return NULL;
        ADD(pool_bytes, (uint64_t)1 << c);
    }
    return k;
}

/* ---- the decode: the state a recording leaves bound, as the Vulkan replay's ops set it ---- */
#define MAX_BUF 64
#define MAX_HND 256
#define MAX_REF 8
typedef struct {
    nvn_pk_buf buf[MAX_BUF];
    nvn_pk_handle hnd[MAX_HND];
    nvn_pk_ref ref[MAX_REF];
    nvn_pk_vertex vtx;
    nvn_pk_draw draw;
    int nbuf, nhnd, nref, has_vtx, nops, why;
    uint8_t flags;
} bld;

static int buf_set(bld *b, uint32_t ssbo, const nvn_p_bind_buf *v) {
    for (int i = 0; i < b->nbuf; i++)
        if (b->buf[i].ssbo == ssbo && b->buf[i].b.stage == v->stage && b->buf[i].b.index == v->index) {
            b->buf[i].b = *v;
            return 1;
        }
    if (b->nbuf == MAX_BUF) return b->why = ENG_WHY_FULL, 0;
    nvn_pk_buf *e = &b->buf[b->nbuf++];
    memset(e, 0, sizeof *e);
    e->b = *v, e->ssbo = ssbo;
    return 1;
}
static int hnd_set(bld *b, int what, int stage, int slot, uint64_t h) {
    for (int i = 0; i < b->nhnd; i++)
        if (b->hnd[i].what == what && b->hnd[i].stage == stage && b->hnd[i].slot == slot) {
            b->hnd[i].h = h;
            return 1;
        }
    if (b->nhnd == MAX_HND) return b->why = ENG_WHY_FULL, 0;
    nvn_pk_handle *e = &b->hnd[b->nhnd++];
    memset(e, 0, sizeof *e);
    e->what = (uint8_t)what, e->stage = (uint8_t)stage, e->slot = (uint16_t)slot, e->h = h;
    return 1;
}
static void vtx_attrs(bld *b, int n, const uint32_t *st) {
    b->has_vtx = 1;
    b->flags |= NVN_PKF_VATTR;
    memset(b->vtx.vattr, 0, sizeof b->vtx.vattr);
    if (n > 16) n = 16;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) b->vtx.vattr[i] = st[i];
    b->vtx.nvattr = (uint32_t)n;
}
/* a called list's packet into this one, at the place of the call (its state over what is bound so far) */
static int merge(bld *b, const nvn_pkt *k) {
    for (int i = 0; i < k->nbuf; i++)
        if (!buf_set(b, k->buf[i].ssbo, &k->buf[i].b)) return 0;
    for (int i = 0; i < k->nhandle; i++)
        if (!hnd_set(b, k->hnd[i].what, k->hnd[i].stage, k->hnd[i].slot, k->hnd[i].h)) return 0;
    b->flags |= k->flags & NVN_PKF_TEX;
    if (k->vtx) {
        b->has_vtx = 1;
        if (k->flags & NVN_PKF_VATTR) vtx_attrs(b, (int)k->vtx->nvattr, k->vtx->vattr);
        for (int i = 0; i < 16; i++) {
            if (k->vtx->vstream_mask >> i & 1) b->vtx.vstream[i] = k->vtx->vstream[i], b->vtx.vstream_mask |= (uint16_t)(1u << i);
            if (k->vtx->vbuf_mask >> i & 1) b->vtx.vbuf[i] = k->vtx->vbuf[i], b->vtx.vbuf_mask |= (uint16_t)(1u << i);
        }
    }
    if (k->draw.op) {
        if (b->draw.op) return b->why = ENG_WHY_TWO_DRAWS, 0;
        b->draw = k->draw;
    }
    b->nops += k->nops;
    if (b->nref < MAX_REF) {
        nvn_pk_ref *r = &b->ref[b->nref++];
        r->pkt = k, r->handle = k->handle, r->serial = k->serial, r->kind = k->kind;
    }
    return 1;
}
#define PAYLOAD(T, a, avail, v) do { memset(&(v), 0, sizeof(v)); memcpy(&(v), (a), (avail) < sizeof(v) ? (avail) : sizeof(v)); } while (0)
static int decode(bld *b, const uint8_t *p, const uint8_t *end, int depth) {
    if (depth > 8) return b->why = ENG_WHY_DEEP, 0;
    for (;;) {
        if (end && p + 8 > end) return b->why = ENG_WHY_CORRUPT, 0;
        nvn_op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= NOP_COUNT_ || (end && p + h.size > end)) return b->why = ENG_WHY_CORRUPT, 0;
        const uint8_t *a = p + 8;
        const size_t avail = h.size - 8;
        if (h.op == NOP_END) return 1;
        if (h.op != NOP_BEGIN && h.op != NOP_JUMP && b->draw.op) return b->why = ENG_WHY_AFTER_DRAW, 0;
        if (h.op != NOP_BEGIN && h.op != NOP_JUMP && h.op != NOP_CALL && h.op != NOP_PKT) b->nops++;
        switch (h.op) {
        case NOP_BEGIN: case NOP_JUMP: break;
        case NOP_CALL: {   /* (HOS_NVN_LIST_REF: the callee's records, which live as long as this recording) */
            nvn_p_call c;
            PAYLOAD(nvn_p_call, a, avail, c);
            if (!c.target) break;
            if (!decode(b, c.target, NULL, depth + 1)) return 0;
            break;
        }
        case NOP_PKT: {   /* (a called list with a packet: folded in, its records after this one skipped) */
            nvn_p_pkt r;
            PAYLOAD(nvn_p_pkt, a, avail, r);
            const nvn_pkt *k = nvn_pkt_live(&r);
            if (!k) break;   /* (gone: its records follow) */
            if (!merge(b, k)) return 0;
            p += h.size + r.skip;
            continue;
        }
        case NOP_BIND_UBO: case NOP_BIND_SSBO: {
            if (avail > sizeof(nvn_p_bind_buf)) return b->why = ENG_WHY_CENSUS, 0;   /* (RX_VK_UBO_CENSUS's payload) */
            nvn_p_bind_buf v;
            PAYLOAD(nvn_p_bind_buf, a, avail, v);
            if (v.stage >= 0 && v.stage < 8 && v.index >= 0 && v.index < 16 && !buf_set(b, h.op == NOP_BIND_SSBO, &v)) return 0;
            break;
        }
        case NOP_BIND_BUFS: {
            nvn_p_bind_bufs v;
            PAYLOAD(nvn_p_bind_bufs, a, avail, v);
            if (v.src) return b->why = ENG_WHY_DEFERRED, 0;
            for (int i = 0; i < v.n && i < 16; i++) {
                const int index = v.first + i;
                if (v.stage < 0 || v.stage >= 8 || index < 0 || index >= 16) continue;
                nvn_p_bind_buf x = {v.stage, index, v.r[i].address, v.r[i].size};
                if (!buf_set(b, v.ssbo != 0, &x)) return 0;
            }
            break;
        }
        case NOP_BIND_TEX: case NOP_BIND_SAMP: case NOP_BIND_IMG: {
            nvn_p_bind_handles v;
            PAYLOAD(nvn_p_bind_handles, a, avail, v);
            if (v.src) return b->why = ENG_WHY_DEFERRED, 0;
            b->flags |= NVN_PKF_TEX;
            if (v.stage < 0 || v.stage >= 8 || v.first < 0) break;
            const int what = h.op == NOP_BIND_TEX ? 0 : h.op == NOP_BIND_SAMP ? 1 : 2, lim = what == 2 ? 32 : 128;
            for (int k = 0; k < v.n && k < 32 && v.first + k < lim; k++)
                if (!hnd_set(b, what, v.stage, v.first + k, v.h[k])) return 0;
            break;
        }
        case NOP_BIND_VATTR: {
            nvn_p_bind_vattr v;
            PAYLOAD(nvn_p_bind_vattr, a, avail, v);
            vtx_attrs(b, v.n, v.st);
            break;
        }
        case NOP_BIND_VSTREAM: {   /* (as the replay: by i, not first + i) */
            nvn_p_bind_vstream v;
            PAYLOAD(nvn_p_bind_vstream, a, avail, v);
            b->has_vtx = 1;
            for (int i = 0; i < v.n && i < 16; i++) b->vtx.vstream[i] = v.st[i], b->vtx.vstream_mask |= (uint16_t)(1u << i);
            break;
        }
        case NOP_BIND_VBUF: {
            nvn_p_bind_vbuf v;
            PAYLOAD(nvn_p_bind_vbuf, a, avail, v);
            b->has_vtx = 1;
            for (int i = 0; i < v.n && v.first + i < 16; i++)
                if (v.first + i >= 0) b->vtx.vbuf[v.first + i] = v.r[i], b->vtx.vbuf_mask |= (uint16_t)(1u << (v.first + i));
            break;
        }
        case NOP_ENG: break;   /* (E4/E6's marks: no GPU work, no state) */
        case NOP_DRAW: b->draw.op = NOP_DRAW; PAYLOAD(nvn_p_draw, a, avail, b->draw.u.d); break;
        case NOP_DRAW_IDX: b->draw.op = NOP_DRAW_IDX; PAYLOAD(nvn_p_draw_idx, a, avail, b->draw.u.di); break;
        case NOP_DRAW_MESH: b->draw.op = NOP_DRAW_MESH; PAYLOAD(nvn_p_draw_mesh, a, avail, b->draw.u.dm); break;
        default: return b->why = (int)h.op, 0;
        }
        p += h.size;
    }
}

static void dump_line(const nvn_pkt *k) {
    static volatile int32_t lock;
    char line[8192];
    int n = snprintf(line, sizeof line, "P %d %llx raw %u ops %u meta %llx %llx %llx %llx |", k->kind, (unsigned long long)k->handle,
                     k->raw_bytes, k->nops, (unsigned long long)k->meta[0], (unsigned long long)k->meta[1],
                     (unsigned long long)k->meta[2], (unsigned long long)k->meta[3]);
#define PUT(...) do { if (n < (int)sizeof line - 200) n += snprintf(line + n, sizeof line - n, __VA_ARGS__); } while (0)
    for (int i = 0; i < k->nbuf; i++)
        PUT(" %s%d.%d=%llx+%llx", k->buf[i].ssbo ? "s" : "u", k->buf[i].b.stage, k->buf[i].b.index,
            (unsigned long long)k->buf[i].b.addr, (unsigned long long)k->buf[i].b.size);
    PUT(" |");
    for (int i = 0; i < k->nhandle; i++) PUT(" %c%d.%d=%llx", "tsi"[k->hnd[i].what], k->hnd[i].stage, k->hnd[i].slot, (unsigned long long)k->hnd[i].h);
    PUT(" |");
    if (k->vtx) {
        if (k->flags & NVN_PKF_VATTR) {
            PUT(" va%u", k->vtx->nvattr);
            for (uint32_t i = 0; i < k->vtx->nvattr; i++) PUT(":%x", k->vtx->vattr[i]);
        }
        for (int i = 0; i < 16; i++)
            if (k->vtx->vstream_mask >> i & 1) PUT(" vs%d=%u/%u", i, k->vtx->vstream[i].stride, k->vtx->vstream[i].divisor);
        for (int i = 0; i < 16; i++)
            if (k->vtx->vbuf_mask >> i & 1) PUT(" vb%d=%llx+%llx", i, (unsigned long long)k->vtx->vbuf[i].address, (unsigned long long)k->vtx->vbuf[i].size);
    }
    PUT(" |");
    if (k->draw.op == NOP_DRAW_IDX) {
        const nvn_p_draw_idx *d = &k->draw.u.di;
        PUT(" di %d %d %d %d %d %d %llx", d->prim, d->itype, d->count, d->base_vertex, d->base_instance, d->instances, (unsigned long long)d->ibuf);
    } else if (k->draw.op == NOP_DRAW) {
        const nvn_p_draw *d = &k->draw.u.d;
        PUT(" d %d %d %d %d %d", d->prim, d->first, d->count, d->base_instance, d->instances);
    } else if (k->draw.op == NOP_DRAW_MESH) {
        const nvn_p_draw_mesh *d = &k->draw.u.dm;
        PUT(" dm %d %d %d %d %llx", d->kind, d->x, d->y, d->z, (unsigned long long)d->indirect);
    }
    PUT(" |");
    for (int i = 0; i < k->nref; i++) PUT(" r%u=%llx", k->ref[i].kind, (unsigned long long)k->ref[i].handle);
#undef PUT
    spin_lock(&lock);
    fprintf(dump_f, "%s\n", line);
    spin_unlock(&lock);
}

nvn_pkt *eng_lists_build(int kind, const uint8_t *rec, size_t bytes, const uint64_t meta[4]) {
    if (!on_mode || kind <= 0 || kind >= NVN_PK_KINDS) return NULL;
    const int64_t t0 = clock_ns();
    static _Thread_local bld *tb;
    if (!tb) tb = (bld *)malloc(sizeof *tb);
    bld *b = tb;
    if (!b) return NULL;
    b->nbuf = b->nhnd = b->nref = b->has_vtx = b->nops = b->why = 0;
    b->flags = 0;
    memset(&b->vtx, 0, sizeof b->vtx);
    memset(&b->draw, 0, sizeof b->draw);
    nvn_pkt *k = NULL;
    if (decode(b, rec, rec + bytes, 0)) {
        const size_t hdr = (sizeof(nvn_pkt) + 7) & ~(size_t)7;
        const size_t o_buf = hdr, o_hnd = o_buf + sizeof(nvn_pk_buf) * (size_t)b->nbuf;
        const size_t o_ref = o_hnd + sizeof(nvn_pk_handle) * (size_t)b->nhnd, o_vtx = o_ref + sizeof(nvn_pk_ref) * (size_t)b->nref;
        const size_t total = o_vtx + (b->has_vtx ? sizeof(nvn_pk_vertex) : 0);
        unsigned cls = 0;
        k = pkt_get(total, &cls);
        if (k) {
            uint8_t *base = (uint8_t *)k;
            memset(k, 0, hdr);
            k->magic = NVN_PKT_MAGIC;
            k->kind = (uint8_t)kind, k->flags = b->flags, k->cls = (uint16_t)cls;
            k->nbuf = (uint16_t)b->nbuf, k->nhandle = (uint16_t)b->nhnd, k->nref = (uint16_t)b->nref, k->nops = (uint16_t)b->nops;
            k->bytes = (uint32_t)total, k->raw_bytes = (uint32_t)bytes;
            if (meta) memcpy(k->meta, meta, sizeof k->meta);
            k->draw = b->draw;
            memcpy(base + o_buf, b->buf, sizeof(nvn_pk_buf) * (size_t)b->nbuf);
            memcpy(base + o_hnd, b->hnd, sizeof(nvn_pk_handle) * (size_t)b->nhnd);
            memcpy(base + o_ref, b->ref, sizeof(nvn_pk_ref) * (size_t)b->nref);
            k->buf = (const nvn_pk_buf *)(base + o_buf);
            k->hnd = (const nvn_pk_handle *)(base + o_hnd);
            k->ref = (const nvn_pk_ref *)(base + o_ref);
            if (b->has_vtx) {
                memcpy(base + o_vtx, &b->vtx, sizeof b->vtx);
                k->vtx = (const nvn_pk_vertex *)(base + o_vtx);
            }
            uint32_t sn = __atomic_fetch_add(&serial_next, 1, __ATOMIC_RELAXED);
            if (!sn) sn = __atomic_fetch_add(&serial_next, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&k->serial, sn, __ATOMIC_RELEASE);
            ADD(built[kind], 1);
            ADD(live, 1);
            ADD(live_bytes, (uint64_t)1 << cls);
            ADD(raw_bytes, bytes);
        } else {
            b->why = ENG_WHY_FULL;
        }
    }
    if (!k) {
        ADD(refused[kind], 1);
        if (b->why >= 0 && b->why < (int)(sizeof st.why / sizeof *st.why)) ADD(why[b->why], 1);
    }
    ADD(build_ns[kind], clock_ns() - t0);
    return k;
}
void eng_lists_set_handle(nvn_pkt *k, uint64_t handle) {
    if (!k) return;
    k->handle = handle;
    if (dump_f) dump_line(k);
}
void eng_lists_released(nvn_pkt *k) {
    if (!k || !__atomic_load_n(&k->serial, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&k->serial, 0, __ATOMIC_RELEASE);
    ADD(released, 1);
}
void eng_lists_freed(nvn_pkt *k) {
    if (!k) return;
    __atomic_store_n(&k->serial, 0, __ATOMIC_RELEASE);
    const unsigned c = k->cls;
    ADD(freed, 1);
    SUB(live, 1);
    SUB(live_bytes, (uint64_t)1 << c);
    SUB(raw_bytes, k->raw_bytes);
    spin_lock(&pfree_lock);
    k->next = pfree[c];
    pfree[c] = k;
    spin_unlock(&pfree_lock);
}
int eng_lists_called(const nvn_pkt *k) {
    if (!k || on_mode != 1 || !__atomic_load_n(&k->serial, __ATOMIC_ACQUIRE)) return 0;
    ADD(calls[k->kind], 1);
    if (k->kind == NVN_PK_SHAPE) ADD(shape_k_calls[k->meta[2] & 3], 1);
    return 1;
}
void eng_lists_call_plain(void) { ADD(calls_plain, 1); }

void eng_lists_get_stats(eng_lists_stats *s) {
    uint64_t *d = (uint64_t *)s;
    const uint64_t *f = (const uint64_t *)&st;
    for (size_t i = 0; i < sizeof st / sizeof(uint64_t); i++) d[i] = __atomic_load_n(&f[i], __ATOMIC_RELAXED);
}

void eng_lists_log(const char *why, double frames) {
    if (!on_mode) return;
    static eng_lists_stats last;
    eng_lists_stats t;
    eng_lists_get_stats(&t);
    if (frames <= 0) frames = 1;
    static const char *kn[NVN_PK_KINDS] = {"-", "vertex", "draw", "shape", "material", "vertex scratch", "draw scratch"};
    char line[2048];
    int n = snprintf(line, sizeof line, "eng: lists (%s): built (refused) [ms] since the last line:", why);
    for (int k = 1; k < NVN_PK_KINDS; k++)
        n += snprintf(line + n, sizeof line - n, " %s %llu (%llu) [%.1f]%s", kn[k], (unsigned long long)(t.built[k] - last.built[k]),
                      (unsigned long long)(t.refused[k] - last.refused[k]), (t.build_ns[k] - last.build_ns[k]) / 1e6,
                      k + 1 < NVN_PK_KINDS ? "," : "");
    say("%s", line);
    n = snprintf(line, sizeof line, "eng: lists: calls a frame by packet:");
    for (int k = 1; k < NVN_PK_KINDS; k++)
        n += snprintf(line + n, sizeof line - n, " %s %.1f,", kn[k], (t.calls[k] - last.calls[k]) / frames);
    n += snprintf(line + n, sizeof line - n, " without %.1f; shapes by variant k %.1f %.1f %.1f %.1f; live %llu packets (%.1f MB, "
                  "pool %.1f MB, from %.1f MB of recordings), %llu released, %llu freed; in all built", (t.calls_plain - last.calls_plain) / frames,
                  (t.shape_k_calls[0] - last.shape_k_calls[0]) / frames, (t.shape_k_calls[1] - last.shape_k_calls[1]) / frames,
                  (t.shape_k_calls[2] - last.shape_k_calls[2]) / frames, (t.shape_k_calls[3] - last.shape_k_calls[3]) / frames,
                  (unsigned long long)t.live, t.live_bytes / 1048576.0, t.pool_bytes / 1048576.0, t.raw_bytes / 1048576.0,
                  (unsigned long long)t.released, (unsigned long long)t.freed);
    for (int k = 1; k < NVN_PK_KINDS; k++)
        n += snprintf(line + n, sizeof line - n, " %llu/%llu", (unsigned long long)t.built[k], (unsigned long long)t.begun[k]);
    say("%s", line);
    int any = 0;
    n = snprintf(line, sizeof line, "eng: lists: refused, in all, by what:");
    for (size_t i = 0; i < sizeof t.why / sizeof *t.why; i++) {
        if (!t.why[i]) continue;
        static const char *wn[] = {"deferred", "after the draw", "two draws", "too many", "census", "too deep", "corrupt"};
        if (i < NOP_COUNT_) n += snprintf(line + n, sizeof line - n, " op %zu: %llu,", i, (unsigned long long)t.why[i]);
        else if (i - NOP_COUNT_ < sizeof wn / sizeof *wn) n += snprintf(line + n, sizeof line - n, " %s: %llu,", wn[i - NOP_COUNT_], (unsigned long long)t.why[i]);
        any = 1;
    }
    if (any) say("%s", line);
    last = t;
}
