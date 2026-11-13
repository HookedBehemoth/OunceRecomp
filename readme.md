<h1 align="center">
  <br>
  <a>
  <img src="assets/hookshot.png" alt="Hookshot" width="200">
</a>

  <br>
  <b>Hookshot</b>
  <br>
</h1>

<h4 align="center">The Legend of Zelda: Ocarina of Time (Nintendo Switch 2) running natively on Linux, with no emulator.
</h4>

Hookshot is a static recompilation. The game's code is translated to C and compiled into a Linux program, the console's
system software is reimplemented, and the Switch 2 graphics calls are replayed on Vulkan by one renderer, the direct back
end (`gpu/direct`, `hookshot_direct.so`). The renderer is developed and tested on one machine: Linux with Mesa RADV on an
AMD RX 6950 XT. All of this happens **on your PC, from your own copy of the game**.

> [!IMPORTANT]
> This repository **does not contain the game, keys or an executable**, and it does not explain how to get them. You
> need your own copy. Everything the build creates (`hookshot`, the generated code, the caches and your saves) comes
> from your game and stays on your PC. Never share those files.

## Build and run

You need a dump of the game's program `0400486024DEC000`, update v131072, build `e0f3ccfa`; other versions do not work.
A build takes tens of minutes and about 60 GB of disk; a run uses about 10 GB of RAM.

```sh
scripts/linux/deps.sh                      # LLVM, Python packages, FFmpeg, SDL2, Vulkan headers
python -I nx2/nup.py <your-game.nup> --extract work/dump
python -I nx2/dump.py --dump work/dump --set
python -I nx2/lift.py --standalone --write
python -I nx2/build.py --standalone --jobs 4
```

The build writes `work/sa/build/hookshot` and `hookshot_direct.so`. Run it with the image, the RomFS and a folder for
the saves:

```sh
work/sa/build/hookshot work/sa/image/image.bin --romfs work/dump/romfs --user work/user
```

| Argument | What it does |
|---|---|
| `image.bin` | The game image to run |
| `--romfs <folder>` | The extracted RomFS folder |
| `--user <folder>` | Where the game keeps saves, caches and temp files |
| `--trace` | Log every Horizon OS call (the same as `HOS_TRACE=1`) |
| `--prepare-shaders` | Prepare the shaders as a first start does, then exit |

`scripts/run-vk.sh <name> <seconds>` is a headless test run with screenshots. The [development guide](docs/development.md)
has the pipeline, the build and run loop, and the switches; [CONTRIBUTING.md](CONTRIBUTING.md) has the rules for changes.

## How it works

1. `nx2/lift.py` translates the game's ARM64 code to C.
2. `nx2/build.py` compiles that C together with `host/`, which plays the part of the console's system software.
3. `host/nvn_native.c` records the game's NVN2 graphics calls, and `gpu/direct` replays them on Vulkan.
4. Switch 2 shaders are translated to SPIR-V by a fork of Eden's shader recompiler (`gpu/direct/eden`).

The [design notes](docs/design.md) have the architecture, and the [direct back end plan](docs/direct-backend-plan.md) the
renderer's design and open problems. Upstream's older renderers are kept as unbuilt reference source in `gpu/legacy`.

## Credits

Hookshot exists thanks to the work of many people:

- **NotRexed**: Hookshot, BlueShell (Mario Kart World), Skybound (Tears of the Kingdom) and the a64-kit tools.
- **[Eden](https://git.eden-emu.dev/eden-emu/eden)** and **Eglantine**: the shader recompiler and Switch 2 shader work.
- **suyu**: the graphics decoders and the AArch64 translator.
- **[seedds/nxnative](https://github.com/seedds/nxnative)**: reference for how task and mesh shaders work.
- **[EvilBoyce / evilmfkr](https://github.com/evilmfkr/Hookshot)** (EvilOOT): the GPU culling that brings back Kokiri's foliage
  (trees, ferns, grass, falling leaves, fireflies), the ambient light probes, the pond's water, the game's own fog, the freeze
  after Load (Horizon's mutex handoff) and the exposure flicker fix.
- **[zortos293/Lichii](https://github.com/zortos293/Lichii)**: reference for Ampere shader lowering, and its compare-exchange retry fix.
- **[kinnay/zbic](https://github.com/kinnay/zbic)**, **Opus** and **SDL**: decompression, audio and controllers.
- **[sostenesfreitas/ocarina-recomp](https://github.com/sostenesfreitas/ocarina-recomp)**: this README's layout, and its notes
  confirmed the mesh stage's control section.
- **[MythosOfs/Orchid](https://github.com/MythosOfs/Orchid)**: the fog and exposure workaround for Ocarina of Time.
- **Mesa (NAK)**: the reference for Ampere's instruction encoding.
- Everyone who sent fixes: SkippySteve (Linux and the AppImage, and many more), monsterlag, klimovich008, hckrman101 (NVIDIA
  DLSS, DLAA and Reflex, the exposure flicker and fog fixes, build speed, CI), madebyjordan (AMD FSR 4.1), N3oTraX (FSR 4 on
  Linux), shayderrr, gab9281, Wpaif, nicholasgower, ZurielAtr, Joey-Magnus, Zorkats,
  d3zessete, chrispository, vzeol, leonidasmark, Segmentree, ShyVortex, hiKareeem and i900gaming, among others.

## License

Hookshot is free software under the [GNU General Public License, version 3 or later](LICENSE) (GPL-3.0-or-later).
It includes code from Eden, suyu and Orchid, which are under the GPL too. Third-party parts under other licenses that
work with the GPL (Sirit, the Vulkan Memory Allocator, SDL, Opus, Dear ImGui, zstd, the Libertinus fonts, zbic and the
a64-kit tools) keep their own notices in their folders.

Hookshot is an unofficial fan project, not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of
Time are trademarks of Nintendo.
