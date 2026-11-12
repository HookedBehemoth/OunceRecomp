# M0 GPU experiments: native task shaders, the depth-grid walk, deferred texture tables (2026-11-12)

Build: worktree `work/wt/m0diag` (branch agent/m0diag, native-vk 23d0aa6 + temporary instrumentation, not landed), caches
copied from `work/wt/e7`. RX 6950 XT, RADV Mesa 26.2.4, kernel 7.2.11. All runs `RUN=timing`, run folders
`work/runs/m0_*`. Kokiri = `chisel_kokiri`, HF = `chisel_hf_market` (the smoke test's "hf"). Walk K =
`45:LUP/10000,56:RRIGHT/6000,63:LUP/8000,72:LLEFT/4000,77:LUP/9000`; walk HF = `30:LUP/4000,35:RRIGHT/4000,40:LUP/8000,49:RLEFT/6000,56:LUP/8000`
(both with `HOS_INPUT_EXCLUSIVE=1`). FB = `RX_VK_TASK_MESH_FALLBACK`. `RX_VK_MESH_NOCULL=2c56dc1006942f74` unless stated.

## Runs

| run | scene | settings | result |
|---|---|---|---|
| m0_warm_k | Kokiri 75 s | FB=1, HOS_M0_DEFSTAT=1 | ok, 59 fps |
| m0_def_kturn | Kokiri 80 s | FB=1, DEFSTAT, camera turns | ok, 55 fps |
| m0_warm_hf | HF 80 s | FB=1, DEFSTAT | ok, 34-40 fps |
| m0_n_k1..k4 | Kokiri 75 s | FB=0: default / CAPTURE=0 / HOS_CORES=8 / HOS_VCORES=0 | 4 x ok (44-60 fps) |
| m0_n_k5, k6 | Kokiri 100 s | FB=0, walk K, CAPTURE=0 / HOS_CORES=8 | 2 x ok (56, 46 fps) |
| m0_n_k7 | Kokiri 100 s | FB=0, CAPTURE=0 + CORES=8 + VCORES=0, walk K | **lost**: gfx_0.0.0 timeout (reset ok), then comp_1.2.1 (reset failed, mode 1); thread `mkw_vk order` |
| m0_n_h1 | HF 75 s | FB=0, default | **lost**: gfx_0.0.0, thread sead::MainThread |
| m0_n_h2 | HF 75 s | FB=0, CAPTURE=0 | **lost**: gfx_0.0.0, sead::MainThread |
| m0_n_h3 | HF 75 s | FB=0, HOS_CORES=8 | **lost**: gfx_0.0.0, sead::MainThread |
| m0_n_h4 | HF 75 s | FB=0, HOS_VCORES=0 | **lost**: comp_1.3.0, OptimizeChangeBanc |
| m0_n_h5 | HF 90 s | FB=0, CAPTURE=0, walk HF | **lost**: comp_1.2.0, OptimizeChangeBanc |
| m0_n_h6 | HF 90 s | FB=0, all three, walk HF | **lost**: gfx_0.0.0 (reset ok), then comp_1.2.1, OptimizeChangeBanc |
| m0_r3_k1..k3 | Kokiri 100 s | FB=1, `RX_VK_MESH_NOCULL=` (empty), walk K | 3 x ok (60 fps) |
| m0_c_h1, h2 | HF 75 s | FB=0, RX_VK_MESH_CLAMP=1 | **lost** x2: gfx_0.0.0 then comp_1.3.1 / comp_1.2.0, OptimizeChangeBanc |
| m0_c_h3 | HF 75 s | FB=0, CLAMP=1, VCORES=0 | **lost**: gfx_0.0.0, `mkw_vk order` |
| m0_s_h1, h2 | HF 75 s | FB=0, RADV_DEBUG=syncshaders | **lost** x2: gfx_0.0.0 then comp_1.2.1; comp_1.3.1 alone; OptimizeChangeBanc |
| m0_u_h1 | HF 75 s | FB=0, `RX_VK_MESH_NOCULL=` (empty) | **lost** (device lost at present 360); kernel lines gone with the reboot |

Every loss happened at the map load, with 360 or 480 presents logged (~18-25 s). Each one is a `VK_ERROR_DEVICE_LOST`,
with the kernel naming either the gfx ring or one of the async-compute queues (`comp_1.<pipe>.<queue>`). When gfx times
out first and its ring reset succeeds, a compute queue times out next, and that one fails to reset.

## What is confirmed

- **Native task shaders still lose the device on today's base (CONFIRMED).** HF: 6 of 6 runs without the clamps,
  **at default timing too**. Kokiri: 0 of 6 runs with one faster-timing switch at a time (8.3 min, 2 walks), but 1 of 1
  with the three switches combined. With the fallback, nothing hung (6 runs, ~9 min, 3 walks).
- **The ring.** The kernel names the gfx ring, an ACE queue, or both in turn: the gang-submit pair. RADV runs the task
  stage on the ACE. The hang is not gfx work alone.
- **RADV_DEBUG=syncshaders does not prevent it (CONFIRMED, 2 of 2 lost).** By fallbacks.md 3c step 2, that points
  away from a missing barrier of ours, towards the shader or its data, or a gang-semaphore fault that idling does not
  clear.
- **The two loop clamps of m0-task-mesh.md section 4 do not prevent it (CONFIRMED, 3 of 3 lost).** The log shows they
  applied: one trip bound clamped to 7 in `8fb3`, `919b`, `1813`, `d91e`, and 4 grid ends in `2c56`
  (`RX_VK_MESH_CLAMP=1`, code in m0diag's `gpu/vk/vk_shader.cpp`, `mesh_clamp`). So the n = 0 runaway of the B family
  is not what hangs here, or not the only thing.
- **NOCULL makes no difference to hang 1:** m0_u_h1 (NOCULL empty, native) hung the same way.
- **R3 / hang 2: not reproduced.** The `2c56` depth-grid walk with the fallback on and NOCULL empty survived 3 of 3
  Kokiri walks (5 min). The old gfx hang on that route is not seen on today's base.
- **The devcoredump could not be read.** A dump appeared after the first loss (`/sys/class/devcoredump/devcd19`, then
  `devcd20`), but it is root-only (0600) and `sudo -n` asks for a password. umr was not run for the same reason.

## Could not be finished

- **Experiment 3 (Mesa main in `$HOME/opt/mesa-main`)** was not built. The checkout `../mesa` is main e04357c4642
  (2026-11-10). So whether the fault is fixed upstream is open.
- The wave state at the hang (ACE stuck in a wait, or task waves running) is unknown: it needs root for the devcoredump.
- **The resets themselves.** The kernel counter went from `GPU reset(43)` (m0_n_h1, 02:58) to `GPU reset(59)`
  (m0_s_h2, 03:10). That is 17 resets in this boot from my 12 losses before m0_u_h1; earlier work this boot made the
  first 42. After m0_u_h1, the 13th loss, the GPU did not come back and the user had to reboot. The previous boot's
  journal was not kept (`journalctl -b -1` is empty). Back-to-back hang runs are not safe on this PC beyond about a
  dozen resets.
- The worktree build was relinked between m0_n_h4 and m0_n_h5 to add `mesh_clamp`. It does nothing unless
  `RX_VK_MESH_CLAMP=1` is set, so the runs before and after are comparable.

## Experiment 4: the deferred texture tables (HOS_M0_DEFSTAT=1)

Instrumentation in m0diag: `host/m0_defstat.c` hooks `f_80667d20` at entry and takes the address of the table it
writes. `resolve_deferred` (`host/nvn_native.c`) passes it each deferred texture bind: the words at bind time, and the
words at submit. Every 300 presents it logs `m0 defstat:` averages a frame.

| a frame, steady | Kokiri standing | Kokiri turning | HF standing |
|---|---|---|---|
| deferred texture binds (1 word each) | 937 | 1,180-1,346 | 1,290 |
| distinct table words read | 71.5 | 74.5 | 77.5 |
| words changed since that address was last read (2 frames ago) | 4.5 (max 12) | 4.6-4.9 (max 16) | 4.3-4.7 (max 18) |
| binds whose word differs between bind and submit | 104 (11 %) | 137-164 (12 %) | 158-163 (12 %) |
| table rewritten N presents after its last read | N=2: 92 %, 1: 4-5 % | 2: 68-85 %, 1: 14-30 % | **1: 97 %**, 3: 3 % |

- **The ids are not stable (CONFIRMED).** The words that change are slots 4, 8, 17, 18, 19 and 22 of the main view
  context's tables (`ctx 0x113b6436f8`, view indices 15, 18, 19, 20, 21). They get a new texture id every frame (slot 4:
  `0x10201a03 -> 0x10201afa -> 0x10201c22`). Target parking keeps the memory but not the pool ids: rt_slots logs
  ~14 texture inits a frame. Every other word was constant.
- **The tables are double-buffered (CONFIRMED).** Each view index (`f_80667d20`'s x1, 0..31) has two copies 0xf0
  apart. The byte at entry+16 picks the copy and flips every frame, so each word is read every second frame.
- **The copy can be rewritten before its frame's GPU work has finished.** In HF (CPU-bound, 34-40 fps) a table is
  rewritten one present after the submit that read it, in 97 % of cases. A GPU read in place is therefore safe only with
  at most about one frame in flight. At Kokiri's 60 fps cap the margin is mostly 2 presents.

## Decision for the plan

- **Native task shaders: needs the task stage moved off the async ring** (plan "Task and mesh shaders", step 3, in the
  per-workgroup form m0-task-mesh.md describes). On RADV 26.2.4 they are not viable as is: HF loses the device every
  time, even at default timing. Neither syncshaders nor the loop clamps change it. A driver fix is unproven because Mesa
  main was not tried. If it is tried, use a single run with root for the devcoredump, never a series: about a dozen
  resets left the GPU dead. Keep the clamps as cheap guards, but they are not the fix. NOCULL for `2c56` looks
  unnecessary with the fallback (R3 3 of 3 clean), so drop it from run-vk.sh after a longer soak.
- **Deferred tables: needs a submit snapshot**, or the patch of `f_80667d20` that does the same: it writes the heap
  index and the image in one place. Reading at record time is wrong for 11-12 % of binds, all of them this frame's
  render targets. Reading in place on the GPU races the next rewrite in HF. A snapshot is cheap: only ~75 distinct
  words a frame, and only 6 slots of them change.
