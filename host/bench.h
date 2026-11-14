/* The pure parts of HOS_BENCH (host/bench.c): the ready condition held for a number of frames in a row, and the summary of
   the frames' wall times. No host calls, so tests/test_bench.py compiles it alone. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* What is settled in a frame, one bit a part; ready when all are */
enum { BENCH_PLAY = 1, BENCH_TEXTURES = 2, BENCH_SHADERS = 4, BENCH_STREAMING = 8, BENCH_RESOURCES = 16, BENCH_PARTS = 5, BENCH_ALL = 31 };

typedef struct { int run; } BenchReady;
/* A frame's settled parts: 1 once every part has been settled `need` frames in a row */
static inline int bench_ready_frame(BenchReady *r, unsigned settled, int need) {
    r->run = settled == BENCH_ALL ? r->run + 1 : 0;
    return r->run >= need;
}

typedef struct { double total, mean, median, p99, max; } BenchSummary;
static int bench_cmp(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
/* Sum, mean and percentiles of n values (the percentile as the back end's frame-time line takes it: the value at p * n in
   sorted order); scratch holds n values */
static inline BenchSummary bench_summary(const double *v, int n, double *scratch) {
    BenchSummary s = {0, 0, 0, 0, 0};
    if (n <= 0) return s;
    memcpy(scratch, v, (size_t)n * sizeof *v);
    qsort(scratch, (size_t)n, sizeof *scratch, bench_cmp);
    for (int i = 0; i < n; i++) s.total += scratch[i];
    const int p99 = (int)(0.99 * n);
    s.mean = s.total / n;
    s.median = scratch[n / 2];
    s.p99 = scratch[p99 < n ? p99 : n - 1];
    s.max = scratch[n - 1];
    return s;
}
