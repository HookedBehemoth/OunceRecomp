/* Game patch: DKB's frame rate follows the pacing past 60 fps through the game's own variable-rate path.

   The framework's per-frame virtual (sys_framework_proc_frame) runs one of two frame-rate controllers at [fw+0x218]:
   the fixed one (whole display intervals, fps = 60 / interval) or, while the display reports a variable refresh
   (fw+0x1ca, from QueryDisplayVrrSupported each frame in sys_framework_frame_body), sys_fps_controller_vrr. That one
   takes the rate the frame's work time allows, clamped to [ctrl+0x20, ctrl+0x24], to ctrl+0x50; f_80224934 makes it
   the limiter's rate (fw+0xe0) and asks the layer for it (SetLayerExtendedSwapInterval). The game steps by
   60 / [ctrl+0x50] everywhere it reads the rate (sys_frame_global_step and the scene steps), so its time follows the
   frames. The runtime reports a variable refresh while the cap is above 60 (runtime/nn_vi.c); the one row here raises
   the controller's top rate, 60 as shipped, to the rate the runtime paces at (the cap, or the window's display when
   it is slower). */
#include "patches.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int hos_pace_rate(void);   /* (runtime/nn_vi.c) */

/* the VRR controller's fields that the row reads and sets */
typedef struct __attribute__((may_alias)) {
    uint8_t _0[0x20];
    int32_t least;   /* +0x20: the clamp's least rate (32) */
    int32_t most;    /* +0x24: its most (60) */
    int32_t step;    /* +0x28: a raise's step (2) */
    uint8_t _1[0x24];
    int32_t rate;    /* +0x50: the rate the game steps by (60 / rate) */
    int32_t probe;   /* +0x54: the next rate a raise tries */
} DkbFpsController;
_Static_assert(offsetof(DkbFpsController, most) == 0x24 && offsetof(DkbFpsController, rate) == 0x50 &&
               offsetof(DkbFpsController, probe) == 0x54, "controller");

static volatile uint64_t controller;   /* (the VRR controller last seen, for the log and the game clock) */

/* sys_fps_controller_vrr: its clamp's bounds just loaded (w13 the least rate, w9 the most) */
PATCH_AT(0x80224698, word(0x2944266d), after, w9, dkb_fps_top, as(w9, top), as(x19, ctrl)) {   /* ldp w13, w9, [x19, #0x20] */
    controller = ctrl;
    const int rate = hos_pace_rate();
    DkbFpsController *k = (DkbFpsController *)A64_PTR(ctrl);
    if (rate > 60 && k->most != rate) {
        /* (the bound as a setting: the controller's raise path reads it again from memory after its listeners. Its probe
           target, the rate it tries next, is the current rate as shipped (60 = 60): one step up, as after a drop) */
        k->most = rate;
        if (k->probe <= k->rate) k->probe = k->rate + k->step < rate ? k->rate + k->step : rate;
        hos_log("fps: the frame-rate controller's top rate %d (its rate %d, next %d)", rate, k->rate, k->probe);
    }
    return rate > (int)top ? (uint32_t)rate : top;
}

/* the game's clock: the global step (g_82514b30, 60 / the controller's rate, written by sys_frame_global_step each
   frame) summed at each present, in 1/60 s */
static double clock_60ths;
double game_clock(void) { return clock_60ths; }

/* HOS_FPS_LOG=1: the controller's rate and bounds and the game's speed (game time over wall time), every 300 presents */
void game_frame_timing(int64_t now) {
    float step;
    memcpy(&step, GP(0x82514b30), 4);
    if (step > 0.0f && step < 8.0f) clock_60ths += step;
    static int log_on = -1;
    static long long n;
    static double clock_before;
    static int64_t at_before;
    if (log_on < 0) log_on = getenv("HOS_FPS_LOG") != NULL;
    if (!log_on || ++n % 300) return;
    const uint64_t c = controller;
    if (at_before && now > at_before) {
        const double wall = (double)(now - at_before) / 1e9;
        hos_log("fps: %.1f fps, controller %d (bounds %d-%d), step %.3f, game speed %.3f, game clock %.1f s", 300.0 / wall,
                c ? *(int32_t *)GP(c + 0x50) : 0, c ? *(int32_t *)GP(c + 0x20) : 0, c ? *(int32_t *)GP(c + 0x24) : 0,
                (double)step, (clock_60ths - clock_before) / 60.0 / wall, clock_60ths / 60.0);
    }
    clock_before = clock_60ths, at_before = now;
}
