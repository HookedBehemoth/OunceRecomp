<h1 align="center">
  <br>
  <a>
  <img src="assets/logo.png" alt="OunceRecomp" width="200">
</a>

  <br>
  <b>OunceRecomp</b>
  <br>
</h1>

<h4 align="center">The Legend of Zelda: Ocarina of Time (Nintendo Switch 2) running natively on Linux, with no emulator.
</h4>

OunceRecomp is a static recompilation. The game's code is translated ahead of time and compiled into a Linux program,
the console's system software is reimplemented, and the Switch 2 graphics calls run on Vulkan through one renderer, the
direct back end. It is developed and tested on Linux with Mesa RADV on an AMD RX 6950 XT. All of this happens **on your
PC, from your own copy of the game**.

> [!IMPORTANT]
> This repository **does not contain the game, keys or an executable**, and it does not explain how to get them. You
> need your own copy. Everything the build creates (the game's executable, the generated code, the caches and your saves) comes
> from your game and stays on your PC. Never share those files.

## Build and run

You need a dump of the game's program `0400486024DEC000`, update v131072, build `e0f3ccfa`; other versions do not work.
A build takes tens of minutes and about 60 GB of disk; a run uses about 10 GB of RAM. The tools it needs are listed
in the [development guide](docs/development.md#set-up).

```sh
cargo build --release -p ounce --manifest-path ounce/Cargo.toml
export PATH=$PWD/ounce/target/release:$PATH                # ounce is ounce/target/release/ounce
ounce dump <your-game.nup> --extract <folder> --set     # writes the dump and records it in ~/.config/ounce/config.toml
ounce build oot                                         # the renderer (Meson), then the lift, compile and link
ounce run oot                                           # a window, sound and your own saves
```

The build writes the game folder `work/oot/game`: the game's executable (game.toml's `exe`, `Ocarina of Time`),
`ounce_renderer.so` and `image.bin`. `ounce run` starts it with the dump's RomFS and a folder for the saves; started by
hand it takes these arguments:

| Argument | What it does |
|---|---|
| `image.bin` | The game image to run |
| `--romfs <folder>` | The extracted RomFS folder |
| `--user <folder>` | Where the game keeps saves, caches and temporary files |
| `--trace` | Logs every Horizon OS call (the same as `HOS_TRACE=1`) |

The [development guide](docs/development.md) has the build and run loop and the test tools, and the
[documentation index](docs/README.md) lists every page. The [design page](docs/design.md) has the rules every change keeps.

## How it works

1. `ounce` (`ounce/`) translates the game's AArch64 code to LLVM IR and compiles it to native code
   ([the lifter](docs/lifter.md)).
2. `runtime/` plays the part of the console's system software and is linked with the lifted code and the game's
   patches (`games/oot/patches`).
3. `runtime/nvn_native.c` records the game's NVN2 graphics calls, and `renderer` replays them on Vulkan.
4. A fork of Eden's shader recompiler (`third_party/eden`) translates the Switch 2 shaders to SPIR-V.

The [design page](docs/design.md) shows how the parts fit.

## Credits

OunceRecomp exists thanks to the work of many people:

- **NotRexed**: OunceRecomp (formerly Hookshot), BlueShell (Mario Kart World), Skybound (Tears of the Kingdom) and the a64-kit tools.
- **[Eden](https://git.eden-emu.dev/eden-emu/eden)** and **Eglantine**: the shader recompiler and Switch 2 shader work.
- **suyu**: the graphics decoders, and the AArch64 translator the first lifter used.
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

OunceRecomp is free software under the [GNU General Public License, version 3 or later](LICENSE) (GPL-3.0-or-later).
It includes code from Eden, suyu and Orchid, which are under the GPL too. Third-party parts under other licenses that
work with the GPL (Sirit, the Vulkan Memory Allocator, SDL, Opus, zstd, zbic and the
a64-kit tools) keep their own notices in their folders.

OunceRecomp is an unofficial fan project, not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of
Time are trademarks of Nintendo.
