/* Game patch: DKB's frame takes the window's shape (or aspect's) at any ratio, and the 3D camera, the HUD
   layouts and the vignette take the frame's aspect, while the game runs.

   The frame: the game renders a frame inside its picture. Its display settings (f_8032f840, each frame) hold a frame
   size at +0x1a0 (width | height << 32). When the docked / handheld mode changes, or +8 bit 4 asks for it, they crop
   the window to that size (f_80330618, nvnWindowSetCrop in sys_framework_wait_present), set the 3D's scale against the
   mode's size (+0x1a8, +0x1ac) and the render config's frame size (+0x780). display_output_width / height make the
   picture (the window textures and display buffers, made once at boot) 3840 x 2160, and a row at the settings' update
   writes the window's shape to +0x1a0 and sets bit 4 after each settled resize (game_frame_size, from
   runtime/media/display_sdl.c). The frame keeps the game's 16:9 pixel budget on its short side (1620 high when wider,
   2880 wide when taller) inside the picture, so no shape gets bars.

   The aspect: nothing in the game follows the frame's shape. The 3D camera (an al camera: near +0xf8, far +0xfc,
   aspect +0x100, vertical fov in degrees from its poser) fills its sead::PerspectiveProjection each frame in
   f_804fb588 with the aspect fixed at 16:9; the scene's projection copies it (f_8033f294 -> f_8033f570). The HUD's
   nn::ui2d layouts draw through an ortho projection of the layout's size (f_800d1000, 1920 x 1080), stretched over the
   frame. The vignette's shader gets 16:9 as its aspect (f_8109fc64). Rows at each of those give them the frame's
   aspect: the camera Hor+ when wider than 16:9 (the same vertical fov), Vert+ when taller (the same horizontal fov, as
   the game's 16:9 view keeps everything a 16:9 screen shows); 16:9 layouts widen (or grow taller) about their centre,
   so the HUD keeps its 16:9 placement in the middle of the frame. */
#include "patches.h"
#include <math.h>
#include <string.h>

extern volatile float hos_wide_ratio;   /* (runtime/hos_main.c: aspect's picture ratio, 0 the window's) */

enum { PICTURE_W = 3840, PICTURE_H = 2160, BASE_W = 2880, BASE_H = 1620 };
#define BASE_ASPECT (16.0f / 9.0f)
static volatile uint64_t frame_size;   /* (width | height << 32 of the frame to set; 0 none yet) */
static volatile int frame_pending;      /* (1: the settings take frame_size at their next update) */
static volatile float frame_aspect;     /* (the frame's width / height the settings took; 0 before) */
static volatile int frame_gen;          /* (counts the frames the settings took: the projection check's) */

static uint32_t round4(double v) { const uint32_t r = (uint32_t)(v / 4 + 0.5) * 4; return r < 64 ? 64 : r; }

/* the frame for a shape: 1620 high when wider than 16:9, 2880 wide when taller, then inside the picture; whole multiples of 4 (1620 is one) */
static uint64_t frame_for(double r) {
    double w, h;
    if (r >= BASE_ASPECT) h = BASE_H, w = h * r;
    else w = BASE_W, h = w / r;
    if (w > PICTURE_W) w = PICTURE_W, h = w / r;
    if (h > PICTURE_H) h = PICTURE_H, w = h * r;
    return (uint64_t)round4(h) << 32 | round4(w);
}

/* the window's size at boot (start 1) and after a settled resize; aspect's shape stays over it */
void game_frame_size(uint32_t w, uint32_t h, int start) {
    if (!start && (w < 64 || h < 64)) return;
    const double r = hos_wide_ratio > 0.0f ? hos_wide_ratio : (double)w / h;
    const uint64_t f = frame_for(r);
    if (!start && f == frame_size) return;
    frame_size = f;
    __atomic_store_n(&frame_pending, 1, __ATOMIC_RELEASE);
    hos_log("frame: window %ux%u: frame %ux%u", w, h, (uint32_t)f, (uint32_t)(f >> 32));
}

/* the picture: 3840 x 2160 */
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
    if (!frame_size) game_frame_size(16, 9, 1);   /* (no window shape given: aspect's, else 16:9) */
    if (!__atomic_exchange_n(&frame_pending, 0, __ATOMIC_ACQ_REL)) return;
    const uint64_t f = frame_size;
    *(uint64_t *)A64_PTR(settings + 0x1a0) = f;
    io->w11 |= 0x10;
    frame_aspect = (float)(uint32_t)f / (float)(uint32_t)(f >> 32);
    frame_gen++;
    hos_log("frame: the display settings take %ux%u (aspect %.4f)", (uint32_t)f, (uint32_t)(f >> 32), (double)frame_aspect);
}

static float q_f(hos_q q) { float f; memcpy(&f, &q, 4); return f; }
static hos_q f_q(float f) { hos_q q = {0, 0}; memcpy(&q, &f, 4); return q; }

/* a camera's vertical fov (degrees) for the frame against the camera's own aspect: the same when the frame is wider
   (Hor+), widened to keep the horizontal fov when taller (Vert+) */
static float fit_fovy(float fovy, float base) {
    const float a = frame_aspect;
    if (a <= 0.0f || base <= 0.0f || a >= base) return fovy;
    return atanf(tanf(fovy * (float)(M_PI / 360.0)) * base / a) * (float)(360.0 / M_PI);
}

/* the camera's projection, each frame (f_804fb588): s0 its vertical fov in degrees, s12 the camera's aspect (+0x100,
   16:9) just loaded */
PATCH_AT(0x804fb8ac, word(0x2d5fb26b), after, none, dkb_camera_aspect, io(q0, q12)) {   /* ldp s11, s12, [x19, #0xfc] */
    if (frame_aspect <= 0.0f) return;
    io->q0 = f_q(fit_fovy(q_f(io->q0), q_f(io->q12)));
    io->q12 = f_q(frame_aspect);
}
/* the same in cam_update_projection (f_8047e204, after a near or far change; camera in x19): the fov just loaded,
   then the aspect */
PATCH_AT(0x8047e238, word(0xbd410660), after, none, dkb_camera_fovy_set, as(x19, cam), io(q0)) {   /* ldr s0, [x19, #0x104] */
    io->q0 = f_q(fit_fovy(q_f(io->q0), *(float *)A64_PTR(cam + 0x100)));
}
PATCH_AT(0x8047e24c, word(0x2d5fb26b), after, none, dkb_camera_aspect_set, io(q12)) {   /* ldp s11, s12, [x19, #0xfc] */
    if (frame_aspect > 0.0f) io->q12 = f_q(frame_aspect);
}

/* the check: the scene projection's matrix just made (f_8033f570, projection in x19), its x / y scale against the
   frame's 1 / aspect, once a frame size (its 30th update, past the camera's own) */
PATCH_AT(0x8033f6c0, word(0x3cc5c260), after, none, dkb_projection_check, as(x19, proj)) {   /* ldur q0, [x19, #0x5c] */
    static int gen = -1, calls;
    if (gen != frame_gen) gen = frame_gen, calls = 0;
    if (++calls != 30 || frame_aspect <= 0.0f) return;
    const float *m = (const float *)A64_PTR(proj + 0xc);
    hos_log("widescreen: projection %#llx: x/y scale %.4f, 1/aspect %.4f (frame %ux%u, fovy %.2f deg)",
            (unsigned long long)proj, (double)(m[0] / m[5]), 1.0 / frame_aspect, (uint32_t)frame_size,
            (uint32_t)(frame_size >> 32), (double)(*(float *)A64_PTR(proj + 0xa0) * (float)(180.0 / M_PI)));
}

/* a layout's ortho projection (f_800d1000: s0, s1 the layout's width and height): a 16:9 layout takes the frame's
   aspect about its centre, wider or taller */
PATCH_AT(0x800d1000, word(0xd503237f), after, none, dkb_layout_aspect, io(q0, q1)) {   /* pacibsp */
    const float a = frame_aspect, w = q_f(io->q0), h = q_f(io->q1);
    if (a <= 0.0f || h <= 0.0f || fabsf(w / h - BASE_ASPECT) > 0.01f) return;
    if (a > BASE_ASPECT) io->q0 = f_q(h * a);
    else io->q1 = f_q(w / a);
}

/* the vignette (f_8109fc64): its parameters' 16:9, a scale (s0, from 0x81ca9e54) and the aspect (w8, mov / movk) */
static float vignette_aspect(void) { return frame_aspect > 0.0f ? frame_aspect : BASE_ASPECT; }
PATCH_AT(0x8109fdb0, word(0xbd4e5500), after, s0, dkb_vignette_scale) {   /* ldr s0, [x8, #0xe54] */
    return vignette_aspect();
}
PATCH_AT(0x8109fdd4, word(0x72a7fc68), instead, w8, dkb_vignette_aspect) {   /* movk w8, #0x3fe3, lsl #16 */
    const float a = vignette_aspect();
    uint32_t u;
    memcpy(&u, &a, 4);
    return u;
}
