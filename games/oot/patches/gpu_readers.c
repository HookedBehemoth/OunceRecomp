/* Game patch: the CPU's reads of two GPU results, as host code over the game's objects (docs/direct/pool-claims.md,
   "GPU results the CPU reads"): the exposure object's three light sums (exposure_light_readback) and the lights'
   visibility counters (render_light_visibility_readback). Each sum is cleared on the GPU, in list order, right before
   the pass that adds to it: an nvnCommandBufferClearBuffer on the pass's command buffer. The readers are replaced whole
   and read what the copy out of the last completed submission left in guest memory; they never write, invalidate or
   flush that memory (the game's own readers zeroed each sum after reading it). */
#include <stddef.h>
#include <string.h>
#include "hos.h"
#include "patches.h"

/* the game's objects in guest memory; a uint64_t field holds a guest address */
typedef struct { uint32_t count, offset, size; } Member;
typedef struct { uint64_t members; } Decl;
typedef struct {              /* agl::UniformBlock, 0x88 bytes */
    uint64_t vtable, _08;
    uint64_t decl;            /* Decl */
    uint64_t cpu;             /* the CPU address of its current copy */
    uint64_t offset;          /* in its memory */
    uint64_t _28, pool;
    uint64_t mem;             /* the GPUMemBlockBase: [+0x10] its owner, whose [+0x100] is the CPU base */
    uint32_t size, _44;
    uint16_t elements;
    uint8_t copy;             /* the current copy in the high nibble */
    uint8_t _4b[0x88 - 0x4b];
} UniformBlock;
typedef struct { UniformBlock u; uint32_t align, _8c; } StorageBlock;   /* agl::ShaderStorageBlock */
typedef struct {              /* a moving average: a ring of the last cap values */
    uint32_t cap, _04;
    uint64_t ring;            /* float[cap] */
    float sum, avg;
    uint8_t at, n;
    uint8_t _1a[6];
} Average;
typedef struct {              /* exposure_light_buffers_create's object */
    uint8_t _000[0x30];
    StorageBlock sum_a;       /* the light sums the passes add to: a float */
    StorageBlock record;      /* the exposure record (0x90 bytes) */
    UniformBlock light;       /* the 0x40-byte light block */
    StorageBlock sum_b;       /* four floats, the fourth read */
    StorageBlock sum_c;       /* a float */
    uint8_t _2f8[0x308 - 0x2f8];
    float b_lo, b_hi, a_scale;
    uint32_t _314;
    Average avg[3];           /* A, B, C */
    uint32_t _378;
    float light_w, a_fixed, c_fixed, b_fixed;
    uint32_t _38c;
    uint64_t params;          /* [+0x150]: the parameter object whose getters give the averages' lengths */
    uint32_t mode;            /* 0: read sum A; 1: A is 1; else 0 */
    uint32_t _39c;
    uint8_t fixed;            /* bits 3, 5, 4: A, B, C take their fixed value once */
    uint8_t _3a1[3];
    uint32_t state;           /* bit 0: the first frame (set from outside), bit 1: the frame after it */
    float tint[4];
    uint8_t _3b8, skip;       /* 1: skip one frame */
} Exposure;
typedef struct {              /* a light of render_light_visibility_buffer (0x870 bytes) */
    uint8_t _000[0x5f8];
    uint32_t flags;           /* bit 14: it has a visibility counter */
    uint8_t _5fc[0x720 - 0x5fc];
    StorageBlock counter;     /* a float the visibility program adds to */
    uint8_t _7b0[0x7ec - 0x7b0];
    float visibility;         /* the result: counter / (12 x samples), clamped to [0, 1] */
    uint32_t samples;
} Light;
typedef struct {              /* the lights' list (render_light_visibility_readback's x0) */
    uint8_t _000[0xa68];
    uint8_t mutex[0x40];
    int32_t count, _aac;
    uint64_t entries;         /* count pointers, each to an entry whose +0x10 is a Light */
    uint8_t _ab8[0xc0c - 0xab8];
    uint8_t off;              /* bit 1: no readback */
} LightList;
_Static_assert(offsetof(UniformBlock, decl) == 0x10 && offsetof(UniformBlock, cpu) == 0x18 &&
               offsetof(UniformBlock, mem) == 0x38 && offsetof(UniformBlock, size) == 0x40 &&
               offsetof(UniformBlock, elements) == 0x48 && offsetof(UniformBlock, copy) == 0x4a && sizeof(UniformBlock) == 0x88,
               "UniformBlock");
_Static_assert(offsetof(StorageBlock, align) == 0x88 && sizeof(StorageBlock) == 0x90, "StorageBlock");
_Static_assert(offsetof(Average, ring) == 8 && offsetof(Average, sum) == 0x10 && offsetof(Average, at) == 0x18 &&
               sizeof(Average) == 0x20, "Average");
_Static_assert(offsetof(Exposure, sum_a) == 0x30 && offsetof(Exposure, record) == 0xc0 && offsetof(Exposure, light) == 0x150 &&
               offsetof(Exposure, sum_b) == 0x1d8 && offsetof(Exposure, sum_c) == 0x268 && offsetof(Exposure, b_lo) == 0x308 &&
               offsetof(Exposure, avg) == 0x318 && offsetof(Exposure, light_w) == 0x37c && offsetof(Exposure, params) == 0x390 &&
               offsetof(Exposure, mode) == 0x398 && offsetof(Exposure, fixed) == 0x3a0 && offsetof(Exposure, state) == 0x3a4 &&
               offsetof(Exposure, tint) == 0x3a8 && offsetof(Exposure, skip) == 0x3b9, "Exposure");
_Static_assert(offsetof(Light, flags) == 0x5f8 && offsetof(Light, counter) == 0x720 && offsetof(Light, visibility) == 0x7ec &&
               offsetof(Light, samples) == 0x7f0, "Light");
_Static_assert(offsetof(LightList, mutex) == 0xa68 && offsetof(LightList, count) == 0xaa8 && offsetof(LightList, entries) == 0xab0 &&
               offsetof(LightList, off) == 0xc0c, "LightList");

/* the blocks' virtual methods, as the game compares them (its GOT's words): +0x28 the alignment (ShaderStorageBlock's,
   UniformBlock's: 0x100), +0x40 the invalidate (empty) */
#define SSB_ALIGN G64(0x84cb4fa8)
#define UBO_ALIGN G64(0x84cb56e0)
#define NO_INVALIDATE G64(0x84cb4fb0)

static void *at(uint64_t a, uint64_t size) {
    if (!hos_guest_range(a, size)) hos_fatal_lr(0, "gpu readers: 0x%llx outside guest memory", (unsigned long long)a);
    return GP(a);
}
static float getf(uint64_t a) { float f; memcpy(&f, at(a, 4), 4); return f; }
static uint32_t align_of(const UniformBlock *b) {
    const uint64_t *vt = at(b->vtable, 0x30);
    if (vt[5] == UBO_ALIGN) return 0x100;
    if (vt[5] == SSB_ALIGN) return ((const StorageBlock *)b)->align;
    hos_fatal_lr(0, "gpu readers: a block's alignment method is 0x%llx", (unsigned long long)vt[5]);
}
/* the block's first member in its current copy */
static uint64_t first(const UniformBlock *b) {
    const Decl *d = at(b->decl, sizeof(Decl));
    return b->cpu + ((const Member *)at(d->members, sizeof(Member)))->offset;
}
/* Each sum's passes recorded since its reader last read it (host state, by the sum's address): a reader that finds none
   reads 0, as it read the game's own zero when no pass added to the sum (a light no pass draws keeps no old count) */
enum { SUMS = 128 };
static struct { uint64_t addr; uint32_t passes, seen; } sums[SUMS];
static int sum_slot(uint64_t addr, int add) {
    for (int i = 0; i < SUMS; i++) {
        uint64_t a = __atomic_load_n(&sums[i].addr, __ATOMIC_ACQUIRE);
        if (a == addr) return i;
        if (!a) {
            if (!add) return -1;
            if (__atomic_compare_exchange_n(&sums[i].addr, &a, addr, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) || a == addr) return i;
        }
    }
    hos_fatal_lr(0, "gpu readers: more than %d sums", SUMS);
}
/* what the passes added in the last completed submission (the copy out's value), or 0 if no pass came since the last read */
static float sum_read(const UniformBlock *b, uint64_t plus) {
    const uint64_t *vt = at(b->vtable, 0x48);
    if (vt[5] != SSB_ALIGN || vt[8] != NO_INVALIDATE)
        hos_fatal_lr(0, "gpu readers: a sum's block methods are 0x%llx 0x%llx, not the storage block's", (unsigned long long)vt[5],
                     (unsigned long long)vt[8]);
    const uint64_t addr = first(b);
    const int s = sum_slot(addr, 0);
    if (s < 0) return 0;
    const uint32_t passes = __atomic_load_n(&sums[s].passes, __ATOMIC_ACQUIRE);
    if (passes == sums[s].seen) return 0;
    sums[s].seen = passes;
    return getf(addr + plus);
}

/* exposure_light_readback's moving averages: v into the ring; the getter (of the parameter object) gives the length */
static void average(Average *a, float v, uint64_t len_at) {
    const int32_t len = *(int32_t *)at(len_at, 4);
    const uint8_t n = (uint8_t)(a->n + 1);
    a->n = n;
    float *ring = at(a->ring, 4ull * (a->cap ? a->cap : 1));
    if (len < n) {
        a->n = (uint8_t)len;
        a->sum -= ring[a->cap > a->at ? a->at : 0];
    }
    ring[a->cap > a->at ? a->at : 0] = v;
    const int32_t next = a->at + 1;
    a->sum += v;
    a->avg = a->sum / (float)a->n;
    a->at = (uint8_t)(len ? next - next / len * len : next);
}

/* The exposure record's tint (+0x10, twice), as the reader last computed it. The reader runs on a job thread
   (ModuleSystemWorker or sead::MainThread), exposure_record_reset on the thread recording exposure_record_pass; nothing
   orders the two within a frame, so the tint is a sequence lock (one writer): the reset takes the last whole tint */
static struct { uint32_t seq, bits[4]; } tint;
static void tint_put(const float v[4]) {
    const uint32_t s = tint.seq;
    __atomic_store_n(&tint.seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (int i = 0; i < 4; i++) {
        uint32_t b;
        memcpy(&b, &v[i], 4);
        __atomic_store_n(&tint.bits[i], b, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&tint.seq, s + 2, __ATOMIC_RELEASE);
}
/* 0 before the reader's first tint */
static int tint_get(uint32_t bits[4]) {
    for (;;) {
        const uint32_t s = __atomic_load_n(&tint.seq, __ATOMIC_ACQUIRE);
        if (!s) return 0;
        if (s & 1) continue;
        for (int i = 0; i < 4; i++) bits[i] = __atomic_load_n(&tint.bits[i], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&tint.seq, __ATOMIC_RELAXED) == s) return 1;
    }
}

/* exposure_light_readback (x0 the exposure object, w1 bit 0: read), once a frame: the three sums into their averages, the
   exposure record and the light block */
PATCH_FUNC(0x80357060, word(0xd503237f), body(0x913d2f9eccdb34d6), aapcs, none, exposure_light_readback,
           guest(0x831cd830, 0x831cdb60, 0x831cdcb0), sp, as(x0, obj), as(w1, flags)) {
    if (!(flags & 1)) return;
    Exposure *o = at(obj, sizeof(Exposure));
    const uint32_t st = __atomic_load_n(&o->state, __ATOMIC_RELAXED);
    if (st & 1) __atomic_fetch_and(&o->state, ~1u, __ATOMIC_SEQ_CST), __atomic_fetch_or(&o->state, 2u, __ATOMIC_SEQ_CST);
    if (st & 2) __atomic_fetch_and(&o->state, ~2u, __ATOMIC_SEQ_CST);
    if (o->skip == 1) { o->skip = 0; return; }
    const uint64_t params = G64(o->params + 0x150);

    /* A: the scene's light, 256 the neutral sum */
    float a;
    if (o->mode) a = o->mode == 1 ? 1.0f : 0.0f;
    else if (o->fixed & 8) a = o->a_fixed;
    else {
        const float v = sum_read(&o->sum_a.u, 0);
        a = __builtin_fmaf(o->a_scale * (1.0f / 256), v + -256.0f, 1.0f);
    }
    if (!(st & 1) || (o->fixed & 8)) {
        average(&o->avg[0], a, guest_call(g, sp, 0x831cd830, params, 0, 0, 0, 0));
        o->fixed &= ~8;
    }

    /* B: its fourth float between b_lo and b_hi, as 0..1 */
    float b = 0;
    int skip_b = 0;
    if (o->fixed & 0x20) b = o->b_fixed;
    else {
        if (o->b_hi > o->b_lo) {
            float v = sum_read(&o->sum_b.u, 12) * (1.0f / 256);
            if (v != v) v = o->b_lo;
            const float t = (v - o->b_lo) / (o->b_hi - o->b_lo);
            b = t < 0 ? 0.0f : t > 1.0f ? 1.0f : t;
        }
        skip_b = (st & 3) != 0;
    }
    if (!skip_b) {
        average(&o->avg[1], 1.0f - b, guest_call(g, sp, 0x831cdb60, params, 0, 0, 0, 0));
        o->fixed &= ~0x20;
    }

    /* the exposure record's tint: host state, written into the record by exposure_record_reset */
    {
        const float k = getf(G64(G64(G64(0x84c9e740)) + 0x1e8) + 0xdb0);
        float v[4];
        for (int i = 0; i < 4; i++) v[i] = o->tint[i] * k;
        tint_put(v);
    }

    /* C: a sum of 256ths */
    float c;
    if (o->fixed & 0x10) c = o->c_fixed, o->fixed &= ~0x10;
    else {
        const float v = sum_read(&o->sum_c.u, 0);
        c = v * (1.0f / 256);
    }
    average(&o->avg[2], c, guest_call(g, sp, 0x831cdcb0, params, 0, 0, 0, 0));

    /* the light block: the sun's direction (normalized, negated) and the environment's words, into its other copy */
    const uint64_t gl = G64(G64(G64(0x84c9e740)) + 0x1e8), env = G64(gl + 0xc98);
    float dx = getf(env + 0x6c0), dy = getf(env + 0x6c4), dz = getf(env + 0x6c8);
    float len = __builtin_sqrtf(__builtin_fmaf(dz, dz, __builtin_fmaf(dy, dy, dx * dx)));
    float sx = 0, sy = 0, sz = 0;
    if (!(len <= getf(0x83e604ac))) {
        if (len == __builtin_inff()) {
            const float k = getf(0x83e60efc);
            dx *= k, dy *= k, dz *= k;
            len = __builtin_sqrtf(__builtin_fmaf(dz, dz, __builtin_fmaf(dy, dy, dx * dx)));
        } else dx = -dx, dy = -dy, dz = -dz;
        const float inv = 1.0f / len;
        sx = inv * dx, sy = inv * dy, sz = inv * dz;
    }
    const float w = getf(gl + 0xdac) * getf(env + 0x9cc);
    float e960 = getf(env + 0x960), e980 = getf(env + 0x980), e9a0 = getf(env + 0x9a0);
    if (e960 < 0) e960 = 0;
    if (e980 < 0.5f) e980 = 0.5f;
    if (e9a0 < 0) e9a0 = 0;
    UniformBlock *lb = &o->light;
    const uint32_t copy = (1u - (lb->copy >> 4)) & 0xf;
    lb->copy = (uint8_t)((lb->copy & 0xf) | copy << 4);
    const uint32_t align = align_of(lb);
    uint64_t base = lb->mem ? G64(lb->mem + 0x10) : 0;
    if (base) base = G64(base + 0x100);
    lb->cpu = base ? base + lb->offset + (uint64_t)((lb->size + align - 1) & -align) * (copy * lb->elements) : 0;
    float *l = at(first(lb), 0x40);
    l[0] = sx, l[1] = sy, l[2] = sz, l[3] = getf(env + 0xaf0);
    l[4] = o->light_w, l[5] = l[6] = l[7] = 0;
    l[8] = w > 0 ? e960 : 0.0f, l[9] = w > 0 ? e980 : 1.0f, l[10] = w > 0 ? e9a0 : 1.0f, l[11] = w;
    l[12] = getf(env + 0x9c0), l[13] = getf(env + 0x9c4), l[14] = getf(env + 0x9c8), l[15] = 1.0f;
}

/* render_light_visibility_readback (x0 the lights' list), once a frame: each light's visibility from its counter */
PATCH_FUNC(0x80354060, word(0x39703008), body(0x716da3baefb584c3), aapcs, none, render_light_visibility_readback,
           guest(0x83c62460, 0x83c62470), sp, as(x0, list)) {
    LightList *ls = at(list, sizeof(LightList));
    if (ls->off & 2) return;
    guest_call(g, sp, 0x83c62460, list + offsetof(LightList, mutex), 0, 0, 0, 0);
    const uint64_t *e = ls->count > 0 ? at(ls->entries, 8ull * (uint32_t)ls->count) : NULL;
    for (int32_t i = 0; i < ls->count; i++) {
        Light *li = at(G64(e[i] + 0x10), sizeof(Light));
        if (!(li->flags & 0x4000)) continue;
        const float v = sum_read(&li->counter.u, 0);
        const float t = v / (float)(li->samples * 12u);
        li->visibility = t < 0 ? 0.0f : t > 1.0f ? 1.0f : t;
    }
    guest_call(g, sp, 0x83c62470, list + offsetof(LightList, mutex), 0, 0, 0, 0);
}

/* The resets, made on the GPU before the passes that add to the sums: pass_scene_pipeline's exposure passes
   (exposure_pass_sum_a, _b, _c) and the lights' visibility pass (light_visibility_pass, once per light with a counter)
   each bind their buffer (ubo_object_bind, x0 the block, x1 the pass context, its command buffer at +0xd8) and then
   make a dispatch whose atomic adds make the frame's sum. Before the bind the words the reader reads are cleared with
   nvnCommandBufferClearBuffer on that command buffer, so the dispatch adds onto zero in GPU order and each frame's result
   is complete on its own */
uint64_t nvn_native_proc(const char *name);   /* nvn_native.c */
static uint64_t p_clear_buffer;
static uint64_t clear_proc(void) {
    uint64_t clear = __atomic_load_n(&p_clear_buffer, __ATOMIC_RELAXED);
    if (!clear) __atomic_store_n(&p_clear_buffer, clear = nvn_native_proc("nvnCommandBufferClearBuffer"), __ATOMIC_RELAXED);
    return clear;
}
PATCH_AT(0x8029410c, word(0x94000ac9), before, none, gpu_sum_reset, guest(computed), sp, as(x0, buf), as(x1, ctx), as(k(4), size)) {
    const uint64_t addr = first(at(buf, sizeof(UniformBlock)));
    guest_call(g, sp, clear_proc(), G64(ctx + 0xd8), addr, size, 0, 0);
    __atomic_fetch_add(&sums[sum_slot(addr, 1)].passes, 1, __ATOMIC_RELEASE);
}
PATCH_AT_SITE(0x80294978, word(0x940008ae), before, none, gpu_sum_reset, guest(computed), sp, x0, x1, k(16));
PATCH_AT_SITE(0x802b6b38, word(0x97ff803e), before, none, gpu_sum_reset, guest(computed), sp, x0, x1, k(4));
PATCH_AT_SITE(0x80658d44, word(0x97f0f7bb), before, none, gpu_sum_reset, guest(computed), sp, x0, x1, k(4));
/* the exposure record, written on the GPU only: its words +0x30..+0x70 by GPU work, first by exposure_record_pass
   (f_80291ab0, from pass_vegetation_compute: it binds the light block and the record, x0 the block, x1 the pass context,
   then one 1x1x1 dispatch), then by env_cube_exposure_pass (f_802952b0); exposure_pass_sum_b reads the tint. Before the
   first, +0..+0x10 and +0x30..+0x90 are cleared and the tint is written at +0x10 and +0x20, a 4-byte clear for each
   float's bits */
PATCH_AT(0x80291b30, word(0x94001440), before, none, exposure_record_reset, guest(computed), sp, as(x0, buf), as(x1, ctx)) {
    const uint64_t rec = first(at(buf, sizeof(UniformBlock))), cmd = G64(ctx + 0xd8);
    guest_call(g, sp, clear_proc(), cmd, rec, 0x10, 0, 0);
    guest_call(g, sp, clear_proc(), cmd, rec + 0x30, 0x60, 0, 0);
    uint32_t bits[4];
    if (!tint_get(bits)) return;
    for (int i = 0; i < 8; i++) guest_call(g, sp, clear_proc(), cmd, rec + 0x10 + 4ull * i, 4, bits[i & 3], 0);
}

