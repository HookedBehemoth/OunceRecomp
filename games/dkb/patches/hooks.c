/* DKB's side of the runtime's hook points (runtime/game_hooks.h) that are not one patch file's own, and the patches'
   log lines: one each every 600 presents with HOS_DBG=logpatch, direct mode only. */
#include "patches.h"

void passes_log(double frames);

static void patches_frame(void) {
    static long long presents;
    if (++presents % 600) return;
    static int on = -1;
    if (on < 0) on = hos_log_switch("logpatch");
    if (!on) return;
    passes_log(600);
}

void game_frame(void) {
    if (rx_direct()) patches_frame();
}
