#include "frame_whole.h"
#include <math.h>
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    static FrameWhole w;
    const uint64_t a = 0x1000, b = 0x2000;
    uint32_t n = 0;
    for (int i = 0; i < 144; i++) n += frame_whole(&w, a, 60.0f / 144.0f, 1);
    CHECK(n == 60 || n == 59);   /* (a second at 144 fps: 60 frames, give or take the float's last one) */
    n = 0;
    for (int i = 0; i < 1440; i++) n += frame_whole(&w, a, 60.0f / 144.0f, 1);
    CHECK(n >= 599 && n <= 601);
    CHECK(frame_whole(&w, b, 0.42f, 1) == 0 && frame_whole(&w, b, 0.42f, 1) == 0 && frame_whole(&w, b, 0.42f, 1) == 1);
    n = 0;
    for (int i = 0; i < 4; i++) n += frame_whole(&w, b + 8, 1.25f, 1);   /* (the step times 3: its fractions kept) */
    CHECK(n == 5);
    CHECK(frame_whole(&w, a, 0.42f, 0) == 0 && frame_whole(&w, a, 1.5f, 0) == 1 && frame_whole(&w, a, 2.0f, 0) == 2);
    CHECK(frame_whole(&w, a, -1.0f, 1) == 0 && frame_whole(&w, a, NAN, 1) == 0 && frame_whole(&w, a, 0.0f, 1) == 0);
    for (uint64_t k = 0; k < 100; k++) frame_whole(&w, 0x9000 + k, 0.5f, 1);   /* (more counters than slots: the oldest go) */
    CHECK(frame_whole(&w, 0x9000 + 99, 0.5f, 1) == 1);   /* (a slot still held: 0.5 + 0.5) */
    CHECK(frame_whole(&w, 0x9000, 0.5f, 1) == 0);   /* (the oldest's slot was reused: it starts again from 0) */
    /* (a scene's water surfaces, each stepped once a frame at 144 fps: 40 of them, more than the old 16 slots, each still counts
       its 60 frames a second) */
    {
        static FrameWhole water;
        uint32_t passes[40] = {0};
        for (int f = 0; f < 144; f++)
            for (int k = 0; k < 40; k++) passes[k] += frame_whole(&water, 0x7000000 + (uint64_t)k * 0x400, 60.0f / 144.0f, 1);
        for (int k = 0; k < 40; k++) CHECK(passes[k] == 60 || passes[k] == 59);
    }
    if (!fails) printf("PASS\n");
    return fails != 0;
}
