# Development guide

This page gets you from a dump of the game to a running build, and shows the loop you use while changing Hookshot:
what to rebuild after a change, how to run it, and where things live. Read it before your first build.

Hookshot lifts the game's AArch64 code to native x86-64 code, implements the console's operating system (Horizon) and
graphics API (NVN2) in `host/`, and replays the game's GPU work on Vulkan with the direct back end in `gpu/direct`. It
runs on Linux only, and the renderer is developed on Mesa RADV.

No game files are in this repository. The build reads your dump where it lies, and everything made from it goes to
`work/`, which git ignores and which never leaves your PC.

## Set up

1. Install the tools: clang and lld from LLVM 23 (the lifter refuses another major version), FFmpeg with dav1d, SDL2,
   and the Vulkan, SPIR-V and VMA headers. `scripts/linux/deps.sh` fetches and builds pinned versions into
   `build-linux/deps`.
2. Create a Python environment with the pipeline's packages:

   ```sh
   python3 -m venv work/venv
   work/venv/bin/pip install -r requirements.txt
   ```

   Run every Python tool with `-I` from the repository root, as `work/venv/bin/python -I ...`.
3. Extract your dump and record where it is:

   ```sh
   python -I nx2/nup.py <your-game.nup> --extract work/dump
   python -I nx2/dump.py --dump work/dump --set
   ```

   `nx2/dump.py` checks that the dump is the supported build: program `0400486024DEC000`, update v131072, build
   `e0f3ccfa`. Other versions are refused. `--set` writes the path to `work/dump.txt`.
4. If the headers are not under `/usr`, point the build at them with `HOOKSHOT_VULKAN_HEADERS`,
   `HOOKSHOT_SPIRV_HEADERS` and `HOOKSHOT_VMA_HEADERS` (a package root or its `include/`), or with `RX_CPM` for a CPM
   cache. Without them the build stops and says which header is missing.

A build needs about 60 GB of disk, and a running game about 10 GB of memory.

## Build and run

To lift the game and build it, run:

```sh
python -I nx2/lift.py --game games/oot --standalone --write
python -I nx2/build.py --game games/oot --standalone
```

`scripts/relift.sh` runs both for the checkout it is in. The build writes `work/sa/build/hookshot`,
`hookshot_direct.so` and the shader caches beside them. To run the game in a window:

```sh
work/sa/build/hookshot work/sa/image/image.bin --romfs work/dump/romfs --user work/user
```

The game's own arguments are `image.bin`, `--romfs <folder>`, `--user <folder>` (saves, caches and temporary files)
and `--trace` (log every Horizon call). The first line of `hookshot_log.txt` names the build.

## Iterate quickly

After a change, rebuild only what the change needs:

| You changed | Run |
|---|---|
| `host/` or `gpu/direct/`, including patch declarations | `nx2/build.py`: about a minute |
| Only the renderer | `RX_DIRECT_ONLY=1 nx2/build.py ...`: builds `hookshot_direct.so` alone |
| The lifter | `nx2/build.py`: every partition is lifted and compiled again |
| The lift, discovery, an `HLE` or `PATCH_ANSWER`, the hot list, or the dump | `scripts/relift.sh` |

The details, including what the object cache keeps, are in [Build and iterate on the lifter](lifter-build.md). A new
file in `host/` belongs in the `host` list of `games/oot/game.py`.

Then test the change in the game. [Test in the game](testing.md) covers the headless test run, the smoke test, the
benchmark and profiling.

To keep two builds side by side, copy `hookshot`, `hookshot_direct.so`, `hookshot.map` and the caches (`*.bin`) to
`work/sa/<name>/` and run with `EXE=work/sa/<name>/hookshot`.

### Work in an agent worktree

To work without disturbing the main checkout's build, run `scripts/agent-worktree.sh new <name>`. It creates
`work/wt/<name>` on branch `agent/<name>`. Its `work/` shares the main dump, image, runs, overlays, virtual
environment, locks and object cache, but has its own `sa/gen` and `sa/build`, so a host-only build is quick. Run with
`EXE=work/sa/build/hookshot` from inside it. `land <name>` rebases the branch onto the main checkout's branch and
fast-forwards it; `drop <name>` removes the worktree and the branch once it has landed.

### Play the current build

`scripts/play.sh [title|kokiri|hf|market|<zoo scene>]` plays the main checkout's build with window and sound, and
`scripts/play.sh record <name> ...` also records the input. `scripts/play-refresh.sh` keeps that build at the head of
the tracking branch and writes `work/sa/build/BUILD.txt` when it is done. It refuses a dirty tree or another branch.
`--install-hook` adds a local `post-merge` hook that refreshes in the background after each merge.

## Where things are

| Path | What it holds |
|---|---|
| `nx2/` | The dump tools, the lift and the build drivers; the lifter in `nx2/lift2` ([The lifter](lifter.md)) |
| `games/oot/` | The game description and the function labels |
| `host/` | The Horizon runtime, the NVN2 layer (`nvn_native.c`), and the game patches; `host/posix` maps its Win32-named calls to Linux |
| `gpu/direct/` | The direct back end, with its fork of Eden's shader recompiler in `eden/` ([Renderer docs](direct/README.md)) |
| `gpu/api/` | What the host and the back end share: the versioned interface (`vk_api.h`) and the NVN command records |
| `gpu/legacy/` | Upstream's older renderers, kept as reference source; not built or tested |
| `scripts/` | Test runs, the play build, log and profile readers, the save copier |
| `tools/` | Readers for the game's formats (`tools/dump`), reverse-engineering searches (`tools/re`), the mesh-shader workbench (`tools/meshlab`) |
| `tests/` | The game-free checks, all run by `tests/run_all.py` |
| `third_party/` | Upstream code with its own licenses: a64-kit's runtime and loader, Opus, zbic, SDL2's license, Dear ImGui, zstd, fonts |

## More pages

- [Test in the game](testing.md): test runs, smoke test, benchmarks, recordings, profiles.
- [Switches](switches.md): the environment switches for tests and debugging, and the log format.
- [Reverse engineering](reverse-engineering.md): function labels and the tools for reading the game's code and files.
- [Architecture](design.md): how the parts fit, and the rules the GPU replay keeps.
