#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hd_rumble.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); return 1; } } while (0)
static int same(const uint8_t *a, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    return a[0] == b0 && a[1] == b1 && a[2] == b2 && a[3] == b3;
}
static const struct { double amp; int ha, la; } amps[] = { {0.0, 0, 64}, {0.007843, 2, 32832}, {0.011823, 4, 65}, {0.019885, 10, 32834}, {0.033442, 16, 68}, {0.047296, 20, 69}, {0.079542, 26, 32838}, {0.112491, 30, 32839}, {0.117471, 32, 72}, {0.189185, 54, 32845}, {0.224982, 62, 32847}, {0.229908, 64, 80}, {0.395124, 114, 32860}, {0.501433, 136, 98}, {0.773316, 176, 108}, {1.002867, 200, 114} };
static const struct { double hz; int hf, lf; } freqs[] = { {40.875885, -1, 1}, {59.073048, -1, 18}, {140.500168, 104, 58}, {160.0, 128, 64}, {299.866821, 244, 93}, {320.0, 256, 96}, {626.286133, 380, 127}, {640.0, 384, -1}, {1099.916382, 484, -1}, {1252.572266, 508, -1} };
int main(void) {
    /* the tables' amplitudes: codes and both bands' bytes */
    for (unsigned i = 0; i < sizeof amps / sizeof amps[0]; i++) {
        CHECK(hd_rumble_amp_code((float)amps[i].amp) * 2 == amps[i].ha);
        const int code = hd_rumble_amp_code((float)amps[i].amp);
        CHECK((((code & 1) << 15) | (code / 2 + 0x40)) == amps[i].la);
    }
    /* every code from 1 to 100 comes back from its own amplitude (the three ranges' formulas), and the codes grow */
    for (int c = 1; c <= 100; c++) {
        const double a = c >= 32 ? pow(2.0, c / 32.0) / 8.7 : c >= 16 ? pow(2.0, c / 16.0) / 17.0 : c >= 2 ? pow(2.0, c / 4.0) / 119.6 : 0.007843;
        CHECK(hd_rumble_amp_code((float)a) == c);
    }
    for (float a = 0.0f, last = 0; a <= 1.2f; a += 0.0005f) {
        const int c = hd_rumble_amp_code(a);
        CHECK(c >= last && c <= HD_RUMBLE_AMP_TOP);
        last = (float)c;
    }
    /* the tables' frequencies */
    for (unsigned i = 0; i < sizeof freqs / sizeof freqs[0]; i++) {
        if (freqs[i].hf >= 0) CHECK((hd_rumble_freq_code((float)freqs[i].hz, 0x61, 0xDF, 0) - 0x60) * 4 == freqs[i].hf);
        if (freqs[i].lf >= 0) CHECK(hd_rumble_freq_code((float)freqs[i].hz, 0x41, 0xBF, 0) - 0x40 == freqs[i].lf);
    }

    uint8_t out[4];
    /* silence: 00 01 40 40 (both bands at 0 amplitude, 320 and 160 Hz), as SDL sends it */
    hd_rumble_neutral(out);
    CHECK(same(out, 0x00, 0x01, 0x40, 0x40));
    { const float v[4] = {0.0f, 160.0f, 0.0f, 320.0f}; hd_rumble_encode(v, 100, out); CHECK(same(out, 0x00, 0x01, 0x40, 0x40)); }
    { const float v[4] = {0.0f, 0.0f, 0.0f, 0.0f}; hd_rumble_encode(v, 100, out); CHECK(same(out, 0x00, 0x01, 0x40, 0x40)); }
    { const float v[4] = {0.002f, 100.0f, 0.001f, 300.0f}; hd_rumble_encode(v, 100, out); CHECK(same(out, 0x00, 0x01, 0x40, 0x40)); }
    { const float v[4] = {NAN, 100.0f, -1.0f, 300.0f}; hd_rumble_encode(v, 100, out); CHECK(same(out, 0x00, 0x01, 0x40, 0x40)); }
    { const float v[4] = {0.5f, 100.0f, 0.5f, 300.0f}; hd_rumble_encode(v, 0, out); CHECK(same(out, 0x00, 0x01, 0x40, 0x40)); }

    /* the reverse engineering notes' example: high 0x01a8 at 0x88, low 0x63 at 0x804d -> a8 89 e3 4d */
    {
        const float v[4] = {0.189185f, 10.0f * powf(2.0f, 163.0f / 32.0f), 0.501433f, 10.0f * powf(2.0f, 202.0f / 32.0f)};
        hd_rumble_encode(v, 100, out);
        CHECK(same(out, 0xa8, 0x89, 0xe3, 0x4d));
    }
    /* the defaults' frequencies at 0.501433 low and 0.395124 high */
    { const float v[4] = {0.501433f, 160.0f, 0.395124f, 320.0f}; hd_rumble_encode(v, 100, out); CHECK(same(out, 0x00, 0x73, 0x40, 0x62)); }
    /* one band alone: the other at 0 amplitude, its frequency kept (or 160 / 320 Hz when it has none) */
    { const float v[4] = {0.0f, 0.0f, 0.229908f, 320.0f}; hd_rumble_encode(v, 100, out); CHECK(same(out, 0x00, 0x41, 0x40, 0x40)); }
    { const float v[4] = {0.229908f, 160.0f, 0.0f, NAN}; hd_rumble_encode(v, 100, out); CHECK(same(out, 0x00, 0x01, 0x40, 0x50)); }

    /* clamping: the frequencies to their bands, the amplitude to code 100 (1.003, the top the table calls safe) */
    { const float v[4] = {0.5f, 10.0f, 0.5f, 20.0f}; hd_rumble_encode(v, 100, out); CHECK(out[0] == 0x04 && (out[1] & 1) == 0 && (out[2] & 0x7F) == 0x01); }
    { const float v[4] = {0.5f, 5000.0f, 0.5f, INFINITY}; hd_rumble_encode(v, 100, out); CHECK(out[0] == 0xFC && (out[1] & 1) == 1 && (out[2] & 0x7F) == 0x7F); }
    { const float v[4] = {3.0f, 160.0f, 3.0f, 320.0f}; hd_rumble_encode(v, 100, out); CHECK(same(out, 0x00, 0xc8 | 1, 0x40, 0x72)); }
    { const float v[4] = {0.9f, 160.0f, 0.9f, 320.0f}; hd_rumble_encode(v, 150, out); CHECK(same(out, 0x00, 0xc8 | 1, 0x40, 0x72)); }
    /* over 100%, the bands keep their balance: 0.9 and 0.45 at 150% are 1.0 (code 100) and 0.5 (code 68), not both 100 */
    { const float v[4] = {0.9f, 160.0f, 0.45f, 320.0f}; hd_rumble_encode(v, 150, out); CHECK(same(out, 0x00, 0x88 | 1, 0x40, 0x72)); }
    {   /* under the top, it is a plain factor: 0.2 and 0.1 at 150% play as 0.3 and 0.15 */
        const float v[4] = {0.2f, 160.0f, 0.1f, 320.0f}, w[4] = {0.3f, 160.0f, 0.15f, 320.0f};
        uint8_t o2[4];
        hd_rumble_encode(v, 150, out);
        hd_rumble_encode(w, 100, o2);
        CHECK(!memcmp(out, o2, 4));
    }
    /* the strength: 50% of 0.459817 is 0.229908 (code 32 -> 0x40) */
    { const float v[4] = {0.459817f, 160.0f, 0.459817f, 320.0f}; hd_rumble_encode(v, 50, out); CHECK(same(out, 0x00, 0x41, 0x40, 0x50)); }

    /* (issue 263) the amplitudes as they are to be felt: (a + a^0.35) / 2, 0 and 1 kept, the order kept, frequencies
       kept; what OoT asks for (a heart caught: 0.06 at 55 Hz, cut grass 0.03) plays at about a fifth, not a few percent */
    {
        CHECK(hd_rumble_felt_amp(0.0f) == 0.0f && hd_rumble_felt_amp(-1.0f) == 0.0f && hd_rumble_felt_amp(NAN) == 0.0f);
        CHECK(hd_rumble_felt_amp(0.0038f) == 0.0f && hd_rumble_felt_amp(0.004f) > 0.07f);   /* (below code 1: silent) */
        CHECK(fabsf(hd_rumble_felt_amp(1.0f) - 1.0f) < 1e-6f && hd_rumble_felt_amp(3.0f) == 1.0f);
        CHECK(fabsf(hd_rumble_felt_amp(0.06f) - 0.2165f) < 0.001f && fabsf(hd_rumble_felt_amp(0.03f) - 0.1615f) < 0.001f);
        for (float a = 0.004f, last = 0.0f; a <= 1.0f; a += 0.001f) {
            const float f = hd_rumble_felt_amp(a);
            CHECK(f > last && f >= a && f <= 1.0f);
            last = f;
        }
        const float v[4] = {0.06f, 55.0f, 0.02f, 231.0f};
        float w[4];
        hd_rumble_felt(v, w);
        CHECK(w[1] == 55.0f && w[3] == 231.0f && w[0] > 0.2f && w[2] > 0.12f);
        hd_rumble_encode(w, 100, out);
        CHECK(hd_rumble_amp_code(w[0]) == 30 && (out[1] >> 1) == 20);   /* (raw, 0.06 and 0.02: codes 11 and 5) */
        const float q[4] = {0.0f, 160.0f, 0.002f, 320.0f};   /* (silence and a value under code 1 stay silent) */
        hd_rumble_felt(q, w);
        hd_rumble_encode(w, 150, out);
        CHECK(same(out, 0x00, 0x01, 0x40, 0x40));
    }

    /* (a single Joy-Con) both sides merged: each band the stronger side's amplitude with that side's frequency */
    {
        const float l[4] = {0.2f, 100.0f, 0.6f, 300.0f}, r[4] = {0.5f, 140.0f, 0.1f, 250.0f}, q[4] = {0.0f, 160.0f, 0.0f, 320.0f};
        float m[4];
        hd_rumble_merge(l, r, m);
        CHECK(m[0] == 0.5f && m[1] == 140.0f && m[2] == 0.6f && m[3] == 300.0f);
        hd_rumble_merge(r, l, m);
        CHECK(m[0] == 0.5f && m[1] == 140.0f && m[2] == 0.6f && m[3] == 300.0f);
        hd_rumble_merge(l, q, m);   /* (one side only: that side as it is) */
        CHECK(!memcmp(m, l, sizeof m));
        hd_rumble_merge(q, r, m);
        CHECK(!memcmp(m, r, sizeof m));
        hd_rumble_merge(q, q, m);
        CHECK(!memcmp(m, q, sizeof m));
        const float e[4] = {0.3f, 120.0f, 0.3f, 280.0f}, f[4] = {0.3f, 90.0f, 0.3f, 330.0f};
        hd_rumble_merge(e, f, m);   /* (equal: the left's frequency) */
        CHECK(m[1] == 120.0f && m[3] == 280.0f);
    }

    /* the packet: report 0x10, then each side where the controller plays it */
    const uint8_t L[4] = {1, 2, 3, 4}, R[4] = {5, 6, 7, 8};
    uint8_t p[HD_RUMBLE_PACKET_SIZE];
    hd_rumble_packet(HD_RUMBLE_PRO, L, R, p);
    CHECK(p[0] == 0x10 && p[1] == 0 && !memcmp(p + 2, L, 4) && !memcmp(p + 6, R, 4));
    hd_rumble_packet(HD_RUMBLE_JOYCON_PAIR, L, R, p);
    CHECK(!memcmp(p + 2, L, 4) && !memcmp(p + 6, R, 4));
    hd_rumble_packet(HD_RUMBLE_JOYCON_LEFT, L, R, p);
    CHECK(p[0] == 0x10 && !memcmp(p + 2, L, 4) && !memcmp(p + 6, L, 4));
    hd_rumble_packet(HD_RUMBLE_JOYCON_RIGHT, L, R, p);
    CHECK(p[0] == 0x10 && !memcmp(p + 2, R, 4) && !memcmp(p + 6, R, 4));
    {   /* (SDL's SwitchCommonOutputPacket_t, laid out the same) */
        typedef struct { uint8_t type, number; struct { uint8_t data[4]; } rumble[2]; } Sdl;
        CHECK(sizeof(Sdl) == HD_RUMBLE_PACKET_SIZE);
    }

    /* which pads: SDL3's and SDL2's Switch Pro and Joy-Con types, nothing else (Xbox, PlayStation, GameCube, virtual) */
    CHECK(hd_rumble_kind(3, 7) == HD_RUMBLE_PRO && hd_rumble_kind(3, 8) == HD_RUMBLE_JOYCON_LEFT);
    CHECK(hd_rumble_kind(3, 9) == HD_RUMBLE_JOYCON_RIGHT && hd_rumble_kind(3, 10) == HD_RUMBLE_JOYCON_PAIR);
    CHECK(hd_rumble_kind(2, 5) == HD_RUMBLE_PRO && hd_rumble_kind(2, 11) == HD_RUMBLE_JOYCON_LEFT);
    CHECK(hd_rumble_kind(2, 12) == HD_RUMBLE_JOYCON_RIGHT && hd_rumble_kind(2, 13) == HD_RUMBLE_JOYCON_PAIR);
    for (int t = 0; t < 7; t++) CHECK(hd_rumble_kind(3, t) == HD_RUMBLE_NONE);
    for (int t = 11; t < 16; t++) CHECK(hd_rumble_kind(3, t) == HD_RUMBLE_NONE);
    for (int t = 0; t < 16; t++) if (t != 5 && (t < 11 || t > 13)) CHECK(hd_rumble_kind(2, t) == HD_RUMBLE_NONE);
    CHECK(hd_rumble_kind(0, 7) == HD_RUMBLE_NONE);
    CHECK(hd_rumble_start(HD_RUMBLE_PRO, 1, 0) == HD_RUMBLE_UNKNOWN);
    CHECK(hd_rumble_start(HD_RUMBLE_PRO, 0, 0) == HD_RUMBLE_OFF && hd_rumble_start(HD_RUMBLE_PRO, 1, 1) == HD_RUMBLE_OFF);
    CHECK(hd_rumble_start(HD_RUMBLE_NONE, 1, 0) == HD_RUMBLE_OFF);
    CHECK(hd_rumble_after_send(1) == HD_RUMBLE_ON && hd_rumble_after_send(0) == HD_RUMBLE_OFF);

    /* the cadence */
    const uint8_t n[4] = {0x00, 0x01, 0x40, 0x40}, on[4] = {0x00, 0x41, 0x40, 0x50}, on2[4] = {0x00, 0x43, 0x40, 0x50};
    uint8_t quiet[10], loud[10], loud2[10];
    hd_rumble_packet(HD_RUMBLE_PRO, n, n, quiet);
    hd_rumble_packet(HD_RUMBLE_PRO, on, on, loud);
    hd_rumble_packet(HD_RUMBLE_PRO, on2, on, loud2);
    const long long G = HD_RUMBLE_GAP_NS, F = HD_RUMBLE_REFRESH_NS, t = 5000000000LL;
    CHECK(G >= 30000000LL && F > G && F < 50000000LL);   /* (SDL: 30 ms between writes, its own refresh every 50 ms) */
    CHECK(!hd_rumble_due(quiet, quiet, 0, t, 0));           /* a new pad, silence: nothing */
    CHECK(hd_rumble_due(loud, quiet, 0, t, 0));             /* a new pad, a rumble playing: at once */
    CHECK(!hd_rumble_due(loud2, loud, 1, t + G - 1, t));    /* a change within the gap waits */
    CHECK(hd_rumble_due(loud2, loud, 1, t + G, t));         /* and goes when it is over */
    CHECK(!hd_rumble_due(loud, loud, 1, t + F - 1, t));     /* the same rumble, refreshed */
    CHECK(hd_rumble_due(loud, loud, 1, t + F, t));
    CHECK(!hd_rumble_due(quiet, loud, 1, t + G - 1, t));    /* a stop, after the gap */
    CHECK(hd_rumble_due(quiet, loud, 1, t + G, t));
    CHECK(!hd_rumble_due(quiet, quiet, 1, t + 10 * F, t));  /* silence that was sent: not again */
    return 0;
}
