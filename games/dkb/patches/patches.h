/* What DKB's patch files share: their calls of each other, over the runtime's renderer.h and its hook points
   (game_hooks.h). */
#pragma once
#include "renderer.h"
#include "game_hooks.h"

/* passes.c : the job recorder's entry in direct mode (eng_jobs.c): 1 = take the game's early return */
int passes_recorder(uint64_t key);
