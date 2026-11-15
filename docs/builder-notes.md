# A player builder: what a new one needs

The player builders (the Windows WPF app, the Linux AppImage with its Tk window, `nx2/make_game.py` under both, the
setup, release-part and update scripts) were removed: they drove the old lifter's C output, packaged the old renderers
(DLSS, shader seeds, `hookshot_vk.so`), and never made the direct back end's prepared assets. This page is the starting
point for a new one. The developer path (`nx2/nup.py`, `scripts/relift.sh`, `nx2/build.py`) is what it should drive.

## The sequence

A builder is a thin front end over steps that exist:

1. **Read and verify the dump.** `nx2/nup.py <file.nup> --extract <dir>`: the newest title of the package, its hashes
   checked; `nx2/unpack.py` rewrites ZBIC NSOs as plain ones; `nx2/dump.py` checks it is the supported build
   (program `0400486024DEC000`, update v131072, build `e0f3ccfa`). Other versions are refused, not attempted.
2. **Damaged files.** An incomplete dump (about 23.3 GB instead of 23.8 GB) has damaged `.zs` textures:
   `nup.stand_in_damaged(romfs, progress)` replaces each by a whole file of the same kind and returns what it did; say
   so to the player (without it the game leaves out the walls and floors using them, issue 1002). It opens every `.zs`
   file: minutes, so report progress, and do it once per unpack (a stamp beside the romfs).
3. **Lift.** The lifter's own entry point (lift 2), as `scripts/relift.sh` runs it.
4. **Build.** `nx2/build.py --standalone --jobs N`: the game and `hookshot_direct.so`. It needs clang/lld, the Vulkan,
   SPIR-V and VMA headers (`HOOKSHOT_SPIRV_HEADERS`, `HOOKSHOT_VMA_HEADERS`) and FFmpeg (`scripts/linux/deps.sh` pins
   and builds the versions).
5. **Prepared assets.** `nx2/prepare_textures.py`, `prepare_terrain.py`, `prepare_skybin.py` into `prepared/` (the
   direct back end's texture store and terrain overlay, `host/direct_assets.c`; without them the game runs with
   placeholder terrain). See [direct/converters.md](direct/converters.md).
6. **Button prompts.** `nx2/prompt_assets.py --romfs <romfs> --out <game>/prompts` (needs fontTools; without it the
   game keeps the Switch's icons).
7. **The game folder.** The executable, `hookshot_direct.so`, the SDL library, `image.bin`, `romfs` (a link or copy),
   `prepared/`, `prompts/`, the saves folder (docs/saves.md).

## Talking to a front end

The removed driver printed one line per event and the windows parsed them; worth keeping:

- `@@stage <id> <text>` at each step, `@@progress <0..1> <text>` within one (at most a few a second, and the last),
  `@@error <sentence>` (shown whole, ends the build), `@@done <folder>`.
- A failed compile is named by its first `error:` line, not by the last lines of the log: the error is often hundreds of
  lines earlier.
- `nx2/build.py` already prints `@@plan` / `@@file` / `@@eta` when `HOOKSHOT_TIMES=<build_times.json>` gives it the
  seconds per module of earlier builds (a countdown); the builder writes that file after each build.

## Facts not to re-derive

- **Memory decides the compile's parallelism, not cores.** `nx2/build.py`'s MemoryGate starts a compile only when the
  free memory, less what the running compilers may still take, holds its estimate; a giant function counts for more
  than its bytes; the compiler's own process is measured, not its driver (llvm-mingw's clang.exe runs clang-NN.exe).
  8 GB of RAM with no swap closed the game before the title (issue 1241): say so at start (`host/hos_main.c`
  memory_notice).
- **Free space.** A build needs about 60 GB free on the disk of the game folder (the dump's copy, the lift, objects, the
  object cache); measure the disk the folder is on, with `~` expanded.
- **Object cache.** Objects are cached by content key in `work/objcache` and hard-linked into each build; `build.py`
  drops the cache when the compiler or the system headers change, and after a build removes cached objects no build
  links that are older than `HOOKSHOT_OBJCACHE_DAYS` (3). An update then compiles only what changed.
- **Interrupted steps.** A lift stopped half way must not be built from (`nx2/lift_mark.py`); a second build in the same
  folder must stop the first one's compilers first.
- **Read-only installs.** Files copied from the tools folder (a Nix store, a packaged install) must be made writable.
- **Paths are UTF-8** everywhere; a game folder named outside the system code page must work.
- **Links.** Refuse symbolic links inside the dump's folders and keep the game's file system inside its mounts
  (`tests/test_fs_links.py`).
