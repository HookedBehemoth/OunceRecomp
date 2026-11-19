/* The settings of the runtime, the renderer and the game patches: one table of typed keys (runtime/settings.c is the store,
   its one reader and writer; docs/switches.md says how they are given).

   A user setting is kept in the options file (ounce_options.ini beside the game, or the file --options names) and the game's
   Options screen may show it; a change from there is written back. A dev setting is given only on the command line
   (--set key=value) and never kept. --set also sets a user setting for one run, without writing it.

   Each row: B (bool), I (int), F (float), E (enum: the choice's index, written by name, "a|b|c"), S (string); its id, its key,
   USER or DEV, its default, its range (I, F) or choices (E), and what it does. `<exe> --list-settings` prints the table. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OUNCE_SETTINGS(B, I, F, E, S)                                                                                          \
    /* ---- the picture and the frame rate */                                                                                  \
    E(FRAME_RATE, "frame_rate", USER, 1, "30|60|120|vsync", "frame rate cap; vsync: the display's refresh rate, presented in step with it") \
    E(RENDER_SCALE, "render_scale", USER, 6, "50%|58%|67%|75%|80%|83%|100%|150%|200%", "the 3D's resolution against the output's; above the boot value it applies at the next start") \
    E(UPSCALER, "upscaler", USER, 0, "game|bilinear|catmull-rom", "what takes the 3D to the output: the game's network (only up to 100%), or a host filter") \
    E(SHADOW_RESOLUTION, "shadow_resolution", USER, 1, "50%|100%|200%|400%", "the sun's and moon's shadow resolution against the game's; above the boot value it applies at the next start (the render-target pool grows with it)") \
    E(LOCAL_SHADOW_RESOLUTION, "local_shadow_resolution", USER, 1, "50%|100%|200%|400%", "the shadow resolution of lamps, torches and other lights against the game's (at most 4096); above the boot value it applies at the next start") \
    E(ANISOTROPIC_FILTERING, "anisotropic_filtering", USER, 0, "game|2x|4x|8x|16x", "the least anisotropic filtering of the textures (sharper surfaces seen at an angle)") \
    E(TEXTURE_LOD_BIAS, "texture_lod_bias", USER, 2, "-1|-0.5|0|+0.5|+1", "added to the textures' mip level: below 0 sharper (and may shimmer), above 0 softer") \
    B(BLOOM, "bloom", USER, 1, "the glow around bright lights")                                                               \
    B(DEPTH_OF_FIELD, "depth_of_field", USER, 1, "the blur of the distance and of what the camera does not focus on")          \
    E(FIELD_OF_VIEW, "field_of_view", USER, 2, "80%|90%|100%|110%|120%|130%|140%", "the camera's view against the game's (the width of its picture)") \
    E(OUTPUT_RES, "output_res", USER, 0, "2048x1152|2560x1440|3840x2160|1920x1080|5120x1440", "the game's output size (docked); applies at the next start") \
    B(HANDHELD, "handheld_profile", USER, 0, "the handheld profile: 1080p output and the game's handheld resolutions") \
    B(FULLSCREEN, "fullscreen", USER, 0, "start in fullscreen")                                                                \
    S(WINDOW_SIZE, "window_size", DEV, "", "<w>x<h>: the window's (or the headless image's) size")                             \
    F(ASPECT, "aspect", DEV, 0, 0, 10, "the picture's width over its height (0: the window's)")                                \
    /* ---- the game */                                                                                                       \
    E(LANGUAGE, "language", USER, 0, "en-US|en-GB|fr|fr-CA|de|it|es|es-419|pt-BR|nl|pl|ru|ja|ko|zh-Hans|zh-Hant", "the game's language; applies at the next start") \
    E(TEXT_SPEED, "text_speed", USER, 0, "normal|2x|4x|instant", "how fast the game's texts appear")                          \
    E(BUTTON_PROMPTS, "button_prompts", USER, 0, "auto|switch|xbox|ps4|ps5|steamdeck", "the button icons; applies at the next start") \
    /* ---- sound */                                                                                                          \
    E(AUDIO_OUTPUT, "audio_output", USER, 0, "auto|stereo|5.1", "the speakers the game mixes for; applies at the next start")  \
    B(AUDIO, "audio", DEV, 1, "sound on (headless runs: off)")                                                                \
    /* ---- the mouse, the pads, motion */                                                                                    \
    F(MOUSE_SENSITIVITY, "mouse_sensitivity", USER, 1, 0.05, 20, "the mouse camera's sensitivity (- and = change it)")         \
    B(MOUSE_INVERT_Y, "mouse_invert_y", USER, 0, "the mouse's up and down swapped")                                           \
    E(MOUSE_CAMERA, "mouse_camera", USER, 0, "stick|native", "the mouse as the right stick, or turning the camera itself")      \
    B(PADS_IN_BACKGROUND, "pads_in_background", USER, 0, "controllers count while another window is in front")                \
    I(STICK_DEAD_ZONE, "stick_dead_zone", USER, 0, 0, 5, "0: XInput's (~24%), 1 to 5: 5, 10, 15, 20, 30%")                    \
    B(STICK_INVERT_X, "stick_invert_x", USER, 0, "the right stick's left and right swapped")                                  \
    B(STICK_INVERT_Y, "stick_invert_y", USER, 0, "the right stick's up and down swapped")                                     \
    I(RUMBLE, "rumble", USER, 100, 0, 100, "rumble strength in percent")                                                      \
    B(HD_RUMBLE, "hd_rumble", USER, 1, "HD rumble on Switch controllers")                                                     \
    B(MOTION, "motion", USER, 1, "motion controls from a controller's gyroscope")                                             \
    I(MOTION_SENS_X, "motion_sensitivity_x", USER, 100, 10, 400, "motion's horizontal sensitivity in percent")                \
    I(MOTION_SENS_Y, "motion_sensitivity_y", USER, 100, 10, 400, "motion's vertical sensitivity in percent")                  \
    B(MOTION_INVERT_X, "motion_invert_x", USER, 0, "motion's horizontal turned around")                                       \
    B(MOTION_INVERT_Y, "motion_invert_y", USER, 0, "motion's vertical turned around")                                         \
    I(MOTION_DEAD_ZONE, "motion_dead_zone", USER, 1, 0, 2, "motion's drift threshold: tight, standard, loose")                 \
    OUNCE_SETTINGS_CONTROLS(I, S)                                                                                             \
    /* ---- runs, tests and input scripts */                                                                                  \
    B(HEADLESS, "headless", DEV, 0, "no window: the frames drawn offscreen")                                                  \
    S(SHOTS, "shots", DEV, "", "a folder: screenshots of the presented frames")                                               \
    I(SHOT_EVERY, "shot_every", DEV, 300, 1, 1000000000, "a screenshot every this many presents")                             \
    S(BENCH, "bench", DEV, "", "<frames>,<settled>[,...]: a benchmark (runtime/bench.c)")                                     \
    S(INPUT_SCRIPT, "input.script", DEV, "", "presses timed from the first controller read: <s>:<button>[+...][/<ms>],...")   \
    S(INPUT_LIVE, "input.live", DEV, "", "a file of presses a test appends to, read every 30 ms")                              \
    B(INPUT_EXCLUSIVE, "input.exclusive", DEV, 0, "the script alone drives the game until 5 s after its last press")          \
    E(INPUT_FRAME_CLOCK, "input.frame_clock", DEV, 0, "off|presents|game", "the script's time: the wall clock, 60 presents a second, or the game clock") \
    B(INPUT_SCRIPT_BACKGROUND, "input.script_background", DEV, 0, "scripted presses count with the window behind another")    \
    S(INPUT_RECORD, "input.record", DEV, "", "a folder: the session's input and the saves it started from")                    \
    S(INPUT_REPLAY, "input.replay", DEV, "", "an input.rec to replay")                                                        \
    E(INPUT_REPLAY_KEY, "input.replay_key", DEV, 0, "read|present|ms", "what a replay's lines are matched by")              \
    S(FS_OVERLAY, "fs.overlay", DEV, "", "a folder whose files stand in for the romfs's")                                     \
    S(MODS, "mods", DEV, "", "the mods folder (default: mods beside the game)")                                               \
    S(PREPARED, "prepared", DEV, "", "the texture store (default: prepared beside the game)")                                 \
    I(FIXED_TIME, "fixed_time", DEV, 0, 0, INT64_MAX, "POSIX seconds: the clock starts there")                                \
    I(MEMORY_MB, "memory_mb", DEV, 0, 0, 65536, "the application's memory in MB (0: the console's)")                         \
    I(RT_POOL_MB, "rt_pool_mb", DEV, 0, 0, 65536, "the render targets' GPU pool in MB (0: scaled with the output)")           \
    I(RT_SLOTS_MB, "rt_slots_mb", DEV, 0, 0, 65536, "OoT: the render-target slots' arenas in MB (0: the game's)")              \
    I(RT_PARK_FRAMES, "rt.park_frames", DEV, 6, 0, 1000, "OoT: frames a freed render-target slot stays parked")               \
    I(RT_PARK_FILL, "rt.park_fill", DEV, 40, 0, 100, "OoT: the arena's fill in percent above which parked slots go")          \
    I(RT_QUANT, "rt.quant", DEV, 1, 0, 4096, "OoT: local shadow maps' size classes (0: the game's sizes, more: multiples of it)") \
    B(RT_SLOTS_LOG, "rt.log", DEV, 0, "OoT: the render-target slots, every 600 presents")                                     \
    B(RT_CHECK, "rt.check", DEV, 0, "OoT: render-target slots checked for overlaps")                                          \
    B(OCCLUSION_OFF, "occlusion_off", DEV, 0, "OoT: the game makes no occlusion queries")                                    \
    B(SCENE_NATIVE, "scene_native", DEV, 0, "OoT: mode 1's scene base scale 1.0 instead of 0.8")                              \
    B(FIX_RES, "fix_res", DEV, 0, "OoT: a cached 3D scale that differs from the one asked for is recomputed")                 \
    /* ---- threads and timing */                                                                                             \
    S(PIN, "threads.pin", DEV, "", "<thread name>=<cpu>,...: threads pinned")                                                 \
    S(PRIO, "threads.prio", DEV, "", "<thread name>=<-2..2>,...: thread priorities")                                          \
    I(SPIN_US, "threads.spin_us", DEV, 5, 0, 100000, "microseconds a contended mutex spins before it sleeps") \
    I(NVN_REC_SHIFT, "nvn.rec_shift", DEV, 16, 12, 24, "the recorded copies' bucket size, as a power of two")                  \
    S(FS_DELAY, "fs.delay_ms", DEV, "", "<ms>[,<after s>]: every file read takes that much longer")                           \
    I(STATS_AFTER, "stats_after", DEV, 0, 0, 100000, "seconds: the process's statistics logged then")                         \
    /* ---- logs and traces (a line each when on) */                                                                          \
    B(TRACE, "trace", DEV, 0, "trace the host calls, the TLS and the threads")                                               \
    B(LOG_TIME, "log.time", DEV, 1, "the log's lines carry the time")                                                         \
    B(LOG_FPS, "log.fps", DEV, 0, "the frame limiter, the rate and the game speed, every 300 presents")                       \
    B(LOG_RES, "log.res", DEV, 0, "OoT: the 3D's scale, every 300 presents")                                                  \
    B(LOG_FP, "log.fp", DEV, 0, "the guest FPCR and the host MXCSR at presents 1, 60 and 600")                                \
    B(LOG_SLEEP, "log.sleep", DEV, 0, "sleeps and their overshoot")                                                           \
    B(LOG_TICK, "log.tick", DEV, 0, "the guest tick's reads")                                                                 \
    B(LOG_TLS, "log.tls", DEV, 0, "TLS slot use")                                                                             \
    B(LOG_MIC, "log.mic", DEV, 0, "the microphone's streams")                                                                 \
    B(LOG_MOTION, "log.motion", DEV, 0, "the gyroscope's readings")                                                           \
    B(LOG_VIDEO, "log.video", DEV, 0, "video submissions and acquisitions")                                                   \
    B(LOG_AUDIO, "log.audio", DEV, 0, "the audio graph's setup calls")                                                        \
    B(LOG_AUDIO_STATS, "log.audio_stats", DEV, 0, "once a second, what plays and where the signal is")                        \
    S(AUDIO_DUMP, "audio.dump", DEV, "", "a file: the mixed output as WAV")                                                   \
    S(AUDIO_WB_DUMP, "audio.wb_dump", DEV, "", "a folder: the wave buffers as they are submitted")                            \
    B(AUDIO_NO_REVERB, "audio.no_reverb", DEV, 0, "the reverb effect left out")                                               \
    B(AUDIO_IMMEDIATE, "audio.immediate", DEV, 0, "commands run as they are submitted")                                       \
    B(LOG_FS, "log.fs", DEV, 0, "every file opened")                                                                          \
    S(FS_WATCH, "fs.watch", DEV, "", "<lo hex>:<hi hex>: reads into that guest range, with the file")                         \
    B(LOG_MUTEX_WAKE, "log.wake_latency", DEV, 0, "the latency of each wake")                                                 \
    B(LOG_WAITS, "log.waits", DEV, 0, "every wait")                                                                           \
    B(LOG_LONG_WAITS, "log.long_waits", DEV, 0, "an untimed wait still blocked after 5 s")                                    \
    B(LOG_QUEUES, "log.queues", DEV, 0, "message queue sends and receives")                                                   \
    S(MUTEX_WATCH, "mutex.watch", DEV, "", "<hex>: that mutex's locks and unlocks")                                           \
    I(MUTEX_TRACE_MS, "mutex.trace_ms", DEV, 0, 0, 100000, "mutexes held longer than this")                                   \
    S(WAIT_WHO, "wait.who", DEV, "", "a thread name: who wakes its waits")                                                    \
    S(WAIT_WHO_LR, "wait.who_lr", DEV, "", "<hex>: only waits from that caller")                                              \
    B(LOG_SYNC, "log.sync", DEV, 0, "fences and sync point waits (nvdrv)")                                                    \
    B(LOG_MAPS, "log.maps", DEV, 0, "every GPU mapping change (nvdrv)")                                                       \
    B(HOSTCALL_STATS, "hostcall.stats", DEV, 0, "at exit, how often each host call ran")                                      \
    B(HOSTCALL_SLOW, "hostcall.slow", DEV, 0, "host calls that take long")                                                    \
    B(KEEP_FREED_PAGES, "mem.keep_freed", DEV, 0, "freed guest pages are kept, not released")                                 \
    B(ZERO_ALL_PAGES, "mem.zero_all", DEV, 0, "every page given to the guest zeroed")                                         \
    S(PRESENT_TRACE, "present_trace", DEV, "", "a CSV file: each present's times")                                            \
    B(NO_PACING, "no_pacing", DEV, 0, "frames presented as fast as they come")                                                \
    B(NO_SDL, "no_sdl", DEV, 0, "no SDL controllers")                                                                         \
    I(SDL_VERSION, "sdl_version", DEV, 3, 2, 3, "the SDL to load (3, or 2 when 3 is not there)")                                     \
    /* ---- the NVN layer's diagnostics */                                                                                    \
    B(NVN_LOG_OBJECTS, "nvn.log_objects", DEV, 0, "every NVN object made")                                                    \
    B(NVN_LOG_DEVINFO, "nvn.log_devinfo", DEV, 0, "the device's queries")                                                    \
    S(NVN_LOG_VMAP, "nvn.log_vmap", DEV, "", "virtual mappings (a value: from that size)")                                    \
    B(NVN_LOG_POOLS, "nvn.log_pools", DEV, 0, "who makes each memory pool")                                                   \
    B(NVN_LOG_SIZES, "nvn.log_sizes", DEV, 0, "texture and buffer sizes asked for")                                           \
    B(NVN_LOG_CHMASK, "nvn.log_chmask", DEV, 0, "channel mask changes")                                                       \
    S(NVN_SYNC_LOG, "nvn.sync_log", DEV, "", "syncs, from that present on")                                                   \
    S(NVN_LOG_TEX, "nvn.log_tex", DEV, "", "<w>x<h>: textures of that size")                                                  \
    S(NVN_WATCH_TEX_SIZE, "nvn.watch_tex_size", DEV, "", "<w>x<h>: CPU writes into textures of that size")                    \
    S(NVN_WATCH_TEX_AT, "nvn.watch_tex_at", DEV, "", "<hex>: only the texture at that address")                               \
    I(NVN_WATCH_AFTER, "nvn.watch_after", DEV, 0, 0, 100000, "seconds: only writes after that")                               \
    B(NVN_WATCH_ALL, "nvn.watch_all", DEV, 0, "the Resource Memory thread's writes too")                                      \
    S(NVN_WATCH_TEXELS_SRC, "nvn.watch_texels", DEV, "", "<w>x<h>: uploads into textures of that size")                       \
    B(NVN_LOG_TEXELS, "nvn.log_texels", DEV, 0, "every texel upload")                                                         \
    S(NVN_WATCH_RT, "nvn.watch_rt", DEV, "", "<hex>: draws into that render target")                                          \
    S(NVN_WATCH_TEX, "nvn.watch_tex", DEV, "", "<hex>: draws that sample that texture")                                       \
    S(NVN_WATCH_DRAW, "nvn.watch_draw", DEV, "", "<count>[,<after s>]: that many draws logged")                               \
    S(NVN_WATCH_DRAW_THREAD, "nvn.watch_draw_thread", DEV, "", "only draws recorded on threads of that name")                 \
    B(NVN_LOG_B2T, "nvn.log_b2t", DEV, 0, "who records the big buffer-to-texture copies")                                     \
    S(SHADER_DUMP, "nvn.shader_dump", DEV, "", "a folder: the game's shaders as they are set")                                \
    /* ---- debug aids (renderer and patches) */                                                                              \
    B(DBG_LOGALL, "debug.logall", DEV, 0, "every debug log below")                                                            \
    B(DBG_LOGMESH, "debug.logmesh", DEV, 0, "the renderer's mesh draws")                                                      \
    B(DBG_LOGPASS, "debug.logpass", DEV, 0, "the renderer's passes")                                                          \
    B(DBG_LOGDRAW, "debug.logdraw", DEV, 0, "the renderer's draws")                                                           \
    B(DBG_LOGLISTS, "debug.loglists", DEV, 0, "the renderer's lists")                                                         \
    B(DBG_LOGCOMPUTE, "debug.logcompute", DEV, 0, "the renderer's dispatches")                                                \
    B(DBG_LOGPATCH, "debug.logpatch", DEV, 0, "the game patches' counters, every 600 presents")                               \
    B(DBG_LOGNVN, "debug.lognvn", DEV, 0, "the NVN layer's blocks")                                                           \
    B(DBG_LOGVI, "debug.logvi", DEV, 0, "the display's pacing")                                                               \
    B(DBG_VALIDATION, "debug.validation", DEV, 0, "the Khronos validation layer, with synchronization validation")            \
    S(DBG_STRICT, "debug.strict", DEV, "", "any validation message ends the run (a value: only that reason)")                 \
    B(DBG_STRICTALL, "debug.strictall", DEV, 0, "with debug.strict: every reason's first messages, no stop")                 \
    S(DBG_DUMP, "debug.dump", DEV, "", "<present>[+<frames>]: every pass's targets of those frames as PNGs")                  \
    S(DBG_DUMPDIR, "debug.dumpdir", DEV, "", "the dumps' folder")                                                             \
    S(DBG_DUMPW, "debug.dumpw", DEV, "", "the dumps' largest width")                                                          \
    B(DBG_DUMPRAW, "debug.dumpraw", DEV, 0, "the dumps' raw texels too")                                                      \
    B(DBG_FRAMES, "debug.frames", DEV, 0, "frames.csv: a row per present")                                                    \
    B(DBG_GPUTIME, "debug.gputime", DEV, 0, "the GPU's busy and idle time per present")                                       \
    B(DBG_MARKS, "debug.marks", DEV, 0, "GPU breadcrumbs for a lost device")                                                  \
    S(DBG_PSTATS, "debug.pstats", DEV, "", "<present>|@<seconds>|bench: every draw's time over one frame (pstats.csv)")                  \
    B(DBG_CAPTURE, "debug.capture", DEV, 0, "a capture's passes kept apart")                                                  \
    S(DBG_CAPID, "debug.capid", DEV, "", "the capture's pass id")                                                             \
    S(DBG_CRIT, "debug.crit", DEV, "", "[<first present>[:<presents>]]: a trace of the guest threads' critical path")          \
    B(DBG_CONDCHECK, "debug.condcheck", DEV, 0, "conditional draws' conditions checked")                                      \
    B(DBG_DEFERREDCHECK, "debug.deferredcheck", DEV, 0, "deferred draws checked")                                             \
    S(DBG_DRAWLOG, "debug.drawlog", DEV, "", "draws logged (a value: their filter)")                                          \
    S(DBG_MESHLOG, "debug.meshlog", DEV, "", "mesh draws logged (a value: their filter)")                                     \
    B(DBG_LATEVERTEX, "debug.latevertex", DEV, 0, "late vertex fetch for every program")                                      \
    B(DBG_NOLATE, "debug.nolate", DEV, 0, "no late SPIR-V edits")                                                             \
    S(DBG_LOOPCAP, "debug.loopcap", DEV, "", "shader loops capped at that count")                                             \
    B(DBG_NOCHANMAP, "debug.nochanmap", DEV, 0, "the game's swizzle alone")                                                   \
    B(DBG_PHLOG, "debug.phlog", DEV, 0, "placeholder textures logged")                                                        \
    B(DBG_QCPUTS, "debug.qcputs", DEV, 0, "query timestamps of the recording time")                                           \
    B(DBG_QFIXED, "debug.qfixed", DEV, 0, "query samples fixed at 4096")                                                     \
    B(DBG_QNOCOND, "debug.qnocond", DEV, 0, "conditions not evaluated, their draws drawn")                                    \
    B(DBG_REPLOG, "debug.replog", DEV, 0, "query reports logged")                                                             \
    S(DBG_POOLOF, "debug.poolof", DEV, "", "<hex>: the pool of that address")                                                 \
    B(DBG_RTLOG, "debug.rtlog", DEV, 0, "render targets logged")                                                              \
    B(DBG_TMWAIT, "debug.tmwait", DEV, 0, "task-mesh waits logged")                                                           \
    B(DBG_UBOCLAMP, "debug.uboclamp", DEV, 0, "uniform ranges clamped")                                                       \
    B(DBG_UBORANGE, "debug.uborange", DEV, 0, "uniform ranges checked")                                                       \
    S(DBG_PEEK, "debug.peek", DEV, "", "a dispatch's buffer read back")                                                       \
    S(DBG_SKIPCS, "debug.skipcs", DEV, "", "dispatches of that program skipped")                                              \
    S(DBG_WRITER, "debug.writer", DEV, "", "who writes that buffer")                                                          \
    S(DBG_SUBLOG, "debug.sublog", DEV, "", "submissions logged")                                                              \
    S(DBG_WORKERS, "debug.workers", DEV, "", "the renderer's worker threads")                                                 \
    S(DBG_ASSETCHECK, "debug.assetcheck", DEV, "", "prepared textures compared with the game's")                              \
    B(DBG_ASSETDUMP, "debug.assetdump", DEV, 0, "prepared textures dumped")                                                   \
    B(DBG_NOASSETS, "debug.noassets", DEV, 0, "the texture store not used")                                                   \
    B(DBG_TILELOG, "debug.tilelog", DEV, 0, "OoT: terrain tiles logged")                                                      \
    B(DBG_TEXWATCH, "debug.texwatch", DEV, 0, "CPU writes into texture storage")                                              \
    S(DBG_MEMWATCH, "debug.memwatch", DEV, "", "guest addresses whose writes are reported")                                   \
    S(DBG_MEMFIND, "debug.memfind", DEV, "", "<values>[@<present>]: where in guest memory those values are")                  \
    B(DBG_ENVLOG, "debug.envlog", DEV, 0, "OoT: the environment's parameters")                                                \
    S(DBG_NODATA, "debug.nodata", DEV, "", "OoT: loads with no data (a value: that name)")                                    \
    S(DBG_RESIZE, "debug.resize", DEV, "", "<w>x<h>@<present>[+...]: the window resized then")                                \
    B(DBG_SJ, "debug.sj", DEV, 0, "setjmp and longjmp logged")                                                                \
    B(DBG_WHOLE, "debug.whole", DEV, 0, "OoT: whole-frame counters")                                                          \
    B(DBG_MOVIE, "debug.movie", DEV, 0, "OoT: a movie's frame counts, every 5 s")

/* the controls (runtime/nn_hid.c): the gamepad input that presses each Switch button (HOS_PAD_* position, -1 none, -2 the
   default) and its two keys (<code>,<code>; "" the default), the keys of the sticks after them */
#define OUNCE_SETTINGS_CONTROLS(I, S)                                                                                          \
    I(PAD_A, "pad_a", USER, -2, -2, 31, "") I(PAD_B, "pad_b", USER, -2, -2, 31, "") I(PAD_X, "pad_x", USER, -2, -2, 31, "")   \
    I(PAD_Y, "pad_y", USER, -2, -2, 31, "") I(PAD_LS, "pad_ls", USER, -2, -2, 31, "") I(PAD_RS, "pad_rs", USER, -2, -2, 31, "") \
    I(PAD_L, "pad_l", USER, -2, -2, 31, "") I(PAD_R, "pad_r", USER, -2, -2, 31, "") I(PAD_ZL, "pad_zl", USER, -2, -2, 31, "")  \
    I(PAD_ZR, "pad_zr", USER, -2, -2, 31, "") I(PAD_PLUS, "pad_plus", USER, -2, -2, 31, "")                                   \
    I(PAD_MINUS, "pad_minus", USER, -2, -2, 31, "") I(PAD_LEFT, "pad_left", USER, -2, -2, 31, "")                             \
    I(PAD_UP, "pad_up", USER, -2, -2, 31, "") I(PAD_RIGHT, "pad_right", USER, -2, -2, 31, "")                                 \
    I(PAD_DOWN, "pad_down", USER, -2, -2, 31, "")                                                                             \
    S(KEY_A, "key_a", USER, "", "") S(KEY_B, "key_b", USER, "", "") S(KEY_X, "key_x", USER, "", "")                           \
    S(KEY_Y, "key_y", USER, "", "") S(KEY_LS, "key_ls", USER, "", "") S(KEY_RS, "key_rs", USER, "", "")                       \
    S(KEY_L, "key_l", USER, "", "") S(KEY_R, "key_r", USER, "", "") S(KEY_ZL, "key_zl", USER, "", "")                         \
    S(KEY_ZR, "key_zr", USER, "", "") S(KEY_PLUS, "key_plus", USER, "", "") S(KEY_MINUS, "key_minus", USER, "", "")           \
    S(KEY_LEFT, "key_left", USER, "", "") S(KEY_UP, "key_up", USER, "", "") S(KEY_RIGHT, "key_right", USER, "", "")           \
    S(KEY_DOWN, "key_down", USER, "", "") S(KEY_MOVE_UP, "key_move_up", USER, "", "")                                         \
    S(KEY_MOVE_DOWN, "key_move_down", USER, "", "") S(KEY_MOVE_LEFT, "key_move_left", USER, "", "")                           \
    S(KEY_MOVE_RIGHT, "key_move_right", USER, "", "") S(KEY_CAM_UP, "key_cam_up", USER, "", "")                               \
    S(KEY_CAM_DOWN, "key_cam_down", USER, "", "") S(KEY_CAM_LEFT, "key_cam_left", USER, "", "")                               \
    S(KEY_CAM_RIGHT, "key_cam_right", USER, "", "")

typedef enum {
#define OUNCE_SETTING_ID_(id, ...) SET_##id,
    OUNCE_SETTINGS(OUNCE_SETTING_ID_, OUNCE_SETTING_ID_, OUNCE_SETTING_ID_, OUNCE_SETTING_ID_, OUNCE_SETTING_ID_)
#undef OUNCE_SETTING_ID_
    SET_COUNT
} setting_id;

/* (runtime/settings.c; the renderer reaches them through its host table) */
int setting_bool(setting_id id);
int64_t setting_int(setting_id id);   /* (an int, an enum's index, a bool) */
double setting_float(setting_id id);
const char *setting_str(setting_id id);   /* (a string's value, "" unset) */
int setting_given(setting_id id);         /* (set by the options file or --set, not the default) */
const char *setting_text(setting_id id, char *out, size_t cap);   /* (any setting's value as text: an enum's choice by name) */
/* a user setting changed (the game's Options screen, the - and = keys): kept and written to the options file; a dev
   setting's change stops the run */
void setting_write_int(setting_id id, int64_t v);
void setting_write_float(setting_id id, double v);

/* (runtime: main's first call) the defaults, the options file, then --set; --options and --set taken out of argv, the new
   argc returned. --list-settings prints the table and ends the process */
int settings_init(int argc, char **argv);
void settings_report(void);   /* (the log: what the start found and every value not the default) */

#ifdef __cplusplus
}
#endif
