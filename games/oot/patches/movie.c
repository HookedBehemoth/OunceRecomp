/* Game patch: the movies play without the NDVE enhancement network.

   movie_module_play (f_83b7c020) gets the movie's NDVE resource as its third argument and passes it to the player; with none
   the player builds its plain renderer, which draws the decoded picture's two planes as they are (docs/direct/movie-player.md).
   The game's function (orig) runs without the argument, so every movie takes that path; HOS_NDVE=1 leaves it to the game. */
#include "patches.h"
#include <stdlib.h>

PATCH_FUNC(0x83b7c020, word(0x3940a008), body(0xa1ed7c22e2e7ada9), aapcs, x0, movie_play,
           orig, x0, x1, as(x2, ndve)) {
    static int keep = -1;
    if (keep < 0) keep = getenv("HOS_NDVE") != NULL;
    return guest_orig(orig, x0, x1, keep ? ndve : 0);   /* (x2 changed: x0-x2 passed) */
}
