/* Game patch: the movies play without the NDVE enhancement network.

   movie_module_play (f_83b7c020) gets the movie's NDVE resource as its third argument and passes it to the player; with none
   the player builds its plain renderer, which draws the decoded picture's two planes as they are (docs/direct/movie-player.md).
   The argument is cleared at the function's entry, so every movie takes that path; HOS_NDVE=1 leaves it to the game.

   The movie file is read through nandroid::NuCachedSource2, whose looper re-checks an idle cache only every 100 ms
   (ALooper::post, delay in microseconds). The host decoder takes every packet at once, so the game's input thread drains
   the cache faster than that check refills it and then waits for the next one: the movie arrives in bursts. The
   post is shortened to 1 ms, so the cache is refilled as soon as it is read. */
#include "direct.h"
#include <stdlib.h>

ENTRY_HOOK(0x83b7c020) {
    if (!getenv("HOS_NDVE")) ARG(2) = 0;
    return 0;
}

ENTRY_HOOK(0x8547efe0) {   /* nandroid::ALooper::post(this, message, delay in microseconds) */
    if ((int64_t)ARG(2) == 100000) ARG(2) = 1000;
    return 0;
}
