/* Game patch: DKB's picture takes the window's shape (or HOS_ASPECT's) at the game's height, 1620.

   display_output_width / display_output_height return the output size, 2880 x 1620, to the framework's create arg
   (sys_framework_create: the window textures and the display buffers), the root's heaps (sys_root_init) and the
   layouts' captures. Wider than 16:9 they return 1620 high at the screen's shape, at most 3840 x 2160's pixels, so
   the game's frame, its render targets and the cameras that take their ratio from them are that shape. The shape is
   the window's at boot (runtime/hos_main.c game_frame_size) or HOS_ASPECT's (hos_wide_ratio). */
#include "patches.h"
#include <math.h>

extern volatile float hos_wide_ratio;   /* (runtime/hos_main.c: HOS_ASPECT's picture ratio, 0 the game's) */
static volatile float window_ratio;     /* (game_frame_size: the window's at boot) */

/* the window's size at boot (start 1) and after a settled resize: its shape, taken at boot only (the frame's size is
   fixed when the framework is made) */
void game_frame_size(uint32_t w, uint32_t h, int start) {
    if (!start || w < 64 || h < 64) return;
    window_ratio = (float)w / (float)h;
    hos_log("frame: window %ux%u, picture ratio %.4f", w, h, (double)window_ratio);
}

/* the picture's size: 0 width, 1 height */
static uint32_t picture(int height) {
    const double game = 16.0 / 9.0;
    double r = hos_wide_ratio > 0.0f ? hos_wide_ratio : window_ratio;
    if (!(r > game * 1.01)) return height ? 1620 : 2880;   /* (16:9 or narrower: the game's own) */
    if (r > 32.0 / 9.0) r = 32.0 / 9.0;
    uint32_t h = 1620, w = (uint32_t)(h * r / 8 + 0.5) * 8;
    if ((double)w * h > 3840.0 * 2160.0) {   /* (at most 3840 x 2160's pixels, the shape kept) */
        h = (uint32_t)(sqrt(3840.0 * 2160.0 / r) / 8) * 8;
        w = (uint32_t)(h * r / 8) * 8;
    }
    return height ? h : w;
}

PATCH_FUNC(0x805df770, word(0x52816800), body(0x360c8b9d6c0d5331), aapcs, x0, dkb_output_width) {   /* mov w0, #2880 */
    static int logged;
    if (!logged++) hos_log("widescreen: picture %ux%u", picture(0), picture(1));
    return picture(0);
}
PATCH_FUNC(0x805df780, word(0x5280ca80), body(0xd70f1578520f618b), aapcs, x0, dkb_output_height) {   /* mov w0, #1620 */
    return picture(1);
}
