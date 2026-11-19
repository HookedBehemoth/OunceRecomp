# Settings

This page explains how the game's settings are given and kept, lists the ones you use most when you test and debug
the game, and describes the log format. The full list is the table in `renderer/api/settings.h`, and the game prints
it.

## How settings are given

Every setting is a typed key in one table (`renderer/api/settings.h`): a bool, an int, a float, an enum (one of named
choices) or a string, with its default and its range or choices. `runtime/settings.c` is their one reader and writer;
the runtime, the renderer and the game patches ask it through `setting_bool`, `setting_int` and their siblings, and
nothing reads the environment for a setting.

- **User settings** are kept in the options file: `ounce_options.ini` beside the game, or the file that `--options
  <file>` names. A line is `<key>=<value>`, and an enum takes its choice's name or number. The game's Options screen
  shows some of them (OoT's Frame Rate row) and writes a change back into the file, with the file's other lines kept.
- **Dev settings** are given on the command line only, as `--set <key>=<value>`, and never kept. `--set` also sets a
  user setting for one run without writing it.

A value the setting does not take stops the start when it comes from `--set`, and is reported and left out when it
comes from the file. The log's first lines name every setting that is not at its default, and where it came from.

To list every setting, its value now, its default and what it does, run the game with `--list-settings`:

```sh
"work/oot/game/Ocarina of Time" --list-settings
```

`ounce run` and `ounce test` pass `--set <key>=<value>` through. A headless run, a test and a replay get an options
file of their own in the run folder, so they never change yours; a window run uses the game folder's.

## Common settings

| Setting | What it does |
|---|---|
| `frame_rate=<30, 60, 120, vsync>` | The frame-rate cap (user setting, the Options screen's Frame Rate row, live). `vsync`: the display's refresh rate, presented in step with it. Above 60 the game is told the display has a variable refresh |
| `render_scale=<50%..200%>` | The 3D's resolution against the output (user setting, the Render Resolution row). Below 100% the upscaler takes it up; above 100% and above the value at the start it applies at the next start, as the render-target pool is sized then |
| `upscaler=<game, bilinear, catmull-rom>` | What takes the 3D to the output (user setting, the Upscaler row, live). The game's network only up to 100%: the row offers it only then |
| `output_res=<choice>` | A fixed output resolution (user setting, at the next start); without one the frame follows the window |
| `headless=1` | No window: the renderer draws offscreen |
| `window_size=<w>x<h>` | The window's size (by default 720 high at the monitor's shape), or the headless image's (by default 1920x1080). The game's frame follows the window's pixels |
| `audio=0` | Opens no sound device |
| `aspect=<ratio>` | A fixed picture shape instead of the window's; wider than 16:9 the HUD, menus and movies keep the middle 16:9 |
| `fs.overlay=<folder>` | Patched game files read before the RomFS (a chiseled boot) |
| `trace=1` | Logs every Horizon call |
| `bench=<frames>[,<ready frames>[,<GPU frames>[,noplay]]]` | A benchmark run (`ounce run --bench` sets it). `<GPU frames>`: the GPU timed by pass over the first measured frames (`passtime.csv`); `noplay`: ready without the player in play (`ounce profile` sets both) |
| `shots=<dir>`, `shot_every=<n>` | Saves a PNG of the presented frame every `n` presents, in the frame's shape with a long side of 1280 |

## Drive the game with scripted input

| Setting | What it does |
|---|---|
| `input.script=<presses>` | Presses timed from the game's first controller read |
| `input.live=<file>` | Reads lines of presses that a test appends, every 30 ms |
| `input.exclusive=1` | The script alone drives the game until 5 s after its last press; without it your devices add to the script |
| `input.frame_clock=<off, presents, game>` | The script's time: the wall clock, 60 presents a second, or the game's clock |
| `input.record=<dir>`, `input.replay=<dir>/input.rec` | Records or replays a session ([Test in the game](testing.md#record-and-replay-a-session)) |

A press is `<seconds>:<button>[+<button>...][/<ms>]`, and presses are separated by commas, for example
`2:A,5.5:LUP/1500,9:ZL+R`. The buttons are `A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN`, and the sticks
pushed to their edge are `LUP LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT`. A press lasts 200 ms unless `/<ms>` says
otherwise. Names are case-sensitive.

Scripted input reaches the game even when its window is not in front; a log line saying so does not mean the presses
were lost. The game must still be in a state that reads the controller.

## Turn on debug aids

The `debug.*` settings turn on debug aids of the renderer, the host and the game patches. Each costs a load and a
branch while off. These are the ones you reach for first:

| Setting | What it does |
|---|---|
| `debug.validation=1` | The Khronos validation layer, with synchronization validation |
| `debug.strict=<present>` | Any validation warning or error from that present on ends the run |
| `debug.logall=1`, `debug.log<part>=1` | Brings back the detailed per-block log lines (`logmesh`, `logpass`, `logdraw`, `loglists`, `logcompute`, `logpatch`, `lognvn`, `logvi`) |
| `debug.dump=<present>[+<frames>]` | Saves every pass's targets of the frame after that present as PNGs |
| `debug.frames=1`, `debug.gputime=1` | `frames.csv`: a row per present, with the GPU's busy and idle time |
| `debug.marks=1` | GPU breadcrumbs for a lost device: the unfinished draws are logged |
| `debug.crit=<first>[:<presents>]` | A trace of every guest thread over a window of presents, for `tools/crit_path.py` |
| `debug.resize=<w>x<h>@<present>[+...]` | Resizes the window (or the headless image) at that present, as a drag would |
| `debug.noassets=1` | Turns off the prepared-asset patches, for a before-and-after comparison |

The `log.*` settings each turn on one subsystem's log lines (`log.fps`, `log.fs`, `log.sleep` and the others).

## Read the log

Every line of `ounce_log.txt` (or a test run's `log.txt`) starts with `[hos <seconds> p<present> <thread>]`.
`log.time=0` leaves out the time and present. A line longer than 4 KB ends in `~`.

- The first line names the build as `<version> (source <hash>)`. The hash is a fingerprint of `runtime/`, `renderer/` and
  `games/`, so it tells apart two builds of one commit. The lines after it name the settings not at their default.
- `settings: <key> = <value>, kept in <file>` means the Options screen changed a user setting.
- `vk: <n> presents, <x> fps` gives the frame rate.
- `FATAL: the game threw a C++ exception of type <name> from <address>` means a throw stopped the game: the lifted code
  cannot unwind ([What the lifter emits](lifter-output.md#fibers-and-c-exceptions)).
- A line naming an unimplemented import or NVN proc means the game called something the host lacks
  ([Write a patch](patches.md#calls-that-stop-the-game)).
- `ring owner: ring N pushed by <A> and <B>` means a second thread wrote one of the game's per-core rings, which the
  game updates without a lock. It is an assertion that the host's threading keeps the game's assumption.
