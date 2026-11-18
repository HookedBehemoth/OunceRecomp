#include "bench.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    BenchReady r = {0};
    for (int i = 0; i < 59; i++) CHECK(!bench_ready_frame(&r, BENCH_ALL, 60));
    CHECK(bench_ready_frame(&r, BENCH_ALL, 60));
    BenchReady s = {0};
    for (int i = 0; i < 59; i++) bench_ready_frame(&s, BENCH_ALL, 60);
    CHECK(!bench_ready_frame(&s, BENCH_ALL & ~BENCH_TEXTURES, 60));   /* (one part not settled: the count starts again) */
    for (int i = 0; i < 59; i++) CHECK(!bench_ready_frame(&s, BENCH_ALL, 60));
    CHECK(bench_ready_frame(&s, BENCH_ALL, 60));
    CHECK(!bench_ready_frame(&(BenchReady){0}, BENCH_PLAY | BENCH_SHADERS | BENCH_STREAMING, 1));
    double v[100], scratch[100];
    for (int i = 0; i < 100; i++) v[i] = 100 - i;   /* (100 down to 1) */
    const BenchSummary m = bench_summary(v, 100, scratch);
    CHECK(m.total == 5050 && m.mean == 50.5 && m.median == 51 && m.p99 == 100 && m.max == 100);
    CHECK(v[0] == 100 && v[99] == 1);   /* (the input as it was) */
    double one = 7;
    const BenchSummary o = bench_summary(&one, 1, scratch);
    CHECK(o.median == 7 && o.p99 == 7 && o.mean == 7);
    CHECK(bench_summary(v, 0, scratch).total == 0);
    /* game steps: a 2-then-0 pair is the clock read racing the game thread, not a violation; a fraction, a lasting
       miss or a lasting double step is */
    double st[8] = {1, 1, 2, 0, 1, 1, 1, 1};
    CHECK(bench_steps_off(st, 8) == 0);
    double fr[4] = {1, 1.5, 0.5, 1};
    CHECK(bench_steps_off(fr, 4) == 2);
    double miss[5] = {1, 0, 1, 1, 1};
    CHECK(bench_steps_off(miss, 5) == 3);
    double dbl[4] = {1, 2, 1, 1};
    CHECK(bench_steps_off(dbl, 4) == 2);
    double ok[3] = {1, 1, 1};
    CHECK(bench_steps_off(ok, 3) == 0);
    if (!fails) printf("PASS\n");
    return fails != 0;
}
