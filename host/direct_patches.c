/* (native-vk M3a) The direct back end's game patches (host/direct_passes.c, direct_job_sync.c, direct_rt_ids.c,
   direct_depth_copy.c, direct_mesh_args.c, direct_mesh_binds.c, direct_mesh_system.c): one log line each every 600 presents, called from the present in direct mode only. */
#include "direct.h"

void direct_passes_log(double frames);
void direct_job_sync_log(double frames);
void direct_rt_ids_log(double frames);
void direct_depth_copy_log(double frames);
void direct_mesh_args_log(double frames);
void direct_mesh_binds_log(double frames);
void direct_mesh_system_log(double frames);

void direct_patches_frame(void) {
    static long long presents;
    direct_texwatch_frame();
    if (++presents % 600) return;
    direct_passes_log(600);
    direct_job_sync_log(600);
    direct_rt_ids_log(600);
    direct_depth_copy_log(600);
    direct_mesh_args_log(600);
    direct_mesh_binds_log(600);
    direct_mesh_system_log(600);
}
