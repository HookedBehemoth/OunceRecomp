# The game's movie player (guest code, 0x83b5e5c0 and 0x83b7bff0..0x83b83513)

Read from the lift with `work/ghidra/decomp.sh` (title movie, intro). The movie files are AV1, video only; their sound is a
separate `Sound/Resource/Stream/Dm_*.bwav` the sound system plays. Labels are in `games/oot/labels.txt` (`movie_`, `ndve_`).

## Who calls whom
- `f_83b5e5c0` (event movie component start): takes the movie's path from the component and an NDVE resource (the component's
  +0x228 resource, found by `f_801b1520`; null when there is none), then `f_83b7c020`.
- `f_83b7c020` (`movie_module_play`): makes the player (`f_83b7c940` allocates 0x468 bytes, `f_83b7cec0` constructs it), hands it
  the NDVE resource with `f_83b7e4c0` (stored at player+0x460) and starts it with `f_83b7cd90` (`f_83b7cbd0` opens the file).
- A message handler (`f_83b7d200`, the player's thread) takes the commands 8 (open: `nn::movie::Initialize`, decoder and renderer
  set-up, start), 9 (close) and 10 (restart: `MediaExtractor::SeekTo(0)`, flush, start). Player state is the flag word at
  player+0x450 (bit 0 playing, 6 open requested, 7 close requested, 8 restart requested, 9-11 a request in progress).
- Each game update `f_83b7bff0` (`movie_module_update`) reads the game's frame time in seconds (`[0x84eb4b20]+0x18`) times 60 and
  calls `f_83b7ca60` (`movie_frame_chooser`).

## The frame chooser (measured from the decompile, not yet from a run)
`f_83b7ca60(delta, module)`: `step = 60 / fps` (fps = player+0x324, 60 for these movies); `acc += delta`; `n = floor(acc / step)`
(at least 1 before the first frame); `n` is capped by player+0x20; `acc -= step * n`; if `n > 0` it calls `f_83b7e180(player, n)`
and adds `n` to a played counter. So the clock is the game's own frame time, not the sound and not the wall clock; frames
that are late are dropped (`f_83b81ea0` pops `n` decoded frames from the queue at +0x198 and shows the last), nothing waits when
early. A movie with an audio track uses `f_83b7e350` instead: `n` from the audio position minus the frame's time stamp (only
when the player has an audio object at +0x2a8; these movies have none). At a steady 16.7 ms `delta` is about 1.0 and `n` is 1
per update (a rounding error in `delta` can make it 0 then 2).
`f_83b81ea0` also copies the shown frame's NV12 planes (`movie::VideoFrame::GetBuffer`) with `memcpy` into the double-buffered
plane buffers (index at +0xb8) the renderer reads, and keeps the time stamp at +0xc0.

## The renderer and the NDVE switch
`f_83b7f500` makes the renderer: when the player's NDVE resource (+0x460) is non-null and has a size (+0x10), a 0x2f0-byte
renderer (`f_83b82cd0`) and the NDVE object (`f_821056c0`, only if the network's input size equals the movie's; else the global
byte at 0x84eb5f61 is set to 1 and NDVE is off); with no resource a 0x1c0-byte plain renderer (`f_83b819e0`). Its draw method
(vtable +0x18): `f_83b83360` when NDVE is on (allocates "NDVE_Output", runs `f_821057e0`), `f_83b82450` otherwise (draws the two
plane textures). Both are reached from `f_83b82250` (the plane copy). So "enhancement off" is: the NDVE resource argument of
`f_83b7c020` is null.
The plain draw, `f_83559360`: program 0 or 2 of a shader archive the game holds (the draw's last argument, bit 0, picks 2),
bound by `f_802a1760` once its ready bit (+0x20 bit 0) is set; colour constants by colour space (`f_835592b0`, 1..4) in
uniform blocks 0x17 and 0x18 (`f_809f11e0`); the UV plane texture, then the Y plane with the full-screen draw (`f_809f0850`).
It draws the picture as decoded: the shown frame is the source's, its colours the source's converted from BT.709 video range, in the
movie layer. `HOS_NDVE=1` (host/direct_movie.c) leaves the network to the game, for a comparison.

## How the movie file reaches the player
`MovieMediaSourceClient` (f_83b827e0's thread) reads the file from romfs in 64 KB chunks and appends them to the SDK's media source
(`nn::movie::MediaExtractor` in the sdk module, which wraps it in `nandroid::NuCachedSource2`, the Android cache with its own
`nandroid::ALooper` thread, high water 5 MB, low water 2.5 MB). `MediaExtractorWorker` reads samples through `readAt`, the movie input
thread submits them to `movie::VideoDecoder`. When a read needs bytes the cache does not hold, `onRead` retries every 1 ms and
the cache is refilled by `onFetch`, which when it is not fetching re-posts itself 100 ms out (the looper's timed wait is that 100 ms;
nothing is lost).

The pace of that reading is the decoder's. The movie input thread (`f_83b7e600`, its event handler: 1 a sample can be read into
one of its two input buffers, 2 the decoder takes input, 3 decoded frames can be acquired) submits with
`movie::VideoDecoder::DecodeAsync`, which clears the decoder's submission-ready event (`GetInputReadyEvent` hands out the
nn::codec one) and calls `SubmitXDecoderInputBuffer`. A full decoder answers 0x1c266f; `movie::detail::SubmitDecoderInputBuffer`
(0x84ecfd20) turns that into 0x7a, the handler keeps the sample and waits for the event; any other failure aborts. So a decoder
that takes everything at once lets the reader run through the whole file in the movie's first seconds, emptying the cache
between two of its 100 ms checks: the frames then come in bursts of six or seven until the file is read, and the movie's first
seconds jump. host/nn_video.c refuses input while it holds `MAX_IN_FLIGHT` submissions and pictures, as the hardware's queue
does, and signals the event when it has room again (an acquire, a flush); the reader then keeps the movie's pace and the cache
stays filled. A buffer acquired before its picture is decoded (the library acquires two before its first submission) gets a
fence that the next picture decoded into it reaches, as the hardware's does. The movie's clock is the game's frame time
alone: the host drops no picture to a wall clock.
