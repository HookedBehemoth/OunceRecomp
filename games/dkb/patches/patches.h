/* What DKB's patch files share: their calls of each other, over the runtime's renderer.h and its hook points
   (game_hooks.h). */
#pragma once
#include "renderer.h"
#include "game_hooks.h"

/* passes.c : the job recorder's entry in direct mode (eng_jobs.c): 1 = take the game's early return */
int passes_recorder(uint64_t key);
/* assets.c : the host's asset patches are on (direct mode, and not HOS_DBG=noassets); their line */
int assets_on(void);
void assets_log(void);
/* texdata.c : agl::TextureDataMgr's texture events */
void texdata_created(uint64_t td);
void texdata_line(char *out, size_t cap);
void texdata_vram_line(char *out, size_t cap);
