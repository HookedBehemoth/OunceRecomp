# Test Hookshot

This page shows you how to check a change: the game-free checks that CI runs, and runs of the built game (a headless
run with screenshots, the smoke test, a benchmark, a recorded session, and a profile). Read it before you land a
change.

## Run the game-free checks

These need no dump, and CI runs them. Every check must run without the game: a run in the game is something you do
locally and report. To run them all:

```sh
python -I -B tests/run_all.py
```

It runs every `tests/test_*.py` and `nx2/test_*.py` in its own process, byte-compiles every tracked Python file, goes
on after a failure, and ends with a table (slowest first) and each failure's output. To run only some checks, name part
of their path: `python -I -B tests/run_all.py lift2`.

Many checks read source text: they cut a function out of a source file and compile it with stubs. A rename or a
reshaped function can break a check far from your change. Update the check in the same change rather than weakening
it.

Some checks need a compiler, `spirv-val`, the SPIR-V or Vulkan headers (`HOOKSHOT_SPIRV_HEADERS`,
`HOOKSHOT_VULKAN_HEADERS`, or `RX_CPM`), or the `zstandard` package. A check whose tool is missing is skipped and says
so; `--strict` (as in CI) fails it instead. The skip list at the top of `tests/run_all.py` names the checks that cannot
run in CI; `--no-skip` runs them anyway.

To add a check, write a small `tests/test_*.py` or `nx2/test_*.py` that reads source files and runs them, or compiles a
tiny C or C++ fixture; `tests/run_all.py` picks it up. Keep it on the standard library (the pipeline's own packages
aside) and fast: all checks share one CI job with a 20-minute limit. A check that takes tools as arguments gets an
entry in `ARGS` at the top of `tests/run_all.py`; one that cannot run in CI gets an entry in `SKIP`, with the reason.

## Run the game

All runs need a build ([Development guide](development.md)) and the dump. Everything a run writes is made from the
game and stays in `work/`.

### Run one game at a time

A game holds about 10 GB of guest memory in a memfd shared with the GPU, which RSS does not show, and a GPU reset kills
every run beside it. The run scripts take a GPU lock themselves; do not wrap them in `flock`:

- `RUN=timing` takes the lock exclusively. Use it for frame rates, launch times, and anything that may hang the GPU.
- `RUN=check` shares the lock with at most `RUN_SLOTS` runs (default 1). Each runs on a private copy of the build
  folder and waits for `RUN_MEM_GB` (default 14) of free memory.

After `VK_ERROR_DEVICE_LOST`, `journalctl -k` names the ring and thread that hung.

### Make a test run

To run the build headless and muted, run:

```sh
scripts/run-vk.sh <name> <seconds> [-- NAME=VALUE ...]
```

The run writes `work/runs/<name>/`: `shots/` (a screenshot every `SHOT_EVERY` presents, default 300), `log.txt`,
`stdout.txt` and `timeline.txt` (second, presents, RSS). Arguments after `--` are switches for the game
([Switches](switches.md)). Other settings:

| Setting | What it does |
|---|---|
| `EXE=<path>` | Runs another build |
| `OVERLAY=work/overlay/<dir>` | Reads patched game files first, for example a chiseled boot |
| `ZOO=<scene>` | Uses an actor-zoo scene as the overlay |
| `USER_FROM=<folder>` | Starts from a copy of that save folder |
| `WINDOW=1`, `SOUND=1` | Turns the window or the sound on |

The first run of a build folder is cold: shaders and pipelines are prepared. Copy a warm folder's caches before you
compare timings. Delete the screenshots once you have looked at them; they reach gigabytes.

### Start in a scene

The title menu and the opening take minutes. A *chiseled boot* skips them: patched sequence graphs go straight into a
map. To make one, run:

```sh
python -I tools/dump/make_chisel_overlay.py work/overlay/chisel_kokiri --route newgame --banc KokiriForest_Child
```

`--route load [--slot N]` continues a save instead, `--start <name>` picks a start position, and `--skip-title` and
`--skip-fades` cut the title movie and the loading fades. The new-game route saves, so run it with the script's own
user folder. Maps that work include `KokiriForest_Child`, `HyruleField_Child`, `HyruleField_Adult`,
`KakarikoVillage_Child` and `InsideTheDekuTree`.

An *actor zoo* is a scene described in `zoo/<scene>.toml`: an existing map, a start position, which of its actors stay,
and actors to add. `tools/dump/make_zoo.py` turns it into an overlay; `ZOO=<scene>` and `scripts/play.sh <scene>` make
it on demand. A zoo scene keeps the base map's collision, navigation and baked lighting.

### Run the smoke test

The smoke test answers one question: did anything break? It stands in one scene for `SECS` (40) seconds and prints a
verdict of about ten lines.

1. Once per build, with no switches, record the golden run:

   ```sh
   GOLDEN=1 scripts/smoke.sh <name> kokiri
   ```

2. Run the test, with or without switches:

   ```sh
   scripts/smoke.sh <name> kokiri [-- NAME=VALUE ...]
   ```

The verdict lists fatal log lines, screenshots whose perceptual hash differs from the golden run's by more than 12 of
64 bits, rough numbers beside the golden run's (frame rate, replay times, memory), and warning groups that are not in
`work/smoke/<scene>/known.txt`. Read the verdict, not the log. `KEEP_SHOTS=1` keeps the screenshots.

### Benchmark a scene

To measure frame times, run:

```sh
scripts/bench.sh <scene> [frames] [-- NAME=VALUE ...]
```

`<scene>` is `kokiri`, `hf`, `market`, an actor-zoo scene, or an overlay folder's name; the default is 600 frames. The
game steps exactly 1/60 s a frame and presents as fast as it can, so two runs simulate the same game time whatever
their speed. Measuring starts once the scene has been settled for 60 frames in a row: Link in play, no load, no
shader or texture work pending, no file reads. The summary gives when the scene was ready, the frames' mean, median,
p99 and maximum, draws a frame, and CPU seconds by thread group. `work/runs/<name>/bench.csv` has a row per frame.

The `vk: <n> presents, <x> fps` line in any run's log gives the frame rate of an ordinary run.

### Record and replay a session

To record, start the game with `HOS_INPUT_RECORD=$PWD/work/rec/<name>` (an absolute path), or use
`scripts/play.sh record <name>`. The save folder is copied to `work/rec/<name>/user`, and every pad state the game
reads goes to `work/rec/<name>/input.rec`.

To replay it headless from that save, run `scripts/replay.sh <name> <seconds>`. Lines match the game's pad reads, so a
load that takes longer in the replay still shifts the timing: release the controls during loads, and wait a second
after one.

For short scripted input, set `HOS_INPUT_SCRIPT` ([Switches](switches.md#drive-the-game-with-scripted-input)).

### Profile the CPU

- **One thread:** run with `HOS_PROFILE=<seconds> HOS_SAMPLE=<thread> HOS_SAMPLE_FILE=<file>`, then read the file with
  `python -I scripts/prof.py <file>`.
- **All threads:** run with `HOS_PROFILE=5 HOS_SAMPLE_ALL=<file>`; sampling runs while `<file>.go` exists. Read it with
  `RX_MAPS=work/sa/build python -I scripts/crit.py <file>`. `scripts/hotlist.py` makes the hot list from the same file.
- **Launch time:** `python -I scripts/launch_time.py work/runs/<name>` says when gameplay began and when it was
  playable.
