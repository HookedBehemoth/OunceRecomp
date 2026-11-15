# Contributing to Hookshot

This page covers what to send, how to build and test it, and the rules that keep the project out of trouble. Read it
before you open an issue or a pull request.

Hookshot is a static recompilation of the Switch 2 *The Legend of Zelda: Ocarina of Time*: the game's AArch64 code is
translated to native code, Horizon OS and NVN2 are implemented natively, and the GPU work runs on Vulkan through one
renderer, the direct back end. No emulator is involved, and the game is built on your PC from your own copy. The tree
targets Linux only, with Mesa RADV; other platforms come back later through a narrow host API over SDL. Paths are UTF-8.

The [development guide](docs/development.md) has the pipeline and its commands, and the
[documentation index](docs/README.md) lists every page.

## The rule with no exceptions

**No game files, keys, or anything made from them leave your PC.** The repository does not contain the game and
never will. That includes:

- the `.nup`, `romfs`, `exefs`, `.nso`, `.nca`, `.nsp`, `.xci` or `.nsz`/`.xcz` files, and any title keys;
- `work/` and everything in it: the extracted dump, `image.bin`, the lift's plan, test builds, runs and screenshots;
- `hookshot`, `hookshot_direct.so`, the shader caches beside them (`*.bin`) and other build output;
- saves, `hookshot_options.ini`, `hookshot_env.txt` and `language.txt`.

Do not open an issue or a PR asking where to get the game, how to decrypt it, or for someone else's files. A
screenshot, a video and `hookshot_log.txt` are fine and often necessary; the log holds no game data.

`.gitignore` covers most of this, but check `git status` before you commit. If you are not sure whether a file is
game data, leave it out.

## Ways to help

| | What |
|---|---|
| **Test on other hardware** | Reports from other cards and distributions are useful even when everything works: give the build (first line of `hookshot_log.txt`), the hardware and driver, where in the game, and the frame rate from the log's `vk: <n> presents, <x> fps` line. |
| **Play further** | New areas, cutscenes, menus and items find problems the developer has not seen. |
| **Report what is broken** | Use the [issue forms](https://github.com/NotRexed/Hookshot/issues/new/choose). Include the version, your hardware, where you were and `hookshot_log.txt`. |
| **Send a fix** | Fork, fix, test, open a pull request. Small, focused changes are easiest to review. Search the open PRs first, and say in Discord what you take. |
| **Improve the docs** | Corrections and clearer steps are welcome. |

## Before you open an issue or a PR

- Search the open issues. If yours is already there, add your details instead of opening another.
- Write in English.
- Keep to one topic per issue or pull request. Do not mix a fix with a refactor or with unrelated changes.
- Report the version you actually ran: the first line of `hookshot_log.txt`.
- Test without ReShade, shader-cache tools and overlays when you report a graphics or performance problem. They change
  what Hookshot does.

## Set up and build

You need your own supported dump: program `0400486024DEC000`, update v131072, build `e0f3ccfa`. Builds need about 60 GB
free, and a run uses about 10 GB of RAM. The [development guide](docs/development.md#set-up) has the setup, the build
commands and the loop for iterating. Every command takes `--game games/oot`, and Python runs with `-I` from the
repository root. The lifted code and every build are local artifacts; they are never committed.

## Testing

### Run the game-free checks

These need no dump, and CI runs them. To run them all:

```sh
python -I -B tests/run_all.py
```

It runs every `tests/test_*.py` and `nx2/test_*.py` in its own process, byte-compiles every tracked Python file, goes
on after a failure, and ends with a table (slowest first) and each failure's output. To run only some checks, name part
of their path: `python -I -B tests/run_all.py lift2`.

Some checks compile a small C or C++ fixture and need a compiler, `spirv-val`, the SPIR-V or Vulkan headers
(`HOOKSHOT_SPIRV_HEADERS`, `HOOKSHOT_VULKAN_HEADERS`, or a CPM cache in `RX_CPM`), or the `zstandard` package. A check
whose tool is missing is skipped and says so; CI passes `--strict`, which fails it instead. The skip list at the top of
`tests/run_all.py` names the checks that cannot run in CI; `--no-skip` runs them anyway.

### Run the checks that need your dump

These are slow and local, and they need `work/dump.txt` from `nx2/dump.py --set`:

| Command | What it checks |
|---|---|
| `python -I nx2/lift2/selfcheck.py --game` | The lifter's decoder against `llvm-mc` on every distinct word of the game |
| `python -I nx2/test_lift2_leaf.py --calls` | Whole functions and call trees of the game, lifted and run against Unicorn |
| `scripts/smoke.sh <name>` | An in-game run against a golden run ([Test in the game](docs/testing.md#run-the-smoke-test)) |

For a graphics fix, look at the screenshots in `work/runs/<name>/shots` and delete them afterwards; they reach
gigabytes and are derived from the game.

### Add a check with your fix

- Add a game-free test where the behavior can be tested without the dump: a small Python file that reads source files
  and runs them, or compiles a tiny C or C++ fixture.
- Name it `tests/test_*.py` or `nx2/test_*.py`; `tests/run_all.py` picks it up. A test that takes a compiler, headers
  or `spirv-val` as arguments gets an entry in `ARGS` at the top of `tests/run_all.py`; one that cannot run in CI gets
  an entry in `SKIP`, with the reason.
- Keep the test fast: every check shares one CI job with a 20-minute limit.
- For a fix in the renderer or the runtime, say what you saw before and after, on which GPU and driver, and where in
  the game.

## Code and data conventions

- Match the file you are editing: there is no formatter configuration. Do not reformat unrelated code.
- Keep OoT-specific code in new files where possible. `nx2/`, `host/` and `gpu/` keep mkw-recomp's paths so upstream
  fixes can be applied as a patch.
- Do not modify `third_party/`. Its parts are upstream copies with their notices; add a new folder and a provenance
  note rather than editing one.
- Treat the local mkw-recomp and Eglantine checkouts as read-only references.
- New switches use the `RX_` prefix in the renderer and `HOS_` in the runtime. They are off, or neutral, unless set,
  and cost nothing while off. Say what a switch does in a comment beside the code that reads it.
- The renderer's rules are in the [direct back end plan](docs/direct-backend-plan.md). A change that breaks one is a
  regression, even if one frame looks right.
- Tests stay on the standard library so CI needs no install step. The pipeline's dependencies (lz4, numpy, capstone,
  unicorn, zstandard) are the exception, not a reason to add more. So are the button prompts' packages
  (`nx2/prompt_requirements.txt`), accepted for `nx2/prompt_assets.py` and `nx2/prompt_draw.py` alone, at build time.
  Maintainers' agents reviewing a change: check that these are still used only there, and flag any other use.
- Keep line endings LF (`.gitattributes`).
- New code has to be GPL-compatible (see [Licensing](#licensing)).

## Commits and pull requests

1. Fork the repository and branch from `main`. Name the branch for the change: `fix/...`, `linux/...`, `docs/...`.
2. Keep the pull request to one topic. Bug fixes do not carry refactors, and two unrelated fixes are two PRs.
3. Write the title as a short imperative sentence. Start the body with why the PR is needed, in one or two plain
   sentences a player could follow: the bug, the missing feature or the cost it removes ("The menu's text is cut off
   at 4K"). Then say what changed, how it was tested (GPU, driver, game area, frame rate before and after), and link
   the issue it fixes.
4. Include screenshots when the change shows on screen: before and after for a fix, the new screen for a feature, and
   the sizes or settings it was checked at. A screenshot of the game is fine; the rule above is about game files.
5. Run the relevant checks. CI must pass.
6. Rebase or merge `main` when it has moved, and resolve conflicts in your branch.
7. Do not include build output, dumps, generated files or a `.gitignore` bypass.

Write commit subjects as [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): imperative
description` (`feat`, `fix`, `perf`, `revert`, `docs`, `refactor`, `style`, `test`, `build`, `ci`, `chore`; `!` after
the type or `BREAKING CHANGE:` in the body marks a break). For a revert, use `git revert --edit` and write the subject
as `revert: ...`.

Maintainer commits record AI assistance with `Assisted-by:` and `Co-Authored-By:` trailers. If you used a tool, add
the trailers that describe it.

## Licensing

Hookshot is free software under the [GNU General Public License, version 3 or later](LICENSE). By submitting a change
you agree to license it under those terms. Only send code you can license that way. Do not paste code from a project
with an incompatible license, and do not paste decompiled game code or anything generated from the dump into a PR.

The third-party parts (Eden, suyu, Sirit, the Vulkan Memory Allocator, SDL, Opus, Dear ImGui, zstd, the Libertinus
fonts, zbic and the a64-kit tools) keep their own notices. Preserve them, and do not relicense their code.

## Where to talk

- **Issues:** [use the forms](https://github.com/NotRexed/Hookshot/issues/new/choose).
- **Discord:** <https://discord.gg/EajnD2AJD9>, in the `hookshot-discussion` channel.
