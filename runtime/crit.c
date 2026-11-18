/* HOS_DBG=crit: the trace for the frame's critical path (crit.h). Measurement only. */
#include "crit.h"
#include "renderer.h"
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

typedef struct {
    uint64_t t0, t1, a, b;
    uint16_t kind, thread;
    uint32_t pad;
} crit_ev;

volatile int crit_rec;
static crit_ev *evs;
static volatile int64_t nev;
static int64_t cap = 12000000;   /* (480 MB, touched as it fills: about 1500 frames) */
static volatile int64_t lost;
static long long first = 3000, count = 600;
static char names[512][48];
static volatile int32_t nthreads;
static _Thread_local int my_index = -1;
static uint64_t tsc0, tsc1;
static int64_t ns0, ns1;

int crit_wanted(void) {
    static int v = -1;
    if (v < 0) {
        const char *d = getenv("HOS_DBG");
        const char *p = d ? strstr(d, "crit") : NULL;
        v = p != NULL;
        if (v && p[4] == ':') {
            first = atoll(p + 5);
            const char *q = strchr(p + 5, ':');
            if (q && (!strchr(p + 5, ',') || q < strchr(p + 5, ','))) count = atoll(q + 1);
        }
        if (v) {
            evs = (crit_ev *)calloc((size_t)cap, sizeof *evs);
            if (!evs) v = 0;
            hos_log("crit: a trace of the guest threads and the back end's calls over presents %lld-%lld (HOS_DBG=crit)", first, first + count);
        }
    }
    return v;
}

static int thread_index(void) {
    if (my_index >= 0) return my_index;
    const int32_t i = os_atomic_inc(&nthreads) - 1;
    if (i >= 512) return my_index = 511;
    HosThread *t = hos_self();
    snprintf(names[i], sizeof names[i], "%s", t ? hos_thread_name(t) : "(host)");
    return my_index = (int)i;
}

void crit_span(int kind, uint64_t t0, uint64_t t1, uint64_t a, uint64_t b) {
    if (!crit_rec) return;
    const int64_t i = os_atomic_inc(&nev) - 1;
    if (i >= cap) { os_atomic_inc(&lost); return; }
    crit_ev *e = &evs[i];
    e->t0 = t0, e->t1 = t1, e->a = a, e->b = b, e->kind = (uint16_t)kind, e->thread = (uint16_t)thread_index();
}

static void write_out(void) {
    FILE *f = fopen("crit.bin", "wb");
    const int64_t n = nev < cap ? nev : cap;
    if (f) {
        /* header: magic, events, tsc0, ns0, tsc1, ns1 */
        const uint64_t h[6] = {0x31544952435ull, (uint64_t)n, tsc0, (uint64_t)ns0, tsc1, (uint64_t)ns1};
        fwrite(h, sizeof h, 1, f);
        fwrite(evs, sizeof *evs, (size_t)n, f);
        fclose(f);
    }
    FILE *g = fopen("crit_threads.txt", "w");
    if (g) {
        for (int32_t i = 0; i < nthreads && i < 512; i++) fprintf(g, "%ld %s\n", (long)i, names[i]);
        fclose(g);
    }
    hos_log("crit: %lld events written to crit.bin (%lld lost), %ld threads", (long long)n, (long long)lost, (long)nthreads);
}

void crit_present(void) {
    static long long presents;
    const long long p = ++presents;
    if (p == first) {
        tsc0 = crit_tsc(), ns0 = hos_now_ns();
        crit_rec = 1;
    }
    if (crit_rec) {
        const uint64_t t = crit_tsc();
        crit_span(CRIT_PRESENT, t, t, (uint64_t)p, 0);
    }
    if (p == first + count && crit_rec) {
        crit_rec = 0;
        tsc1 = crit_tsc(), ns1 = hos_now_ns();
        os_sleep_ns(20LL * 1000000);   /* (calls in flight finish their writes) */
        write_out();
    }
}

/* ---- the back end's calls, timed ---- */
static gpu_fns real;
static _Thread_local uint64_t burst_t0, burst_t1, burst_sum, burst_n;

void crit_emit_flush(void) {
    if (burst_n) crit_span(CRIT_BE_EMIT, burst_t0, burst_t1, burst_sum, burst_n);
    burst_n = 0;
}
static void w_emit(void *list, uint32_t op, const void *payload, uint32_t size) {
    if (!crit_rec) { burst_n = 0; real.emit(list, op, payload, size); return; }
    const uint64_t t0 = crit_tsc();
    real.emit(list, op, payload, size);
    const uint64_t t1 = crit_tsc();
    /* (a gap over ~20 us, about 80k TSC ticks at 4 GHz, or 512 calls: a new burst) */
    if (burst_n && (t0 - burst_t1 > 80000 || burst_n >= 512)) crit_emit_flush();
    if (!burst_n) burst_t0 = t0, burst_sum = 0;
    burst_t1 = t1, burst_sum += t1 - t0, burst_n++;
}
#define TIMED(kind, call)                                  \
    do {                                                   \
        if (!crit_rec) { call; return; }                   \
        crit_emit_flush();                                 \
        const uint64_t t0_ = crit_tsc();                   \
        call;                                              \
        crit_span(kind, t0_, crit_tsc(), 0, 0);            \
    } while (0)
#define TIMED_R(kind, call)                                \
    do {                                                   \
        if (!crit_rec) return call;                        \
        crit_emit_flush();                                 \
        const uint64_t t0_ = crit_tsc();                   \
        const int r_ = call;                               \
        crit_span(kind, t0_, crit_tsc(), 0, 0);            \
        return r_;                                         \
    } while (0)
static void *w_list_begin(const gpu_list_info *info) {
    if (!crit_rec) return real.list_begin(info);
    crit_emit_flush();
    const uint64_t t0 = crit_tsc();
    void *r = real.list_begin(info);
    crit_span(CRIT_BE_BEGIN, t0, crit_tsc(), info->kind, info->lr);
    return r;
}
static void w_list_call(void *list, void *const *callees, uint32_t n) { TIMED(CRIT_BE_CALL, real.list_call(list, callees, n)); }
static void w_list_end(void *list) { TIMED(CRIT_BE_END, real.list_end(list)); }
static void w_list_released(void *list) { TIMED(CRIT_BE_RELEASED, real.list_released(list)); }
static void w_submit(uint64_t q, void *const *lists, uint32_t n) { TIMED(CRIT_BE_SUBMIT, real.submit(q, lists, n)); }
static int w_asset_load(const gpu_asset_key *k) { TIMED_R(CRIT_BE_OTHER, real.asset_load(k)); }
static void w_texels(uint32_t source, const void *tex, unsigned level, unsigned layer0, const void *region, const void *src,
                     uint64_t row, uint64_t img) {
    TIMED(CRIT_BE_OTHER, real.texels(source, tex, level, layer0, region, src, row, img));
}
void crit_wrap(gpu_fns *f) {
    real = *f;
    f->emit = w_emit, f->list_begin = w_list_begin, f->list_call = w_list_call, f->list_end = w_list_end;
    f->list_released = w_list_released, f->submit = w_submit, f->asset_load = w_asset_load, f->texels = w_texels;
}
