#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hd_rumble.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); return 1; } } while (0)
#define MAX_PADS 8
@PADS@
static int fake_type = 7, fake_has = 1, fake_ok = 1, sent_n, logged;
static uint8_t sent_last[HD_RUMBLE_PACKET_SIZE];
static int hos_sdl_load(void) { return 3; }
static int hos_sdl_pad_type(void *pad) { (void)pad; return fake_type; }
static int hos_sdl_pad_has_effect(void) { return fake_has; }
static int hos_sdl_pad_effect(void *pad, const void *data, int size) {
    (void)pad;
    if (size != HD_RUMBLE_PACKET_SIZE) return 0;
    sent_n++;
    memcpy(sent_last, data, HD_RUMBLE_PACKET_SIZE);
    return fake_ok;
}
static const char *hos_sdl_pad_name(void *pad) { (void)pad; return "pad"; }
static const char *hos_sdl_error(void) { return "unsupported"; }
static void hos_log(const char *fmt, ...) { (void)fmt; logged++; }
@OPEN@
@POLL@
int main(void) {
    static int dummy;
    uint8_t side[3][4];
    memset(side, 0, sizeof side);
    const float quiet[4] = {0.0f, 160.0f, 0.0f, 320.0f}, left[4] = {0.5f, 120.0f, 0.3f, 280.0f}, right[4] = {0.2f, 100.0f, 0.0f, 320.0f};
    const long long t = 1000000000LL;

    /* a Pro Controller whose SDL takes the packets: nothing while silent, then the game's two sides, refreshed */
    pads[0].gc = &dummy;
    hd_open(0, &dummy);
    CHECK(pads[0].hd == HD_RUMBLE_UNKNOWN && pads[0].hd_kind == HD_RUMBLE_PRO);
    hd_rumble_encode(quiet, 100, side[0]), hd_rumble_encode(quiet, 100, side[1]);
    CHECK(hd_poll(0, side, t) == 1 && sent_n == 0);
    hd_rumble_encode(left, 100, side[0]), hd_rumble_encode(right, 100, side[1]);
    hd_rumble_encode(quiet, 100, side[2]);   /* (the merged side: not a Pro Controller's) */
    CHECK(hd_poll(0, side, t + 4000000) == 1 && sent_n == 1 && pads[0].hd == HD_RUMBLE_ON);
    CHECK(sent_last[0] == 0x10 && !memcmp(sent_last + 2, side[0], 4) && !memcmp(sent_last + 6, side[1], 4));
    for (long long now = t + 8000000; now < t + 4000000 + 1000000000LL; now += 4000000) CHECK(hd_poll(0, side, now) == 1);
    CHECK(sent_n >= 1 + 1000 / 41 && sent_n <= 1 + 1000 / 32);   /* (a held rumble: about every 40 ms, never faster than 32) */
    const int before = sent_n;
    hd_rumble_encode(quiet, 100, side[0]), hd_rumble_encode(quiet, 100, side[1]);
    long long now = t + 2000000000LL;
    CHECK(hd_poll(0, side, now) == 1 && sent_n == before + 1 && hd_rumble_silent(sent_last));   /* the stop */
    for (int k = 0; k < 100; k++) {
        now += 4000000;
        CHECK(hd_poll(0, side, now) == 1);
    }
    CHECK(sent_n == before + 1);   /* silence is not sent again */

    /* a pad whose SDL refuses the packet (SDL2's own driver, a Switch 2 controller, the kernel's driver, a combined
       pair): its first rumble falls back to the motors, for good */
    sent_n = 0, fake_ok = 0;
    pads[1].gc = &dummy;
    pads[1].rumble_lo = 7, pads[1].rumble_ns = 99;
    hd_open(1, &dummy);
    hd_rumble_encode(left, 100, side[0]), hd_rumble_encode(left, 100, side[1]);
    CHECK(hd_poll(1, side, t) == 0 && sent_n == 1 && pads[1].hd == HD_RUMBLE_OFF);
    CHECK(pads[1].rumble_lo == 0 && pads[1].rumble_ns == 0);   /* (the motor path sends what plays at once) */
    CHECK(logged == 2);   /* (HD rumble on the first pad, the second's fallback: once each) */
    fake_ok = 1;
    CHECK(hd_poll(1, side, t + 1000000000LL) == 0 && sent_n == 1);

    /* not Switch controllers, or SDL without the call: the motor path, nothing sent */
    fake_type = 1;   /* (SDL_GAMEPAD_TYPE_STANDARD) */
    hd_open(2, &dummy), pads[2].gc = &dummy;
    CHECK(pads[2].hd == HD_RUMBLE_OFF && hd_poll(2, side, t) == 0 && sent_n == 1);
    fake_type = 7, fake_has = 0;
    hd_open(3, &dummy), pads[3].gc = &dummy;
    CHECK(pads[3].hd == HD_RUMBLE_OFF && hd_poll(3, side, t) == 0 && sent_n == 1);
    /* a single right Joy-Con plays the right side */
    fake_has = 1, fake_type = 9;
    hd_open(4, &dummy), pads[4].gc = &dummy;
    hd_rumble_encode(left, 100, side[0]), hd_rumble_encode(right, 100, side[1]);
    {   /* a single Joy-Con plays both sides merged, in both halves of the packet */
        float m[4];
        hd_rumble_merge(left, right, m);
        hd_rumble_encode(m, 100, side[2]);
    }
    CHECK(hd_poll(4, side, t) == 1 && sent_n == 2 && !memcmp(sent_last + 2, side[2], 4) && !memcmp(sent_last + 6, side[2], 4));
    {   /* an effect the game sends to the left side only still plays on a right Joy-Con (and a right-only one on a
           left Joy-Con) */
        float m[4];
        hd_rumble_encode(left, 100, side[0]), hd_rumble_encode(quiet, 100, side[1]);
        hd_rumble_merge(left, quiet, m), hd_rumble_encode(m, 100, side[2]);
        CHECK(!memcmp(side[2], side[0], 4));
        CHECK(hd_poll(4, side, t + 1000000000LL) == 1 && sent_n == 3 && !memcmp(sent_last + 2, side[0], 4) &&
              !memcmp(sent_last + 6, side[0], 4));
        fake_type = 8;   /* (a left Joy-Con) */
        hd_open(5, &dummy), pads[5].gc = &dummy;
        hd_rumble_encode(quiet, 100, side[0]), hd_rumble_encode(right, 100, side[1]);
        hd_rumble_merge(quiet, right, m), hd_rumble_encode(m, 100, side[2]);
        CHECK(hd_poll(5, side, t) == 1 && sent_n == 4 && !memcmp(sent_last + 2, side[1], 4) && !memcmp(sent_last + 6, side[1], 4));
    }
    return 0;
}
