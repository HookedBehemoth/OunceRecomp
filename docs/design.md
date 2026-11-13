# Hookshot (oot-recomp): design

A native x86-64 Linux build of the Switch 2 *The Legend of Zelda: Ocarina of Time* remake: the game's AArch64 code
translated to C ahead of time from the user's own decrypted dump and compiled with clang, the Horizon OS and NVN2
implemented natively, and the GPU work replayed on Vulkan by one renderer, the direct back end. No CPU or GPU emulation
at any point. The project forks mkw-recomp (Mario Kart World) and keeps its paths.

## 1. Short answer

The game is a Switch 2 title, so the base is mkw-recomp, not TOTKPort:

| | Switch 2 (this game) | Switch 1 (TOTK, TOTKPort's base) |
|---|---|---|
| CPU code | ARMv8.2+ (Cortex-A78C): PAC returns, LSE atomics, CAS, LDAPR | ARMv8.0 (Cortex-A57) |
| Relocations | DT_RELR packed + RELA | RELA |
| GPU API | NVN2 (API 231.x): native images, mesh/task programs | NVN 55.x |
| Shaders | Ampere SASS (SM86) in `Nin_Ounce_NVN2` packages | Maxwell SASS in `Nin_NX_NVN` packages |
| Services | nx2 gs/gpu/vi2 sessions, no nvdrv in the npdm | nvdrv, vi:u |

`nx2/dump.py` checks this on the real dump with independent signals (program id prefix, ACID layout, nvdrv in the
services, DT_RELR, ARMv8.1+ instructions, NVN2 proc names); all must say Switch 2.

## 2. What is reused

| From | What | Where here |
|---|---|---|
| mkw-recomp | lift driver and its fixes, Switch 2 forms, DT_RELR, Eden's image layout, 64-bit guest window, loop-header polls, stable chunks, Unicorn tests, build; the standalone runtime; the native NVN2 layer | `nx2/`, `host/`, `third_party/` |
| a64-kit (Rexcomp's AArch64 kit) | NSO loader, image builder, lifter, suyu's translator, instruction harness | `third_party/a64-kit` |
| Eden's shader recompiler | SM86 -> IR -> SPIR-V, task/mesh programs | `gpu/direct/eden` (a fork) |
| TOTKPort, Eglantine | the original HLE runtime, Switch 2 service protocols | reference only, read and never edited |

Keeping the fork's paths identical to mkw-recomp's is deliberate: a later mkw-recomp commit comes over as a patch
(`git diff <base> <new> -- nx2 host third_party`, applied with `git apply -3`). OoT-specific code goes into new files where
it can, so the shared files stay easy to patch.

## 3. Architecture

```
 exefs/{rtld,main,subsdk*,sdk} + main.npdm      (the user's dump, read in place: work/dump.txt)
        |  a64-kit tools/nso.py + nx2/nso2.py (DT_RELR)
        v
 modules: LZ4 segments, MOD0, dynamic section, symbols, RELA + RELR, .eh_frame_hdr starts
        |  nx2/image.py (Eden's layout from 0x80000000: rtld, main, subsdk*, sdk; we are rtld)
        v
 image: relocated, imports bound across modules; the HLE set hooked (nx2/standalone.py)
        |  a64-kit lift/a64_lift.py + nx2/insn.py + nx2/lift_fixes.py + nx2/poll.py + nx2/chunks.py
        v
 one C function per guest function (work/sa/gen, local), 64-bit guest window (nx2/rtgen.py)
        |  clang -O1, the hot functions -O2 with the play profile, lld (nx2/build.py --standalone)
        v
 hookshot            = lifted game + host/ (Horizon: threads, SVCs, memory, fs, hid, audio, vi, gs, NVN2)
 hookshot_direct.so  = the direct back end (gpu/direct), loaded by host/rx_backend.c
```

- **The SDK is replaced at the nn:: boundary** (TOTKPort's model): `HLE(<mangled name>)` functions in `host/` stand in for
  the SDK's OS-facing functions; the rest of the SDK (libc, nn::util, nn::mem, the NVN2 driver's front end) runs as lifted
  code. Unimplemented OS-facing imports get generated stubs that log their first call.
- **Memory.** A 512 GB reservation; the image at `0x80000000`, the heap from 64 GB, address regions growing down from
  512 GB; committed as handed out or on first touch (`host/hos.h`).
- **GPU.** `host/nvn_native.c` implements the NVN2 procs over guest memory in place: objects live where the game puts
  them, command buffers record an op stream (`gpu/api/rx_nvn.h`), queue submits hand lists to the back end through the
  versioned table in `gpu/api/vk_api.h` and `gpu/direct/direct_api.h`. The direct back end replays them on Vulkan 1.3:
  guest memory as window buffers shared with the GPU, SM86 programs through the forked recompiler to SPIR-V, task and mesh
  programs with `VK_EXT_mesh_shader`. Its design, rules and open problems are in
  [direct-backend-plan.md](direct-backend-plan.md); what we know about the game is in [direct/](direct/README.md).

## 4. The lessons from MKW, as rules here

These cost days in mkw-recomp; the checks keep them true for a second game.

1. **Cross-queue `nvnCommandBufferWaitEvent` is honoured.** The game's queues pass stages back and forth with events;
   replaying submissions in arrival order on one Vulkan queue flashed. Each submission replays as far as its waits allow.
2. **Nothing the replay reads comes from live guest memory.** Games write per-frame data ahead of the GPU: every read is
   from submit-time data.
3. **Texture and sampler registrations are queued in order with the submits**, never applied out of band.
4. **The CPU saturates first.** The game's threads, the replay and pipeline compiles share it, and extra helper threads
   have lowered fps before. Thread counts are measured, never raised by default (`HOS_THREAD_TIMES=<seconds>`, the
   `HOS_SAMPLE` profiler with `scripts/prof.py`).
5. **2D views of 3D textures need `VK_EXT_image_2d_view_of_3d`.**

MKW's frame-pacer hooks at fixed guest addresses are gated on MKW's build id (`hos_game_is_mkw`); OoT gets its own, never
by address coincidence.

## 5. What OoT needs that MKW did not

| | Found | Plan |
|---|---|---|
| Dump format | one `.nup` package (base v65536 + update v131072, each an exefs/ and romfs/ tree, data stored as is) | `nx2/nup.py` extracts the newest title to `work/dump` |
| NSO compression | `main` is ZBIC (NSO flags bit 7, firmware 22.0.0+: zstd with its own magic and BIC-coded tables); rtld, sdk, subsdk0 are LZ4 | `nx2/unpack.py` rewrites ZBIC NSOs as plain ones (hashes checked), decoder `third_party/zbic` |
| SDK / NVN2 version | 22.2.8 (MKW: 21.4.0); same 46 services and 49 SVCs as MKW | 26 NVN proc names MKW never asked for, 14 not in `host/nvn_native.c` yet (stubs that stop the game only when called; DLSS parameter procs, window acquire timeout, HDR metadata, `nvnProgramBuilderGetShaders`, ...); the device limits in `nvn_native.c` are MKW sdk's (`devinfo[]`) and may need OoT's |
| Instructions | 4 UDOT (FEAT_DotProd) sites, the only forms suyu's translator lacked | `nx2/insn.py` `dot()` |
| Engine | romfs laid out like TOTK's (agl, gsys, Phive, Banc, Bake), Havok 2025.2, NintendoWare G3d2/Vfx3/Ui, SMAA, `.zs` (zstd) files | romfs read from `work/dump/romfs` |
| Microphone (the ocarina by humming) | to check at runtime (nn::audio input; `audsmx:u`, `auddmg:u` are in the services, as in MKW) | a stub that hears silence |
| Cutscene video / voice | `Movie/` in the romfs | played by the host (FFmpeg) |

## 6. Testing

- **Instruction forms** (`nx2/test_insn.py`): every form the game's code contains, random states, Unicorn's ARMv8.x "max"
  CPU against the lifted C: X0-X30, SP, NZCV, V0-V31, FPSR and memory compared.
- **Whole functions** (`nx2/test_leaf.py`, `--calls` for call trees): random functions of every module on the real
  relocated image, natively and in Unicorn, on the 64-bit window with polls firing; registers, V registers and every
  changed page compared.
- **Game-free source checks:** `python -I -B tests/run_all.py`.
- **In the game:** `scripts/run-vk.sh` and `scripts/smoke.sh` (see the development guide), the sampled profiler
  (`HOS_SAMPLE` + `scripts/prof.py`).

## 7. Rules

- No game files in the repository: decompressed modules, the image, lifted C, builds, romfs copies, dumps and keys stay in
  `work/` (ignored) or in the dump's own folder.
- Treat the mkw-recomp and Eglantine checkouts as read-only references.
- One game run at a time on this PC (a run takes ~10 GB); runs are muted (`HOS_AUDIO=0`) and screenshot folders are
  deleted once looked at.
- The lifted C and every build are GPL (suyu's translator is GPL-2.0-or-later; Eden's code is GPL-3.0).
