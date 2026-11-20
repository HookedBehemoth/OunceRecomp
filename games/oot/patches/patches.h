/* What the game's patch files share: their calls of each other, over the runtime's renderer.h and its hook points
   (game_hooks.h, which hooks.c fills). */
#pragma once
#include "renderer.h"
#include "game_hooks.h"

/* assets.c : the host's asset patches are on (direct mode, and not debug.noassets) */
int assets_on(void);
#include "nvn_texels.h"
/* tex_dedup.c (M6f, game patch): a load with no data, td its agl::TextureData: when it is a UnifyTex alias, its
   target's NVN texture as an GPU_ASSET_SAME key (1); otherwise 0 */
int tex_alias(uint64_t td, gpu_asset_key *k);
/* texdata.c : agl::TextureDataMgr's texture events */
void texdata_created(uint64_t td, int alias);
const void *game_texture_moving(void);   /* (this thread's texture being moved by compaction, else NULL) */
void texdata_line(char *out, size_t cap);
/* nvnMemoryPoolInitialize (lr its return address): agl::TextureDataMgr's pool is named to the back end */
/* nvn_native.c nvnTextureInitialize, after the back end made its image: terrain tiles given for its storage before it */
extern volatile long assets_tiles_pending;
extern volatile long assets_tilelog;   /* (M6f: debug.tilelog, a diagnosis) */
void assets_texture_init(const void *nvn_texture);   /* (an nvn_tex) */
/* passes.c : the job recorder's entry in direct mode (eng_jobs.c): 1 = take the game's early return */
int passes_recorder(uint64_t key);
/* patch_log.c : the game patches' counters, every 600 presents (nvn_native.c's present, direct mode) */
void patches_frame(void);
/* graphics.c : the local lights' shadow resolution scale (local_shadow_resolution: 0.5, 1, 2 or 4), for rt_slots.c's sizes */
float oot_local_shadow_scale(void);
