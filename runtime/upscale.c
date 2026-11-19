/* The upscaler (upscale.h): the choice, and the host's upscale as the back end's op (gpu_upscale). */
#include "upscale.h"
#include "hos.h"
#include "renderer.h"

static volatile int choice = HOS_UPSCALER_GAME;   /* (the compiled default; the settings store sets it) */

static const char *const names[HOS_UPSCALER_KINDS] = {"game", "bilinear", "catmull-rom", "fsr3", "xess", "dlss"};
const char *hos_upscaler_name(hos_upscaler kind) { return (unsigned)kind < HOS_UPSCALER_KINDS ? names[kind] : "?"; }

int hos_upscaler_set(hos_upscaler kind) {
    if (kind != HOS_UPSCALER_GAME && kind != HOS_UPSCALER_BILINEAR && kind != HOS_UPSCALER_CATMULL_ROM) return 0;
    if (__atomic_exchange_n(&choice, (int)kind, __ATOMIC_RELAXED) != (int)kind) hos_log("upscale: %s", names[kind]);
    return 1;
}
hos_upscaler hos_upscaler_get(void) { return (hos_upscaler)__atomic_load_n(&choice, __ATOMIC_RELAXED); }

int hos_upscaler_allows(hos_upscaler kind, float scale) {
    return kind == HOS_UPSCALER_BILINEAR || kind == HOS_UPSCALER_CATMULL_ROM || (kind == HOS_UPSCALER_GAME && scale <= 1.0f);
}

void hos_upscale_record(const hos_upscale *u) {
    const hos_upscaler k = hos_upscaler_get();
    const gpu_upscale_args a = {k == HOS_UPSCALER_CATMULL_ROM ? GPU_UPSCALE_CATMULL_ROM : GPU_UPSCALE_BILINEAR,
                                u->src_id, u->src_w, u->src_h, u->dst_id, u->dst_w, u->dst_h, u->sharpness};
    static int told;
    if (!told) {
        told = 1;
        hos_log("upscale: %s, %ux%u (texture %u) to %ux%u (image %u), the game's parameter %.3f", names[k], u->src_w, u->src_h,
                u->src_id, u->dst_w, u->dst_h, u->dst_id, u->sharpness);
    }
    rx_direct()->upscale(cmd_list(u->cmd), &a);
}
