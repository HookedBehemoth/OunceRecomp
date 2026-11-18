/* The direct back end's game patches (games/oot/patches/passes.c, job_sync.c, rt_ids.c,
   depth_copy.c, mesh_args.c, mesh_binds.c, mesh_system.c, mesh_lists.c, sky_lut.c, layer_ahead.c): one log line each every 600 presents with HOS_DBG=logpatch, called from the present in direct mode only. */
#include "patches.h"

void direct_passes_log(double frames);
void direct_job_sync_log(double frames);
void direct_rt_ids_log(double frames);
void direct_depth_copy_log(double frames);
void direct_mesh_args_log(double frames);
void direct_mesh_binds_log(double frames);
void direct_mesh_system_log(double frames);
void direct_mesh_lists_log(double frames);
void direct_sky_lut_log(double frames);
void direct_layer_ahead_log(double frames);

void direct_patches_frame(void) {
    static long long presents;
    if (++presents % 600) return;
    static int on = -1;
    if (on < 0) on = hos_log_switch("logpatch");
    if (!on) return;
    direct_passes_log(600);
    direct_job_sync_log(600);
    direct_rt_ids_log(600);
    direct_depth_copy_log(600);
    direct_mesh_args_log(600);
    direct_mesh_binds_log(600);
    direct_mesh_system_log(600);
    void hos_oot_hud_log(void);   /* (oot_probes.c) */
    hos_oot_hud_log();
    direct_mesh_lists_log(600);
    direct_sky_lut_log(600);
    direct_layer_ahead_log(600);
}
