# M4a report: compute (2026-11-12)

Branch `agent/m4a`, rebased onto native-vk with M4b (69bb31e). Runs `work/runs/m4a_*` (local), RX 6950 XT, RADV,
`RUN=timing`, `chisel_kokiri`, `chisel_hf` (dumps), `chisel_hf_market` (timing, validation), title (no overlay). Pacing off
(`HOS_NO_PACING=1 HOS_FPS_MAX=240 HOS_FPS_TARGET=1000`) for timing; dumps paced, at the frozen dumps' presents. **No device
was lost.** Tests: 291 pass / 5 fail (the baseline).

## What was built (+1,722 / -54)

| Part | Where |
|---|---|
| Dispatches, direct and indirect, in every list that records: the list's own compute state (program, stage 5's buffers, handles, `PUSH_CONST`), NVN2's packed c0 in the frame arena, uniform buffers through the address table, `vkCmdDispatch[Indirect]` on the arguments in layer 0. Compute programs through the forked translator (step B: handles in c0 read by the shader, storage and global memory by address, no 0x880 table); variants by c0 texture answers; own workers, cache file and **shader-binary cache** (`hookshot_direct_compute{,_objects}.bin`: the networks' objects took 26-76 s each, now 0.6 s in all). The networks run as the game has them (matrix multiplies in software): no stand-ins | `direct_compute.{h,cpp}` (new) |
| The grid as `NumWorkgroups`: c0+0/4/8 folded by the translator (`ReplaceConstant::NumWorkgroups*`, a `NumWorkgroups` opcode) | fork, 11 files, +33 |
| Copies between buffers and buffer clears (`vkCmdCopyBuffer`, `vkCmdFillBuffer`, a barrier after, as texture clears) | `direct_compute.cpp`, `direct_pass.cpp` |
| **Chunks** (plan, Submission): lists cut their command buffer at each event signal / wait; a submission runs its command buffers and frame ops in order; before a signal is written the work before it is queued; after a wait a full barrier. All chunks of a pass of `progress()` go in one `vkQueueSubmit2` and one submit info | `direct_sync.cpp`, `direct_lists.cpp`, `direct_pass.cpp` |
| Storage views of formats the GPU cannot store (B10G11R11, E5B9G9R9): the unsigned format of their size (the game stores them raw) | `direct_heap.cpp` +8 |
| Shared files, small and additive: `direct_shader.cpp` (+22, exports for compute: `direct_shader_parts.h`), `direct_draw.{h,cpp}` (arena / window table / scene-up exports; `HOS_DBG=uborange,uboclamp`; meshlog's after-GPU read), `direct_state.cpp` (+2: a compute-only `BindProgram` leaves the graphics program alone), `direct_main`, `direct_present` (+1 call each). M4b's all-stage banks in `direct_state` are untouched: compute keeps its own packed c0 (`ListComp`), so stage 5's graphics-layout bank there is unused | |

## Done-items

| Item | Kokiri | Hyrule Field | Title |
|---|---|---|---|
| Dispatches recorded / emitted a frame (steady) | 91.8 / 91.8 (layer 17, compute 74.8; 12 indirect) `m4a_t6_k` | 160 / 160 (13 + 147; 14 indirect) `m4a_t_hf` | 14 / 14 `m4a_dd_title` |
| Skipped | 0 in play; at the start a missing program waits (load time only) | 0 | 0 |
| Lit colour and composite | show the scene (below) | the field's ground and sky missing (terrain is task+mesh `2c56`, M4c; the sky LUT, below) | the movie plays |
| Frame time, pacing off, mean / p99 | **11.45 / 13.13 ms** (`m4a_t6_k`; native-vk with M4b, same session: 11.02 / 12.77 `m4a_base_k`; M3b 10.78) | **12.76 / 14.79** (M3b 11.71) | |
| `hsd_emit` a frame by kind | job 0.912, layer 0.821, compute 0.132, gate 0.019; dispatch path 0.096 | job 1.976, layer 1.240, compute 0.174; 0.125 | layer 0.184 |
| Submits | 34 chunks (23 after an event wait) in 3 `vkQueueSubmit2`, 0.49 ms a frame | same, 0.49 ms | |
| Validation, sync on (`m4a_val2_k`, `m4a_val_hf`) | one type: M2b's `ShaderOutputNotConsumed-DynamicRendering`. **No hazard** between compute writes and graphics reads | same | |

**Per pass against the frozen back end** (`direct_dump_compare.py`'s alignment, each target's last state; `m4a_dd4_k`,
`m4a_dd_hf` against `m2b_fd_*_b`). Within noise: Kokiri depth D32F (0.08), 384x216 B10G11R11F (0.12), RGBA16F 768x432 (0.00),
the 96x54 R8UI (0.00); the lit B10G11R11F 1536x864 now has the frozen brightness (means 0.023 / 0.022 / 0.012 against 0.022 /
0.014 / 0.005: too green and blue). Still differ, with the cause: **the 256x256 sky / ambient LUT (37.6, black)**: its source,
a 256x32x16 RGBA16F array (`0x113c19b000`), is written by the CPU straight into the texture's memory (its guest bytes are
constant and nonzero, `HOS_DBG=peek`; no dispatch binds it), so the direct image never gets them: the ambient light is wrong
everywhere after it; the env cube mips 128x128 to 1x1 (7-9: from the sky); mesh-drawn geometry (M4b/M4c: the house walls,
the ground's grass, HF's terrain, and the depth chain built from it, 17-87); the composite (Kokiri 11.0, HF 13.3) from all of
these. Render conditions are not evaluated (M5). Direct against direct (`m4a_dd4_k`, `m4a_dd5_k`): the alignment varies (the
cubemap refilter every 8th frame), median 3.5.

## Evidence

- **No dispatch needs data the CPU reads at record time.** Indirect counts are read by the GPU (`vkCmdDispatchIndirect`);
  programs read their grid as `NumWorkgroups`, and none of the 107 variants reads c0's grid words any other way (counted by
  an IR scan after translation: 0). The frozen back end's CPU read of "grid" words is gone without a patch.
- **Ordering.** Before the chunks (`m4a_r1_k`, `HOS_DBG=sublog`), the CPU-held events let the gate primary run to its end
  first: the whole graphics frame was sent before all seven compute lists, so every compute output was read a frame late and
  every compute input was this frame's. Not the game's order. With chunks (`m4a_r2_k`) graphics and compute chunks alternate
  at each edge as `engine-frame.md` 5 has them; the sync check then reports nothing, so the game's barriers are complete.
- **CPU readers of compute outputs.** The memory is shared (layer 0): the culling compute's writes are in guest memory: the
  argument records read after the GPU ran the frame hold real counts (`m4a_ml2_k`: 45, 13, 12, ...). So `f_80362ff0`,
  `f_80365f00` (grid) and `f_80357060` (exposure) read values the GPU wrote, with no fence: as on the console, at whatever lag
  their two-frame latency gives. Whether each value is sane was not checked reader by reader (M5).
- **Uniform buffers read past their bound size** (`HOS_DBG=uborange`, `m4a_dd4_k`): 316 bindings, 314 dynamically indexed
  arrays (Eden's 64 KB used size; bound 0.6-41 KB; 139 have nonzero bytes after) and 2 vertex programs (`f8d01f66`,
  `fd2ef130`) reading c6 to 688 B on 192 bound. Zeros past the bound (`HOS_DBG=uboclamp`, `m4a_ddc_k`) change no pass
  beyond noise (depth 0.01, lit 0.09, composite 1.7 from Navi's particles). **Settled:** the translator's interface keeps a
  game buffer as its address alone; no bound is passed (noted at `ubo_address`).
- **M4b's questions.** Mesh-only indirect records after the GPU (`m4a_ml2_k`): **6 of 34 draw** (the lists `f_80362ff0`'s
  patch writes as `{x, 1, 1, n}`); the GPU lists `0x117a4d60xx`, `0x117caa00xx`, `0x117982a000` get counts (x = 1-123) but
  their words 1 and 2 are **never written by anyone** (M4b's sentinel stays there with every dispatch running): their writers
  (`102f5b0a`, `abccf81b`, `7650519f`, `3de6d86e`) touch +0 and +12 only. So no reset needs patching: words 1 and 2 = 1 once
  at the lists' creation makes Vulkan's record (11 more mesh-only draws would draw). Not built (its allocation site is not found yet).

## Deviations

- **Chunks** were built though the spec said the queue order stays: without them the order is wrong (above). Events stay
  CPU-written; a chunk after a wait has a full barrier, not a semaphore (34 semaphore submits cost 4.2 ms a frame).
- **A missing compute program waits** until the first job draw (plan rule 2, load time); a cold start with the networks then
  holds the game for minutes (`m4a_dd3_k` never reached the scene in 75 s). Warm, nothing waits.
- `HOS_DBG` aids added: `sublog`, `uborange`, `uboclamp`, `skipcs`, `peek`; meshlog's after-GPU read.

## Candidate game patches

1. The CPU-written LUT array `0x113c19b000` (ambient / sky): find its writer and hand its texels to `hsd_texels`.
2. Words 1 and 2 of the GPU lists' records = 1 at their creation (above).

## Open problems

The sky LUT; mesh geometry (M4b's remaining lists, M4c); +0.4 ms (Kokiri) / +1.0 ms (HF) over the tree before; the compute
cache follows the translator's stamp, so any change to `direct_shader.cpp` or the fork makes a cold start long.

## Pictures (direct, frozen), under `work/runs/`

- composite Kokiri: `m4a_dd4_k/dump_1800/140_pass_c0_prog_0000000000000000_list_gate_rt_2_#1_1004137000.png`,
  `m2b_fd_kokiri_b/shots/dump_1800/323_pass_c0_prog_b7914ae65792d0c8_#3_1004137000.png`
- lit colour Kokiri: `m4a_dd4_k/dump_1800/117_pass_c0_prog_0000000000000000_list_layer_rt_67_#4260_1001a8a0000.png`,
  `m2b_fd_kokiri_b/shots/dump_1800/294_pass_c0_prog_402f575872a22079_#954_1001b8b0000.png`
- composite HF: `m4a_dd_hf/dump_1500/219_pass_c0_prog_0000000000000000_list_gate_rt_2_#1_1004137000.png`,
  `m2b_fd_hf_b/shots/dump_1500/647_pass_c0_prog_b7914ae65792d0c8_#4_1005037000.png`
- title: `m4a_dd_title/dump_1800/003_pass_c0_prog_0000000000000000_list_gate_rt_2_#1_1004137000.png`,
  `m2b_fd_title_b/shots/dump_1800/003_pass_c0_prog_b7914ae65792d0c8_#3_1004137000.png`
- sky LUT: `m4a_dd4_k/dump_1800/002_pass_c0_prog_0000000000000000_list_layer_rt_2_#463_113c2a5000.png`, frozen `038_*`

What they show: Kokiri's scene is in the composite now (M3b: black), Link, Navi, the house's lanterns and curtain lit; it is
darker and green-blue where the frozen one is warm, the ground's grass and the house's walls are missing, and a few magenta
streaks lie on the ground. HF reads as a dark arch: the field's ground and sky are missing. The title's movie plays (another
frame of it than the frozen dump's), a little hazier.
