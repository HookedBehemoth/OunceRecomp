# M3a report: jobs, packets and the game patches (2026-11-12)

Branch `agent/m3a` (from native-vk 148ba5a; M3b has not landed, so this is not rebased). The runs are in `work/runs/m3a_*`
(local). RX 6950 XT, RADV, `RUN=timing`, one game process at a time, no device lost. Kokiri is `chisel_kokiri`. Hyrule
Field (HF) is `chisel_hf_market` for timing and validation and `chisel_hf` for dumps, because the frozen HF dumps
(`m2b_fd_hf_a/b`) were taken there.

## What was built (+999 / -46 lines)

| Part | Files |
|---|---|
| Jobs | `direct_pass.cpp` (+229): the format table by pass key. It is lock-free: 1024 key slots, each pointing at an interned format set (mutex on intern only). `pass_job_begin`: when the key is known, the job is one rendering secondary of its thread's slot (`direct_cmd` +33, `cmd_secondary`: `RENDER_PASS_CONTINUE`, inherited formats). At the call (`pass_call`) the formats are compared, the job is executed in the caller's rendering (`vkCmdExecuteCommands`), the caller's draw state is invalidated, and the key's formats are noted. Every rendering is begun with `CONTENTS_SECONDARY_COMMAND_BUFFERS | CONTENTS_INLINE_KHR` (`VK_KHR_maintenance7`, enabled in `direct_main` +6). A job's executed slot is held by the caller's submission. Exports `hsd_pass_known` and `hsd_pass_inline`. |
| Packets | `direct_lists.cpp` (+126): load-time and unclassified lists are packed at emit time as `{op, size, payload}`, with BEGIN/END/JUMP/ENG dropped and nested calls flattened in place. They are expanded through the normal path (`emit_one`) into the caller's state, command buffer and frame ops. |
| Draw (M3b's file, small) | `direct_draw.cpp` (+50): `l->rec` replaces the kind test. Targets are tested by format and aspect, because a job's pass has no images. Samples come from the pass. A draw under a render condition is drawn and counted. Added `draw_invalidate`, per-list drawn/mesh/step-A counts, and `HOS_DBG=deferredcheck`. |
| Host patches (direct mode only) | `direct_passes.c` (126): the recorder's early return, the inline-path hook, keys matched by their bytes. `direct_job_sync.c` (203): waits, barriers and dispatches moved out of jobs. `direct_depth_copy.c` (64). `direct_rt_ids.c` (105). `direct_patches.c` (17): the 600-present lines. `eng_jobs.c` +11 (one call; the job's command buffer kept). `nvn_native.c` +12 (`direct_cmd_list`; one guarded call at the present). `direct.h`, `rx_backend.c`. |

## Done items (Kokiri `m3a_fin_k` / HF `m3a_fin_hf`, 90 s, pacing off)

| Item | Kokiri | HF |
|---|---|---|
| Jobs executed / recorded a frame | 100.5 / 100.5 | 101 / 101 |
| Passes drawn inline a frame, steady | 0. At the load, each of the 23 keys once (0.23 early returns a frame in the first line) | 0 (`m3a_fin_hf`, `m3a_dd_hf`) |
| Jobs not executed for a format mismatch | 0 | 0 |
| Frame time, mean / median / p99 | 11.89 / 11.81 / 13.86 ms (baseline `m3a_warm_k` 10.70 / 10.67 / 12.58) | 12.98-13.48 / 12.88 / 14.87 ms (M2b: 11.57) |
| `hsd_emit` a frame, by list kind | job 1.39, layer 1.35, gate 0.02, compute 0.02 (baseline job 0.32, layer 1.52) | job 2.68, layer 2.15 |
| `hsd_list_call` a frame (packet expansion, with its draws), by caller kind | job 2.34, layer 0.23 (baseline 0.12 / 0.21) | job 4.14, layer 0.24 |
| Draw path a frame | 4.16 ms | 7.41 ms |
| Vulkan objects | +2 per 600 presents (images, as in M2b) | +2 per 1,200 presents |
| Validation, sync on (`m3a_val_k`, `m3a_val_hf`) | One message type, at the 200 limit: `ShaderOutputNotConsumed-DynamicRendering` (M2b's). The three depth-copy hazards are gone. No message about secondaries mixed with inline commands. | same |
| Tests, frozen back end | 291 pass / 5 fail (the baseline) | |

Mixed secondary and inline rendering is accepted by RADV and by the validation layer with only `maintenance7` and those
two flags. The inheritance info's flags are 0.

**Packets, before and after.** In play no list is recorded as a load list. Recording the 37,000 load lists at the load
cost 319 ms of `hsd_emit` before; as packets it costs 16 ms. They hold 5.7 MB live. In play the cost moves to the call:
1,137 calls a frame expand 6,066 ops in Kokiri, and 2,954 calls expand 13,964 ops in HF. Most of that time is the draw
path recording the draws the packets hold, which recorded nothing before.

**What a job secondary sets.** It sets everything at its first draw: the heap, shaders, all dynamic state, vertex input
and push data. That first draw costs 10.4 µs (HF 6.7 µs); later draws cost 3.0 µs (2.2 µs). About 7.4 µs a job,
0.37 ms a frame over 50 jobs with draws. After each `vkCmdExecuteCommands` the caller sets everything again too.

**Deferred words** (`HOS_DBG=deferredcheck`, `m3a_t2_k` / `m3a_dd_hf`): 5,452 / 5,493 words are copied a frame. 748 / 858
differ between bind and submit, so the submit snapshot is still needed. 3.4 / 3.3 change from frame to frame. The id
patch did not apply (below), so these counts are its "before".

## The patches and their counters (600-present lines "direct patch ...")

| Patch | Counter a frame (Kokiri = HF) |
|---|---|
| passes: recorder early return, inline names the key | asked 100.5, early 0 (steady); inline passes no recorder asked for 0 / 4 (the game's own) |
| job sync: f_82c02f20, f_82c03170, f_831fa4a0 → f_806688c0's call (0x80668c84) | kept 3 waits, 2 barriers, 2 dispatches; placed 3, 1 (bits OR'd), 2; dropped 0; passed through 0 |
| depth copy (0x80289cc4 HalfDepth, 0x806675dc SoftEdgeDepth) | 1.00 + 1.00 copies with a barrier before and after |
| rt ids (f_800634b0 from 0x806680dc) | 1 call, 0 kept: "entry changed" every frame (see below) |

## Per-pass comparison against the frozen back end

Run with `scratchpad/pcmp.py`: the passes are aligned as `direct_dump_compare.py` aligns them, but each target is taken in
its *last* state of the run, because the frozen dumps record after every draw. Noise, frozen against frozen: Kokiri
median 0.00, max 8.1; HF median 0.01, max 9.1.

- **Within noise:** Kokiri (`m3a_dd_k`) depth D32F 1536x864 (0.08 ×5), 768x432 (0.08 ×6), 384x216 (0.00 ×11); the D16
  1600x1600 shadow cascades (0.03-0.32); the probe depths; R32F/R16F/RG16F/RGBA32F 384x216; RGBA16F 768x432 (0.00 ×7).
- **Differ, with the cause the counters give.** The per-key lines name each pass's job draws as drawn, mesh or step A
  (`m3a_key_k`).
  - **Handle-program textures** (M3b's step B; 223 job draws a frame in Kokiri, 527 in HF): key `0x113b648038` (147
    step A), the cube probe `0x1137375608` (69; RGBA16F 256x256 37.6). With `agent/m3b` merged on trial (`m3atry_dd_k`,
    not kept) step A is 0 and the lit colour target goes from a mean of 0.0002 to 0.0067 (frozen 0.075).
  - **Mesh draws** (M4): key `0x113b647338` (325 drawn, 14 mesh) and the cascades (7 / 7 / 5 mesh each). In HF the mesh
    terrain explains depth 1536x864 (4.4), the shadow D16 (87: walls missing) and the R32F/R16F chain built from depth
    (17-37).
  - **Compute input** (M4; 83 dispatches a frame skipped): the lit B10G11R11F 1536x864 and everything after it, down to
    the composite. Even on the trial merge the lighting shows only emissives (lanterns, Navi); the ground and Link stay
    black. The RGBA8 1536x864 mask pass matches before lighting (direct 059 against frozen 219: means 1 0.969 0.998 on both).
- **The final composite does not show the scene** (black but the hearts, with or without M3b). Done-item not met: it
  waits for compute (M4).

PNGs, under `work/runs/`:
- composites: direct `m3a_dd_k/dump_1800/141_pass_c0_prog_0000000000000000_list_gate_rt_2_#1_1004137000.png` and
  frozen `m2b_fd_kokiri_b/shots/dump_1800/323_pass_c0_prog_b7914ae65792d0c8_#3_1004137000.png`;
- the opaque depth (matches): `m3a_dd_k/dump_1800/060_pass_depth_prog_0000000000000000_rt_24_#1989_10015f70000.png` and
  `m2b_fd_kokiri_b/shots/dump_1800/220_pass_depth_prog_1e26755d8cb22dc8_#751_10015f70000.png`;
- the lit colour: trial merge `m3atry_dd_k/dump_1800/094_pass_c0_prog_0000000000000000_list_layer_rt_48_#3660_1000fd50000.png`
  and frozen `m2b_fd_kokiri_b/shots/dump_1800/252_pass_c0_prog_3741fc69b2a60e64_#954_1001b8b0000.png`;
- HF depth (terrain missing): `m3a_dd_hf/dump_1500/070_pass_depth_prog_0000000000000000_rt_33_#2136_10010310000.png` and
  `m2b_fd_hf_b/shots/dump_1500/529_pass_depth_prog_248f4c4bf12ace11_#246_10010310000.png`.

## Deviations

- **The water wait** is not a direct WaitEvent call. `[renderer + 0x150]` is `f_831fa4a0`, which tail-jumps to
  WaitEvent through x4. The splice sits there, not in `f_80279400`.
- **Every patch is a call-target swap.** A splice after the line that loads the NVN proc into a register points the call at
  a host function, which records through the real procs (`hos_call_guest`). The dispatch is an entry hook on `f_8067c310`
  inside a job; it is replayed at the job call with the layer list's draw context (`[sp + 0x10]`).
- **f_8028a2a0's inline path** passes a stack copy of the key. The host notes each recorder key by its first 0x70 bytes,
  as the game matches records, and fills *every* key with those bytes. The two cube-face sets share their bytes: naming
  only the first left `0x11373756xx` inline every other frame in HF (`m3a_t_hf`). Fixed in 49b1ae4.
- **"Pre-resolved"** packets: only flattened, with the no-ops dropped. Nothing about addresses is resolved ahead.
- **The job's size** (render area, default scissor) comes from the table. A size change is counted but is not a
  mismatch (0 seen).

## The rt-id patch: its premise does not hold

From the first call on, the entry handed to `f_800634b0` from `0x806680dc` carries the same stale id every frame
(`0x10c5`, `0x1118`, ...). The call writes a fresh one (`0x2cc8`, `0x2cef`, ... about +0x24 a frame). Something resets
`+0x68` before the next call, and it is not `f_800634b0`: only `0x806680dc` and `0x802a6a68` call it. A page watch on the
word caught no write: it was armed every 200 calls and the page was made read-only. So the write goes through another
mapping, or the entry is copied whole from elsewhere.

The patch stays in place, counting why the game's path ran (always "entry changed"). It must not reuse an id while the
real writer may free it. Open: find that writer, for example with a watch on the table's source entry, or by following
the frame's copy of the system-texture table in `f_80667d20`'s caller.

## New candidate game patches

1. The writer of the system-texture entry's id (above).
2. Calls through `f_806688c0`'s other CallCommands path (`0x80668e58`, x11) would leave kept ops. They are counted as
   dropped; 0 seen.

## Open problems

- **Frame time:** +1.2 ms (Kokiri) and +1.6 ms (HF) over M2b. This is the real job draws (646 / 1,889 a frame) in the
  draw path. M3b's work removes part of it: the trial merge ran at 11.0 ms in Kokiri.
- `ShaderOutputNotConsumed` (M2b's) remains.
- The step-A, mesh and compute exceptions above are counted, not listed per pass in the dump: the per-key lines name a
  pass by its target address, which differs between runs.
