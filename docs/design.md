# Architecture

This page explains how Hookshot's parts fit together, from your dump to a running game, and the rules that hold
across them. Read it to orient yourself before you work on any part; each part's own pages go deeper.

Hookshot is a native x86-64 Linux build of the Switch 2 *The Legend of Zelda: Ocarina of Time*. The game's AArch64 code
is translated ahead of time, the console's operating system and graphics API are implemented natively, and the GPU
work runs on Vulkan. No CPU or GPU is emulated at any point.

## From dump to executable

```
exefs/{rtld,main,subsdk0,sdk} + romfs/       your dump, read where it lies (work/dump.txt)
      |  nx2/nup.py, nx2/unpack.py: extract the package, make compressed modules plain
      v
image: modules loaded at 0x80000000 and relocated, imports bound, replaced SDK functions hooked   (nx2/image.py)
      |  nx2/lift.py: find the functions and write the plan                                     (work/sa)
      v
lifter: AArch64 -> typed SSA IR -> LLVM IR, one module per partition                          (nx2/lift2)
      |  nx2/build.py: clang, then link with the runtime and host/
      v
hookshot            the lifted game + host/ (Horizon: threads, memory, files, input, audio, display, NVN2)
hookshot_direct.so  the direct back end (gpu/direct), loaded by the host at start
```

The [lifter](lifter.md) pages cover the translation. The game description, `games/oot/game.py`, ties a lift and a build
to one dump: another dump or another game stops them.

## The runtime

- **The SDK is replaced at the `nn::` boundary.** Host functions declared with `HLE(<symbol>)` stand in for the SDK's
  operating-system functions. The rest of the SDK (libc, utility code, the graphics driver's front end) runs as
  lifted code. An OS-facing import the host neither implements nor answers stops the game when called, naming it.
- **Memory.** The guest window is a 512 GB reservation: the image at `0x80000000`, the heap from 64 GB up, and address
  regions from 512 GB down (`host/hos.h`). Pages are committed as they are handed out or first touched.
- **Threads.** Every guest thread runs on its own host thread, with no scheduler between them.
- **Game patches** are declarations in `host/` that run host code at guest instructions or replace guest functions
  ([Write a patch](patches.md)).

## The renderer

`host/nvn_native.c` implements the NVN2 procs over guest memory: objects live where the game puts them, and command
buffers record an op stream (`gpu/api/rx_nvn.h`). Queue submits hand the lists to the direct back end through the
versioned interface in `gpu/api/vk_api.h`. The back end replays them on Vulkan 1.3: guest memory pools become Vulkan
memory at the guest address, the game's SM86 shader programs go through a fork of Eden's shader recompiler to SPIR-V,
and task and mesh programs use `VK_EXT_mesh_shader`.

The renderer's design is in [the back end's architecture](direct/architecture.md), its rules and open work in the
[direct back end plan](direct-backend-plan.md), and the rest in the [renderer docs](direct/README.md).

## Rules that hold everywhere

- **No game files in the repository.** Decompressed modules, the image, the plan, builds, RomFS copies, screenshots,
  dumps and keys stay in `work/` or in the dump's own folder.
- **Fix the game's behavior where it happens.** An awkward behavior of the game is fixed with a patch, a shader or a
  data file, not with special cases in the back end.
- **Assert, do not tolerate.** A patch site checks the game's instruction words, and a missing piece stops the game with
  what was missing, instead of a silent fallback.
- **The CPU saturates first.** The game's threads, the replay and pipeline compiles share the CPU, and extra helper
  threads have lowered the frame rate before. Measure thread counts (`HOS_THREAD_TIMES`, the samplers) rather than
  raising them by default.
- **One game run at a time** on a development PC: a run takes about 10 GB.

## Where the code came from

| From | What | Where here |
|---|---|---|
| mkw-recomp (Mario Kart World) | The lift driver, the image layout, the standalone runtime, the native NVN2 layer | `nx2/`, `host/` |
| a64-kit | The NSO loader, the image builder, the guest runtime | `third_party/a64-kit` |
| Eden's shader recompiler | SM86 to SPIR-V, task and mesh programs | `gpu/direct/eden` (a fork) |
| TOTKPort, Eglantine | The original Horizon runtime; Switch 2 service protocols | Reference only |

`host/PROVENANCE.md` records what the runtime took and changed.

## Testing

- **Game-free checks:** `python -I -B tests/run_all.py` ([CONTRIBUTING.md](../CONTRIBUTING.md#testing)).
- **The lifter against the game:** the decoder against `llvm-mc`, and whole functions against Unicorn
  ([Build and iterate on the lifter](lifter-build.md#check-the-lifter)).
- **In the game:** the smoke test and the benchmarks ([Test in the game](testing.md)).
