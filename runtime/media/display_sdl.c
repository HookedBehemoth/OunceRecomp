/* The game's window on SDL (runtime/media/hos_sdl.c: SDL3, Wayland when there is one, else X11; or SDL2). The window lives on the process's main thread, as SDL wants it (hos_main.c runs the
   game on a thread of its own); the Vulkan back end makes its surface through SDL (renderer/api/vk_api.h
   MkwVkWindow), on a thread of its own so the window keeps answering while the shaders are prepared.

   The keyboard's state, the window's focus and the screen's size are kept here for nn_hid.c (hos_key_down,
   hos_window_focused, hos_screen_size: hos_sdl.h).
   HOS_HEADLESS=1: no window. HOS_WINDOW_SIZE=<w>x<h>: its size (default 1280x720). F11: fullscreen (HOS_FULLSCREEN=1 to
   start so). Mouse: a click in the window captures it (relative motion, for nn_hid.c), Esc or leaving releases it. */
#include "hos.h"
#include "game_hooks.h"
#include "hos_sdl.h"

/* (the window's app id: a desktop entry names it as its StartupWMClass) */
#define HOS_APP_ID "io.github.NotRexed.OunceRecomp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "hos_paths.h"
#include "hos_keys.h"
#include "vk_api.h"

int rx_start(void *hwnd);
void rx_resize(int width, int height);
void rx_close_asked(void);
void hos_sdl_pads_pump(void);   /* runtime/media/pad_sdl.c: controllers, on this thread, as SDL's queue wants */

volatile int64_t hos_present_count;   /* (nvn_native.c: the game's presents) */
static volatile uint8_t keys[256];
static volatile int focused, screen_w, screen_h;
int hos_key_down(int vk) { return keys[vk & 255] != 0; }
int hos_window_focused(void) { return focused; }
void hos_screen_size(int *width, int *height) { *width = screen_w, *height = screen_h; }

static volatile int32_t mouse_dx, mouse_dy, mouse_captured;
int hos_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = os_atomic_xchg(&mouse_dx, 0);
    *dy = os_atomic_xchg(&mouse_dy, 0);
    return mouse_captured;
}

/* (mouse_native.c) motion taken and not used, put back for the next taker (the player camera's) */
void hos_mouse_give_back(int32_t dx, int32_t dy) {
    os_atomic_fetch_add(&mouse_dx, dx);
    os_atomic_fetch_add(&mouse_dy, dy);
}

/* ---- the free mouse (renderer/api/vk_api.h MkwVkPointer): set on the main thread, read on the renderer's ---- */
static volatile int32_t ptr_x, ptr_y, ptr_in, ptr_held;   /* (x, y in window units) */
static void *window;
int hos_mouse_pointer(MkwVkPointer *out) {
    memset(out, 0, sizeof *out);
    if (!window || mouse_captured) return 0;
    int ww = 0, wh = 0, pw = 0, ph = 0;
    hos_sdl_window_size(window, &ww, &wh);   /* (SDL's position is in the window's units: scaled to its pixels) */
    hos_sdl_pixel_size(window, &pw, &ph);
    if (ww <= 0 || wh <= 0 || pw <= 0 || ph <= 0) return 0;
    out->x = (int32_t)((int64_t)ptr_x * pw / ww), out->y = (int32_t)((int64_t)ptr_y * ph / wh);
    out->width = pw, out->height = ph;
    out->held = (uint32_t)ptr_held;
    return ptr_in && focused;
}
static uint32_t sdl_button_bit(int b) { return b == 1 ? HOS_MOUSE_LEFT : b == 3 ? HOS_MOUSE_RIGHT : b == 2 ? HOS_MOUSE_MIDDLE : 0; }

/* ---- keys: SDL's scancodes (USB HID usages) as Windows' virtual keys ---- */
static void keys_update(void) {
    int n = 0;
    const uint8_t *k = hos_sdl_keys(&n);
    uint8_t v[256];
    memset(v, 0, sizeof v);
    if (k && focused) {
#define K(sc) ((sc) < n && k[sc])
        for (int i = 0; i < 26; i++) v['A' + i] = K(4 + i);
        for (int i = 0; i < 9; i++) v['1' + i] = K(30 + i);
        v['0'] = K(39);
        v[VK_RETURN] = K(40) || K(88), v[VK_ESCAPE] = K(41), v[VK_BACK] = K(42), v[VK_TAB] = K(43), v[VK_SPACE] = K(44);
        v[VK_OEM_MINUS] = K(45), v[VK_OEM_PLUS] = K(46), v[VK_OEM_1] = K(51), v[VK_OEM_3] = K(53);
        v[VK_OEM_COMMA] = K(54), v[VK_OEM_PERIOD] = K(55), v[VK_OEM_2] = K(56), v[VK_CAPITAL] = K(57);
        v[VK_OEM_4] = K(47), v[VK_OEM_6] = K(48), v[VK_OEM_5] = K(49), v[VK_OEM_7] = K(52);   /* ([ ] \ ') */
        for (int i = 0; i < 12; i++) v[VK_F1 + i] = K(58 + i);
        v[VK_PAUSE] = K(72), v[VK_INSERT] = K(73), v[VK_HOME] = K(74), v[VK_PRIOR] = K(75), v[VK_DELETE] = K(76);
        v[VK_END] = K(77), v[VK_NEXT] = K(78), v[VK_RIGHT] = K(79), v[VK_LEFT] = K(80), v[VK_DOWN] = K(81), v[VK_UP] = K(82);
        v[VK_DIVIDE] = K(84), v[VK_MULTIPLY] = K(85), v[VK_SUBTRACT] = K(86), v[VK_ADD] = K(87);
        for (int i = 0; i < 9; i++) v[VK_NUMPAD0 + 1 + i] = K(89 + i);
        v[VK_NUMPAD0] = K(98), v[VK_DECIMAL] = K(99);
        v[VK_LCONTROL] = K(224), v[VK_LSHIFT] = K(225), v[VK_LMENU] = K(226);
        v[VK_RCONTROL] = K(228), v[VK_RSHIFT] = K(229), v[VK_RMENU] = K(230);
#undef K
        v[VK_CONTROL] = v[VK_LCONTROL] || v[VK_RCONTROL];
        v[VK_SHIFT] = v[VK_LSHIFT] || v[VK_RSHIFT];
        v[VK_MENU] = v[VK_LMENU] || v[VK_RMENU];
        const uint32_t b = hos_sdl_mouse_buttons();
        v[VK_LBUTTON] = (b & 1) != 0, v[VK_MBUTTON] = (b & 2) != 0, v[VK_RBUTTON] = (b & 4) != 0;
    }
    for (int i = 0; i < 256; i++) keys[i] = v[i];
}

/* ---- the window ---- */
static volatile int renderer;   /* the renderer presents into the window */
static os_event renderer_up = OS_EVENT_INIT(true);   /* (set once rx_start has returned, ready or not: hos_renderer_wait) */
static int fullscreen;

static int create_surface(void *w, void *instance, void *surface) { return hos_sdl_vulkan_surface(w, instance, surface); }
static void drawable_size(void *w, int *width, int *height) { hos_sdl_pixel_size(w, width, height); }
/* (SDL's display calls belong on this thread: the loop asks every second, the renderer reads what it found) */
static volatile int refresh_now;
static int refresh_mhz(void *w) { (void)w; return __atomic_load_n(&refresh_now, __ATOMIC_RELAXED); }
int hos_display_refresh_mhz(void) { return __atomic_load_n(&refresh_now, __ATOMIC_RELAXED); }   /* (nn_vi.c: the pacing) */
static MkwVkWindow vk_window = {MKWVK_WINDOW_VERSION, create_surface, drawable_size, NULL, refresh_mhz};

static void mouse_capture(int on) {
    if (on == (int)mouse_captured) return;
    mouse_captured = on;
    os_atomic_store(&mouse_dx, 0);
    os_atomic_store(&mouse_dy, 0);
    hos_sdl_relative_mouse(window, on);
}
static void set_fullscreen(int on) {
    if (on == fullscreen) return;
    fullscreen = on;
    hos_sdl_fullscreen(window, on);
    hos_log("display: %s", on ? "fullscreen (F11 for a window)" : "windowed");
}

static void renderer_main(void *p) {
    (void)p;
    const int ok = rx_start(&vk_window);
    if (!ok) hos_log("display: the renderer did not start: the window stays black");
    renderer = ok;
    if (ok) {   /* (a size change while it started) */
        int w = 0, h = 0;
        hos_sdl_pixel_size(window, &w, &h);
        if (w > 0 && h > 0) rx_resize(w, h);
    }
    os_event_set(&renderer_up);
}

/* The game's first work for the GPU waits for the renderer (nvn_native.c), so nothing it submits while
   the shaders are prepared is lost. Without a window the renderer starts on the game's own thread, before this is asked */
void hos_renderer_wait(void) {
    { const char *e = getenv("HOS_NO_RENDERER_WAIT"); if (e && atoi(e) != 0) return; }
    os_event_wait(&renderer_up, OS_FOREVER);
}

/* ---- the game's frame follows the window. SDL sends resize events all through a drag and has no event for its end,
   so a size applies once no resize came for 250 ms; until then the old frame is letterboxed into the window.
   HOS_DBG=resize:<w>x<h>@<present>[+<w>x<h>@<present>...] resizes as a window would, for headless tests */
static volatile uint64_t resize_size;   /* (width | height << 32 of the last resize not applied yet) */
static volatile int64_t resize_ns;
static void resize_note(int w, int h) {
    __atomic_store_n(&resize_ns, os_now_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&resize_size, (uint64_t)(uint32_t)h << 32 | (uint32_t)w, __ATOMIC_RELEASE);
}
static const char *dbg_resize;   /* (the next HOS_DBG resize, or NULL) */
static int dbg_resize_parsed;
void hos_display_presented(void) {   /* (the Presentation Thread, after each present) */
    if (!dbg_resize_parsed) {
        dbg_resize_parsed = 1;
        const char *d = getenv("HOS_DBG");
        if (d && hos_dbg_item("resize")) dbg_resize = strstr(d, "resize:") ? strstr(d, "resize:") + 7 : NULL;
    }
    int w, h;
    long long at;
    if (dbg_resize && sscanf(dbg_resize, "%dx%d@%lld", &w, &h, &at) == 3 && hos_present_count >= at) {
        const char *next = strpbrk(dbg_resize, "+,");
        dbg_resize = next && *next == '+' ? next + 1 : NULL;
        hos_log("display: HOS_DBG resize to %dx%d", w, h);
        rx_resize(w, h);
        resize_note(w, h);
    }
    const uint64_t size = __atomic_load_n(&resize_size, __ATOMIC_ACQUIRE);
    if (!size || os_now_ns() - __atomic_load_n(&resize_ns, __ATOMIC_RELAXED) < 250 * 1000000LL) return;
    if (!__atomic_compare_exchange_n(&resize_size, &(uint64_t){size}, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    game_frame_size((uint32_t)size, (uint32_t)(size >> 32), 0);
}

/* a file of the game folder (beside the executable: the builder's title.txt and icon.bmp) */
static void game_file(char *out, size_t cap, const char *name) {
    uint32_t n = hos_game_path_a(out, (uint32_t)cap);
    while (n && out[n - 1] != '/') n--;
    snprintf(out + n, cap - n, "%s", name);
}
/* the window's title: title.txt's first line, the game's name; "OunceRecomp" without one */
static void window_title(char *out, size_t cap) {
    char path[OS_PATH_MAX];
    game_file(path, sizeof path, "title.txt");
    snprintf(out, cap, "OunceRecomp");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) snprintf(out, cap, "%s", line);
    }
    fclose(f);
}

static volatile int start_asked;
static int sdl_ready;
int hos_display_owns_sdl(void) { return sdl_ready; }   /* (controllers stay on this thread while it runs SDL) */

/* main(), before the game: SDL's video on this, the main, thread; the screen's size for hos_screen_size.
   Returns 1 when ready, 0 for explicit headless, -1 for startup failure. */
int hos_display_init(void) {
    if (getenv("HOS_HEADLESS")) return 0;
    if (!hos_sdl_load()) {
        hos_log("display: no SDL: no window");
        fprintf(stderr, "display: no SDL: no window\n");
        return -1;
    }
    hos_sdl_identity("OunceRecomp", HOS_APP_ID);
    hos_sdl_hint("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "0");
    hos_sdl_hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    /* (bundled SDL tries x11 before wayland. NVIDIA's X11 surface has no mailbox, and an immediate
       present then drops vblank for every output. Wayland has mailbox. SDL_VIDEODRIVER (SDL3: SDL_VIDEO_DRIVER) still wins.
       Under gamescope (XDG_CURRENT_DESKTOP names it: Steam Deck game mode) SDL keeps its own order.) */
    const char* desktop = getenv("XDG_CURRENT_DESKTOP");
    const int gamescope = desktop && strstr(desktop, "gamescope");
    if (!getenv("SDL_VIDEODRIVER") && !getenv("SDL_VIDEO_DRIVER") && !gamescope)
        hos_sdl_hint(hos_sdl_load() == 3 ? "SDL_VIDEO_DRIVER" : "SDL_VIDEODRIVER", "wayland,x11");
    if (!hos_sdl_init(HOS_SDL_VIDEO | HOS_SDL_EVENTS)) {
        hos_log("display: SDL_Init: %s: no window", hos_sdl_error());
        fprintf(stderr, "display: SDL_Init: %s: no window\n", hos_sdl_error());
        return -1;
    }
    int w, h;
    if (hos_sdl_desktop_size(&w, &h)) screen_w = w, screen_h = h;
    sdl_ready = 1;
    return 1;
}

/* the game's thread, when NVN is up (hos_main.c): the window is made on the main thread, which waits for this */
void hos_display_start(void) {
    if (getenv("HOS_HEADLESS") || !sdl_ready) {   /* no window: the renderer still draws, offscreen (screenshots) */
        rx_start(NULL);
        os_event_set(&renderer_up);
        return;
    }
    __atomic_store_n(&start_asked, 1, __ATOMIC_RELEASE);
}

/* the main thread's loop, until the process ends: the window once asked for, then its events */
void hos_display_loop(void) {
    if (!sdl_ready) {
        for (;;) os_sleep_ns(1000LL * 1000000);
    }
    HosSdlEvent e;
    while (!__atomic_load_n(&start_asked, __ATOMIC_ACQUIRE)) hos_sdl_wait_event(&e, 10);   /* (nothing to show yet) */
    extern float hos_aspect_ratio;
    /* (the picture's shape when Monitor's shape set one, wider or narrower than 16:9: 960 x 720 for 4:3, 1720 x 720 for 21:9) */
    int win_w = hos_aspect_ratio > 0.0f ? (int)(720 * hos_aspect_ratio + 0.5f) : 1280, win_h = 720;
    if (getenv("HOS_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (sscanf(getenv("HOS_WINDOW_SIZE"), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) win_w = w, win_h = h;
    }
    char title[256];
    window_title(title, sizeof title);
    window = hos_sdl_window(title, win_w, win_h);
    if (!window) {
        hos_log("display: SDL_CreateWindow: %s: no window", hos_sdl_error());
        fprintf(stderr, "display: SDL_CreateWindow: %s: no window\n", hos_sdl_error());
        hos_log_flush();
        os_exit(1);
    }
    vk_window.window = window;
    focused = 1;
    {   /* (the window's pixels can differ from the size asked for: a HiDPI scale, a tiling compositor) */
        int w = 0, h = 0;
        hos_sdl_pixel_size(window, &w, &h);
        if (w > 0 && h > 0) resize_note(w, h);
    }
    {   /* the window's icon: the game folder's icon.bmp, for X11 and the compositors that take one from the window; the
           others find it through the menu entry */
        char bmp[OS_PATH_MAX];
        game_file(bmp, sizeof bmp, "icon.bmp");
        if (os_path_kind(bmp) != OS_PATH_NONE) hos_sdl_icon(window, bmp);
    }
    if (getenv("HOS_FULLSCREEN") && atoi(getenv("HOS_FULLSCREEN"))) set_fullscreen(1);
    __atomic_store_n(&refresh_now, hos_sdl_refresh_mhz(window), __ATOMIC_RELAXED);
    os_thread_release(os_thread_start(renderer_main, NULL, 0, NULL));
    uint32_t refresh_asked = (uint32_t)(os_now_ns() / 1000000);
    for (;;) {
        hos_sdl_pads_pump();
        if ((uint32_t)(os_now_ns() / 1000000) - refresh_asked >= 1000) {   /* (the window may have moved to another display) */
            refresh_asked = (uint32_t)(os_now_ns() / 1000000);
            __atomic_store_n(&refresh_now, hos_sdl_refresh_mhz(window), __ATOMIC_RELAXED);
        }
        if (hos_sdl_wait_event(&e, 4)) {
            do {
                switch (e.type) {
                case HOS_SDL_EV_QUIT:
                case HOS_SDL_EV_CLOSE:
                    rx_close_asked();   /* (a start still preparing the shaders keeps what it made) */
                    hos_log("display: window closed, exiting");
                    hos_log_flush();
                    os_exit(0);
                case HOS_SDL_EV_RESIZED:
                    if (renderer) {
                        int w = 0, h = 0;
                        hos_sdl_pixel_size(window, &w, &h);
                        rx_resize(w, h);
                        resize_note(w, h);
                    }
                    break;
                case HOS_SDL_EV_FOCUS_GAINED:
                    focused = 1;
                    break;
                case HOS_SDL_EV_FOCUS_LOST:
                    focused = 0;
                    mouse_capture(0);
                    ptr_in = 0, ptr_held = 0;
                    break;
                case HOS_SDL_EV_MOUSE_LEAVE:
                    /* (Wayland: turning the relative mode on sends a leave in the same millisecond (Mutter), and
                       releasing here dropped the capture before the first motion: the mouse turned nothing and its
                       buttons did nothing. The capture goes with the window's focus, not with the pointer.) */
                    ptr_in = 0;
                    break;
                case HOS_SDL_EV_MOUSE_ENTER:
                    ptr_in = 1;
                    break;
                case HOS_SDL_EV_KEY_DOWN: {
                    const int scancode = e.scancode, mod = e.mod;
                    if (e.scancode == 41) mouse_capture(0);   /* Esc */
                    if (!e.repeat && ((scancode == 68 && !(mod & 0x0FC3)) || (scancode == 40 && (mod & 0x300)))) set_fullscreen(!fullscreen);   /* F11, Alt+Enter */
                    break;
                }
                case HOS_SDL_EV_MOUSE_DOWN:
                    ptr_x = (int32_t)e.x, ptr_y = (int32_t)e.y, ptr_in = 1;
                    os_atomic_fetch_or(&ptr_held, (int32_t)sdl_button_bit(e.button));
                    if (renderer && !mouse_captured) mouse_capture(1);
                    break;
                case HOS_SDL_EV_MOUSE_UP:
                    ptr_x = (int32_t)e.x, ptr_y = (int32_t)e.y;
                    os_atomic_fetch_and(&ptr_held, ~(int32_t)sdl_button_bit(e.button));
                    break;
                case HOS_SDL_EV_MOUSE_MOTION:
                    if (!mouse_captured) ptr_x = (int32_t)e.x, ptr_y = (int32_t)e.y, ptr_in = 1;
                    if (mouse_captured) {   /* (SDL3's motion comes in fractions of a pixel: the remainders kept for the next) */
                        static float rx, ry;
                        rx += e.dx, ry += e.dy;
                        const int32_t dx = (int32_t)rx, dy = (int32_t)ry;
                        rx -= (float)dx, ry -= (float)dy;
                        os_atomic_fetch_add(&mouse_dx, dx);
                        os_atomic_fetch_add(&mouse_dy, dy);
                    }
                    break;
                }
            } while (hos_sdl_poll_event(&e));
        }
        keys_update();
    }
}
