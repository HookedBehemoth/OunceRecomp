/* DKB's side of the runtime's hook points (runtime/game_hooks.h) that are not one patch file's own, and the patches'
   log lines every 600 presents, direct mode only: the assets' always, the others' with debug.logpatch. */
#include "patches.h"

void passes_log(double frames);

static void patches_frame(void) {
    static long long presents;
    if (++presents % 600) return;
    char b[512];
    assets_log();
    texdata_vram_line(b, sizeof b);
    hos_log("assets: %s", b);
    static int on = -1;
    if (on < 0) on = hos_log_switch(SET_DBG_LOGPATCH);
    if (!on) return;
    passes_log(600);
}

void game_frame(void) {
    if (rx_direct()) patches_frame();
}
