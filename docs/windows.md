# Windows

This page says how to build and run OunceRecomp on Windows, and what differs from Linux. Read the
[development guide](development.md) first: the commands are the same.

The Windows build is new. It is developed on an NVIDIA RTX 3500 Ada (laptop) with NVIDIA's Windows driver; AMD and Intel
GPUs on Windows have not been tried.

## Install

- Visual Studio 2022 or newer with the C++ desktop workload (MSVC's x64 tools and a Windows 10/11 SDK);
- LLVM 23's Windows archive, `clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.xz` from LLVM's releases, unpacked anywhere
  (it holds the static LLVM, clang and lld libraries `ounce` links); set `LLVM_CONFIG` to its `bin/llvm-config.exe` and
  put its `bin/` on the PATH;
- Rust (rustup, the MSVC toolchain), ISPC for Windows, Python 3 with `pip install meson ninja`, and the Vulkan SDK
  (its headers and `glslangValidator`; `VULKAN_SDK` names it);
- Git for Windows: the commands below are for its Git Bash.

The FFmpeg and SDL the game uses are downloaded by the first build (pinned, checked by SHA-256) into the cache folder:
FFmpeg 9.0.2 with dav1d (an LGPL build, its DLLs beside the game; `OUNCE_FFMPEG=<folder>` takes another unpacked one)
and SDL 3.4.20's official build. There is no sysroot: the game links MSVC's static C runtime, so it needs no
redistributable.

## Build and run

Run everything from a shell that has the Visual Studio developer environment (`vcvars64.bat`'s variables:
`VCToolsInstallDir`, `WindowsSdkDir`, `WindowsSDKVersion`, `INCLUDE`, `LIB`), with MSVC's `bin/Hostx64/x64` before Git's
`/usr/bin` on the PATH (Git's `link` is not the linker).

```sh
cargo build --release -p ounce --manifest-path ounce/Cargo.toml
ounce config set oot.dump <folder with exefs/ and romfs/>
ounce build oot
ounce run oot
```

If your virus scanner stops freshly built programs (Defender does, outside folders it excludes), put Cargo's output and
the work folders in an excluded folder: `CARGO_TARGET_DIR=<dir>/target` and `OUNCE_WORK=<dir>/work`.

The folders: the config is `%APPDATA%\ounce\config.toml`, the cache `%LOCALAPPDATA%\ounce\cache`, the data (saves,
goldens, recordings) `%LOCALAPPDATA%\ounce\data`. The game folder holds `<exe>.exe`, `ounce_renderer.dll`,
`SDL3.dll` and FFmpeg's DLLs.

## What differs from Linux

- **The OS layer.** `runtime/os/os_windows.c` implements `os.h`: the futex words sleep on `WaitOnAddress`, sleeps use
  high-resolution waitable timers, faults come through a vectored exception handler, and the crash report is the
  unhandled-exception filter. `runtime/os/os_crt_windows.h` gives the POSIX names the host's C uses their UCRT spelling;
  the builder force-includes it on Windows.
- **The guest window.** `runtime/os/os_guest_windows.c` reserves the 512 GB window and commits what the runtime hands
  out (`os_guest_commit`, from `hos_commit`) or what the game touches first (a fault handler). Ranges reach the GPU as
  host pointers (`VK_EXT_external_memory_host`) and stay committed once exported.
- **Layer 0's regions.** NVIDIA's Windows driver binds only device-local memory into sparse buffers, so the 4 GB
  windows cannot hold the game's memory, and it refuses device addresses chosen by the application. Layer 0 then holds
  each GPU-readable pool in a *region* of its own: a plain buffer over the pool's pages, bound into a 64 MB *arena* of the
  guest window imported once (an import costs about 11 ms however small), or device-local memory for a `CPU_NO_ACCESS`
  pool (the VRAM class). The translated shaders' address table (`bda_emit.h`) has an entry per 64 KB granule, the
  distance from guest to device address; a granule that pools share (the game leaves one page between them) is split into
  16 page entries. The Linux windows fill the same table.
- **The renderer's calls into the game.** A DLL cannot import from an executable it does not know by name, so
  `renderer/api/host_exports.h` lists what the renderer calls; the executable exports exactly those
  (`runtime/os/os_exports_windows.c`) and the renderer finds them by name (`renderer/vk_host_windows.cpp`).
- **The link.** lld's COFF driver in the builder's process; the link map names the functions (the labels are ELF-only).

## Known problems

- **3D scenes are slow (about 5 fps in Kokiri Forest on the RTX 3500 laptop).** The picture is right, but a few
  async-compute programs (OoT's light sums, `frame_compute_list_begin`) loop over CPU-visible pools with loads and
  atomics, and on this GPU a shader's access to system memory costs 4.5 times a VRAM load and 75 times a VRAM atomic on a
  shared word (measured: 4.6 us). The same scene with every pool in VRAM (CPU-visible data then wrong) runs at 100 fps
  with 3.9 ms of GPU a frame. Menus and DKB's title run at 60 fps.
- The first run translates every shader: run `ounce build <game> --shaders` for a game folder with its caches made.
