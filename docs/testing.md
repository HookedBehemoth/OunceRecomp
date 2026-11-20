# Test OunceRecomp

This page shows you how to check a change: the game-free checks, and runs of the built game (a headless
run with screenshots, the smoke test, a benchmark, a recorded session, and a profile). Read it before you land a
change.

## Run the game-free checks

These need no dump. Every check must run without the game: a run in the game is something you do locally and report.
To run them all, run:

```sh
cargo test --manifest-path ounce/Cargo.toml
```

This runs the lifter's and the builder's unit tests and the checks in `ounce/tests`, in parallel. To run some checks
only, name part of their path, for example `cargo test --manifest-path ounce/Cargo.toml -p ounce-tests frame::`.

`ounce/tests` holds one test binary with a module per area:

| Module | What it checks |
|---|---|
| `audio` | The mixer's deadline, the output section, the voices' filters and wave buffers, which SDL the sound loads |
| `frame` | Frame pacing above 60 fps, the benchmark mode, the movie count, the widescreen shapes, DLSS's camera math |
| `input` | Key bindings, rumble, motion, the mouse camera, the sticks' rest, headless shutdown |
| `nvn` | The NVN layer's pools, buffers, textures and host tables, the render-target slots, host copies into guest memory |
| `os` | The operating-system layer, the guest FPCR, `setjmp` calls, threads' cores, mutexes and multi-waits, the file system's links |
| `renderer` | The claims' regions, strict mode, the register-pressure pass, Sirit's SPIR-V, the translator's subgroup IR operations |
| `host_reads` | What the host reads of the guest context against what its patch declarations say |
| `source` | Rules read from the source text, such as UTF-8 paths |
| `python` | The Python tools' own tests (`ounce/tests/py`) and a byte-compile of every tracked Python file |

Most checks read source text: they cut a function out of a source file, put it into a small C or C++ fixture
(`ounce/tests/fixtures`) and compile and run it. A rename or a reshaped function can break a check far from your change.
Update the check in the same change rather than weakening it.

The checks need `clang` and `clang++` (or `CC` and `CXX`), `python3` with `numpy` and `zstandard`, `spirv-val`, and
`glslangValidator` with `spirv-opt` for one stage of the register-pressure check. A missing tool fails the check that
needs it. The SPIR-V headers are `OUNCE_SPIRV_HEADERS`, by default `third_party/SPIRV-Headers/include`.

To add a check, add a `#[test]` to the module of its area. Use the helpers in `ounce/tests/src/lib.rs` to cut source
text, compile a fixture, and run it. A check of a Python tool stays in Python, in `ounce/tests/py`, and gets a
one-line test in `python`. Keep checks fast: the whole run takes seconds.

## Run the game

All runs need a build ([Development guide](development.md)) and the dump. Everything a run writes is made from the
game and stays on your PC.

### Run one game at a time

A game holds about 10 GB of guest memory in a memfd shared with the GPU, which RSS does not show, and a GPU reset kills
every run beside it. Pass `--exclusive` to `ounce run` and `ounce test`: it waits until no other build or game run is
going, and a run also waits until `limits.run_free_mem_gb` (default 14) GB of memory is free. Do not wrap `ounce` in
`flock`. After `VK_ERROR_DEVICE_LOST`, `journalctl -k` names the ring and thread that hung; `ounce run` prints the
kernel's GPU reset lines itself.

### Make a test run

To run the build headless and muted, run:

```sh
ounce run oot --headless --seconds <n> --exclusive [--set NAME=VALUE ...]
```

The run writes `work/oot/runs/headless/` (`--name` picks another folder): `shots/` (a screenshot every `--shots`
presents, default 300), `log.txt`, `stdout.txt`, `timeline.txt` (second, presents, RSS) and `mem.txt` (free and shared
memory). A headless run keeps its saves in the run folder. `--build <folder>` runs a player's game folder (one
made with `ounce build <game> --out <folder> --dump <dump>`) with its own `romfs/`. `--set` passes a switch to the game
([Switches](switches.md)).

The first run of a build is cold: shaders and pipelines are prepared. Run once more before you compare timings.
Delete the screenshots once you have looked at them; they reach gigabytes.

### Start in a scene

The title menu and the opening take minutes. A *chiseled boot* skips them: patched sequence graphs go straight into a
map. To make one and run it, run:

```sh
work/venv/bin/python -I tools/dump/make_chisel_overlay.py work/overlay/chisel_kokiri --route newgame --banc KokiriForest_Child
ounce run oot --set fs.overlay=$PWD/work/overlay/chisel_kokiri
```

`--route load [--slot N]` continues a save instead, `--start <name>` picks a start position, and `--skip-title` and
`--skip-fades` cut the title movie and the loading fades. The new-game route saves, so run it headless, with the run's
own saves. Maps that work include `KokiriForest_Child`, `HyruleField_Child`, `HyruleField_Adult`,
`KakarikoVillage_Child` and `InsideTheDekuTree`.

An *actor zoo* is a scene described in `games/oot/zoo/<scene>.toml`: an existing map, a start position, which of its
actors stay, and actors to add. `work/venv/bin/python -I tools/dump/make_zoo.py <scene>` makes its overlay in
`work/overlay/<scene>` when it is missing or out of date, and prints the folder. A scene that sets the time of day
also needs `--set input.live=<folder>/live.txt`. A zoo scene keeps the base map's collision, navigation and baked
lighting.

Both tools raise the game's resource size table entry of every file they make larger. If you patch a game file by
another way, raise its entry too (`tools/dump/rstb.py`): the game sizes the file's heap from that table, and a
larger file overflows it.

### Run the smoke test

The smoke test answers one question: did anything break? It stands in one scene and prints a verdict of about ten
lines. The tests are in `games/<game>/tests.toml`; each names its length and its settings, and a test that starts in a
scene keeps its overlay beside its golden data.

1. Once per build, with no switches, record a golden run. Record two or three for a scene that moves while you stand
   still.

   ```sh
   ounce test oot --golden --exclusive
   ```

2. Run the test, with or without switches:

   ```sh
   ounce test oot [--name <test>] --exclusive [--set NAME=VALUE ...]
   ```

The verdict lists fatal log lines, screenshots whose perceptual hash differs from every golden run's by more than 12
of 64 bits or whose colours differ in 8 or more of 144 cells (a 16 x 9 thumbnail, a cell off by more than 12 in a
channel), rough numbers beside the golden run's (frame rate, replay times, memory), and warning groups that are not
in the test's `known.txt`. It ends with `VERDICT: PASS` or `VERDICT: FAIL (<why>)`, and `ounce test` exits with 1 on a
failure. Read the verdict, not the log. The colours catch a layer that goes without changing the
picture's shapes, such as Kokiri's grass, which the hash misses; golden runs recorded before the thumbnails have none
and are compared by hash only. `--keep-shots` keeps the screenshots, and `--golden-reset` starts the golden
set anew. When a run fails on a view the golden set lacks (the scene moved another way) and its screenshots are right,
`ounce test <game> --golden-kept` adds that kept run to the set without a new run. The golden data is in `~/.local/share/ounce/<game>/golden/<test>/`.

### Benchmark a scene

To measure frame times, run:

```sh
ounce run oot --headless --seconds 110 --bench 600 --exclusive --set fs.overlay=<overlay folder>
```

The game steps exactly 1/60 s a frame and presents as fast as it can, so two runs simulate the same game time whatever
their speed. Measuring starts once the scene has been settled for 60 frames in a row: Link in play, no load, no
shader or texture work pending, no file reads. The summary gives when the scene was ready, the frames' mean, median,
p99 and maximum, draws a frame, and CPU seconds by thread group. The run folder's `bench.csv` has a row per frame.

The `vk: <n> presents, <x> fps` line in any run's log gives the frame rate of an ordinary run.

To measure many scenes at once, with the CPU and GPU time split by part and by pass, see
[Profile scenes](development.md#profile-scenes).

### Record and replay a session

To record, run `ounce run oot --record <name>`. The save folder is copied to the recording, and every pad state the
game reads goes to its `input.rec`, in `~/.local/share/ounce/<game>/rec/<name>/`.

To replay it headless from that save, run `ounce run oot --replay <name> --headless --seconds <n> --exclusive`. Lines
match the game's pad reads, so a load that takes longer in the replay still shifts the timing: release the controls
during loads, and wait a second after one.

For short scripted input, set `input.script` ([Switches](switches.md#drive-the-game-with-scripted-input)).

### Profile the CPU

- **Threads and stacks:** use a system profiler.
- **Launch time:** `python -I tools/launch_time.py work/<game>/runs/<name>` says when gameplay began and when it was
  playable.
