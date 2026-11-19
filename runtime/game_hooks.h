/* The points where a game's patches take part in the runtime's own work. Each has a neutral default in game_hooks.c
   (weak); a game's patch file that defines one replaces the default at the link. The runtime holds no game address or
   game behaviour: what a game needs at these points is in games/<game>/patches. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* ---- textures (nvn_native.c) ---- */
/* a memory pool made at gpu (the patches may know which of the game's pools it is) */
void game_pool_made(uint64_t gpu, uint64_t size);
/* 1: a texture's packaged data of this size is not copied into its storage (the back end has its texels) */
int game_skips_packaged_texels(uint64_t bytes);
/* a texture initialized: obj its guest object, gpu its storage's address, desc its nvn_texdesc, lr the caller's
   return address; before the runtime's own record of it */
void game_texture_initializing(uint64_t obj, uint64_t gpu, const void *desc, uint64_t lr);
/* the same texture after the runtime made its record (an nvn_tex) */
void game_texture_initialized(const void *nvn_texture);
/* the texture (an nvn_tex) this thread is moving for the game, whose Finalize and Initialize are one move; NULL */
const void *game_texture_moving(void);

/* ---- command lists and draws (nvn_native.c, render_lists.c) ---- */
/* a recording that was named a job's list: 1 and its pass key and view record when the game's job recorder runs it
   on this thread; 0 */
int game_job_list(uint64_t cmd, uint64_t *key, uint64_t *view);
/* an indirect mesh draw of a GPU-written list at gpu (max records of stride bytes), before it is recorded */
void game_indirect_mesh_draw(uint64_t gpu, int max, int stride);
/* 1: a mapped-range invalidate of size bytes on this thread needs no wait (the game reads data it can do without) */
int game_invalidate_unneeded(uint64_t size);

/* ---- the frame and the window ---- */
void game_frame(void);                                        /* each present (nvn_native.c) */
/* each present (nn_vi.c), now the present's time in ns: the patches' frame timing (time step, limiter) */
void game_frame_timing(int64_t now);
/* the game's clock in 1/60 s of game time (a benchmark's game step, a speed check); 0 when not known */
double game_clock(void);
void game_frame_size(uint32_t width, uint32_t height, int start);   /* the window's size: at boot (start 1) and after a resize */
void game_bench_report(void);                                 /* the end of a benchmark run (bench.c): the patches' lines */
int game_textures_settled(void);                              /* 1: the game streams no textures now (a benchmark's ready test) */

/* the bytes of memory the game gets beyond the console's, at boot (the render targets' pool its patches grew) */
uint64_t game_memory_extra(void);

/* ---- input ---- */
/* the mouse turning the game's camera itself: 1 captured, 0 not; -1: no such camera now (the mouse is the right stick) */
int game_mouse_camera(void);
/* the button-prompt face the game shows (HOS_PROMPT_*), -1 none (the renderer's menu) */
int game_prompts_face(void);

/* ---- files ---- */
/* 1 and the host file of a RomFS path ("UI/x.zs", lower case) the patches serve, where no mod has it; 0 */
int game_romfs_file(const char *key, char *out, size_t cap);
