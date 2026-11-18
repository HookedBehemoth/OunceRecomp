/* What the game's patch files share: their calls of each other, over the runtime's direct.h and its hook points
   (game_hooks.h, which hooks.c fills). */
#pragma once
#include "direct.h"
#include "game_hooks.h"

/* direct_assets.c : the host's asset patches are on (direct mode, and not HOS_DBG=noassets) */
int direct_assets_on(void);
/* direct_assets.c : the game's block-linear storage bytes for a texture (n bytes, its whole storage), level 0 made
   packed rows and uploaded to its image, timed into st. 0: n is not the storage's size or the layout is not one to
   deswizzle (nothing uploaded) */
typedef struct { volatile int64_t uploads, bytes, ns, max_ns; } direct_bl_stats;
int direct_assets_texels_bl(uint32_t source, const void *tex, const uint8_t *src, uint64_t n, direct_bl_stats *st);   /* (an nvn_tex) */
/* nvn_native.c nvnTextureInitialize: packaged texture data the back end never reads, its copy into the storage skipped
   (1) in direct mode */
int game_skips_packaged_texels(uint64_t bytes);
/* direct_tex_dedup.c (M6f, game patch): a load with no data, td its agl::TextureData: when it is a UnifyTex alias, its
   target's NVN texture as an HSD_ASSET_SAME key (1); otherwise 0 */
int direct_tex_alias(uint64_t td, hsd_asset_key *k);
/* direct_texdata.c : agl::TextureDataMgr's texture events */
void direct_texdata_created(uint64_t td, int alias);
const void *game_texture_moving(void);   /* (this thread's texture being moved by compaction, else NULL) */
void direct_texdata_line(char *out, size_t cap);
/* nvnMemoryPoolInitialize (lr its return address): agl::TextureDataMgr's pool is named to the back end */
void game_pool_made(uint64_t gpu, uint64_t size);
/* nvn_native.c nvnTextureInitialize, after the back end made its image: terrain tiles given for its storage before it */
extern volatile long direct_assets_tiles_pending;
extern volatile long direct_assets_tilelog;   /* (M6f: HOS_DBG=tilelog, a diagnosis) */
void direct_assets_texture_init(const void *nvn_texture);   /* (an nvn_tex) */
/* direct_passes.c : the job recorder's entry in direct mode (eng_jobs.c): 1 = take the game's early return */
int direct_passes_recorder(uint64_t key);
/* direct_patches.c : the game patches' counters, every 600 presents (nvn_native.c's present, direct mode) */
void direct_patches_frame(void);
