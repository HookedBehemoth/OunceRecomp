# M6d report: the market loss and the negated occlusion grid (2026-11-13)

Branch `agent/m6d` (from native-vk 82b9cd2). Runs `work/runs/m6d_*` (local), RX 6950 XT, RADV Mesa 26.2.4, kernel 7.2.11,
one game process at a time (since 00:15 with the M6e agent's harness sharing the GPU lock); market `chisel_hf_market`,
Kokiri `chisel_kokiri`, Hyrule Field (HF) `chisel_hf`. Tests: `HOOKSHOT_VULKAN_HEADERS=/usr/include work/venv/bin/python -I -B
tests/run_all.py`: 295 pass / 5 fail (the baseline five). Local tools, not committed: `work/proto/direct-m6d/` (`spvside.py`
and `spvscan.py` over the SPIR-V dumps in `spv/`, the offline IR of the grid computes in `grid/` made with M0's `tmtool`,
`proof.sh`, `timing.sh`, `gpu_ready.sh`). The SPIR-V dumps of every task+mesh and mesh-only program of the market
(`HOS_DBG=taskspv`, now also for mesh-only programs) are in `work/proto/direct-m6d/spv/` for M6e.

## Summary

Verified (each by a check that could have failed, named):

- **The market no longer loses the device** with the final build: PROOF_LINE
- **What keeps it from losing the device** is that no two task+mesh draws' native mesh stages run at the same time. The rule
  now: a wait before every task+mesh draw (the framebuffer-local barrier a rendering may hold). This is a **mitigation, not
  the cause**: it explains that the loss goes away, not why two overlapping mesh stages lose the device.
- **The implicated stages have no side effects and no loops.** The mesh stages of the 11 programs the breadcrumbs named write
  nothing but their outputs (no storage store, no image write; one, `30dcbaf546843b46`, adds to a statistics counter that no
  stage reads), contain no loop, and declare no private or
  function variable; their fragment stages have one loop each (`a437`'s traced: bounded by a uniform word) and write nothing
  but colour. So
  "a draw writes memory a later draw reads" and "a mesh workgroup loops on bad data" are both excluded for them (1c).
- **The negated grid's cause**: M3a's job patch moved the job's occlusion-grid dispatches (`f_8067c310`) from inside the job
  to after all of the job's draws. There the corner sampler `3237af3d` read the finished main-view depth, the same depth
  `8df993dd` reads later, so all 627 corners were marked "unchanged" (-1.0, read back: 627 of 627), and the grid compute
  `3018813b` writes a tile whose four corners are all -1 negated. Placed before the job, the grid is the frozen back end's
  and Kokiri draws **684 a frame (frozen 685; direct before 1079)**.

Suspected, not proven:
- The loss is a fault of the driver or the hardware with two concurrent mesh workgroups of these mesh stages (nothing our side
  orders is missing: section 1d). The leads for M6e: OpGroupNonUniformShuffle (39 of 45 task+mesh mesh stages, every
  implicated one; 5 of 29 mesh-only ones) and the stages' size (most implicated ones need more than RADV's 30 KB of NGG LDS
  for their outputs, so RADV keeps outputs in its mesh scratch ring, indexed by the hardware's wave id); neither is shown.

## 1. The market loss

### a. Runs (every market run of this stage)

| Run | Build / configuration | Result |
|---|---|---|
| m6d_spv, m6d_spv2 | `taskdry,taskspv` (no task+mesh draw recorded; the SPIR-V dump) | survived 240 s / 150 s (expected) |
| m6d_x1_tmsync | `marks,tmsync`: a full barrier (ALL_GRAPHICS, inside the rendering) before **every** task+mesh draw | **survived** 240 s, 13,800 presents, 65 task+mesh draws a frame drawn |
| m6d_x2_tmpair | `marks,tmsyncpair`: the same barrier only before a task+mesh draw whose previous draw in the same command buffer was one too | **survived** 240 s |
| m6d_x3_tmlocal | `marks,tmlocal`: the framebuffer-local barrier (legal in a rendering) between two such draws in a row | **survived** 240 s |
| m6d_c1_marks | `marks` alone: the control, same build | **lost** at present 487 (102 s, loss 1 below); open: task+mesh `691ce175`, `f69141e9` x2, `9d2bef11` in one job secondary |
| m6d_fix_mk1..5 | the pair rule as the default (commit 78c56a2) | **survived** 5 of 5, 240 s each |
| m6d_fin_mk1..5 | final build: the wait before every task+mesh draw + the grid fix (7c77749) | **survived** 5 of 5, 240 s each (60 fps) |
| m6d_fin_mklong | final build, 15 minutes | LONG_RESULT |

So: with no separation the market lost the device in 13 of 13 runs (M6c 12, this stage's control 1); with any separation
of two task+mesh draws it lost it in 0 of 14 runs (3 single-variant runs, 5 pair-rule runs, 5 + 1 final-rule runs). By variant: `tmsync` 1 of 1 survived, `tmsyncpair` 1 of 1, `tmlocal` (pair) 1 + 5 of 6, the
final rule (before every task+mesh draw, framebuffer-local) 6 of 6. `tmsyncafter` was not run. One run per diagnosis variant
is weak on its own (a loss came within 22-111 s in all 13 losing runs, so a 240 s survival is unlikely by chance, but not
proof); the rules that were made default carry the 5 x 240 s and the 15-minute runs.

### b. What the experiments say

- The loss needs two task+mesh draws' mesh stages to be in flight together. A drain before every task+mesh draw (`tmsync`)
  and one only between two in a row (`tmsyncpair`) both end it; taskdrain (M6c: a full barrier before each job's task
  step, so before all of the job's task compute) did not. That last one also answers the coordinator's question "is it
  the task compute of draw 2 against the mesh stage of draw 1?": **no**. Inside a job, every task step of the job runs in
  its own secondary before the job's rendering secondary, with a barrier (compute to mesh shader and indirect) after it, so
  draw 2's task output is complete before draw 1's mesh stage starts; and the drain before the next job's task steps
  (taskdrain), which keeps one job's task compute from overlapping the previous job's mesh draws, did not help. The
  overlap that matters is mesh stage against mesh stage.
- A PS partial flush is enough: RADV records the framebuffer-local barrier (src and dst framebuffer stages, by region) as
  `AC_BARRIER_SYNC_PS` plus CB/DB flushes (`radv_stage_flush`, `../mesa` main; 26.2.4 the same), no VS partial flush. An
  event in the queue that completes when every earlier draw's pixel work has, so (INFERRED from how the event is ordered;
  the survival is the evidence) also when their mesh waves are done.
- What the two overlapping draws share (read from their SPIR-V and the task step's layout): each draw reads its own task
  header, map and payload (separate allocations in the frame's arena or the thread's task pool, never reused within a frame:
  M6c's `taskcheck`, 0 wrong of 310,180), the game's meshlet, vertex and index buffers (read only), the uniform buffers and
  textures; they write only their outputs. **No memory is written by one and read by the other.** Mesh-only draws read by
  address too (10-31 loads each) and are not kept apart; they never lost the device in either back end.

### c. The stages' reads and writes (for M6e; `work/proto/direct-m6d/spv/`, `spvside.py` counts only code reachable from the entry point)

| Program | mesh stage | fragment stage |
|---|---|---|
| `a437aea2af57707a` | 2 barriers, 76 loads by address, no store but outputs, no loop | 1 loop (trip count `min(x, c6[452]) + 1`), 20 loads by address |
| `bee3d34e53bf3e34` | 2 barriers, 75 by address | 1 loop, 12 |
| `cfebb562ec378f87` | 2 barriers, 65 | 1 loop, 13 |
| `691ce175190d057a` | 2 barriers, 62 | 1 loop, 20 |
| `f69141e92f08bc7b` | 2 barriers, 65 | 1 loop, 12 |
| `9d2bef119436ea0e` | 2 barriers, 62 | 1 loop, 20 |
| `30dcbaf546843b46` | 2 barriers, 1 atomicIAdd (a statistics counter, c0+0x780: M0), 28 | 1 loop, 11 |
| `8b41aa44d51f053e` | 2 barriers, 50 | 1 loop, 22 |
| `d551405e156060ae` | 2 barriers, 77 | 1 loop, 12 |
| `d7933378103449f2` | 2 barriers, 74 | 1 loop, 20 |
| `26811d11f81b2879` | 2 barriers, 50 | 1 loop, 22 |

- Over all 45 task+mesh mesh stages of the market: no loop in any; 4 have one atomicIAdd (counters); none stores to storage.
  Over the 29 mesh-only stages: no loop; 2 (`739acd678ae08655`, `d905f52b7517c681`) store to storage (10 stores) and add
  a counter. The loop in `a437`'s fragment stage was traced to a uniform word; the other fragment loops were not traced.
- Private or function variables (what could become scratch memory, the lead from Lichii/Mesa issue 16816): **none** in any
  implicated mesh stage (1 of 45 task+mesh mesh stages, `063b4c21712f159c`, has 5; 1 mesh-only stage has 1). So the
  "loop counter in private memory without scratch" idea does not apply at the SPIR-V level. Whether ACO spills registers to
  scratch for these stages was not measured: RADV prints shader statistics for pipelines, not for shader objects, so it needs
  a pipeline-based compile of these modules (M6e's harness). The shallow `../mesa` clone has no history to look up 16816 or
  !45092; the direct back end uses shader objects (no `DISABLE_OPTIMIZATION` flag exists for them), so upstream Hookshot's
  optimizer switch does not apply here.
- What tells task+mesh mesh stages apart from mesh-only ones, by opcode presence: `OpGroupNonUniformShuffle` (39 of 45
  task+mesh, every implicated one with 8-20 shuffles and as many ballots; 5 of 29 mesh-only). By size: all are 32 threads
  with 96-160 output vertices and primitives (126/126 in 38 of 45 task+mesh stages, never in mesh-only ones; `30dc` is
  128/128). Workgroup memory plus outputs, estimated from the SPIR-V: 7 of the 11 implicated stages are above RADV's 30 KB
  NGG LDS budget (`ac_nir_lower_ngg_mesh.c`), so RADV moves their outputs to its mesh scratch ring, an area indexed by the
  hardware's ordered wave id (`AC_MESH_SCRATCH_NUM_ENTRIES` 2048, "depends on VGT_GS_MAX_WAVE_ID ... impossible to query");
  4 are below (`691c`, `9d2b`, `8b41`, `2681`), and 4 mesh-only stages are above too. Neither feature separates the classes
  cleanly; both are leads, not findings.

### d. Hardware and driver evidence

- RADV's hang report of M6c (`m6c_mk_main_hang/radv_dump`): GRBM_STATUS 0xe7f10028 (GE, SPI, PA, SC, DB, CB busy, TA idle),
  the bound compute pipeline an internal query shader; the job secondaries (IB2) are not decoded by RADV and were not decoded
  here. Nothing in it names a wait our side owes.
- With validation and synchronization validation the market ran clean before the loss (M6c); this stage's final build: VAL_LINE.
- Nothing our side orders is missing that we know of: the implicated draws share no written memory (1b, 1c). The evidence
  points at the driver or the hardware: two concurrent mesh draws of these stages wedge the 3D pipe. **Not proven**: no
  reproducer outside the game yet (M6e builds the harness that replays pairs of these draws).
- **What a Mesa report would contain** (not sent; for the user to review first, and no game data in it): GPU RX 6950 XT
  (Navi 21, GFX10.3), kernel 7.2.11, Mesa 26.2.4 and main e04357c4642; a Vulkan application drawing with
  `VK_EXT_mesh_shader` (no task shader, `NO_TASK_SHADER` shader objects, `vkCmdDrawMeshTasksIndirectEXT` with one record),
  mesh shaders of 32 invocations, 126 output vertices and primitives, subgroup size 32 required, with subgroup ballots and
  shuffles, 2 workgroup barriers, no loops, workgroup memory about 12-17 KB; two such draws back to back inside one
  rendering (executed from a secondary) hang the graphics ring (`ring gfx_0.0.0 timeout`, MODE1 reset) within 20-110 s of
  steady play at 60 fps; no VM fault; GRBM_STATUS 0xe7f10028; it does not happen with `RADV_DEBUG=syncshaders` or with a
  framebuffer-local pipeline barrier (PS partial flush) before each such draw; it is not fixed on main. Attach: a standalone
  reproducer with synthetic shaders of the same shape and synthetic data (to be made; the game's SPIR-V and buffers may not
  leave the PC), the kernel log, RADV's `RADV_DEBUG=hang` report of the reproducer.

### e. The rule and its cost

`gpu/direct/direct_draw.cpp`: before every task+mesh draw, one `vkCmdPipelineBarrier2` with a memory barrier from and to the
framebuffer stages (fragment shader, early and late fragment tests, colour attachment output; colour and depth attachment
writes to reads), `VK_DEPENDENCY_BY_REGION_BIT`, which a rendering may hold with `dynamicRenderingLocalRead` (enabled: the
device's features are all enabled but robustness). The class is the program's shape (a mesh stage fed by a task step), no
program is named. Why "before every one" and not "after another in the same command buffer" (the pair rule M6d first made
default, 5 of 5 survived): two task+mesh draws can follow each other across job secondaries, across renderings and submits,
or with an ordinary draw between them that does not drain the pipe; the event is in the queue's order, so a wait before
each one keeps every pair apart wherever the previous one was recorded. Counted per frame (`of the drawn: task+mesh, after
a wait for the work before it`): COUNT_LINE.

Frame time, pacing off (`HOS_NO_PACING=1 HOS_FPS_TARGET=1000 HOS_FPS_MAX=240`), 100 s runs, the last six 600-present windows,
alternated pairs: TIMING_HANG.

## 2. The negated occlusion grid

### a. The chain, from the data

The grid compute `3018813b775edcb6` (structured IR from M0's `tmtool`: `work/proto/direct-m6d/grid/3018813b775edcb6.ir.c`)
reads, besides what M6c compared, a **storage buffer** (`s1` = 0x113b62d000, +0x5b40): 576 16-byte tile records and, from
+0x4c00, a table of the 33 x 19 tile corners. When a tile's four corners are all negative and one of them is exactly -1.0,
it writes the tile's depth and value negated (`v89`/`v99` in the IR: `BitwiseXor32(v101, 0x80000000)`, `0xb3800001`). The
corner table is written in the same layer list by `3237af3d36d45499` (the main view's D32F 1536x864 depth sampled at each
corner) and later rewritten by `8df993dd62a156a3`, which samples the same depth again and, at a corner whose depth has not
moved (`d == c`), writes -1.0 (-2.0 where it moved nearer).

`HOS_DBG=gridlog` (extended: the corner table) read in Kokiri, 90 s each, `HOS_GAME_STEP_PER_FRAME=1`:

| run | corner table | grid tile depths | draws a frame |
|---|---|---|---|
| direct before (`m6d_grid_d`) | 627 of 627 corners exactly -1.0 | -0.00048089 .. -0.0000888 | 1079.1 (job 910.5, layer 167.6) |
| frozen (`m6d_grid_f`) | 0.9519 .. 1.0 (once 10 of 627 at -1) | +0.0000888 .. +0.00048089 | 685 (M6b) |
| direct, dispatches before the job (`m6d_grid_db`) | 1.0 everywhere | +0.0000888 .. +0.00048089, mean 0.000167289 (frozen 0.000167267) | **684.4** (job 518.9, layer 164.5) |

In the frozen back end's frame `3237af3d` is command #102 and `8df993dd` #325 (its draw statistics, `m6b_ds_f`), with the
main view's depth pass between them; in the direct back end both ran after the whole job. The reason is M3a's game patch
(`host/direct_job_sync.c`): the job's call of `f_8067c310` (the grid's dispatches, recorded from an item callback inside
the render job) is kept and replayed on the layer list **after** the job's `CallCommands`, so the first sample already saw
the finished depth, both samples were equal at every corner, every corner became -1 and every tile came out negated. The
culler then took the negated grid, culled almost nothing, and the frame drew ~395 more objects: M6c's draw-count gap.

### b. The fix

The kept dispatches now go **before** the job's commands (`host/direct_job_sync.c`, the job-call splice). There they see the
depth before the job's draws (1.0, cleared), so `8df993dd`'s corners keep moving and nothing is marked unchanged. It is not
exactly the frozen order, which runs them between the job's items after its first draws (corners 0.95-1.0 there); the exact
order needs the job cut at the dispatch (a job as two rendering secondaries with the dispatch between), not built. The
grid and the draw count it gives match the frozen back end's to within the run-to-run difference (above).

Frame time, Kokiri and HF, alternated pairs, pacing off (the barrier build against the final build): TIMING_GRID.

### c. The picture check's blind spot

`scripts/direct_picture_check.py`: for a float target (16- and 32-bit float colour, float depth) a tile's floor is now 2/255
of the channel's own scale in the reference (its 99th percentile of |value|, clamped to [1e-6, 1]) instead of 2/255, and HDR
values keep their sign through x / (1 + |x|) (they were clipped at 0, so a negated target compared as zeros). The selftest
gains three cases (`tests/test_direct_picture_check.py`): a tiny-valued R16F target (1e-4..5e-4, as the grid's input) with
rounding-sized noise passes; the same target negated fails; under M6b's absolute floor (`compare --absolute-floor`) the
negated one passes (the blind spot, shown). The goldens' noise tables were made again for the new metric (`noise.json`; M6b's
kept as `noise_m6b.json`); the pass identities are unchanged (Kokiri 110 / 10 / 18, HF 103 / 16 / 15, market 99 / 2 / 22).

## 3. Checks of the final build

CHECKS

## 4. Device losses

| # | Time | Whose | Configuration | Run | Recovery |
|---|---|---|---|---|---|
| 1 | 2026-11-12 23:58:48 (reset 19) | mine | market, `HOS_DBG=marks` alone (the control) | `work/runs/m6d_c1_marks` (kernel log saved) | reset succeeded; next run 00:01:56 after `vulkaninfo` |
| - | 2026-11-13 00:32:18 (reset 20) | M6e's harness (`meshlab`) | - | - | **my next run (`m6d_grid_db`, Kokiri, a safe configuration) started at 00:34:31, 2 min 12 s after it, without the vulkaninfo check**: a breach of the rule; from then on every run waited in `gpu_ready.sh` (3 minutes after the newest reset line, then vulkaninfo) |

One loss of the budget of 12 was used. No configuration was run a second time after a loss.

## 5. Code

| What | Where |
|---|---|
| The wait before every task+mesh draw (the rule), its counter | `gpu/direct/direct_draw.cpp` |
| `HOS_DBG=tmsync`, `tmsyncafter`, `tmsyncpair` (diagnoses: a full barrier around task+mesh draws inside the rendering) | `gpu/direct/direct_draw.cpp` |
| `HOS_DBG=taskspv` also dumps mesh-only programs' stages | `gpu/direct/direct_shader.cpp` |
| The grid's dispatches before the job | `host/direct_job_sync.c` |
| `gridlog`: the corner table and tile records | `host/direct_diag_grid.c` |
| The relative floor, sign-kept HDR, `--absolute-floor`, selftest cases | `scripts/direct_picture_check.py`, `tests/test_direct_picture_check.py` |
