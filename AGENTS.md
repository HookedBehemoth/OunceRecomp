# Notes for AI coding agents

**Start with the [development guide](docs/development.md)** (set up, build, the loop in "Iterate quickly"), the
[architecture](docs/design.md), [the lifter](docs/lifter.md) (how the game's code becomes native code, and
[how to write a patch](docs/patches.md)) and the [direct back end plan](docs/direct-backend-plan.md) (the one renderer).
The [documentation index](docs/README.md) lists every page.

Follow [CONTRIBUTING.md](CONTRIBUTING.md): its rules apply to agents as to everyone, above all that no game files or
anything made from them leave the PC.

What CONTRIBUTING.md does not spell out, and agents trip on:

- **Tests read the source.** Many checks in `tests/` cut a function out of a source file by its text and compile it
  with stubs. Renaming or reshaping a function can break a check far from your change: run
  `python -I -B tests/run_all.py` and update the check in the same PR rather than weakening it.
- **The back end's interface is versioned.** A change to `MkwVkHost` or `MkwVkRenderer` in `gpu/api/vk_api.h` bumps
  `MKWVK_API_VERSION`. `gpu/api/` is what the host and the direct back end share.
- **Generated files are generated.** The SPIR-V headers in `gpu/direct/shaders/` (`*_spv.h`) are compiled from the
  shader beside them; regenerate them with glslang rather than editing them.
- **`gpu/legacy/` is not built.** Upstream's Vulkan and WebGPU renderers, kept as reference source; no build, test or
  CI reaches them.
- **One renderer, one platform.** The direct back end (`gpu/direct`, Linux, RADV) is the only renderer, and the host is
  built for Linux only. Its Win32-named calls go through `host/posix` (kept as it is for now); portability comes back
  later through a narrow host API over SDL, not through more Windows APIs. Paths stay UTF-8 (no `CP_ACP`;
  `tests/test_utf8_paths.py`). No player builder for now: [docs/builder-notes.md](docs/builder-notes.md).
- **The game is a parameter.** `nx2/lift.py`, `nx2/build.py` and the lifter's tools take `--game games/oot`
  (`nx2/gamedesc.py`). A fact of the game goes into `games/oot/` (`game.py`, `labels.txt`: add a label for every
  function an analysis identifies, [docs/reverse-engineering.md](docs/reverse-engineering.md)) or into a patch
  declaration in the host's C, never into `nx2/lift2` (`tests/test_lifter_game_free.py`).
- **Docs are guides.** A doc page says what a part is for, how to do a task, and what to expect; it does not narrate
  code, list what a tool prints, or keep history. Removed features are recorded in
  [docs/parked-patches.md](docs/parked-patches.md).
- **No game in CI.** Checks must run without a dump. A run in the game is for you to do locally and report.

Pull requests: **open the description with why the PR is needed**, in one or two plain sentences, and **include
screenshots when the change shows on screen** (CONTRIBUTING.md, *Commits and pull requests*).
