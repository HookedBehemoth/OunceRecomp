/* Game patch: DKB's frame takes the window's shape (or HOS_ASPECT's), and follows the window while the game runs.

   The game already renders a frame smaller than its picture: its display settings (f_8032f840, each frame) hold a
   frame size at +0x1a0 (width | height << 32). When the docked / handheld mode changes, or +8 bit 4 asks for it, they
   crop the window to that size (f_80330618: window +0xac, dirty +0xaa, applied by nvnWindowSetCrop in
   sys_framework_wait_present), set the 3D's scale against the mode's size (+0x1a8, +0x1ac) and the render config's
   frame size (+0x780). The cameras and nn::ui2d layouts take their shape from that frame.

   Two patches use it: display_output_width / display_output_height make the picture (the framework's window textures
   and display buffers, made once at boot) 3840 x 1620, as wide as 21:9 at the game's height; and a row at the
   settings' first load writes the window's shape fitted into the picture to +0x1a0 and sets bit 4 whenever the
   window's size changed (game_frame_size: at boot and after a settled resize, runtime/media/display_sdl.c). 16:9
   is 2880 x 1620, the game's own frame. */
#include "patches.h"
#include <math.h>

extern volatile float hos_wide_ratio;   /* (runtime/hos_main.c: HOS_ASPECT's picture ratio, 0 the window's) */

enum { PICTURE_W = 3840, PICTURE_H = 1620 };
static volatile uint64_t frame_size;   /* (width | height << 32 of the frame to set; 0 none yet) */
static volatile int frame_pending;      /* (1: the settings take frame_size at their next update) */

/* the frame for a shape: the largest of that shape inside the picture, whole multiples of 8 */
static uint64_t frame_for(double r) {
    if (r < 5.0 / 4.0) r = 5.0 / 4.0;
    if (r > 32.0 / 9.0) r = 32.0 / 9.0;
    uint32_t h = PICTURE_H, w = (uint32_t)(h * r / 8 + 0.5) * 8;
    if (w > PICTURE_W) w = PICTURE_W, h = (uint32_t)(w / r / 8 + 0.5) * 8;
    return (uint64_t)h << 32 | w;
}

/* the window's size at boot (start 1) and after a settled resize; HOS_ASPECT's shape stays over it */
void game_frame_size(uint32_t w, uint32_t h, int start) {
    if (!start && (w < 64 || h < 64)) return;
    const double r = hos_wide_ratio > 0.0f ? hos_wide_ratio : (double)w / h;
    const uint64_t f = frame_for(r);
    if (!start && f == frame_size) return;
    frame_size = f;
    __atomic_store_n(&frame_pending, 1, __ATOMIC_RELEASE);
    hos_log("frame: window %ux%u: frame %ux%u", w, h, (uint32_t)f, (uint32_t)(f >> 32));
}

/* the picture: 3840 x 1620 */
PATCH_FUNC(0x805df770, word(0x52816800), body(0x360c8b9d6c0d5331), aapcs, x0, dkb_output_width) {   /* mov w0, #2880 */
    static int logged;
    if (!logged++) hos_log("widescreen: picture %ux%u", PICTURE_W, PICTURE_H);
    return PICTURE_W;
}
PATCH_FUNC(0x805df780, word(0x5280ca80), body(0xd70f1578520f618b), aapcs, x0, dkb_output_height) {   /* mov w0, #1620 */
    return PICTURE_H;
}

/* the display settings' update (f_8032f840, settings in x0), its flags just loaded (w11 = +8): a new frame size to
   +0x1a0 and bit 4 set, which the update writes back and acts on in this same call */
PATCH_AT(0x8032f85c, word(0x2941200b), after, none, dkb_frame_resize, as(x0, settings), io(w11)) {   /* ldp w11, w8, [x0, #8] */
    if (!frame_size) game_frame_size(16, 9, 1);   /* (no window shape given: HOS_ASPECT's, else 16:9) */
    if (!__atomic_exchange_n(&frame_pending, 0, __ATOMIC_ACQ_REL)) return;
    *(uint64_t *)A64_PTR(settings + 0x1a0) = frame_size;
    io->w11 |= 0x10;
    hos_log("frame: the display settings take %ux%u", (uint32_t)frame_size, (uint32_t)(frame_size >> 32));
}
