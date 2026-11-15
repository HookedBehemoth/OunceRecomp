# host/: provenance

This page records where the standalone runtime in `host/` came from and what changed on the way, for anyone comparing
it with its upstreams or applying a fix from them.

The runtime is a fork of the Horizon OS layer of NotRexed's a64-kit (`hos/`, from TOTKPort commit `e88744a`), the
runtime that runs Tears of the Kingdom natively, as adapted by mkw-recomp for Mario Kart World. It replaces the
Nintendo SDK at the `nn::` boundary: an `HLE(<mangled name>)` function stands in for that SDK function wherever the game
calls it. The rest of the SDK runs as lifted code.

Taken from a64-kit, changed since: `hos.h`, `hos_main.c`, `hos_mem.c`, `hos_thread.c`, `hos_svc.c`, `hos_hostcall.c`,
`nn_os_sync.c`, `nn_fs.c`, `nn_hid.c`, `nn_libc.c`, `nn_diag.c`, `nn_applet.c`, `nn_audio.c`, `nvdrv.c`.

mkw-recomp's changes, kept here:

- **The guest window** is 64-bit, 512 GB, reserved whole, with the image at Eden's `0x80000000`, the heap from 64 GB,
  and address regions from 512 GB down (`hos.h`). Memory is committed as it is handed out, and anything else on first
  touch (`hos_process.c`). TOTK committed its whole 4 GB window up front.
- `hos_main.c` takes the image base from the lift's generated `gen/hos_image.c`; `hos_svc.c` reports the program ID
  from it.
- `hos_mem.c` gives the game a Switch 2 application's 9809 MB by default.
- `nvn_native.c` is TOTKPort's native NVN layer extended to NVN2: the Switch 2 driver's device limits, GPU addresses
  over the 64-bit window, program builders, native images and sparse tile layouts. `nn_vi.c` is mkw-recomp's own.

Hookshot's changes since: the direct back end is the only renderer (`rx_backend.c` loads it); every guest thread runs
on its own host thread with no scheduler; patches are declarations (`patch.h`); a call of an import or NVN proc the
host lacks stops the game. Files bound to TOTK or its Maxwell GPU were left out. `mods.c` is the RomFS part of
Skybound's mod loader (`NotRexed/SkyboundTotk`, MIT).

Licensing: GPL-3.0-or-later, as the rest of Hookshot. `third_party/a64-kit/NOTICE.TOTKPort.md` has the kit's history.
