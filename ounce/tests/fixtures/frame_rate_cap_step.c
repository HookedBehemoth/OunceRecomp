#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include "patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static uint8_t guest[0x4000];
#define GP(a) ((void *)(guest + (a)))
#define MOD 0x1000u
static void hos_log(const char *fmt, ...) { (void)fmt; }
int rx_option(int id, int otherwise) { (void)id; return otherwise; }
static int hos_bench_on(void) { return 0; }   /* (HOS_BENCH off: test_bench.py checks it on) */
int hos_ini_option(const char *key, int otherwise) { (void)key; return otherwise; }
volatile uint64_t hos_gfx_module = MOD;
volatile int hos_fixed_resolution, hos_dynres_runs;
volatile uint32_t hos_fixed_scale_bits;
volatile float hos_text_scale;
volatile uint64_t hos_step_probe, hos_res_module;
volatile uint32_t hos_frame_seconds_bits;
@HOST@
@PROBES@
/* (the lifted divide, fdiv s8, s1, s0, then the At probe: s8 := hos_oot_frame_step_probe(s1, s8, x19)) */
static float lifted_step(float base, float nominal) {
    float q = base / nominal;
    return hos_oot_frame_step_probe(base, q, hos_step_probe);
}
static int64_t now = 1000000000LL;
static float frames(int n, double seconds) {   /* (n presents that far apart: the seconds the hook is given, 0 when off) */
    float f;
    uint32_t bits;
    for (int i = 0; i < n; i++) now += (int64_t)(seconds * 1e9), realtime_step(now);
    bits = hos_frame_seconds_bits;
    memcpy(&f, &bits, 4);
    return bits ? f : 0.0f;
}
static int near(float a, double b) { return a > b * 0.999 && a < b * 1.001; }
int main(int argc, char **argv) {
    int *limit = (int *)GP(MOD + 0xfc), *rate = (int *)GP(MOD + 0x2c8);
    const char *mode = argc > 1 ? argv[1] : "";
    *limit = 60, *rate = 60;
    hos_step_probe = 0x2000;   /* (the lifted code's frame step sets it) */
    realtime_step(now);
    if (!strcmp(mode, "cap 120")) {
        CHECK(near(frames(72000, 1.0 / 120), 1.0 / 120) && *rate == 60 && *limit == 120);   /* (the rate stays the engine's) */
        CHECK(near(lifted_step(60.0f, (float)*rate), 0.5) && near(lifted_step(30.0f, (float)*rate), 0.25));   /* (half a 60th a frame) */
        CHECK(near(frames(10, 1.0 / 40), 1.0 / 40) && *rate == 40 && near(lifted_step(60.0f, (float)*rate), 1.5));
        CHECK(near(frames(10, 1.0 / 1000), 1.0 / 240));   /* (the shortest step) */
        CHECK(near(frames(10, 0.2), 4.0 / 60));           /* (the longest, as the rate's 15) */
    } else if (!strcmp(mode, "cap 60")) {
        CHECK(frames(72000, 1.0 / 120) == 0.0f && *rate == 60 && *limit == 60);
        /* A long unpaced stretch must repay only bounded debt, including the five-frame smoothing transition. */
        int recovered = 0;
        for (int i = 0; i < 24; i++) {
            CHECK(frames(1, 1.0 / 40) == 0.0f && *rate >= 15 && *rate <= 60);
            if (*rate == 40) { recovered = 1; break; }
        }
        CHECK(recovered && lifted_step(60.0f, (float)*rate) == 1.5f);
    } else {   /* (HOS_FPS_STEP=0: the rate up to the cap) */
        CHECK(frames(10, 1.0 / 120) == 0.0f && *rate == 120 && *limit == 120);
        CHECK(lifted_step(60.0f, 60.0f) == 1.0f);
    }
    printf("PASS: %s\n", mode);
    return 0;
}
