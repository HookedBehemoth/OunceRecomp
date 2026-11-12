# M6c report: what the checks found (2026-11-12)

Branch `agent/m6c` (from native-vk 7d28e4a). Runs `work/runs/m6c_*` and `golden_market_*` (local), RX 6950 XT, RADV Mesa
26.2.4 unless said, one game process at a time; Kokiri `chisel_kokiri`, Hyrule Field (HF) `chisel_hf`, market
`chisel_hf_market`. Tests: `HOOKSHOT_VULKAN_HEADERS=/usr/include work/venv/bin/python -I -B tests/run_all.py`: 295 pass / 5 fail
(the baseline five). No change to the frame path except debug aids behind `HOS_DBG` (one load and a branch per draw or
dispatch when off), so no frame-time claim is made and none was measured.

The stage was taken over by the market loss (the coordinator made it first priority): tasks 2-4 were advanced as far as
the evidence below, not finished.

## Summary

Verified (a check that could have failed, named):
- **The market loses the device** with the direct back end: 12 of 12 market runs that drew the task+mesh draws natively without a drain lost it
  (kernel `ring gfx_0.x.0 timeout`, MODE1 reset), on Mesa 26.2.4 and on Mesa main (e04357c, 1 of 1), on this build and on
  5beed38 (before M6b, 1 of 1). The frozen back end survived 3 of 3 runs of 420 s there.
- **Where:** the GPU breadcrumbs (new, `HOS_DBG=marks`) name, at every loss they covered (6 of 6), a run of native mesh
  draws of **task+mesh programs** (their task stage run as compute just before) in one **job secondary** (list kind 2,
  lr 0x8008533c) as started and not finished; the first open command was a task+mesh draw 5 times (`a437aea2af57707a` 3
  times, `bee3d34e53bf3e34`, on Mesa main `30dcbaf546843b46`) and once an indexed draw 8 commands ahead of the same run of
  task+mesh draws.
- **What makes it go away (one 240 s run each, against 22-111 s to a loss otherwise):** not drawing the mesh draws
  (`meshdry`), not drawing the task+mesh draws only (`taskdry`, mesh-only programs still drawn), and the driver draining
  the pipe after every command (`RADV_DEBUG=syncshaders`). **What does not:** a full barrier before each job's task step
  (`taskdrain`), the task stage's culling off (`tasknocull`), Mesa main, the build before M6b, validation (the loss came
  first, no message before it), strict (no event before it).
- **The stuck draw's inputs are sane:** header, map and payload of the first open draws read back after the loss (host
  memory): groups ≤ capacity, every map entry within its draw's task workgroups and threads, payload counts and
  consecutive meshlet ids (`m6c_mk_hdr1`). `taskcheck` checked 310,180 task headers and maps in a losing market run and
  77,920 in Kokiri: 0 wrong. No GPU page fault in any kernel log. The three implicated mesh stages pass `spirv-val`; every
  cached mesh stage sets its output counts exactly once on its single exit, with counts and indices clamped.
- **Resets blamed on kwin_wayland are ours when the game runs:** 6 of this stage's 12 resets named kwin's ring
  (gfx_0.1.0), and 4 of them had our breadcrumbs showing our job's mesh draws stuck at that moment (the two rings share
  the 3D engine; whichever ring's timer fires first is named).
- **Draw-count gap (task 2):** the game's CPU occlusion culling runs in both back ends (the grid accepted 4,379 of 4,380
  frames direct, 4,296 of 4,297 frozen), but **the GPU-written occlusion grid has the opposite sign in direct mode**:
  every tile's depth and value are exactly negated (direct -0.00048089 .. -0.0000888, frozen +0.0000888 .. +0.00048089),
  while every input of the program that writes it (`3018813b775edcb6`) is bit-identical between the back ends: the
  gathered R16F 768x432 target, the three uniform buffers, the camera. `f_80822260` takes each tile's sign into the
  culler. So the grid compute computes something different in the direct back end from identical inputs.
- Kokiri picture check, this build: **FAIL, 13 failing pass states** (exit 1; M6b 15). HF was not rerun.
- Market golden reference made: `work/golden/market` (3 frozen runs, 99 pass identities, 2 not comparable at one frame).

Suspected, not proven:
- The market loss is an overlap of draws inside one rendering (a job secondary) involving the native mesh stage of
  task+mesh programs, that the pipe drain after every command removes, and that is not in the task-side data. Whether it is
  a missing dependency between two of our draws (a storage write by one draw read by the next, which Vulkan cannot order
  inside a rendering), or a driver/hardware fault, is not settled. It has the exact signature of M0's native task+mesh
  hang (GRBM_STATUS 0xe7f10028 in both dumps: GE, SPI, PA, SC busy, TA idle; compute idle).
- The negated occlusion grid is the likeliest cause of the main-view draw-count gap (culling against a negated grid
  removes few or no shapes). Not yet shown: the culler's per-frame culled count on both sides, and the fixed picture.

## 1. The market loss

### Runs (n of m)

All market, `RX_BACKEND=direct` unless said; "loss" = `VK_ERROR_DEVICE_LOST` with a kernel ring timeout and a reset that
succeeded. Kernel logs: `work/runs/<run>/kernel_log.txt`.

| Run | Configuration | Result | Kernel names | Breadcrumbs: first open command |
|---|---|---|---|---|
| m6c_mk1 | plain, cold caches (a 68 s shader-object build) | survived 90 s, but stopped at present 666 before the market's task+mesh programs were first drawn | | |
| m6c_mk2 | plain, warm | **loss** at 32 s (reset 7) | gfx_0.0.0 hookshot Presentation Thread | (no marks) |
| m6c_mk_st | `HOS_DBG=strict` | **loss** at 25 s (reset 8); strict not armed, no event | gfx_0.0.0 hookshot | |
| m6c_mk_val | validation + marks | crash at start (marks bug: its buffer made before the heap's functions were loaded; fixed) | | |
| m6c_mk_val2 | validation (layer proven loaded) | **loss** at 26 s (reset 9); 0 errors, 0 warnings before it (the only warning is the loss itself) | gfx_0.1.0 kwin_wayland | |
| m6c_mk_mk | marks | the same start-up crash | | |
| m6c_mk_mk2 | marks | **loss** at 22 s (reset 10); marks unreadable (their buffer was in VRAM; fixed) | gfx_0.0.0 hookshot OptimizeChangeBanc | |
| m6c_mk_mk3 | marks | **loss** at 28 s, present 685 (reset 11) | gfx_0.1.0 kwin_wayland | task+mesh `a437aea2af57707a` (4 task workgroups), then 17 more task+mesh draws of the same job |
| m6c_mk_main1 | marks, **Mesa main** e04357c (`VK_DRIVER_FILES`, `RUN=check`) | **loss** at 91 s, present 597 (reset 12) | gfx_0.0.0 hookshot | task+mesh `30dcbaf546843b46` (task family C `d91e`), then 6 mesh-only indirect draws |
| m6c_mk_tc | marks, `taskcheck` | **loss** at 93 s, present 4815 (reset 13); 310,180 headers checked, 0 wrong | gfx_0.1.0 kwin | indexed draw `a14724a987c513eb` (438 indices), 7 indexed draws, then 16 task+mesh draws of the same job (`a437...`, `cfeb...`, `bee3...` ...) |
| m6c_mk_base1 | plain, **5beed38** (before M6b) | **loss** at 111 s (reset 14) | gfx_0.1.0 kwin | (no marks in that build) |
| m6c_mk_dry1 | marks, `meshdry` | **survived** 240 s, 13,800 presents | | |
| m6c_mk_tdry1 | marks, `taskdry` | **survived** 240 s, 13,800 presents | | |
| m6c_mk_nc1 | marks, `tasknocull` | **loss** at present 553 (reset 15) | gfx_0.0.0 hookshot | task+mesh `a437aea2af57707a` |
| golden_market_1..3 | **frozen** back end (task and mesh as compute), dump at 1500 | **survived** 3 of 3, 420 s each | | |
| m6c_mk_sync1 | marks, `RADV_DEBUG=syncshaders` | **survived** 240 s, 10,200 presents (45 fps) | | |
| m6c_mk_drain1 | marks, `taskdrain` | **loss** at 29 s, present 992 (reset 16) | gfx_0.1.0 kwin | task+mesh `bee3d34e53bf3e34`; the job's earlier task+mesh draws had finished |
| m6c_mk_main_hang | marks, Mesa main, `RADV_DEBUG=hang` | **loss** (reset 17); RADV's hang report saved (`work/runs/m6c_mk_main_hang/radv_dump`, local) | | (RADV aborts first: no marks) |
| m6c_mk_hdr1 | marks, arena in host memory, header dump | **loss** at present 884 (reset 18) | gfx_0.1.0 kwin | task+mesh `a437aea2af57707a`: header, map, payload sane (below) |

So: with task+mesh draws drawn natively and no drain, 12 losses in 12 market runs (the start-up crashes of the first marks build
and the cold run that never reached the market's programs not counted). Kokiri lost the device in none of this stage's 12 runs
(HF was not run in this stage; none of M6b's HF runs lost it); HF draws
the same programs (`a437...`, `cfeb...`, `30dc...`, `2681...`) with about 32 direct task steps a frame against the
market's 61.

The loss is not at the programs' first draw: in mk3 they were first drawn at present 490 and the loss came at 685; in
m6c_mk_tc at present 4815. The RSS jump seen in every losing run (to 15-22 GB) comes 2-3 s **after** the reset (the game
running on against a dead device), not before it.

### What the breadcrumbs are (new: `HOS_DBG=marks`, `gpu/direct/direct_marks.{h,cpp}`)

Every draw, task step and dispatch is bracketed by `vkCmdWriteBufferMarker2AMD` writes into host memory (top of pipe: the
command processor reached it; bottom of pipe: it and everything before it finished); each command has an entry saying
what it was. At a lost device (the three submit sites) the started-and-not-finished commands are logged in recording
order, with the finished command before each in its command buffer, and for task+mesh draws their task header, map and
first payload. Under marks the back end's mapped buffers (arena, task pools) are made in host memory, because a reset
loses VRAM. Off by default.

### The stuck draw's inputs (m6c_mk_hdr1, read after the loss)

```
open #1397489 task+mesh draw key a437aea2af57707a ... present 884; before it in its command buffer #1397466 (finished)
  header: groups 41 y 1 z 1 taken 41 ... cap 128 tasks 4 dispatch 4 1 1 payload words 64 threads 32
  map (41 of 41): 100 101 102 103 104 105 106 107 108 0 1 2 ... 1f
  payload of task 0 (64 words): 0 20 0 0 b4 b5 b6 b7 b8 ...
```

The mesh workgroup count the GPU read is within the map; every map entry names one of the draw's 4 task workgroups and an
index below its 32 threads; task 0's payload has 32 meshlets (0x20) with consecutive ids. The following open draws look
the same. So the hypothesis "the mesh stage runs on task data that is not there yet" is **rejected for the task step's
output**. Not checked: the game's buffers the mesh stage reads by address with those meshlet ids (meshlet records, vertex
and index data), the next step.

### What it is not

- Not validation-visible API misuse (0 messages before the loss, layer proven loaded); not a strict event.
- Not the task step's data (above; `taskcheck`), not the task stage's culling or depth-grid walk (`tasknocull` loses),
  not a missing dependency before the task step (`taskdrain` loses).
- Not a page fault (none in any kernel log); not output counts or indices out of range (the translator clamps both; every
  cached mesh stage has one `OpSetMeshOutputsEXT` on its single exit, checked offline over the 84 cached mesh modules);
  not a runaway loop on its own (Eden's loop safety caps every loop at 0x2000 trips, and `syncshaders` would not help a
  single runaway draw).
- Not M6b's two changes (5beed38 loses), not fixed in Mesa main (1 of 1).
- Not memory: RSS grows only after the loss.

Comparison with M0 (`m0-hang-dump.md`, `m0-task-mesh.md` "The stuck program"): the same register signature (GRBM_STATUS
0xe7f10028 in M0's dump and in RADV's report here: GE, SPI, PA, SC busy, TA idle, the compute engine idle), and again the
first unfinished draw is not always a mesh draw (M0: the depth-only `ffb53ed102dd9908`; here once an indexed draw ahead
of the task+mesh run). M0 had native task shaders; here there is no task stage on the device at all, so the gang submit
is excluded, and what is common to both is the **native mesh stage of task+mesh programs**, drawn among other draws in one
rendering.

### Not done, for the next stage

- The buffers the stuck mesh stage reads by address (its meshlet records for ids 0xb4.., the vertex and index data they
  point to): read them after a loss as the header was read, and check counts and offsets against their buffers.
- Whether one of the job's draws writes storage a later draw of the same rendering reads (mesh stages `6ba0b411` and
  `9d026703` add primitive counts to a storage buffer, m0-task-mesh.md 1): that is a dependency Vulkan cannot express
  inside a rendering, and the pipe drain would hide it.
- RADV's hang report (`radv_dump`) decodes only the primary; the job secondaries' packets were not decoded.
- No fix: the brief forbids a per-program case, and the evidence does not yet name a general cause.

**For the user:** the market is not playable with the direct back end on either Mesa; Mesa main does not fix it. The
frozen back end and the direct one with `RADV_DEBUG=syncshaders` (45 fps) run it. This is the same hang family as the
native task+mesh hang; a reduced reproducer (the job's command stream replayed) would be what an upstream report needs.

### Hang rules kept, and where not

Each loss: the kernel log saved into the run folder, `vulkaninfo --summary` checked, then 3 minutes. Two runs started
early: `m6c_mk_st` 2 min 40 s after reset 7 and `m6c_mk_mk` 2 min 21 s after reset 9 (both mistakes of mine; recovery was
verified by vulkaninfo before each). Firefox's GPU process segfaulted at three of the resets (kernel log). No configuration
lost the device three times: the plain configuration lost twice (mk2 on this build, base1 on 5beed38), marks twice
(mk_mk2, mk_mk3); `m6c_mk_hdr1` is marks with the arena moved to host memory and the header dump, a different build.

### The market check and strict

`work/golden/market` exists. The direct check of the market was not run: the direct back end loses the device there
before or around the dumped present (1500) in nearly every run. Strict was run (`m6c_mk_st`): no event before the loss.

## 2. The draw-count gap

M6b: direct about 1098 draws a frame in Kokiri, frozen 685, concentrated in the main view's depth prepass and lit job
passes, the same programs about twice as often.

**Not duplicates.** In M6b's direct trace (`m6b_ds_d`), the 181 depth-prepass draws of `ffb53ed102dd9908` are 181 distinct
draws (by vertex buffer, count and model-matrix buffer), so the back end does not expand a packet twice: the game submits
more objects.

**The game's CPU occlusion culling** (readback.md: `f_80365f00` takes the GPU's 32x18 depth grid if its stamp is one or
two frames old, `f_80822260` culls against it only when it was taken). New `HOS_DBG=gridlog` (`host/direct_diag_grid.c`,
either back end, host only) at `f_80822260`'s entry:

| | grid taken (frames) | of them the frame before | tile depths (slot 0, same stamp) |
|---|---|---|---|
| direct (`m6c_grid_d`, `m6c_grid3_d`) | 4,379 of 4,380 | 4,364 | **-0.00048089 .. -0.0000888**, mean -0.000167 |
| frozen (`m6c_grid_f`, `m6c_grid3_f`) | 4,296 of 4,297 | 1,635 | **+0.0000888 .. +0.00048089**, mean +0.000167 |

Both cull every frame, but direct's grid is the frozen grid negated, value for value. Its writer, compute
`3018813b775edcb6`, was compared input by input with `HOS_GAME_STEP_PER_FRAME=1`:
- the texture it gathers (id 29033, the R16F 768x432 colour 2 of pass `ddd080229f785e4d`): identical statistics in the golden
  frozen dump and this stage's direct dump (min 7.761e-05, max 0.0004809, mean 0.000156: positive in both);
- its three uniform buffers (0x113b620000 the camera and grid constants, 0x113b649000, 0x113b65b000; words 0-3 and 768-799):
  bit-identical (`m6c_grid4_*`);
- the camera and projection it copies into the slot: identical.

So the same inputs give a negated output in the direct back end. The direct SPIR-V and the frozen-era SPIR-V of this
program have the same arithmetic (7 gathers, 4 multiplies, 1 divide, 43 extended instructions, no negation); the
differences are how buffers are reached (the direct fork reads them by device address) and the fork itself. Not settled:
which instruction differs. The next check is the program run offline by both translators on these inputs (M0's `tmtool`),
and then the culled-shape count and the draw count with the fix.

The picture check cannot see this: the R16F target and the grid hold values near 0.0005, far below its 2/255 floor after the
x/(1+x) normalisation (section 3).

## 3. The failing passes

Kokiri (`m6c_chk_k0`, `work/runs/m6c_chk_k0/verdict.txt`): **FAIL**, 343 states: 13 FAIL, 3 EXTRA, 9 UNSTABLE, 238 MERGED,
80 PASS (exit 1). The same passes as M6b's list less the two lit 1536x864 `t1` states. The earliest failing pass in frame
order is the RGBA16F 512x512 `0ddde357f741d297` (state 146; G and B 3.2-3.3 % of tiles). Not worked on further: the
market loss took the stage. HF was not rerun.

Found while doing section 2: **what the check can miss.** A float target whose values are small (the R16F 768x432 above,
about 0.0005; any depth-derived or velocity target) can be wrong by its own size, even in sign, and pass, because a tile's
threshold is at least 2/255 after x/(1+x). A relative threshold for float targets, or a sign check, would catch it.

Also fixed: the Kokiri reference's `noise.json` named its map file `noise_k2.json.npz` (it was renamed after it was made),
so the check could not run; it now names `noise.json.npz` (local data).

## 4. The task pool

M6b's proposed rule: each indirect task draw reserves 320 task workgroups' map and payload at record time, from the
culling writers' `atomicMax(ceil(min(total, 0x1400) / 16))`. Verified against M0's IR prints of the five writers
(`work/proto/direct-m0/tm/out`):
- `4247f1b4c3ce6cf3` and `f0fed2bbccae7bdc`, record 0: `ceil(min(n, 0x1400) / 16)`, at most 320: holds;
- `cb2e06ddfb9d7d27`: `ceil(min(n, 0x1000) / 16)`, at most 256: holds;
- `3e6fe279463b020e`: `ceil(min(n, L) / 32)` with **L loaded from memory** (`+0x24` of a table): bounded by data, not by
  the code;
- `f0fed2bb` and `cb2e06dd` also raise the records' word 1 (and record 1's) by `atomicMax(v + 1)` of unbounded values (M0
  said nobody on the GPU writes those words);
- `7984161db3e053b5` only appends (atomicAdd), no group count.

So the 320 bound **does not hold as a rule for every writer**, and a static reservation sized by it could drop a draw's
workgroups. Not built; the pool stays as M4c made it (strict raises a refusal as an event).

## Code (all debug aids behind HOS_DBG; nothing changes with it unset)

| What | Where |
|---|---|
| `HOS_DBG=marks`: breadcrumbs, the report at a lost device, the task header dump; mapped buffers in host memory under it | `gpu/direct/direct_marks.{h,cpp}` (new), `direct_main.cpp`, `direct_sync.cpp`, `direct_present.cpp`, `direct_draw.cpp`, `direct_task.cpp`, `direct_compute.cpp`, `direct_heap.cpp` |
| `HOS_DBG=taskcheck`: every task step's header and map checked when its slot is reset | `direct_task.cpp`, `direct_cmd.{h,cpp}` |
| `HOS_DBG=taskdrain`: a full barrier before each job's task step (a diagnosis) | `direct_pass.cpp` |
| `HOS_DBG=gridlog`: the CPU occlusion culling's grid, its content and its compute's uniform words (either back end) | `host/direct_diag_grid.c` (new) |

Local, not committed: the Mesa main RADV build and its ICD file (`work/mesa-main`), RADV's hang report
(`work/runs/m6c_mk_main_hang/radv_dump`), `work/golden/market`.
