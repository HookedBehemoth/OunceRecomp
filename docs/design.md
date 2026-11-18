# Design

This page is the statement of OunceRecomp's design: how the parts fit, from your dump to a running game, and the rules
the design keeps, each with the reason for it. Read it before you work on any part; each part's own pages go deeper.

OunceRecomp is a native x86-64 Linux build of the Switch 2 *The Legend of Zelda: Ocarina of Time*. The game's AArch64 code
is translated ahead of time, the console's operating system and graphics API are implemented natively, and the GPU
work runs on Vulkan. No CPU or GPU is emulated at any point.

## From dump to executable

```
exefs/{rtld,main,subsdk0,sdk} + romfs/       your dump, read where it lies (work/dump.txt)
      |  ounce-builder dump (ounce/dump): extract the package, make compressed modules plain
      v
image: modules loaded at 0x80000000 and relocated, imports bound, replaced SDK functions hooked   (ounce/lift)
      |  discovery and the plan: the functions, the partitions
      v
lifter: AArch64 -> typed SSA IR -> LLVM IR, one module per partition                          (ounce/lift)
      |  ounce-builder build (scripts/build.sh): LLVM and clang in one process, then lld
      v
<exe>               the lifted game + runtime/ (Horizon: threads, memory, files, input, audio, display, NVN2)
ounce_renderer.so  the direct back end (renderer), loaded by the host at start
```

- **The game is a parameter.** Every lift, build and lifter tool takes one game description, `games/oot/game.toml`,
  which names the dump's identity, the title and icon, the function labels, the hot list and the game's patch files. Another dump or another
  game stops the lift and the build. The lifter itself holds no game data ([The lifter](lifter.md#the-game-boundary)).
- **Patches are C declarations.** Every change to the game's behavior is a declaration in `games/<game>/patches` next to a typed host
  function: a row at an instruction, a replaced function, an SDK replacement, or a constant answer. The lifter scans
  the declarations without compiling the host, and the build checks each one's instruction words against the game
  ([Write a patch](patches.md)).
- **Code pointers are host function pointers.** The lifter translates them where they originate, so an indirect call
  calls its target directly ([What the lifter emits](lifter-output.md#indirect-calls-and-jumps)).

## The host layer

`runtime/` plays the part of the console's system software:

- **The SDK is replaced at the `nn::` boundary.** Host functions stand in for the SDK's operating-system functions;
  the rest of the SDK (libc, utility code, the graphics driver's front end) runs as lifted code. An import the host
  neither implements nor answers stops the game when called, naming it.
- **Memory.** The guest window is a 512 GB reservation: the image at `0x80000000`, the heap from 64 GB up, and address
  regions from 512 GB down (`runtime/hos.h`). Pages are committed as they are handed out or first touched.
- **Threads.** Every guest thread runs on its own host thread, with no scheduler between them.
- **Platform.** The host is built for Linux only. It reaches the operating system through `runtime/os/os.h` alone (threads,
  futex-based locks and events, time, memory, faults, files, modules), implemented in `runtime/os/os_linux.c`; another
  system is another implementation of that header. The window, sound and controllers are SDL's (`runtime/media/hos_sdl.h`).
  Paths are UTF-8.

`runtime/PROVENANCE.md` records where the runtime came from.

## The renderer

There is one renderer, the direct back end in `renderer`, developed on Mesa RADV. `runtime/nvn_native.c` implements the
NVN2 procs over guest memory and records the game's command buffers; queue submits hand the lists to the back end
through the interface in `renderer/api/`. The back end replays them on Vulkan 1.3, with the game's shaders translated to
SPIR-V by a fork of Eden's shader recompiler. Its design is in [the back end's architecture](direct/architecture.md),
its rules and open work in the [direct back end plan](direct-backend-plan.md).

## Rules

- **No game files, keys, or anything made from them leave your PC or enter the repository.** That covers the dump,
  `work/`, builds, caches, saves and decompiled game code. Screenshots and logs are fine. The project exists only
  because each player builds from their own copy.
- **Fix the game's behavior where it happens.** An awkward behavior is fixed with a patch, a shader or a data file, not
  with special cases in the back end, so the back end stays a plain implementation of the API.
- **Assert, do not tolerate.** A patch site checks the game's instruction words, and a missing piece stops the game with
  what was missing. A silent fallback hides a wrong game version or a missing implementation until it shows up as a
  wrong picture.
- **The CPU saturates first.** The game's threads, the replay and pipeline compiles share the CPU, and extra helper
  threads have lowered the frame rate before. Measure thread counts (with a system profiler) rather than
  raising them by default.
- **The back end's interface is versioned.** A change to `MkwVkHost` or `MkwVkRenderer` in `renderer/api/vk_api.h` bumps
  `MKWVK_API_VERSION`, because the host and `ounce_renderer.so` are built and loaded separately.
- **Generated files are regenerated, not edited.** The SPIR-V headers in `renderer/shaders/` (`*_spv.h`) are compiled
  from the shader beside them with glslang; an edit by hand is lost at the next regeneration.
- **`third_party/` stays as upstream ships it.** Its parts keep their own licenses and notices; add a folder rather than
  editing one, so upstream updates stay a copy.
- **Checks run without the game.** CI has no dump, so every check reads source or synthetic data; a run in the game is
  done locally and reported ([Test OunceRecomp](testing.md)).
- **Docs are short guides.** A page says what a part is for, how to do a task and what to expect; it does not narrate
  code, repeat what a tool prints, or keep history. Removed features are recorded in
  [parked patches](parked-patches.md).
