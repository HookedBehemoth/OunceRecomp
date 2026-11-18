/* OoT's frame timing at each present (runtime/game_hooks.h game_frame_timing): the real-time step through the
   graphics module's nominal rate (+0x2c8) and its frame limiter (+0xfc), the frame's seconds for the step divides
   (oot_probes.c), a movie's frame count, the fixed resolution's state and the Insert menu's Text speed. */
#include "hos.h"
#include "game_hooks.h"
#include "os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int rx_option(int id, int otherwise);   /* (runtime/rx_backend.c) */
int hos_fps_max(void);                  /* (runtime/nn_vi.c) */

/* Game time follows the clock. The engine steps every frame by ratio = 60 / <the graphics module's nominal fps,
   an int at +0x2c8> (main+0x6e6cc4, f_83b6e300 makes seconds = ratio / 60 and the rest), so at 50 fps the game ran at 83%
   speed: walking the same 8 s at 52 and 33 fps went as far as those rates. Each present writes the rate of the last 5
   display intervals there, the rounding error carried to the next frame so that no time is lost; a step is at most
   1/15 s, as TotK Recomp's frame_rate.c (slower frames slow the game down, as on the console), at least 1/60 s (the game's
   own limit holds it at 60). HOS_REALTIME_STEP=0: the shipping whole steps. */
volatile uint64_t hos_gfx_module;   /* (f_8042cd20's graphics module: frame_sleep.c sets it at the frame's end) */
volatile int hos_fixed_resolution = 0;   /* (the lifted code reads these: games/oot/patches/oot_probes.c) */
volatile uint32_t hos_fixed_scale_bits = 0x3f800000;   /* (HOS_RES_SCALE: the fixed path's scale) */
volatile int hos_dynres_runs = 0;   /* (the dynamic path's frames: it fills fields the 3D reads, +8 +16 +168) */
volatile float hos_text_scale = 1.0f;   /* (the letters' clock of a text pane, f_80b1e140: games/oot/patches/oot_probes.c hos_oot_text_time) */
/* The Insert menu's Text speed (0 Normal, 1 2x, 2 4x, 3 Instant), HOS_TEXT_SPEED=<0..3> over it: what the time of a letter
   animation's step is multiplied by. Instant: the page at once, as the game's own skip on A */
static float text_scale(const char *env, int menu) {
    const int v = env && *env ? atoi(env) : menu;
    return v == 1 ? 2.0f : v == 2 ? 4.0f : v >= 3 ? 1e6f : 1.0f;
}
volatile uint64_t hos_step_probe = 0;   /* (f_80706be0's object, set by the lifted code's frame-time hook; its
                                            +0xd58 is the game clock in 1/60 s: games/oot/patches/oot_probes.c) */
volatile uint32_t hos_frame_seconds_bits = 0;   /* (the frame's seconds as a float's bits for that hook, 0: the game's own step) */
/* A movie's frame count while the frames' time is given (a Frame rate cap above 60): how many 60ths of a second this update of
   that movie player is worth, the rest carried to its next one, so the count the cutscene's cues are placed by keeps the clock
   as the movie's picture does (issue 1742: the narration ran ahead above 60 fps). 1, the game's own step, otherwise, and with
   HOS_FPS_MOVIE=0 (for comparisons). Called by the lifted f_83b7c710 (games/oot/patches/oot_probes.c). */
/* HOS_DBG=movie (diagnostic): every 5 s of wall time while a movie counts, its updates and the frames they counted, per
   second */
static uint32_t movie_log(uint32_t n) {
    static int on = -1;
    static int64_t since;
    static uint64_t calls, frames;
    if (on < 0) on = hos_dbg_item("movie");
    if (!on) return n;
    const int64_t now = hos_now_ns();
    if (!since) since = now;
    calls++, frames += n;
    if (now - since >= 5000000000LL) {
        const double s = (double)(now - since) / 1e9;
        hos_log("movie: %.1f updates and %.1f frames counted a second", (double)calls / s, (double)frames / s);
        since = now, calls = frames = 0;
    }
    return n;
}
uint32_t hos_movie_frames(uint64_t player) {
    enum { PLACES = 16 };
    const uint32_t bits = __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED);   /* (game_frame_timing writes it at each present) */
    if (!bits) return movie_log(1);
    static os_mutex lock = OS_MUTEX_INIT;
    static int timed = -1;   /* (these four under the lock) */
    static struct { uint64_t player, used; double part; } carry[PLACES];
    static uint64_t calls;
    static unsigned made;
    float secs;
    uint32_t n = 1;
    memcpy(&secs, &bits, 4);
    os_mutex_lock(&lock);
    if (timed < 0) timed = !(getenv("HOS_FPS_MOVIE") && !atoi(getenv("HOS_FPS_MOVIE")));
    if (timed) {
        int i = 0;
        while (i < PLACES && carry[i].player != player) i++;
        if (i == PLACES) {
            /* (a player not seen yet takes the place used longest ago, so a movie that plays keeps its own while others come
               and go: its count is exact while no more than PLACES players, itself among them, update in turn. It starts at a part
               of a 60th drawn from a hash of how many places were made: with that many players at once every update is a first
               one, and each count then still grows by the frames' time on average, whatever the number of players, instead of
               standing still. A sequence with a fixed step, the golden ratio's multiples, stood still for one player in 233) */
            uint32_t x = made++;
            x ^= x >> 16, x *= 0x85ebca6bu, x ^= x >> 13, x *= 0xc2b2ae35u, x ^= x >> 16;   /* (MurmurHash3's last step) */
            i = 0;
            for (int k = 1; k < PLACES; k++)
                if (carry[k].used < carry[i].used) i = k;
            carry[i].player = player, carry[i].part = x / 4294967296.0;
        }
        carry[i].used = ++calls;
        carry[i].part += (double)secs * 60.0;
        n = (uint32_t)carry[i].part;
        carry[i].part -= n;
    }
    os_mutex_unlock(&lock);
    return movie_log(n);
}
volatile uint64_t hos_res_module = 0;   /* (f_831f3200's object, whose +0xe5c is the 3D's scale: games/oot/patches/oot_probes.c) */
/* Quantize the time step to the engine's integer nominal rate. Keep debt from a short frame even when the minimum
   step is 1/60: discarding it made alternating short/long frames advance more game time than elapsed wall time.
   Keep at most four nominal frames of debt so unpaced fast presents cannot leave a long recovery. Steps beyond
   1/15 s are still dropped, rather than caught up after a long stall. */
static int realtime_rate(double ratio, int maximum, double *carry) {
    ratio += *carry;
    if (ratio > 4.0) ratio = 4.0;
    const double least = 60.0 / maximum;
    const double bounded = ratio < least ? least : ratio;
    int fps = (int)(60.0 / bounded + 0.5);
    fps = fps < 15 ? 15 : fps > maximum ? maximum : fps;
    *carry = ratio - 60.0 / fps;
    if (*carry < -4.0) *carry = -4.0;
    return fps;
}
void game_frame_timing(int64_t now) {
    int rx_option(int id, int otherwise);
    /* The game's dynamic resolution steps the 3D's scale with the GPU time, and each new scale makes a new set of render
       targets: it is off unless HOS_DYNRES=1, and the 3D stays at the fixed path's scale (hos_fixed_scale_bits). The fixed
       path skips the dynamic block, whose fields the 3D reads, so it takes over after the dynamic path has run 30 frames */
    {
        static int dyn = -1;
        if (dyn < 0) dyn = getenv("HOS_DYNRES") && atoi(getenv("HOS_DYNRES")) > 0;
        hos_fixed_resolution = !dyn && hos_dynres_runs >= 30;
    }
    {
        static const char *env;
        static int looked;
        if (!looked) looked = 1, env = getenv("HOS_TEXT_SPEED");
        hos_text_scale = text_scale(env, rx_option(5, 0));
    }
    {   /* (the Insert menu's Resolution: the fixed path's scale in 1/10000, live; HOS_RES_SCALE
           below stays over it) */
        const int scale = rx_option(3, 0);
        if (scale > 0 && !getenv("HOS_RES_SCALE")) {
            const float v = scale / 10000.0f;
            memcpy((void *)&hos_fixed_scale_bits, &v, 4);
        }
    }
    {
        static int read_scale;
        if (!read_scale) {
            read_scale = 1;
            if (getenv("HOS_RES_SCALE")) { float v = (float)atof(getenv("HOS_RES_SCALE")); memcpy((void *)&hos_fixed_scale_bits, &v, 4); }
        }
    }
    {   /* every change of the 3D's scale (+0xe5c), with the sizes it is taken against */
        static uint32_t last;
        static long long presents;
        const uint64_t m = hos_res_module;
        ++presents;
        if (m && *(uint32_t *)GP(m + 0xe5c) != last) {
            float f[4];
            memcpy(f, GP(m + 0xe50), sizeof f);
            hos_log("vi: 3D scale %.3f (+0xe50 %.0f %.0f %.0f) at present %lld, fixed %d", f[3], f[0], f[1], f[2], presents,
                    hos_fixed_resolution);
            last = *(uint32_t *)GP(m + 0xe5c);
        }
    }
    {   /* HOS_RES_LOG=1: the 3D's scale every 300 presents. +0xe20: flags (bit 3: 0.75, bit 4: 0.5 in place of the scale),
           +0xe2c bit 3: recompute asked, +0xe50/+0xe54/+0xe58: the sizes the scale is taken against, +0xe5c: the scale */
        static int on = -1;
        static long long n;
        if (on < 0) on = getenv("HOS_RES_LOG") != NULL;
        const uint64_t m = hos_res_module;
        if (on && m && !(n++ % 300)) {
            float f[4], s;
            memcpy(f, GP(m + 0xe50), sizeof f);
            memcpy(&s, (const void *)&hos_fixed_scale_bits, 4);
            hos_log("vi: res: fixed %d (scale %.3f), +0xe20 %llx, +0xe2c %x, +0xe50 %.0f %.0f %.0f, 3D scale %.3f", hos_fixed_resolution, s,
                    (unsigned long long)*(uint64_t *)GP(m + 0xe20), *(uint32_t *)GP(m + 0xe2c), f[0], f[1], f[2], f[3]);
        }
    }
    const uint64_t mod = hos_gfx_module;
    if (!mod) return;
    {   /* The frame limiter (f_8042cd20 sleeps to 1 / +0xfc). The engine's adaptive controller writes +0xfc when
           variable refresh is on (+0x231, f_804fc0f0), any rate from 40 up. HOS_FPS_TARGET=<fps>: that rate kept, 0: left alone;
           HOS_FPS_LOG=1: what the game asks for, every 300 presents */
        static int target = -1, log_on = -1;
        static long long n;
        if (target < 0) target = getenv("HOS_FPS_TARGET") ? atoi(getenv("HOS_FPS_TARGET")) : 0, log_on = getenv("HOS_FPS_LOG") != NULL;
        int *limit = (int *)GP(mod + 0xfc);
        if (log_on && !(n++ % 300))
            hos_log("vi: frame limiter %d, vrr %d %d, nominal %d, controller +0x58.. %d %d %d %d", *limit, *(uint8_t *)GP(mod + 0x231),
                    *(uint8_t *)GP(mod + 0x232), *(int *)GP(mod + 0x2c8), *(int *)GP(mod + 0x270 + 0x58), *(int *)GP(mod + 0x270 + 0x28),
                    *(int *)GP(mod + 0x270 + 0x2c), *(int *)GP(mod + 0x270 + 0x30));
        if (log_on && !((n - 1) % 300) && hos_step_probe) {   /* (game speed: the game clock's 1/60 s units a second of wall time, 60.0 at speed 1) */
            static double clock_before;
            static int64_t at_before;
            double game_clock;
            memcpy(&game_clock, GP(hos_step_probe + 0xd58), 8);
            if (at_before && now > at_before) {
                const double rate = (game_clock - clock_before) * 1e9 / (double)(now - at_before);
                hos_log("vi: game clock %.1f: %.2f a second of wall time (speed %.3f) at %.1f fps", game_clock, rate, rate / 60.0,
                        300e9 / (double)(now - at_before));
            }
            clock_before = game_clock, at_before = now;
        }
        if (target && *limit > 0 && *limit < target) *limit = target;
        if (hos_fps_max() > 60 && *limit > 0 && *limit < hos_fps_max()) *limit = hos_fps_max();
    }
    /* (the Insert menu's Game speed "per frame": a frame is a 60th of a second of game time. Not past 60 fps: the game would run
       faster than real time, so a cap above 60 keeps the real-time step whatever that row says) */
    if ((!rx_option(0, 1) && hos_fps_max() <= 60) || hos_bench_on()) {   /* (HOS_BENCH: per frame at any cap) */
        __atomic_store_n(&hos_frame_seconds_bits, 0, __ATOMIC_RELAXED);
        int *slot = (int *)GP(mod + 0x2c8);
        if (*slot >= 15 && *slot <= 60) *slot = 60;
        return;
    }
    static int64_t last;
    static double samples[5], carry;
    static unsigned count, next;
    static long long steps;
    const double dt = (now - last) / 1e9;
    last = now;
    if (dt <= 0 || dt > 0.25) { count = next = 0; carry = 0; return; }   /* (a load: the average starts again) */
    /* (HOS_REALTIME_WINDOW=<1-5>: frames averaged for the step; 1: each step the last frame's own time) */
    static int window = -1;
    if (window < 0) {
        window = getenv("HOS_REALTIME_WINDOW") ? atoi(getenv("HOS_REALTIME_WINDOW")) : 5;
        window = window < 1 ? 1 : window > 5 ? 5 : window;
    }
    samples[next++ % window] = dt;
    if (count < (unsigned)window) count++;
    double sum = 0;
    for (unsigned i = 0; i < count; i++) sum += samples[i];
    /* A cap above 60: the engine's own rate is 60 / <whole display intervals>, and its controller puts 60 back over a
       higher +0x2c8 before the step is made from it, so every frame still stepped 1/60 s and the game ran fps / 60 times too fast.
       The step is then the frames' own time at the two places the engine makes it (games/oot/patches/oot_probes.c, hos_frame_seconds_bits):
       this same average, at most 1/15 s as below and at least 1/240 s, and the rate under it stays the engine's, 60 at most.
       HOS_FPS_STEP=0 (for comparisons): the rate up to the cap. The lift stops when the hook's divides are not there
       (their PATCH_AT declarations, games/oot/patches/oot_probes.c), so every build has it */
    static int timed_step = -1;
    if (timed_step < 0) timed_step = !(getenv("HOS_FPS_STEP") && !atoi(getenv("HOS_FPS_STEP")));
    const int timed = hos_fps_max() > 60 && timed_step;
    if (timed) {
        const double secs = sum / count;
        const float f = (float)(secs > 4.0 / 60.0 ? 4.0 / 60.0 : secs < 1.0 / 240.0 ? 1.0 / 240.0 : secs);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        __atomic_store_n(&hos_frame_seconds_bits, bits, __ATOMIC_RELAXED);   /* (read by the game's threads) */
    } else {
        __atomic_store_n(&hos_frame_seconds_bits, 0, __ATOMIC_RELAXED);
    }
    const int top = timed ? 60 : hos_fps_max();
    const double ratio = sum / count * 60.0;
    int *slot = (int *)GP(mod + 0x2c8);
    if (*slot < 15 || *slot > hos_fps_max()) {
        static int logged;
        if (!logged++) hos_log("vi: the graphics module's frame rate reads %d: no real-time step", *slot);
        return;
    }
    /* The timed hook already supplies the actual frame time. Its nominal rate tracks this frame without debt. */
    double frame_carry = timed ? 0 : carry;
    const int fps = realtime_rate(ratio, top, &frame_carry);
    carry = timed ? 0 : frame_carry;
    *slot = fps;
    if (!(steps++ % 3000)) hos_log("vi: real-time step on: ratio %.3f, game's frame rate %d", ratio, fps);
}

/* the game clock in 1/60 s (f_80706be0's module +0xd58), 0 before the frame step's hook has run */
double game_clock(void) {
    const uint64_t p = hos_step_probe;
    double v = 0;
    if (p) memcpy(&v, GP(p + 0xd58), 8);
    return v;
}
