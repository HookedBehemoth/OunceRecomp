#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vk_api.h"
#include "os.h"
#include "hos_keys.h"
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
void os_once_run(os_once *o, void (*fn)(void *), void *arg) { if (!o->state) { o->state = 3; fn(arg); } }
static void hos_log(const char *f, ...) { (void)f; }
static const char *ini_dir;
static uint32_t hos_game_path_a(char *b, uint32_t n) { return (uint32_t)snprintf(b, n, "%s/game", ini_dir); }
static int held[256];
static int key(int vk) { return held[vk & 255]; }
@CODE@
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void release(void) { memset(held, 0, sizeof held); }
int main(int argc, char **argv) {
    (void)argc;
    ini_dir = argv[1];
    binds_load();
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_MOVE_UP, 0) == 'Z');   /* from the file */
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_MOVE_LEFT, 0) == 'A');   /* the default */
    CHECK(hos_binding(HOS_BIND_PAD, HOS_KEY_MOVE_UP, 0) == -1);   /* no gamepad input for a keys-only control */
    CHECK(hos_binding(HOS_BIND_KEY, HOS_SW_Y, 0) == VK_LBUTTON && hos_binding(HOS_BIND_KEY, HOS_SW_ZL, 1) == VK_RBUTTON);
    release(), held['Z'] = 1;
    CHECK(bound_down(HOS_KEY_MOVE_UP, 0));
    release(), held[VK_LEFT] = 1;
    CHECK(bound_down(HOS_KEY_CAM_LEFT, 0));
    /* the mouse's left button: held when the capture begins, it counts only after it was let go */
    release(), held[VK_LBUTTON] = 1;
    mouse_buttons_arm(1);
    CHECK(!bound_down(HOS_SW_Y, 1));
    release(), mouse_buttons_arm(1), held[VK_LBUTTON] = 1, mouse_buttons_arm(1);
    CHECK(bound_down(HOS_SW_Y, 1));
    CHECK(!bound_down(HOS_SW_Y, 0));   /* not while the mouse is not captured */
    mouse_buttons_arm(0);
    CHECK(!bound_down(HOS_SW_Y, 1));   /* a new capture arms again */
    /* a key bound to a keys-only control is taken from the button that had it */
    hos_bind(HOS_BIND_KEY, HOS_KEY_CAM_UP, 0, 'E');
    CHECK(hos_binding(HOS_BIND_KEY, HOS_SW_A, 0) == 0 && hos_binding(HOS_BIND_KEY, HOS_KEY_CAM_UP, 0) == 'E');
    hos_bind(HOS_BIND_PAD, HOS_KEY_CAM_UP, 0, HOS_PAD_SOUTH);   /* no gamepad input for it: ignored */
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_B, 0) == HOS_PAD_SOUTH);
    /* Enter with Alt is fullscreen, not Plus */
    release(), held[VK_RETURN] = held[VK_MENU] = 1;
    CHECK(!bound_down(HOS_SW_PLUS, 0));
    hos_bind(HOS_BIND_KEY, -1, 0, 0);
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_MOVE_UP, 0) == 'W' && hos_binding(HOS_BIND_KEY, HOS_SW_A, 0) == 'E');
    if (!fails) printf("PASS\n");
    return fails != 0;
}
