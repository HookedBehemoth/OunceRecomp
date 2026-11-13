# Notes for AI coding agents

**Start with [docs/workflow.md](docs/workflow.md):** what to work on first (severity x reach x confidence, reach from
the hardware people run), the hardware tiers a change must not break, and the steps from claim to release. The open work, scored and sorted,
is in [docs/worklist.md](docs/worklist.md): pick from its top.

Follow [CONTRIBUTING.md](CONTRIBUTING.md): its rules apply to agents as to everyone, above all that no game files or
anything made from them leave the PC. The [development guide](docs/development.md) has the layout and the commands; the
[design notes](docs/design.md) have the architecture and the rules the GPU replay keeps.

What CONTRIBUTING.md does not spell out, and agents trip on:

- **Tests read the source.** Many checks in `tests/` cut a function out of a source file by its text and compile it
  with stubs. Renaming or reshaping a function can break a check far from your
  change: run `python -I -B tests/run_all.py` and update the check in the same PR rather than weakening it.
- **The back end's interface is versioned.** A change to `MkwVkHost` or `MkwVkRenderer` in `gpu/api/vk_api.h` bumps
  `MKWVK_API_VERSION`. `gpu/api/` is what the host and the direct back end share.
- **Generated files are generated.** The SPIR-V headers in `gpu/direct/shaders/` (`*_spv.h`) are compiled from the shader
  beside them; regenerate them with glslang rather than editing them.
- **`gpu/legacy/` is not built.** Upstream's Vulkan and WebGPU renderers, kept as reference source; no build, test or CI
  reaches them.
- **One renderer, one platform for it.** The direct back end (`gpu/direct`, Linux, RADV) is the only renderer. The host
  is built for Windows with llvm-mingw too: keep host code portable, and paths UTF-8
  (no `CP_ACP`; `tests/test_utf8_paths.py`).
- **No game in CI.** Checks must run without a dump. A run in the game is for you to do locally and report.

Pull requests: **open the description with why the PR is needed**, in one or two plain sentences, and **include
screenshots when the change shows on screen** (CONTRIBUTING.md, *Commits and pull requests*).
