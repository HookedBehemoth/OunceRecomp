/* The neutral defaults of game_hooks.h: what the runtime does when no patch takes part. */
#include "game_hooks.h"

#define WEAK __attribute__((weak))
WEAK void game_texture_initializing(uint64_t obj, uint64_t gpu, const void *desc, uint64_t lr) { (void)obj, (void)gpu, (void)desc, (void)lr; }
WEAK void game_texture_initialized(const void *nvn_texture) { (void)nvn_texture; }
WEAK const void *game_texture_moving(void) { return NULL; }
WEAK int game_job_list(uint64_t cmd, uint64_t *key, uint64_t *view) { (void)cmd, (void)key, (void)view; return 0; }
WEAK int game_invalidate_unneeded(uint64_t size) { (void)size; return 0; }
WEAK void game_frame(void) {}
WEAK void game_frame_timing(int64_t now) { (void)now; }
WEAK double game_clock(void) { return 0; }
WEAK uint64_t game_memory_extra(void) { return 0; }
WEAK void game_frame_size(uint32_t width, uint32_t height, int start) { (void)width, (void)height, (void)start; }
WEAK void game_bench_report(void) {}
WEAK int game_textures_settled(void) { return 1; }
WEAK int game_mouse_camera(void) { return -1; }
WEAK int game_prompts_face(void) { return -1; }
WEAK int game_romfs_file(const char *key, char *out, size_t cap) { (void)key, (void)out, (void)cap; return 0; }
