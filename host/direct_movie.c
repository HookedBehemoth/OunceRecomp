/* Game patch: the movies play without the NDVE enhancement network.

   movie_module_play (f_83b7c020) gets the movie's NDVE resource as its third argument and passes it to the player; with none
   the player builds its plain renderer, which draws the decoded picture's two planes as they are (docs/direct/movie-player.md).
   The argument is cleared at the function's entry, so every movie takes that path; HOS_NDVE=1 leaves it to the game. */
#include "direct.h"
#include <stdlib.h>

ENTRY_HOOK(0x83b7c020) {
    if (!getenv("HOS_NDVE")) ARG(2) = 0;
    return 0;
}
