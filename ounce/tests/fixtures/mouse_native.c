#include "hos.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t *fake_heap;
uint64_t hos_heap_size = 64 * 4096;
double hos_mouse_sens = 1.0;
static int64_t clock_ns = 1000000000LL;
void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc(10, stderr); }
int64_t hos_now_ns(void) { return clock_ns; }
static int32_t move_x, move_y;
static int captured = 1, menu_invert, menu_native = 1;
int rx_option(int id, int otherwise) { return id == 8 ? menu_invert : id == 6 ? menu_native : otherwise; }
int hos_mouse_take(int32_t *dx, int32_t *dy) { *dx = move_x, *dy = move_y; move_x = move_y = 0; return captured; }
void hos_mouse_give_back(int32_t dx, int32_t dy) { move_x += dx, move_y += dy; }
int game_mouse_camera(void);
void hos_mouse_camera(uint64_t camera);
static const uint64_t CAM = HOS_HEAP_BASE + 10 * 4096 + 0x40, OTHER = HOS_HEAP_BASE + 20 * 4096;
static float getf(uint64_t a) { float f; memcpy(&f, GP(a), 4); return f; }
static void putf(uint64_t a, float f) { memcpy(GP(a), &f, 4); }
static int fails;
#define PITCH (CAM + 0x18c)
#define YAW (CAM + 0x1a4)
static void state(uint32_t v) { memcpy(GP(CAM + 0x204), &v, 4); }
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (pitch %g yaw %g)\n", __LINE__, #c, getf(PITCH), getf(YAW)); fails++; } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }
static void frame(int32_t dx, int32_t dy) { move_x += dx, move_y += dy; clock_ns += 16666667; hos_mouse_camera(CAM); }
int main(void) {
    fake_heap = (uint8_t *)calloc(64, 4096);
    putf(CAM + 0x178, -85), putf(CAM + 0x17c, 89);
    putf(PITCH, 10), putf(YAW, 30);
    state(4);   /* turned by the player */
    move_x = 5, move_y = 3;
    CHECK(game_mouse_camera() == -1);   /* no camera has run yet: the stick takes it */
    move_x = move_y = 0;
    frame(10, 4);
    CHECK(near(getf(YAW), 29.0f) && near(getf(PITCH), 10.4f));   /* right: yaw falls; down: pitch rises */
    move_x = 3;
    CHECK(game_mouse_camera() == 1 && move_x == 3);   /* the camera's now: the motion stays for it */
    frame(-3, 0);
    frame(0, 100000);
    CHECK(near(getf(PITCH), 89.0f));   /* kept within the limits */
    frame(0, -100000);
    CHECK(near(getf(PITCH), -85.0f));
    putf(YAW, -179.5f);
    frame(10, 0);
    CHECK(near(getf(YAW), 179.5f));   /* wrapped */
    putf(PITCH, 0);
    menu_invert = 1;   /* the menu's Invert mouse vertical: down tilts up */
    frame(0, 10);
    CHECK(near(getf(PITCH), -1.0f));
    menu_invert = 0;
    putf(OTHER + 0x18c, NAN);   /* (no limits, no angles: not a camera) */
    move_x = 50;
    hos_mouse_camera(OTHER);
    CHECK(move_x == 50 && isnan(getf(OTHER + 0x18c)));
    move_x = 0;
    menu_native = 0;   /* the menu's Mouse camera off: the hook does nothing */
    putf(YAW, 0);
    frame(10, 0);
    CHECK(near(getf(YAW), 0.0f));
    move_x = 0;
    menu_native = 1;
    frame(0, 0);
    captured = 0;
    CHECK(game_mouse_camera() == 0);   /* not captured: nothing */
    captured = 1;
    clock_ns += 200000000LL;   /* no camera for 200 ms (the pause menu): the stick again */
    CHECK(game_mouse_camera() == -1);
    /* aiming in first person (the slingshot): the camera keeps updating, state 0, another draws: the stick takes it */
    state(4);
    frame(0, 0);   /* the camera's a moment ago (the hook ran: game_mouse_camera would give 1 for 150 ms) */
    state(0);
    putf(YAW, 0);
    frame(10, 0);
    CHECK(near(getf(YAW), 0.0f) && move_x == 10);
    CHECK(game_mouse_camera() == -1);   /* at once, not 150 ms later */
    move_x = 0;
    state(3);   /* following Link again: the mouse turns it */
    frame(10, 0);
    CHECK(near(getf(YAW), -1.0f));
    CHECK(game_mouse_camera() == 1);
    if (!fails) printf("PASS\n");
    return fails != 0;
}
