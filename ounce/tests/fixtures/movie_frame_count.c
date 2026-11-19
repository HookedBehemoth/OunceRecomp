#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include "patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* (the lock as a flag: taken once, given back once) */
typedef int os_mutex;
#define OS_MUTEX_INIT 0
static void os_mutex_lock(os_mutex *l) { CHECK(!*l); *l = 1; }
static void os_mutex_unlock(os_mutex *l) { CHECK(*l); *l = 0; }
volatile uint32_t hos_frame_seconds_bits;
static void hos_log(const char *fmt, ...) { (void)fmt; }
static int64_t hos_now_ns(void) { return 0; }
@HOST@
/* (the lifted update with the patch, on a small guest: the count at +760 of the player) */
typedef struct { uint64_t x[31]; } Ctx;
static uint32_t guest[2][256];
static uint32_t recomp_load32(Ctx *c, uint64_t a) { (void)c; return guest[a >> 12][(a & 0xfff) / 4]; }
static void recomp_store32(Ctx *c, uint64_t a, uint64_t v) { (void)c; guest[a >> 12][(a & 0xfff) / 4] = (uint32_t)v; }
static uint32_t update(int player) {
    Ctx ctx, *c = &ctx;
    memset(&ctx, 0, sizeof ctx);
    c->x[20] = (uint64_t)player << 12;
@LIFTED@
    return guest[player][760 / 4];
}
static void seconds(double s) {
    const float f = (float)s;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    hos_frame_seconds_bits = s > 0 ? bits : 0;
}
static int within(uint32_t got, uint32_t want) { return got + 1 >= want && got <= want + 1; }
int main(void) {
    uint32_t n = 0, before;
    /* no frame time (a cap of 60): one an update, as the game counts */
    seconds(0);
    for (int i = 0; i < 600; i++) n = update(0);
    CHECK(n == 600);
    CHECK(hos_movie_frames(0x5000) == 1 && hos_movie_frames(0x5000) == 1);
    /* 120 fps: half a 60th an update, a whole one every second update */
    seconds(1.0 / 120);
    before = n;
    for (int i = 0; i < 600; i++) {
        const uint32_t was = n;
        n = update(0);
        CHECK(n - was <= 1);
    }
    CHECK(within(n - before, 300));
    /* 240 fps and 40 fps: a quarter and one and a half */
    seconds(1.0 / 240);
    before = n;
    for (int i = 0; i < 960; i++) n = update(0);
    CHECK(within(n - before, 240));
    seconds(1.0 / 40);
    before = n;
    for (int i = 0; i < 400; i++) {
        const uint32_t was = n;
        n = update(0);
        CHECK(n - was == 1 || n - was == 2);
    }
    CHECK(within(n - before, 600));
    /* two movies at once: each its own count (one carry for both would give each half) */
    seconds(1.0 / 120);
    before = n;
    {
        const uint32_t other = update(1) - 0;   /* (its first update) */
        uint32_t m = other;
        for (int i = 0; i < 600; i++) n = update(0), m = update(1);
        CHECK(within(n - before, 300) && within(m - other, 300));
    }
    /* a movie that plays while others come and go keeps its count: 14 more that play with it and a new player every update,
       16 at a time, at 240 fps (places taken in the order they were made gave this one a new start every second update: a
       quarter of a 60th an update then counted 0 or 1 every two, 1200 only on average) */
    seconds(1.0 / 240);
    {
        uint32_t mine = 0, steady = 0;
        for (int i = 0; i < 4800; i++) {
            mine += hos_movie_frames(0x7000000);
            for (int k = 0; k < 14; k++) steady += hos_movie_frames(0x7100000 + (uint64_t)k * 0x1000);
            hos_movie_frames(0x8000000 + (uint64_t)i * 0x1000);
        }
        CHECK(within(mine, 1200) && steady + 14 >= 14 * 1200 && steady <= 14 * 1200 + 14);
    }
    seconds(1.0 / 120);
    /* more movies at once than the host keeps places for, each updated in turn: every update finds its player's place taken and
       starts from a drawn part of a 60th, so 480 updates of half a 60th give 240 on average, 11 either way as one standard
       deviation. Every count within 5 of those, for any number of players: with a new place started at 0 every count stood
       still, and with a fixed step between the starts one player's did among 233 (and 29 players gave 108 to 131 of 120) */
    {
        static const int counts[] = {9, 17, 29, 40, 233, 377};
        static uint32_t sum[377];
        uint32_t least = ~0u, most = 0;
        for (unsigned t = 0; t < sizeof counts / sizeof counts[0]; t++) {
            const int players = counts[t];
            memset(sum, 0, sizeof sum);
            for (int i = 0; i < 480; i++)
                for (int k = 0; k < players; k++) sum[k] += hos_movie_frames(0x100000 + (uint64_t)t * 0x1000000 + (uint64_t)k * 0x1000);
            for (int k = 0; k < players; k++) {
                CHECK(sum[k] >= 240 - 55 && sum[k] <= 240 + 55);
                least = sum[k] < least ? sum[k] : least, most = sum[k] > most ? sum[k] : most;
            }
        }
        printf("more players than places: %u to %u of 240\n", least, most);
    }
    before = n;
    for (int i = 0; i < 600; i++) n = update(0);
    CHECK(within(n - before, 300));
    /* back at a cap of 60: one an update again */
    seconds(0);
    before = n;
    for (int i = 0; i < 10; i++) n = update(0);
    CHECK(n - before == 10);
    printf("PASS\n");
    return 0;
}
