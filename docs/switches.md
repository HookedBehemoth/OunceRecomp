# Switches

This page lists the environment switches you use most when you test and debug the game, explains the debug items of
`HOS_DBG`, and describes the log format. It also tells you how to find the rest: the code reads many more switches
than this page lists, and the comment beside each read says what it does.

## Set a switch

The game reads its switches from the environment, or from `ounce_env.txt` beside `image.bin` (one `NAME=VALUE` a
line, `#` for notes). `HOS_ENV_FILE=<path>` reads another file instead. `ounce_env.txt` is a file players pass
around, so it accepts only the game's own prefixes (`HOS_`, `RX_`, `OUNCE_` and a few others) and never a switch
that names a file to write. The test scripts pass switches after `--`.

Runtime switches start with `HOS_`, renderer switches with `RX_`, build switches with `OUNCE_` or `RX_`. A switch is
off, or neutral, unless set, and costs nothing while off.

To find a switch that this page does not list, search the code for its prefix:

```sh
git grep -n 'getenv("HOS_' -- runtime
git grep -n 'getenv("RX_' -- runtime renderer
```

The build's own switches are in [Build and iterate on the lifter](lifter-build.md#lifter-switches) and in
`ounce help`.

## Common runtime switches

| Switch | What it does |
|---|---|
| `HOS_HEADLESS` | No window: the renderer draws offscreen |
| `HOS_WINDOW_SIZE=<w>x<h>` | The window's size (by default 720 high at the monitor's shape), or the headless image's (by default 1920x1080). The game's frame follows the window's pixels |
| `HOS_AUDIO=0` | Opens no sound device |
| `HOS_OUTPUT_RES=<0..4>` | A fixed output resolution, over the options file; without one the frame follows the window |
| `HOS_ASPECT=<ratio>` | A fixed picture shape instead of the window's; wider than 16:9 the HUD, menus and movies keep the middle 16:9 |
| `HOS_FS_OVERLAY=<folder>` | Patched game files read before the RomFS (a chiseled boot) |
| `HOS_NO_MOVIES` | Reports movie files as missing, so the game goes on without them |
| `HOS_TRACE=1` | Logs every Horizon call, like `--trace` |
| `HOS_FPS_MAX=<fps>` | The frame-rate cap (60 to 240), over the option; above 60 frames are paced up to the display's refresh rate and the game is told the display has a variable refresh |
| `HOS_BENCH=<frames>[,<ready frames>[,<GPU frames>[,noplay]]]` | A benchmark run (`ounce run --bench` sets it). `<GPU frames>`: the GPU timed by pass over the first measured frames (`passtime.csv`); `noplay`: ready without the player in play (`ounce profile` sets both) |
| `RX_VK_SHOTS=<dir>`, `RX_VK_SHOT_EVERY=<n>` | Saves a PNG of the presented frame every `n` presents, in the frame's shape with a long side of 1280 |

## Drive the game with scripted input

| Switch | What it does |
|---|---|
| `HOS_INPUT_SCRIPT=<presses>` | Presses timed from the game's first controller read |
| `HOS_INPUT_LIVE=<file>` | Reads lines of presses that a test appends, every 30 ms |
| `HOS_INPUT_EXCLUSIVE` | The script alone drives the game until 5 s after its last press; without it your devices add to the script |
| `HOS_INPUT_FRAME_CLOCK=1` | The script's time runs at 60 game frames a second instead of the wall clock |
| `HOS_INPUT_RECORD=<dir>`, `HOS_INPUT_REPLAY=<dir>/input.rec` | Records or replays a session ([Test in the game](testing.md#record-and-replay-a-session)) |

A press is `<seconds>:<button>[+<button>...][/<ms>]`, and presses are separated by commas, for example
`2:A,5.5:LUP/1500,9:ZL+R`. The buttons are `A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN`, and the sticks
pushed to their edge are `LUP LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT`. A press lasts 200 ms unless `/<ms>` says
otherwise. Names are case-sensitive.

Scripted input reaches the game even when its window is not in front; a log line saying so does not mean the presses
were lost. The game must still be in a state that reads the controller.

## Turn on debug aids with HOS_DBG

`HOS_DBG=<item>,<item>:<value>,...` turns on debug aids of the renderer and the host. Each costs a load and a branch
while off. These are the ones you reach for first:

| Item | What it does |
|---|---|
| `validation` | The Khronos validation layer, with synchronization validation |
| `strict` | Any validation warning or error ends the run |
| `logall`, `log<part>` | Brings back the renderer's detailed per-block log lines (`logmesh`, `logpass`, `logdraw`, `loglists`, `logcompute`, `logpatch`, `lognvn`, `logvi`) |
| `dump:<present>[+<frames>]` | Saves every pass's targets of the frame after that present as PNGs |
| `frames`, `gputime` | `frames.csv`: a row per present, with the GPU's busy and idle time |
| `marks` | GPU breadcrumbs for a lost device: the unfinished draws are logged |
| `crit[:<first>[:<presents>]]` | A trace of every guest thread over a window of presents, for `tools/crit_path.py` |
| `resize:<w>x<h>@<present>[+...]` | Resizes the window (or the headless image) at that present, as a drag would |
| `noassets` | Turns off the prepared-asset patches, for a before-and-after comparison |

To find every item, search `runtime/` and `renderer/` for `dbg_has`, `dbg_log` and `hos_dbg_item`.

## Read the log

Every line of `ounce_log.txt` (or a test run's `log.txt`) starts with `[hos <seconds> p<present> <thread>]`.
`HOS_LOG_TIME=0` leaves out the time and present. A line longer than 4 KB ends in `~`.

- The first line names the build as `<version> (source <hash>)`. The hash is a fingerprint of `runtime/`, `renderer/` and
  `games/`, so it tells apart two builds of one commit.
- `vk: <n> presents, <x> fps` gives the frame rate.
- `FATAL: the game threw a C++ exception of type <name> from <address>` means a throw stopped the game: the lifted code
  cannot unwind ([What the lifter emits](lifter-output.md#fibers-and-c-exceptions)).
- A line naming an unimplemented import or NVN proc means the game called something the host lacks
  ([Write a patch](patches.md#calls-that-stop-the-game)).
- `ring owner: ring N pushed by <A> and <B>` means a second thread wrote one of the game's per-core rings, which the
  game updates without a lock. It is an assertion that the host's threading keeps the game's assumption.

## The options file

`ounce_options.ini` beside the game holds `<key>=<number>` lines. The host reads `output_res` (0 to 4, as
`HOS_OUTPUT_RES`), `handheld_profile` (0 or 1) and `frame_rate_cap` (an index into 60, 90, 120, 144, 165, 240).
