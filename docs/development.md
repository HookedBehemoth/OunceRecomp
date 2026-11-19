# Development guide

This page gets you from a dump of a game to a running build, and says where everything goes. Read it before your first
build.

OunceRecomp lifts a game's AArch64 code to native x86-64 code, implements the console's operating system (Horizon) and
graphics API (NVN2) in `runtime/`, and runs the game's GPU work on Vulkan with the direct back end in `renderer/`. It
runs on Linux only, and the renderer is developed on Mesa RADV. One program, `ounce`, builds, runs and tests every game
that has a `games/<game>/game.toml`.

No game files are in this repository. The build reads your dump where it lies, and everything made from it stays on
your PC.

## Install

Install these from your distribution:

- clang and lld from LLVM 23 (the lifter refuses another major version), Rust, and ISPC (the texture encoder's
  compiler; `ISPC=<path>` picks another one, as `CC` does);
- Meson, Ninja and `glslangValidator`, for the renderer;
- the Vulkan headers, FFmpeg with dav1d, and SDL2 or SDL3.

The renderer needs a GPU with Vulkan 1.3 and shader objects, descriptor indexing and extended dynamic state 3. To
check yours, run `ounce probe` once `ounce` is built; [the device check](builder-plan.md#the-device-check) lists
what it checks.

For the Python tools, make a virtual environment with `python3 -m venv work/venv` and
`work/venv/bin/pip install -r requirements.txt`. Then build `ounce`:

```sh
cargo build --release -p ounce --manifest-path ounce/Cargo.toml
export PATH=$PWD/ounce/target/release:$PATH
```

A build needs about 60 GB of disk, and a running game about 10 GB of memory.

## Configure

Tell `ounce` where your dump is: a folder that holds `exefs/` and `romfs/`.

```sh
ounce config set oot.dump <folder>
```

To extract a dump file and record it in one step, run `ounce dump <file.nup> --extract <folder> --set`. `ounce dump`
names the game a dump is, and refuses a version the game does not support.

`ounce config` shows the whole file, `~/.config/ounce/config.toml`.

## Build and run

```sh
ounce build oot
ounce run oot
```

`ounce build` builds the renderer with Meson, then lifts the game, compiles it and links it, and prepares the game's
texture store from the dump. A rebuild does only what changed: the renderer through Ninja, the code through the object
cache, and the store only when the dump or the encoder changed. The first build of a game converts its textures,
which takes several minutes. `ounce run` opens a window with sound and
uses your own saves; it warns when the build is older than the checkout.

`ounce build <game> --shaders` also makes the game's shader pack and fills the game folder's shader caches with the
game's `--precompile`, as a player's game folder always is ([shaders](builder-plan.md#shaders)). Without it, the game
translates each program at its first draw.

On a shared PC, and always as an agent, pass `--exclusive` to `build`, `run` and `test`: it waits until no other build
or game run is going. See [Rules for agents](agents.md).

To test a change, see [Test OunceRecomp](testing.md). To work on the lifter, see
[Build and iterate on the lifter](lifter-build.md).

## Where things go

| Path | What it holds |
|---|---|
| `work/<game>/game/` | The game folder: the executable, `ounce_renderer.so`, `image.bin`, the shader caches, and `prepared/`, the texture store |
| `work/<game>/lift/` | The lift's files for the reverse-engineering tools, and the link map |
| `work/<game>/runs/<name>/` | One run: the log, the timeline and the screenshots |
| `work/renderer/` | The renderer's Meson build, shared by the games |
| `~/.cache/ounce/` | The object cache, shared by every checkout and game |
| `~/.local/share/ounce/` | Your saves, and per game the smoke tests' golden data and the input recordings |

A worktree needs no setup: `git worktree add` and `ounce build` are enough, because the config, the cache and the data
live outside the checkout. A worktree's first build copies the texture store from the main checkout's build of the
game when that one is current, which takes seconds and, on btrfs, almost no disk; it converts only when no current
store exists.

## Profile scenes

To see where a frame's CPU and GPU time goes in many scenes at once, run:

```sh
ounce profile oot --exclusive [--scene <name>]... [--size <w>x<h>] [--shaders]
```

The scenes are listed in `games/<game>/profile.toml`. Each scene is one headless benchmark run: the scene boots
straight into its map, waits until it has settled, and then measures 600 frames (`--frames` sets another number). The
first time a scene runs, `ounce profile` makes its overlay in the data folder's `<game>/profile/<scene>/`. The frame
size is the game folder's Output resolution when `ounce_options.ini` names one, and 2560x1440 otherwise.

For each scene the report gives:

- the frame time;
- the CPU time a frame in four parts, summed over threads: guest logic (the game's frame threads), guest draw (the
  game recording its command buffers), host draw (the renderer's calls and its own threads), and other;
- the GPU time a frame, busy and idle, by kind of work and by pass for the first 300 measured frames, with a line for
  everything outside the top passes.

`--shaders` adds a second short run per scene that times every draw of one frame. It names the costliest programs of
the costliest passes, with ACO's register counts, waves and instruction counts from the newest
[shader census](direct/shader-costs.md) (`--census <file>` picks another one).

The report is printed and written to `work/<game>/profile.txt`. Beside each scene's overlay, `<scene>.png` shows a
frame from the measured stretch, so you can check that the scene measured what you meant. To compare a change, run
the command again on the new build.

A headless run cannot take an RGP capture, because RADV ends a capture's frame at a window's present. To take one, run
the game in a window with `MESA_VK_TRACE=rgp` and `MESA_VK_TRACE_TRIGGER=<file>` set, and create that file in the scene
you want. RADV writes the capture to `/tmp`.

## Where the code is

| Path | What it holds |
|---|---|
| `ounce/` | `ounce` itself (`ounce/builder`), the lifter (`ounce/lift`), the window (`ounce/gui`) and the game-free checks (`ounce/tests`) |
| `runtime/` | Horizon, the NVN2 layer, and the hook points a game's patches fill; `runtime/os` is the operating-system layer, `runtime/media` the window, sound and controllers |
| `renderer/` | The direct back end ([Renderer docs](direct/README.md)) |
| `games/<game>/` | A game: its description, its patches, its labels and its smoke tests |
| `tools/` | Readers for the game's files (`tools/dump`), reverse-engineering tools (`tools/re`), the shader tools, and the run readers (`crit_path.py`, `launch_time.py`, `direct_picture_check.py`) and the save copier (`oot_saves.py`) |
| `third_party/` | Upstream code with its own licenses |
