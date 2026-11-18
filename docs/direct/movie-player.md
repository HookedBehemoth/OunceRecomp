# The movie player

This page explains how the game plays its movies (the title and the intro): how the file reaches the decoder, how frames
are chosen, and how they are drawn. Read it when a movie stutters, jumps or shows wrong colors, or before you change
`runtime/nn_video.c` or `games/oot/patches/movie.c`. The player's functions carry the prefixes `movie_` and `ndve_` in
`games/oot/labels.txt`.

The movie files are AV1, video only. Their sound is a separate stream file that the sound system plays.

## How the file reaches the decoder

1. A reader thread (`MovieMediaSourceClient`) reads the file from the romfs in 64 KB chunks into the SDK's media
   source, which caches it (high water 5 MB, low water 2.5 MB) and refills the cache every 100 ms.
2. The movie's input thread reads samples and submits them to the video decoder. A full decoder answers "busy"; the
   thread keeps the sample and waits for the decoder's input-ready event.

The pace of reading is therefore the decoder's. The host's decoder (`runtime/nn_video.c`) refuses input while it holds as
many submissions as the hardware's queue would, and signals the event when it has room again. If it took everything
at once, the reader would drain the cache between two refills and the movie's first seconds would jump.

## How frames are chosen

Each game update, the player adds the game's frame time to an accumulator and shows `n = floor(acc / step)` frames,
where `step` is 60 / the movie's frame rate. It pops `n` decoded frames and shows the last one. So:

- the clock is the game's frame time, not the sound and not the wall clock;
- late frames are dropped, and nothing waits when the decoder is early;
- at a steady 60 fps one frame is shown per update.

The host drops no picture to a wall clock. The shown frame's two planes (NV12) go to the renderer's plane textures
(`movie_renderer_copy_frame`).

## How frames are drawn

With an NDVE resource, the game builds a renderer that runs NDVE, an enhancement network, into an `NDVE_Output` target.
Without one, it builds a plain renderer that draws the two planes, converting BT.709 video range to RGB in the movie
layer.

`games/oot/patches/movie.c` passes no NDVE resource, so every movie takes the plain path and shows the picture as decoded.
`HOS_NDVE=1` leaves the resource to the game, for a comparison.
