# Hookshot (oot-recomp)

**Hookshot** is the port's name (as Skybound is TOTKPort's and BlueShell is mkw-recomp's); the repository folder stays
`oot-recomp`. The game builds as `hookshot` + `hookshot_direct.so` (Linux, Mesa RADV).

A static recompilation of the Switch 2 *The Legend of Zelda: Ocarina of Time* remake to native x86-64: the game's code
lifted to C (NotRexed's a64-kit, Rexcomp's AArch64 kit), the Horizon OS and NVN2 implemented natively, the GPU work
replayed on Vulkan by the direct back end. It is a fork of mkw-recomp (Mario Kart World), whose paths it keeps so MKW's
fixes apply here.

**No game files are in this repository.** The dump is read where it lies (`work/dump.txt`); everything made from it
(modules, image, the lift's plan, builds, runs) goes to `work/`, which git ignores.

The design and the lessons carried over from MKW: [design.md](design.md). The renderer: [direct-backend-plan.md](direct-backend-plan.md),
and what we know about the game: [direct/](direct/README.md).

## Use

Requirements: Python 3 with `pip install -r requirements.txt` (`lz4`, `numpy`, `capstone`, `unicorn` 2.1, `zstandard`,
and the button-prompt packages); clang and lld (`scripts/linux/deps.sh` installs them, FFmpeg with dav1d, SDL2 and the
Vulkan headers); the Vulkan, SPIR-V and VMA headers under `/usr`, or `HOOKSHOT_VULKAN_HEADERS` /
`HOOKSHOT_SPIRV_HEADERS` / `HOOKSHOT_VMA_HEADERS`, or a CPM cache (`RX_CPM`). Without them the build stops.
[CONTRIBUTING.md](../CONTRIBUTING.md#development-setup) has the rest.

Run everything with `python -I` from this folder.

| Command | What |
|---|---|
| `python -I nx2/nup.py <file.nup> --extract work/dump` | the `.nup` dump package to `work/dump/exefs` + `romfs` (newest title; ZBIC NSOs made plain by `nx2/unpack.py`) |
| `python -I nx2/dump.py --dump <folder with exefs/ and romfs/> --set` | what the dump is (Switch 1 or 2, ids, modules, SDK, NVN); `--set` records it in `work/dump.txt` |
| `python -I nx2/lift.py --game games/oot --standalone [--write]` | image + discovery + the plan (`work/sa/plan.pickle`); `--write` also the runtime's tables and the image's C |
| `python -I nx2/lift2/census.py` | instruction census of the game's code: the forms and what the lifter covers |
| `python -I nx2/lift2/selfcheck.py` | the lifter's decoder against llvm-mc on every distinct word of the game |
| `python -I nx2/test_lift2_leaf.py [--count 500 --states 4] [--calls]` | random whole functions (or call trees) of the game, lifted and compared with Unicorn |
| `python -I nx2/lift.py --game games/oot --standalone --write` then `python -I nx2/build.py --game games/oot --standalone` | the standalone game: `work/sa/build/hookshot` + `hookshot_direct.so`, the only renderer |
| `scripts/run-vk.sh <name> [seconds] [-- NAME=VALUE ...]` | a muted, headless test run (Linux), with screenshots in `work/runs/<name>/shots`; see Iterating below |
| `python -I scripts/prof.py <stacks file>` | a profile sampled with `HOS_PROFILE=<seconds>` + `HOS_SAMPLE=<thread>` + `HOS_SAMPLE_FILE=<stacks file>` (the sampler runs only with `HOS_PROFILE` set), named from the linker maps |

The dump can also come from `OOT_DUMP` (its folder) or `OOT_EXEFS`; `OOT_WORK` moves the work folder.

**The game.** The lift, the build and the lifter's tools take exactly one game description, `--game games/oot`
(`nx2/gamedesc.py`; the scripts pass it): the dump's identity (main's build id and the SHA-256 of `exefs/main`: another
dump stops the lift before anything else is read, and a lift of another game stops the build: `work/sa/game.json`),
the label file (`games/oot/labels.txt`), the hot list (`work/hot_funcs_play.txt`), the host's C files the build compiles
and the patch scan reads, and the game's facts the lifter's checks need. A missing description is an error.

The built game's own arguments (`image.bin`, `--romfs`, `--user`, `--trace`, `--prepare-shaders`) are in the
[README](../readme.md#build-and-run).

Scripted test input (`HOS_INPUT_SCRIPT` and `HOS_INPUT_LIVE`) is applied before the window-focus check. A log saying
the game window is not in front does not mean those presses were discarded. `HOS_HEADLESS` changes window creation, not this input routing. The game must still reach a state that reads
and responds to the supplied controller input.

Physical keyboard and mouse input stays with the foreground window. Physical controllers can also play in the
background when the existing "Controller in background" option or `HOS_INPUT_BACKGROUND=1` enables them;
`HOS_INPUT_BACKGROUND=0` overrides that option. With `HOS_INPUT_EXCLUSIVE` set (to any value, `0` too) the script
alone drives the game until 5 s after its last press; without it the player's devices add to the script's presses. It
is not a focus override.

The heart protection, flight and free-camera cheats and their Cheats page are gone with the present lifter;
[parked-patches.md](parked-patches.md) records what they were.

## Iterating quickly (Linux)

Everything made from the game lives in `work/` (git ignores it). Run Python as `work/venv/bin/python -I`.

**Setup once.** `python3 -m venv work/venv && work/venv/bin/pip install -r requirements.txt`; extract the dump
(`nx2/nup.py <.nup> --extract work/dump`) and record it (`nx2/dump.py --dump work/dump --set`); put the SPIR-V and VMA
headers where `nx2/build.py` finds them (`/usr`, or `HOOKSHOT_SPIRV_HEADERS` and `HOOKSHOT_VMA_HEADERS`; the Vulkan
ones likewise with `HOOKSHOT_VULKAN_HEADERS`); `scripts/relift.sh` lifts the game (the plan in `work/sa/plan.pickle`, the
tables and the image's C in `work/sa/gen`) and builds it.

**Build.** `work/venv/bin/python -I -u nx2/build.py --game games/oot --standalone` writes `work/sa/build/` (`hookshot`,
`hookshot_direct.so`, the caches beside them). A change to a header every file includes, or to the build flags, rebuilds
everything; a change in `host/` or `gpu/direct/` recompiles only that file and links.
- To patch the game, declare the patch next to its body in a `host/*.c` file (`host/patch.h`; docs/lifter.md, "Hooks
  and probes", has one example per kind) and build; no lift is needed:
  - host code at an instruction: `PATCH_AT(pc, word(W), mode, result, fn, operand...) { ... }`, further sites
    `PATCH_AT_SITE(...)`;
  - host code that only watches a call: `PATCH_AT(<the function's address>, word(W), before, none, fn, operand...)`;
  - a function replaced: `PATCH_FUNC(0x8xxxxxxx, word(W), body(H), aapcs, result, fn, [orig,] [guest(A, ...),]
    operand...) { ... }` or `PATCH_FUNC_SYM(name, aapcs, result, fn, ...) { ... }`: `fn` takes the operands it
    declares (only what it uses) and returns the result; with `orig` it may run the game's body, `guest_orig(orig)`
    with the call's registers or `guest_orig(orig, x0, ...)` with arguments it changed, with `guest(A, ...)` and `sp` it calls
    guest code through `guest_call`. `W` is the instruction's or the function's first word, `H` its body's hash (the
    build's error names the right one); names of `PATCH_FUNC_SYM` resolve through `work/sa/image/image.json`;
  - an SDK function replaced: `HLE(symbol) { ... }` (this one does need a lift: the lift hooks it), an NVN proc
    `NV(name) { ... }`, with `reads(reg, "why")` or `clobbers("why")` when a call of it reads more than its arguments
    or keeps no register.
  The scan stops on a malformed declaration with its file:line, and the build on a word that differs from the game's
  or on a declaration the compiler saw and the scan did not. Only the partitions whose functions a changed declaration
  touches are lifted and compiled again. Read the function first: `nx2/lift2/dump.py --game games/oot <address>`.
  A new host C file goes into the game description's `host` list.
- A new `host/*.c` file is picked up by the next build only (the file list is taken at its start).
- `RX_DIRECT_ONLY=1` builds the back end alone, with no lift.
- **A new lift** is needed after a change to the lift (`nx2/lift.py`, `nx2/lift2/discover.py`) or to the dump (a
  change to the patch declarations in `host/*.c` other than an `HLE` added or removed needs only a build; the summaries
  count the rows and replaced functions, so the build recomputes them when their declared effects change): `scripts/relift.sh` lifts and builds, for the
  checkout it is run from; in the main checkout on native-vk through `scripts/play-refresh.sh`, so `BUILD.txt` names the
  head. It stops while a game runs from that build. About 100 s of lift (6 GB at its peak); a cold build about 5
  minutes at 6 jobs; log `work/play/relift.log` (a worktree: `work/sa/relift.log`). `scripts/play-refresh.sh` lifts
  again by itself when the lift's inputs changed (`nx2/lift_mark.py`).
- **The lifter** (`nx2/lift2`, [lifter.md](lifter.md)) lifts every function at build time: a change to it needs only a
  build. Its partitions' objects stay in `work/objcache` by their inputs (the plan, the lifter's sources, the flags, the
  functions, the replaced functions): a host-only rebuild neither lifts nor compiles them again (about a minute). The hot list
  (the description's, `work/hot_funcs_play.txt`, made by `scripts/hotlist.py` from a profile; or `HOOKSHOT_HOT_FUNCS`;
  a missing one stops the lift) puts its functions in partitions built at -O2.
- To keep builds side by side, copy `hookshot`, `hookshot_direct.so`, `hookshot.map` and the caches (`*.bin`, `*.idx`)
  to `work/sa/<name>/` and run with `EXE=`.

**Play build.** Play with `scripts/play.sh [record <name>] [title|kokiri|hf|market] [-- NAME=VALUE]` (`record` writes the input to `work/rec/<name>`): it runs the main checkout's `work/sa/build`
(saves and log in `work/play`), which `scripts/play-refresh.sh` keeps at native-vk's head (build log `work/play/refresh.log`, stamp
`work/sa/build/BUILD.txt`; it refuses on a dirty tree or another branch). `scripts/play-refresh.sh --install-hook` writes a local
`.git/hooks/post-merge` that starts it in the background after every merge onto native-vk (hooks are not tracked: once per clone).

**Run.** `OVERLAY=work/overlay/chisel_kokiri scripts/run-vk.sh <name> <seconds> -- [NAME=VALUE ...]` starts the build
headless and muted, with a private `--user` folder, and writes `work/runs/<name>/`: `shots/` (a picture every
`SHOT_EVERY` presents, 300), `timeline.txt` (second, presents, RSS in MB), `log.txt`, `stdout.txt`. `EXE=` picks another
build, `USER_FROM=<folder>` starts from a save, `WINDOW=1` and `SOUND=1` turn the window and the sound on. A first run of
a build folder is cold (shaders and pipelines are prepared); copy a warm folder's caches for a comparison.
- **Chiseled boot:** `tools/dump/make_chisel_overlay.py <dir> --route newgame --banc <map> --start <name>` (or `--route
  load [--slot N]`) writes patched sequence graphs that skip the title menu and go straight into a map; `OVERLAY=<dir>`
  hands them to the game as `HOS_FS_OVERLAY`. Maps: `KokiriForest_Child`, `HyruleField_Child` and `_Adult`,
  `KakarikoVillage_Child`, `InsideTheDekuTree`. The new-game route saves, so run it with the script's own `--user` folder.
  The opening's events are cut (`--keep-intro` keeps them); `--skip-title` and `--skip-fades` cut the title movie and the
  loading fades. Details: `tools/dump/README.md`.
- **Actor zoo:** scenes we describe ourselves for benchmarks and picture tests, independent of a save and of the game's
  own maps. `zoo/<scene>.toml` names an existing map (`base`), the start, which of its actors stay, and the actors to add
  (name, position, rotation in degrees, scale, a grid of copies, their parameters); `tools/dump/make_zoo.py` turns it into
  an overlay from the user's dump (the chiseled boot, the map's two bancs rewritten, the resource size table with their
  entries raised). `ZOO=zoo_static scripts/run-vk.sh <name> <seconds>` and `scripts/play.sh zoo_static` make
  `work/overlay/<scene>` when it is missing or older than its description (`scripts/zoo-overlay.sh`). Scenes:
  `zoo_static` (288 static props, Sage Room), `zoo_anim` (torches, fairies, fireflies, butterflies, cows),
  `zoo_physics` (pots that fall and break, three Havok cloth carriers that are stepped but look still),
  `zoo_shadows` (rocks and waving flags on the market's drawbridge in Hyrule Field at noon, under the sun's cascades;
  flags wave only in a map's wind, and the Sage Room has almost none). A scene's `hour` sets the time of day after the
  load through the host's world clock (the overlay's `live.txt`, sent as `HOS_INPUT_LIVE`): a new game's field is at
  night. The map's collision, navigation mesh and baked lighting stay the base map's: a scene can only use a map that
  exists. Leaving a room's model actors out (`keep`) hides the room but not its floor: the collision is the map's static
  compound body.
- **Scripted input:** `"HOS_INPUT_SCRIPT=30:LUP/3000,34:RRIGHT/2500" HOS_INPUT_EXCLUSIVE=1` (seconds from the game's
  first controller read; the syntax is under Input for scripted runs).
- **Record and replay a session:** the game started with `HOS_INPUT_RECORD=$PWD/work/rec/<name>` (an absolute path)
  copies the save folder to `work/rec/<name>/user` and writes every pad state the game reads, keys and mouse after their
  mapping included, to `work/rec/<name>/input.rec` (a line a change, one a second; flushed, so a crash keeps it).
  `scripts/replay.sh <name> <seconds> [-- NAME=VALUE ...]` replays it headless and muted from that save, with the
  header's settings, into `work/runs/replay-<name>`. Lines are matched by the game's pad read count: a load that takes
  longer in the replay still shifts it, so let go of the controls during loads and wait a second after one.
- **Launch time:** `work/venv/bin/python -I scripts/launch_time.py work/runs/<name> ...`.
- **Smoke test:** `scripts/smoke.sh <name> [kokiri|hf] [-- NAME=VALUE ...]` runs one standing scene for `SECS` (40) seconds
  with `RUN=timing`, and `scripts/smoke.py` prints a verdict of about ten lines: fatal log lines, a perceptual hash of
  every shot after the load against the golden run's shot of the same present (a Hamming distance over 12 of 64 fails),
  rough numbers beside the golden run's (steady fps, replay times, peak RSS, least free and most shared memory), and
  warning or error groups not in `work/smoke/<scene>/known.txt`. Record the golden run once per build with the switches
  off (`GOLDEN=1 scripts/smoke.sh <name> kokiri`); an A/B is the same build with a switch on. Read the verdict, not
  the log; `KEEP_SHOTS=1` keeps the pictures, which are deleted after the check.
- **One game at a time, unless the runs are checks.** `RUN=timing` takes the GPU lock exclusively (fps, launch time,
  anything that may hang the GPU: a reset kills every run beside it); `RUN=check` takes it shared, at most `RUN_SLOTS`
  (default 1) runs at once, each on a private reflink copy of the build folder, and waits for `RUN_MEM_GB` (14) of free
  memory. Do not wrap either in `flock`. A run waits for the free memory first: a game holds about 10 GB of guest
  memory in a memfd shared with the GPU, which RSS does not show.
- After `VK_ERROR_DEVICE_LOST` the GPU is reset: `journalctl -k` names the ring and thread. `HOS_DBG=validation` (the
  validation layer) and `HOS_DBG=strict` (a warning or error ends the run) are the back end's own aids.

**Agent worktrees.** `scripts/agent-worktree.sh new <name> [base]` makes `work/wt/<name>` on branch `agent/<name>`. Its
`work/` links to the main one (dump, image, runs, overlays, venv, locks, object cache) except `sa/gen` and `sa/build`,
which are its own reflink copies, so hooks splice into its own lift and a host-only build is quick. Run with
`EXE=work/sa/build/hookshot` from it. `land <name>` rebases the branch onto the main checkout's branch and
fast-forwards it; `drop <name>` removes the worktree and the branch once landed.

**Measure.**
- The frame rate is the `vk: <n> presents, <x> fps (direct); frame time ...` line in `log.txt`, taken standing at a named
  place.
- **Benchmark:** `scripts/bench.sh <scene> [frames] [-- NAME=VALUE ...]` (`kokiri`, `hf`, `market`, an actor-zoo scene
  or an overlay folder's name; 600 frames) runs the scene headless and muted with `HOS_BENCH` and prints its summary. The game steps exactly 1/60 s a
  frame and presents as fast as it can, so two runs simulate the same game time whatever their speed. Measuring starts when
  the scene is ready, all of these for 60 frames in a row: Link updated, no map load and no event playing (in play); the
  texture manager's Replace work not pending; no shader job queued or running and no draw skipped for a shader not ready;
  no file read by the game; the resource system quiet (no message taken by ARes Loading, GeneralFormatAsync,
  ParallelDecompThread or BaseProcCreateAndDelete, none past the one a frame of Resource Control and Resource Memory:
  a map goes on loading what it asks for from archives in memory for seconds after Link is in play). The log says when (`bench: ready at <s> since the start`, and how many frames each part held
  the wait), then the frames' mean, median, p99 and max, the draws a frame, the steps that were not 1/60 s (0) and the
  CPU seconds by thread group (Linux). `work/runs/<name>/bench.csv` has a row a frame: its wall time, its game step (in
  1/60 s), the draws recorded and those that waited for a shader, the shader jobs, the file reads, the command lists made
  and called. Left on real time: the system tick and the steady clock (the game's own frame timing and its waits read
  them), the audio thread's clock and thread scheduling.
- CPU, one thread: `HOS_PROFILE=<seconds> HOS_SAMPLE=<thread> HOS_SAMPLE_FILE=<f>`, read with `scripts/prof.py <f>`.
  All threads: `HOS_PROFILE=5 HOS_SAMPLE_ALL=<f>` with `<f>.go` present, then `RX_MAPS=work/sa/build scripts/crit.py <f>`
  (`scripts/hotlist.py` reads the same file).
- The first line of `log.txt` names the build as `<version> (source <hash>)` (`nx2/version.py`); the hash is a fingerprint
  of `host/`, `gpu/` and `nx2/`, so it tells two builds of one commit apart, and a build from a modified checkout has
  nothing else.
- Tests: `work/venv/bin/python -I -B tests/run_all.py`.

**The lifter's output for a function.** `work/venv/bin/python -I nx2/lift2/dump.py --game games/oot <address or
label>... [--tree] [--opt O1|O2] [--asm]` writes the guest code, the IR, the LLVM IR and optionally the optimised IR and the x86 to
`work/lift2/dump/<address>/`, and prints the function's summary when `work/sa/plan.pickle` has one.

**Reverse engineering.** Flat modules `work/re/{main,sdk,subsdk0}.bin` (main's base is 0x80020000), the exports in
`work/sa/image/image.json`, and the scripts in `tools/re/` (`disasm.py`, `xref.py`, `callers.py`; see its README) and
`tools/dump/` (`bntx.py`, `bfres.py`, `bfsha.py`, `ainb.py`, `byml.py`, `sarc.py`, `zs.py`).

## Switches for testing and debugging

The running game reads its switches from the environment or from `hookshot_env.txt` beside `image.bin` (one `NAME=VALUE` a
line, `#` for notes; `HOS_ENV_FILE=<path>` reads that file instead). A switch that names a file or a folder to write
(`*_FILE`, `*_DIR` and `HOS_PRESENT_TRACE`) is not taken from that file, and neither
are the library names SDL loads. The build reads its own switches from the environment. Every name below is read by code in
`host/` or `gpu/direct` today; "set" means any value, `0` included. Switches whose meaning is not stated in the code
beside them are listed at the end, under Undocumented, with where they are read.

### Running a test

| Switch | What it does |
|---|---|
| `HOS_HEADLESS` | (set) No window: the renderer draws offscreen, for screenshots and frame dumps |
| `HOS_WINDOW_SIZE` | `<w>x<h>`: the window's size in pixels (1280x720 by default) |
| `HOS_FULLSCREEN` | `1`: start in fullscreen |
| `HOS_AUDIO` | `0`: no sound device is opened |
| `HOS_LANGUAGE` | `<code>`: the game's language for this run |
| `HOS_OUTPUT_RES` | `<0..4>` over the options file: 0 2048x1152, 1 2560x1440, 2 3840x2160, 3 1920x1080, 4 5120x1440 (handheld forces 0) |
| `HOS_HANDHELD` | `1`: the handheld profile, over the options file |
| `HOS_ASPECT` | `<ratio>`: a picture wider than 16:9 (the HUD, menus and movies keep the middle 16:9); `2.3889` for 3440x1440 |
| `HOS_TEXT_SPEED` | Text speed, over the options file |
| `HOS_GRAPHICS_SCALE` | `<float>`: the scale `GetCurrentGraphicsScale` returns (default 1.0) |
| `HOS_SCENE_NATIVE` | non-zero: the scene's base scale is 1.0 (the game's is 0.8) |
| `HOS_DYNRES` | `1`: the game's dynamic resolution steps the 3D's scale with the GPU time; off by default, the 3D at the fixed scale |
| `HOS_RES_SCALE` | `<float>`: the fixed scale of the 3D (1.0, the game's own, by default) |
| `HOS_RT_POOL_MB` | `<MB>`: the render-target pool's size, over the default (at least 512 MB) |
| `HOS_MODS` | `<dir>`: the mods folder, over the `mods` folder beside the executable (RomFS mods in the layout emulators use, host/mods.c) |
| `HOS_FS_OVERLAY` | `<folder>`: patched game files read before the romfs (the chiseled boot) |
| `HOS_PREPARED` | `<folder>`: the prepared texture and terrain store, over the default |
| `HOS_NO_MOVIES` | set: movie files are reported as not found, so the game goes on without them |
| `HOS_DISCORD` | `0`: no Discord rich-presence thread |
| `HOS_DISCORD_APP_ID` | a Discord application id, over the built-in one |
| `HOOKSHOT_OCCLUSION_OFF` | `1`: the game makes no occlusion queries |
| `HOS_ENV_FILE` | `<path>`: the file read for `NAME=VALUE` lines instead of `hookshot_env.txt` |
| `HOS_TRACE` | `1`: every Horizon call logged, as `--trace` does |
| `HOS_FIXED_TIME` | `<seconds>`: the wall clock the game sees starts at that Unix time |
| `HOS_MEMORY_MB` | `<MB>`: the application memory the game is told (3285 on a retail console) |

### Input for scripted runs

| Switch | What it does |
|---|---|
| `HOS_INPUT_SCRIPT` | presses timed from the game's first controller read (syntax below) |
| `HOS_INPUT_LIVE` | `<file>`: lines of presses appended by a test, read every 30 ms; `!synclog` logs the NVN syncs |
| `HOS_INPUT_FRAME_CLOCK` | `1`: the script's time runs at 60 game frames a second instead of the wall clock |
| `HOS_INPUT_EXCLUSIVE` | (set) the script alone drives the game until 5 s after its last press; without it the player's devices add to the script's presses |
| `HOS_INPUT_BACKGROUND` | controllers count while the window is behind another one (`0` overrides the option) |
| `HOS_INPUT_SCRIPT_BACKGROUND` | (set) scripted input runs with the game in the background |
| `HOS_INPUT_RECORD` | `<dir>`: the save folder copied to `<dir>/user` at the start, and every pad state the game reads written to `<dir>/input.rec` (on each change and once a second) |
| `HOS_INPUT_REPLAY` | `<dir>/input.rec`: its states instead of the devices' until its last line |
| `HOS_INPUT_REPLAY_KEY` | what a replayed line is matched by: `read` (the default: the game's pad reads), `present`, or `ms` (wall time) |
| `HOS_NO_SDL` | (set) controllers are not read through SDL |
| `HOS_HD_RUMBLE` | `0`: no HD rumble |
| `HOS_MOUSE_NATIVE` | `<0|1>` over the menu: native mouse look |
| `HOS_MOUSE_INVERT_Y` | (set) invert the mouse's vertical axis |
| `HOS_MOUSE_SENS` | `<factor>`: mouse sensitivity, over the menu and `mouse_sens.txt` |
| `HOS_MOUSE_DECAY` | `<percent>`: the mouse look's decay rate (default 0.30) |
| `HOS_MOUSE_MIN_DECAY` | `<percent>`: its minimum decay (default 0.20) |

The presses are `<seconds>:<button>[+<button>...][/<ms>]`, separated by commas, for example `2:A,5.5:LUP/1500,9:ZL+R`.
The buttons are `A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN`, and the sticks pushed to their edge `LUP
LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT`. A press is held for 200 ms unless `/<ms>` says otherwise. The names are
case-sensitive, and an unknown one is ignored.

### Pictures and frames

| Switch | What it does |
|---|---|
| `RX_VK_SHOTS` | `<dir>`: a PNG of the presented frame every `RX_VK_SHOT_EVERY` presents (default 300) |
| `RX_VK_SHOT_EVERY` | `<n>`: the interval for `RX_VK_SHOTS` |

### Time, pacing and profiles

| Switch | What it does |
|---|---|
| `HOS_PROFILE` | `<seconds>`: every interval, the busiest threads in the log; it also starts the samplers below |
| `HOS_SAMPLE` | `<thread>`: that thread's stacks sampled every millisecond (needs `HOS_PROFILE`) |
| `HOS_SAMPLE_FILE` | `<file>`: where `HOS_SAMPLE` writes the stacks; `scripts/prof.py` reads it |
| `HOS_SAMPLE_ALL` | `<file>`: every game and renderer thread every 2 ms while `<file>.go` exists (needs `HOS_PROFILE`); `scripts/crit.py` and `scripts/hotlist.py` read it |
| `HOS_THREAD_TIMES` | `<seconds>`: every interval, the threads that used over 5% of a core |
| `HOS_STATS_AFTER` | `<seconds>`: a statistics report after that time (host/hos_process.c:206) |
| `HOS_FPS_LOG` | (set) the frame limiter's state logged every 300 presents |
| `HOS_FPS_MAX` | `<fps>`: the frame-rate cap (60 to 240), over the option |
| `HOS_FPS_STEP` | `0`: the rate goes up to the cap as one step, not timed |
| `HOS_NO_PACING` | (set) no frame pacing |
| `HOS_PRESENT_TRACE` | `<csv>`: a CSV line per game present |
| `HOS_GAME_INTERVAL` | non-zero: paced to the game's own interval |
| `HOS_GAME_STEP_PER_FRAME` | `1`: game speed per frame instead of the option |
| `HOS_BENCH` | `<frames>[,<ready frames>]`: a benchmark run (`host/bench.c`, `scripts/bench.sh`): a 1/60 s game step a frame, unpaced; once the scene has been ready for `<ready frames>` presents in a row (60) it measures `<frames>`, writes `bench.csv` in the working folder and exits with 0 |
| `HOS_FRAME_SLEEP_MS` | `<ms>`: every frame sleeps that long (diagnostic: game speed against frame rate) |
| `HOS_RES_LOG` | (set) the resolution module's state, every 300 presents |

### Threads and synchronisation

| Switch | What it does |
|---|---|
| `HOS_RESERVE_CORE` | `0`: no core kept for the renderer |
| `HOS_NO_RENDERER_WAIT` | non-zero: the game's first GPU work does not wait for the renderer to be up |
| `HOS_LAYER_AHEAD` | `0`: calc stage C's long layer waits for the render jobs, as the game orders it (on by default: it starts with them; docs/direct/patch-sites.md 8) |
| `HOS_SPIN_US` | `<us>`: how long an nn::os wait (event, semaphore, queue, mutex) spins before it sleeps (default 5; `0`: no spin, a wait whose object is not ready sleeps at once; an object whose spins keep missing spins one wait in 32) |

### Memory

| Switch | What it does |
|---|---|
| `HOS_GUEST_MEMFD` | `1`: the guest window is a memfd even where dma-bufs are usable |
| `HOS_NO_THP` | (set) transparent huge pages off for the whole process |
| `HOS_THP` | (set) the guest window keeps transparent huge pages |
| `HOS_ZERO_ALL_PAGES` | (set) zero the whole range above 4 GB too (that once wiped live data) |
| `HOS_PREFAULT` | `1`: new heap pages are touched at once, so the game's first touches are not page faults (experiment) |

### Render-target slots

| Switch | What it does |
|---|---|
| `RX_RT_SLOTS_MB` | `<MB>`: size of the stable render-target slots |
| `HOS_RT_QUANT` | `0` off, `1` size classes, more: multiples of it (default 1) |
| `HOS_RT_SLOTS_LOG` | (set) the render-target slot decisions logged |

### Files and video

| Switch | What it does |
|---|---|
| `HOS_FS_TRACE` | (set) file opens and reads logged |
| `HOS_FS_WATCH` | `<lo>:<hi>` (hex): a log line for a file read into that guest range |
| `HOS_FS_DELAY_MS` | `<ms>[,<after s>]`: slower reads (experiment) |
| `HOS_VIDEO_LOG` | `1`: movie submissions and acquisitions logged |
| `HOS_MIC_LOG` | (set) microphone reads logged |

### Audio

| Switch | What it does |
|---|---|
| `HOS_AUDIO_NO_REVERB` | (set) no reverb |
| `HOS_AUDIO_STATS` | `1`: once a second, what plays and where the signal is |
| `HOS_AUDIO_TRACE` | `1`: the audio graph setup calls logged |

### Tracing the NVN layer and the host

| Switch | What it does |
|---|---|
| `HOS_NVN_LOG_OBJECTS` | (set) NVN object creation logged |
| `HOS_NVN_LOG_POOLS` | (set) who makes each memory pool |
| `HOS_NVN_LOG_VMAP` | (set) virtual pools logged with their flags and caller |
| `HOS_NVN_DEVINFO_LOG` | (set) every `nvnDeviceGetInteger` query and its answer |
| `HOS_NVN_SYNC_LOG` | `<present>`: NVN sync calls logged from that present on |
| `HOS_NVN_WATCH_DRAW` | `<count>[,<after s>]`: logs the draw with that count |
| `HOS_NVN_WATCH_DRAW_THREAD` | only draws recorded on threads whose name holds this |
| `HOS_NVN_WATCH_RT` | `<hex>`: logs a render target at that address |
| `HOS_NVN_WATCH_TEX` | `<hex>`: logs a texture at that address |
| `RX_NVN_LOG_TEX` | `<w>x<h>`: logs textures of that size |
| `RX_NVN_TEXELS_LOG` | (set) CPU texel uploads logged with their callers |
| `RX_NVN_WATCH_TEX` | `<w>x<h>`: write-watch the storage of textures of that size |
| `RX_NVN_WATCH_TEX_AT` | `<hex>`: write-watch the texture at that address only |
| `RX_NVN_WATCH_TEXELS_SRC` | `<w>x<h>`: write-watch sources of uploads into textures of that size |
| `RX_VK_LOG_B2T` | `1`: who records big buffer-to-texture copies |
| `HOS_SYNC_TRACE` | `1`: fences and sync point waits |
| `HOS_MAP_TRACE` | `1`: every GPU mapping change |
| `HOS_TLS_TRACE` | (set) TLS hook results logged per thread |
| `HOS_WAIT_LOG` | (set) waits logged |
| `HOS_LONG_WAITS` | `1`: an untimed wait still blocked after 5 s is logged, then goes on |
| `HOS_MUTEX_TRACE` | `<ms>`: mutex waits longer than that are logged with the holder |
| `HOS_MUTEX_WATCH` | `<hex>`: mutex address to watch |
| `HOS_WAIT_WHO` | `<thread>`: log waits of that thread; `HOS_WAIT_WHO_LR=<hex>` narrows it to one caller |
| `HOS_HOSTCALL_STATS` | (set) host-call statistics (also `HOS_HOSTCALL_SLOW`) |
| `HOS_LOG_TIME` | `0`: log lines without the time and present |
### Log lines

Every log line starts `[hos 12.345 p600 <thread>]`: the seconds since the process started and the present the game is at.
`HOS_LOG_TIME=0` leaves both out. A line longer than the log's 4 KB buffer ends in `~`. The direct back end's per-block
detail is off by default (a block of 600 presents is a few KB; `vk:`, `direct: memory`, `direct: images`, `assets:` and
the other counters the scripts read stay); `HOS_DBG=<item>,...` turns a part on, and `logall` all of them:

| `HOS_DBG` item | The lines it brings back |
|---|---|
| `logmesh` | `task+mesh program:` and `task+mesh draws`, `program serial N is task+mesh` / `mesh-only`, `mesh program first use`, `task steps a frame`, `direct mesh draws` |
| `logpass` | `pass key` (a line each), `conditions a frame`, `transient pool: lists handed to the queue` |
| `logdraw` | the draw breakdowns: `draws a frame in <kind> lists`, `in the lists begun from`, `that step A cannot draw`, `uniform buffers a program may read past`, `state:` |
| `loglists` | `lists by return address` |
| `logcompute` | `compute N first dispatched` |
| `logpatch` | the `direct patch ...` statistics of the game-code patches, a line each, and the first records the mesh patches name |
| `lognvn` | the `nvn:` call traces (`CommandBufferWaitEvent`, `SignalEvent`, `EventSignal`, `QueueSubmitCommands`, `DrawMeshTasks`, `WaitSync`) |
| `logvi` | `vi: N frames presented` (the fps is on the `vk:` line too) |

A line that says the same most blocks (`validation:`, `strict:`, `transient pool: descriptor ids`) is printed at the first
block and again when its text changes.

`FATAL: the game threw a C++ exception of type <name> from <lr>` (`host/cxx_throw.c`): a call of `__cxa_throw`,
`__cxa_rethrow`, `_Unwind_RaiseException` or `_Unwind_ForcedUnwind` stops the game. Unwinding through lifted code
cannot work ([lifter-abi-census.md](lifter-abi-census.md) section 3), so a throw is a fatal stop rather than a wrong
continuation. The type is the mangled name from `__cxa_throw`'s type_info; the line names the thread.

`ring owner: ring N pushed by <A> and <B> (pc <site>)` (always on, `host/oot_probes.c` `hos_oot_ring_write`): a second
thread wrote one of the actor singleton's per-core rings, which the game updates without a lock ([lifter.md](lifter.md),
"Hooks and probes"). A bench run's end logs `rings: second writers N; <ring> <owner> <pushes>+<pops>/frame; ...`.

### HOS_DBG

`HOS_DBG=<item>,<item>:<value>,...` turns on the direct back end's and the host's debug aids; with it unset each costs a
load and a branch. `logall` turns every `log*` item on.

| Item | What it does |
|---|---|
| `validation` | the Khronos validation layer and its synchronization validation |
| `strict` | any validation warning or error ends the run; `strictall` instead surveys every reason's first events and does not stop |
| `noassets` | every patch in `host/direct_assets.c` off, for a before/after |
| `assetcheck:<n>` | reads back `n` prepared images and the sampled terrain and glyph uploads after the load; `assetdump` dumps them |
| `nomeshrepl` | task and mesh programs are drawn from the translator, not the hand-written replacements |
| `tmwait` | keeps the wait before task+mesh draws (off by default) |
| `nolate`, `latevertex` | the register-pressure pass off (it stamps the program cache); `latevertex` makes it run on vertex stages too |
| `loopcap:<n>` | the shader loop safety count of the translator |
| `marks` | GPU breadcrumbs for a lost device: two `VK_AMD_buffer_marker` writes around each draw and dispatch, the unfinished ones logged (`gpu/direct/direct_marks.h`) |
| `crit[:<first present>[:<presents>]]` | a trace of what every guest thread does over a window of presents, to `crit.bin`; `scripts/crit_path.py` reads it |
| `pstats:<present>` | pipeline-statistics query around each draw and dispatch of that frame, written to `direct_pstats.csv` |
| `meshcap:<present>` | one frame's mesh draws captured to `meshcap/frame.bin` for `tools/meshlab` |
| `dump:<present>[+<frames>]` | every pass's targets of the frame after that present as PNGs in `dump_<present>`, in `dumpdir:<dir>` if given; `dumpraw` writes the unrounded values of HDR targets |
| `frames` | `direct_frames.csv`: a row per present |
| `gputime` | the GPU's busy and idle time per frame (rows in `direct_frames.csv`) |
| `drawlog:<present>` | the recording lists' draws of the frame after that present, each with what it was drawn with |
| `phlog` | the first draw that samples each heap id holding the placeholder, named once |
| `rtlog` | 200 `SetRenderTargets` from present 1500 on, named |
| `peek:<hex>` | 64 KB at that guest address: non-zero bytes and a checksum |
| `poolof:<hex>+<hex>...` | the pools of those guest addresses, at each log line |
| `writer:<hex>` | every dispatch whose storage buffers cover that address |
| `texwatch` | which code writes texels into a texture's storage with the CPU (`host/direct_texwatch.c`) |
| `nodata[:<name part>]` | textures `agl::TextureData` makes with no data, logged once each; with a name part their slots are page-watched (`host/direct_diag_nodata.c`) |
| `uboclamp` | uniform-buffer reads past the bound range give zeros, through a copy |
| `ubopush` | the first 8 addresses pushed are logged |
| `uborange` | each binding read past its range, once, with what lies there |
| `qfixed`, `qnocond`, `qcputs` | A/B aids of the query path: samples fixed at 4096; conditions not evaluated and their draws drawn; timestamps of the recording time |
| `deferredcheck` | counts the deferred words that change between their bind and the submit |
| `nochanmap` | images use the game's swizzle alone |

### The shader translator (`gpu/direct/eden`)

| Switch | What it does |
|---|---|
| `RX_VK_BDA_DUMP` | `<folder>`: the stages whose storage buffers go through addresses, written for `spirv-val` |
| `RX_VK_BDA_GLOBAL` | `0`: global-memory accesses stay on descriptors |
| `RX_VK_FOLLOW_STORES` | `1`: writes and atomics go through the address table too |
| `RX_VK_GP_TEST` | `1`: the addressing model alone, `2`: and the table's load, `3` or unset: all of it |
| `RX_VK_MISS_LOG` | `1` (compute stages): a buffer at `c0+0xff0` records the addresses no binding held; `RX_VK_MISS_LOG_PROG=<key>` picks the program, `RX_VK_BLOCK_PROBE=1` counts each block's entries |
| `RX_VK_LOG_BRX` | (set) cbuf-bounded branch tables logged |
| `RX_VK_MESH_NO_DEFAULTS` | `1`: a generic output no line writes stays undefined at the fragment stage |
| `RX_VK_MESH_NO_ZERO` | `1`: unwritten mesh primitives are not zeroed |
| `RX_VK_TASK_CAP` | `<n>`: mesh workgroups a task may start; 0 is one a thread |
| `RX_VK_MMA_NO_LDSM_BARRIER` | `1`: no barrier after the LDSM lowering |
| `RX_VK_NO_CBUF_LOAD_RULE` | `1`: the constant-buffer-load rule of the storage-buffer pass off |
| `RX_VK_NO_EXTREGS` | (set) a program using the lowering's scratch registers is rejected |
| `RX_VK_MARKERS_V205` | (set) lowering markers avoid only the native high registers |
| `RX_VK_PTR_SPLIT_FALLBACK` | `0`: pointers are bound as before the split fallback |
| `RX_VK_PTR_DIAG` | non-zero: pointer diagnostics (`ir_opt/global_memory_to_storage_buffer_pass.cpp:55`) |

### Building

| Switch | What it does |
|---|---|
| `HOOKSHOT_JOBS=<n>` (or `--jobs`) | Jobs at once (default: what the memory holds, the smallest of the scope's `memory.max` and MemAvailable less 3 GB, 1.7 GB a job, at most one per CPU thread; the build prints the number and why; each compiler started when the free memory holds it; `HOOKSHOT_MEM_GATE=0` turns the check off, `HOOKSHOT_MEM_RESERVE_GB` is the memory kept free, 1.5 by default) |
| `RX_DIRECT_ONLY=1` | Only the direct back end, with no lift |
| `HOOKSHOT_VULKAN_HEADERS`, `HOOKSHOT_SPIRV_HEADERS`, `HOOKSHOT_VMA_HEADERS`, `RX_CPM` | Where the headers are (a package root or its `include/`; `RX_CPM` is a CPM cache) |
| `HOOKSHOT_FFMPEG=<prefix>`, `HOOKSHOT_SDL2` | FFmpeg's static libraries, SDL2's library folder |
| `RX_RELEASE=1` | A release-style build: the shaders are prepared before the title |
| `RX_MARCH=<level>` | The CPU level to build for (default this PC's: `x86-64-v3`, else `-v2`, else `x86-64`) |
| `RX_HOT_OPT`, `RX_NO_ORDER` | Optimisation level of the hot code; no hot-function link order |
| `HOOKSHOT_HOT_FUNCS=<file>` | (lift time) The hot list, one `f_<address>` a line (default the game description's, `work/hot_funcs_play.txt`): their partitions are built at `RX_HOT_OPT` |
| `HOOKSHOT_LIFT_VERIFY=1` | (build time) The verification build: every function with a full summary checked at run time (`host/lift2_verify.c`, [lifter.md](lifter.md), "Whole-program summaries and the verification build") |
| `OOT_WORK`, `OOT_DUMP`, `OOT_EXEFS`, `OOT_ROMFS` | The work folder and the dump's folders, over `work/dump.txt` |

### Function labels

The lifted functions are named `f_<guest address>`. `games/oot/labels.txt` gives the ones we know a label, and after the
link `nx2/label_syms.py` renames their symbols to `<label>.f_<address>`, so profiles, backtraces and debuggers show
what a function is (only the symbol table changes; no rebuild of the code).

- **Add a label whenever an analysis identifies a function**, in the same commit as the note that says so.
- A label is a description of ours, not the game's or the SDK's name (never C++ mangling): lower case, digits and
  underscores, a subsystem prefix first (the file's header lists them), unique. It says what the note says and no
  more: a role (`report_reader_b`, `_caller`) when that is all we know; `# ?` marks a doubt or notes that disagree.
- The address must be a function's entry in this lift. A call site, an lr or a splice point names the function that
  contains it (`work/sa/build/hookshot.map`), with the site in the note.
- Each entry ends with `# <file>[ section]: what`, the place that documents it.
- `python -I nx2/label_syms.py --game games/oot check` checks the list; `apply <executable>` reports addresses with no
  such function;
  `name <address> ...` prints labels for scripts.

### Writing a patch

`nx2/patch.py` reads the plan (`work/sa/plan.pickle`, or `--plan`), its summaries and the scan of the host's
declarations. It compiles nothing and lifts nothing.

- `work/venv/bin/python -I nx2/patch.py --game games/oot site <address or label>` (a few seconds) prints:
  - the instruction and its word, with the instructions around it and its function's label;
  - the paths into the instruction: branches, jump tables, a function entry, code shared by several functions. A
    `before` row runs on every one of these paths;
  - the registers live into and out of the instruction, and those it reads and writes;
  - the function's call-boundary record and how many BL sites call it;
  - the declarations already in that function;
  - a `PATCH_AT` to paste, its `word(...)` filled in, its operands suggested from what is read and live there (at a BL,
    from the callee's record). It also prints the prototype the macro generates and the build's verdict on it. At a
    function's entry it also prints the `PATCH_FUNC` form: its `word(...)` and `body(...)`, `orig` and the argument
    registers the body reads.
- `... check --all` runs the checks the build makes on every declaration and prints each error with its file:line.
  The checks: the word, a site in exactly one function, `before` not on a branch, jump-table or other entry target,
  `instead` on a call, a Value's result being its instruction's destination, result and io registers, `PATCH_FUNC`s'
  first words, body hashes, exports and the argument registers an `orig` body reads, `HLE_EFFECTS` symbols. A guest function that a host function calls without declaring it is not
  checked here: `tests/test_host_context_reads.py` compiles the host to find those.
- `... check <file:line>` also prints what that declaration costs. `... check -` does the same for a proposed
  declaration read from stdin. The tool recomputes the whole program's summaries without the declaration (or with
  the proposed one), with the build's summary job and from the summary input the lift left. It then prints the
  functions whose inputs, clobbers, must-writes or results change, the summaries demoted, and the call sites whose
  kept registers change. This takes about two minutes and several GB, so run it like any other heavy job.
- `... report` prints one line per declaration, costliest first, in about half a minute:
  - the registers the declaration reads and writes beyond its instruction's own;
  - how its function's summary differs without it (the callees' summaries are the plan's);
  - the BL sites whose call record changes because of it.

Example. Run `site 0x8009a474` before replacing the sleep call of `job_calc_runner`. It prints the BL to
`os_sleep_thread`; no other path into it; `live in: x0-x7,...`; that `job_calc_runner` is demoted (`tmp_after_call,
tmp_in`), so its callers reload everything and a row cannot make it worse; and the existing
`host/hos_thread.c:865 PATCH_AT 0x8009a474 instead hos_oot_calc_sleep`. It offers
`PATCH_AT(0x8009a474, word(0x94ef225b), instead, ...)` with the build's verdict.

## Undocumented

Read by code, but what they do is not stated beside the read (file:line of the first read):

- `HOS_AUDIO_CHANNELS` host/nn_audio.c:672
- `HOS_AUDIO_DEVICE` host/nn_audio.c:739
- `HOS_AUDIO_DUMP` host/nn_audio.c:764
- `HOS_AUDIO_IMMEDIATE` host/nn_audio.c:1007
- `HOS_AUDIO_WB_DUMP` host/nn_audio.c:1467
- `HOS_COND_RECORD` host/nvn_native.c:2109
- `HOS_DUMP_FILE` host/posix/prof.c:523
- `HOS_FIX_RES` host/hos_main.c:1527
- `HOS_FPS_TARGET` host/nn_vi.c:276
- `HOS_FS_KEEP_CORE_KB` host/nn_fs.c:417
- `HOS_HOSTCALL_SLOW` host/hos_hostcall.c:61
- `HOS_KEEP_FREED_PAGES` host/hos_mem.c:131
- `HOS_MOTION_DEBUG` host/pad_sdl.c:182
- `HOS_NVN_CHMASK_LOG` host/nvn_native.c:2626
- `HOS_NVN_HOLD` host/nvn_native.c:1177
- `HOS_NVN_NO_SWEEP` host/nvn_native.c:1741
- `HOS_NVN_REC_SHIFT` host/nvn_native.c:1937
- `HOS_NVN_SYNC_BLOCK` host/nvn_native.c:1223
- `HOS_OCB_NORMAL` host/hos_thread.c:762
- `HOS_PIN` host/hos_thread.c:752
- `HOS_PRESENT_POLL` host/hos_thread.c:976
- `HOS_PRIO` host/hos_thread.c:753
- `HOS_PROFILE_GATE` host/nn_os_sync.c:824
- `HOS_QUEUE_TRACE` host/nn_os_sync.c:929
- `HOS_REALTIME_WINDOW` host/nn_vi.c:314
- `HOS_RT_CHECK` host/rt_slots.c:387
- `HOS_RT_PARK` host/rt_slots.c:101
- `HOS_RT_PARK_FILL` host/rt_slots.c:280
- `HOS_RT_PARK_FRAMES` host/rt_slots.c:106
- `HOS_SCAN_DT` host/nn_vi.c:174
- `HOS_SDL_VERSION` host/hos_sdl.c:106
- `HOS_SHADER_DUMP` host/nvn_native.c:1110
- `HOS_SIZE_TRACE` host/nvn_native.c:629
- `HOS_SLEEP_LOG` host/hos_thread.c:937
- `HOS_TICK_LOG` host/hos_thread.c:1064
- `HOS_WAKE_LAT` host/nn_os_sync.c:636
- `HOS_WATCH` host/posix/prof.c:533
- `RX_NVN_WATCH_AFTER` host/nvn_native.c:934
- `RX_NVN_WATCH_ALL` host/nvn_native.c:936
- `RX_VK_FOLLOW_GRAPHICS` gpu/direct/eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1306
- `RX_VK_FOLLOW_MESH` gpu/direct/eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp:669
- `RX_VK_MESH_FIRST` gpu/direct/eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp:537
- `RX_VK_NO_FOLLOW` gpu/direct/eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp:669
- `HOS_DBG` items: `condcheck` gpu/direct/direct_query.cpp:360; `skipcs` gpu/direct/direct_compute.cpp:931; `sublog` gpu/direct/direct_sync.cpp:300; `meshlog` gpu/direct/direct_draw.cpp:1036; `replog` gpu/direct/direct_query.cpp:162; `meshcaplost, meshcapprog` gpu/direct/direct_meshcap.cpp:39; `memwatch, memfind` host/direct_texwatch.c:72; `dumpw` gpu/direct/direct_dump.cpp:22; `tilelog` host/direct_assets.c:151


## The options file

`hookshot_options.ini`, in the game folder, is read as `<key>=<number>` lines (`host/nn_applet.c`, `hos_ini_option`). The
direct back end has no menu that writes it; the host reads these keys: `output_res` (0 2048x1152, 1 2560x1440, 2 3840x2160,
3 1920x1080, 4 5120x1440), `handheld_profile` (0 or 1) and `frame_rate_cap` (an index into 60, 90, 120, 144, 165, 240), and
the Discord rich presence reads `discord_app_id`.

## Layout

| Path | What |
|---|---|
| `docs/` | `design.md` (architecture, MKW's lessons as rules, testing), `lifter.md` (the lifter), `direct-backend-plan.md` (the renderer), `direct/` (what we know about the game and the back end) |
| `nx2/` | the image, the lift and the build (`lift.py`, `build.py`; the lifter in `nx2/lift2`), the patch scan and its lowering (`kit.py` has the paths); no player builder for now: [builder-notes.md](builder-notes.md) |
| `host/` | the standalone Horizon runtime (TOTKPort's `hos/` as adapted by mkw-recomp), with the native NVN2 layer `nvn_native.c`; `host/posix` has its Win32-named calls on Linux (the host is Linux-only; a narrow host API over SDL replaces them later) |
| `gpu/direct/` | the direct back end (`hookshot_direct.so`), with its fork of Eden's shader recompiler and sirit in `eden/` |
| `gpu/api/` | what the host and the direct back end share: the versioned table (`vk_api.h`), the NVN objects and command records (`rx_nvn.h`) |
| `gpu/legacy/` | upstream's older renderers, reference source only: not built, not tested |
| `scripts/` | test runs, the profile and log readers, the save copier (`oot_saves.py`), `scripts/linux` (dependencies) |
| `tests/` | the game-free checks, all run by `tests/run_all.py` |
| `third_party/` | a64-kit's runtime, loader and tools (its lifter is gone; Hookshot's changes marked `(hookshot)`), libopus, zbic (Atmosphere's zstd with ZBIC, for unpacking only), SDL2's licence, Dear ImGui and its Vulkan backend, zstd's decoder, the Libertinus fonts |

License: GPL-3.0-or-later, as mkw-recomp: Eden's code is GPL-3.0.
