# Every shader of the game, what it costs, and whether rewriting pays

## Summary

- **The census** (part 1): the romfs holds **42,856 stages in 38,809 programs** (15,852 vertex, 22,356 fragment, 4,128
  compute, 20 task, 500 mesh). The translator fork takes **42,834** of them (22 refuse to lower: 13 "native stage buffer
  access" fragment stages, 9 others) and RADV compiles every one it takes (VERIFIED: `censustool`, a pipeline each, its
  executable statistics). Medians: fragment 897 native instructions → 1,807 IR → 3,651 SPIR-V → **1,449 ACO instructions,
  13 texture samples, 64 VGPRs**; vertex 350 → 1,158 ACO; mesh 1,235 → 2,860 ACO, 21.5 KB LDS. RADV removes most of the
  translation's bulk: ACO / native is **1.56 for fragment**, 1.81 compute, 2.47 mesh, 3.25 vertex, 3.66 task (ESTIMATED as
  a measure of overhead: the two ISAs are not the same size per operation).
- **The scenes** (part 2, `HOS_DBG=pstats`, a new debug aid: a pipeline statistics query and two timestamps around every
  command of one frame): Kokiri draws **1,087 commands with 351 programs** (504 stages), Hyrule Field **2,189 with 463**
  (658 stages); **824 distinct stages** in the two, **338** in both; 42,010 of the game's stages are seen in neither.
  The market (with `HOS_DBG=meshdry`: its mesh draws not drawn) is in section 2.3.
- **Static cost against the GPU's time** (in-game, indicative): ACO's estimate of the frame's shader work is **1.90 ms (Kokiri) and 1.98 ms
  (HF)**; the commands' measured share of the queue's timeline is **6.26 / 7.34 ms**, and the GPU is busy **7.4 / 8.5 ms**
  a frame (gputime). The estimate explains **30 % / 27 %** of the measured command time: most of the frame is not ALU work
  ACO can see (memory latency, fixed function, many small draws and dispatches, the barriers' flushes). By command: 249
  of Kokiri's 351 programs (4.2 of 6.3 ms) and 385 of HF's 463 (4.9 of 7.3 ms) take more than ten times their estimate.
- **Where the time goes** (in-game, indicative): one compute pass, **`fd029a317a9a4b94`** (a tiled post-processing compute: 1,152
  workgroups of 128, 12 gathers and 16 explicit-LOD samples a thread, 5.6 KB of shared memory, 6 barriers) takes **1.03 ms in
  Kokiri (16.5 %) and 1.02 ms in HF (13.9 %)**, twenty times its estimate (in-game timestamps, indicative; why: not
  established; it stores 16-bit values to shared memory through compare-and-swap loops, section 5b).
  Then shadow-map casters (368 vertex draws, 0.28 ms in HF), small compute steps of 256-25k threads at 0.1-0.2 ms each,
  the meshlet lit geometry (`063b4c21712f159c`, 0.19-0.30 ms), grass, and the lit materials (0.1-0.13 ms each). Half the
  measured time is in 21 (Kokiri) / 23 (HF) programs, 80 % in 77 / 86.
- **The rewrites** (part 3): one new decompiled stage, the full-screen **fog and atmosphere composite** (fragment
  `2407c9a14d62a1d6`, in both scenes), **bit-exact** against the translator's on the captured draws (VERIFIED: 4,194,304
  words of float targets equal, Kokiri capture; Hyrule Field below); RADV compiles it to 643 instructions against 656, and
  it runs **no faster** (harness, quiet GPU: ratio 1.007, p10 1.000, p90 1.014, 41 ABBA rounds; also equal on the Hyrule
  Field capture). The fragment harness that
  checks it (`meshlab fs`) is new.
- **Does the size improvement mean speed? No, not for the stages measured.** M6e's two mesh rewrites compile much smaller
  (lit: VGPRs 160 → 48, LDS 18,432 → 9,216 in the pipeline's count, waves 6 → 14 a SIMD, ACO's throughput estimate 1,244 → 962;
  depth: VGPRs 64 → 32, LDS 6,144 → 3,072, IT 645 → 499), and on the captured draws they run **exactly as fast**: ratio
  1.000 (depth, 38 draws, p10-p90 0.993-1.008) and 1.000 (lit, 5 draws, 0.993-1.007), with rasterization off as well (1.001).
  Also with 32-64× the workgroups a draw, where the stage's own work dominates (9.4 and 33 ns a workgroup): 1.001 and 1.000
  (all harness, quiet GPU). The limit is outside the stage's ALU and occupancy (INFERRED: the mesh workgroup launch and
  primitive rate).
- **The translator** (section 5b): RADV removes most of what the translation adds; what it cannot remove and recurs:
  window-table translation of by-address loads (31,952 stages), sub-word shared stores as compare-and-swap loops (15
  compute stages, among them the heaviest pass `fd029a…`), the mesh stage buffer in LDS (499), uniform-register emulation,
  the doubled NaN guard. Prototyped: shared memory in explicit layout for stages with 16-bit stores (Eden supports it; the
  direct profile never enables it): RADV's estimate −33 to −44 % on all 15, `fd029a…` IT 4,096 → 2,484 (static, VERIFIED by
  RADV's statistics; not timed: no compute harness). Prototyped and timed: the NaN guard once: bit-exact on 3 captured
  draws, RADV 3-4 instructions fewer a stage, harness timing within ±1 % but taken with a game process running (not a
  result).
- **What would pay:** not hand-rewriting stages one by one. The ranked list (section 5) puts the heavy compute pass first
  (up to 1 ms a frame if its cost can be found and removed), then the per-command overheads (about 4 ms a frame of commands
  that do little shader work), then translator-wide fixes that RADV does not clean up: the mesh stage buffer in LDS, the
  16-bit shared stores emulated by compare-and-swap loops (in `fd029a…` itself), the doubled NaN guard, predication lowered
  to one branch an instruction.

Device losses of this stage: **none** (0 of the budget of 3). Tests: 296 pass, 5 fail (the baseline five; 296 on native-vk e8b19db).

## Method

### The census (`tools/shadercensus/`)

- **Every program:** `nx2/shader_census.py` (unchanged) walks the romfs and writes the seed `hookshot_shaders.bin`: each
  stage once (control section and code area, the bytes the game hands NVN) and the programs (which stages go together).
  4.9 s for 42,856 stages.
- **Every stage through the translator as the back end translates it:** `censustool` (C++, links the standalone build's
  translator objects `dx_eden_*.o`) uses `tools/meshlab/offline.h`, the offline translation meshtool already did, now shared:
  the code read and lowered (`load`), the translator's questions answered as `direct_shader.cpp` / `direct_compute.cpp`
  answer them with nothing bound (`Env`), the profile and host info of this card. Each stage is translated in the first
  program that uses it: a fragment stage after its vertex (or mesh) stage, whose outputs it is translated against, with
  float colour targets and the NaN guard; a mesh stage with its task stage's registers; a task stage as the compute step.
  Offline differences from play: vertex attributes all float, texture answers "2D" (the type the instruction fixes stays),
  driver bank words zero. One stage was diffed against the game's own translation: the fog composite's captured SPIR-V
  equals `censustool`'s but for the binding numbers (VERIFIED: `spirv-dis` diff, 2,080 lines; the game's bindings start after
  the vertex stage's).
- **RADV's statistics:** the back end makes unlinked shader objects, which report nothing (`RADV_DEBUG=shaderstats` prints
  nothing for them, checked). `censustool --compile` makes a **pipeline** instead, with the back end's heap mappings
  (`direct_shader.cpp finish` for one stage: uniform buffers through the address table at 120, textures by view-type region,
  the sampler arrays, storage images), 32 lanes where the device allows, and reads `VK_KHR_pipeline_executable_properties`:
  ACO's instruction count, code size, VGPRs, SGPRs, LDS, scratch, spills, waves a SIMD, its latency and inverse-throughput
  estimates, VALU / SALU / VMEM / SMEM counts. A graphics stage is compiled with its program's partner (vertex + fragment,
  mesh + fragment), so RADV links them and drops outputs the partner does not read: a vertex stage's numbers are a little
  low against the unlinked object the game runs (INFERRED). The shader cache was off (`MESA_SHADER_CACHE_DISABLE=1`).
- **From the IR** (after translation, before SPIR-V): inputs and outputs (generic lines and components, colour targets,
  mesh vertices, primitives, payload); texture samples by instruction (implicit, explicit LOD, shadow, gather, fetch,
  gradient) and dimension, how many sit in a branch or a loop, distinct descriptors; uniform loads by bank (c1 the program's
  constants, c0/c2 the driver banks, c3+ game buffers; indexed ones), by-address loads and stores, atomics, shared memory,
  barriers; branches and loops by what their conditions depend on (constants, uniforms, inputs, memory), each
  constant-bounded loop's trip count (its phis stepped by an evaluator over the integer ops; past 1,024 trips: "constant,
  long"); discards; markers of the translation (subgroup operations, SelectU1, stage-buffer operations).
- **Resumable:** `census.py` runs the tool in shards of 250 stages, at most N at once, at nice 19; a shard that is complete
  is not run again; a stage that crashes the tool is recorded and the shard rerun without it (none crashed). 42,856 stages
  in 6-12 minutes on 3-8 cores. No GPU lock (pipelines are compiled, nothing drawn).
- **The cost unit,** one a stage kind, from ACO's "Inverse Throughput" (IT: estimated busy cycles of one wave; ACO weights
  blocks in loops by 8, 4 and 2 by nesting, uniform branches by 1/2, divergent ones by 3/4) in SIMD cycles:

  | Kind | Unit | Cost |
  |---|---|---|
  | fragment | a pixel shaded (fragment invocation) | IT / 32 lanes |
  | vertex | a vertex (vertex invocation) | IT / 32 |
  | mesh | a mesh workgroup (meshlet); also an emitted vertex | IT × waves a workgroup; / max vertices |
  | task | a task workgroup | IT × waves a workgroup |
  | compute | an invocation (and a workgroup) | IT / 32 (IT × waves) |

  A frame's estimate is the sum over its commands of cost × count, divided by 160 SIMDs × 2.3 GHz. It is an ALU estimate:
  it knows nothing of memory latency, cache misses, rasterization, blending, launch and synchronisation, and it weights a
  data-bounded loop by 8 whatever the data says.

### The scenes (`HOS_DBG=pstats:<present>`, `tools/shadercensus/scene.py`)

- **Counts:** every draw, mesh draw, task step and dispatch recorded at the given present is bracketed by a pipeline
  statistics query (input vertices and primitives, vertex, clipping, fragment and compute invocations) and two timestamps
  (`vkCmdWriteTimestamp2`, all commands: written when all the work before it has finished; the second after the command).
  Ten presents later `direct_pstats.csv` is written: per command its program, stage code keys, list kind, pass size, CPU
  counts, statistics and timestamps. Off by default (one load and a branch a command). Mesh shader invocations: RADV did
  not offer `meshShaderQueries` on this GPU, so mesh workgroups come from a `meshcap` capture of the same scene (the task
  headers' counts as the GPU left them), by program.
- **Measured time of a command:** the difference of its two timestamps, the time it added to the queue's timeline. A
  command that overlaps the one before counts less than its run time; one that starts with the cache flushes and waits of a
  barrier recorded before it (RADV emits them with the next draw or dispatch) counts them. The differences of a frame add
  up to 6.26 ms (Kokiri) and 7.34 ms (HF) against 7.4 / 8.5 ms busy: the rest is copies, clears, resolves, barriers between
  command buffers and the present (INFERRED).
- **Runs** (`tools/shadercensus/scene_run.sh`: `scripts/run-vk.sh` under the gate and the lock, the hang rules of
  `tools/meshlab/gpu_run.sh`): `chisel_kokiri` and `chisel_hf`, pacing off (`HOS_NO_PACING=1
  HOS_FPS_TARGET=1000 HOS_FPS_MAX=240`), the measured frame present 4500 (the scene has stood since about present 2700:
  the presents of the loading screens run uncapped), 130-160 s. `m6h_ps2_*` with `drawlog` and `meshcap` (no timestamps),
  `m6h_ps3_*` with the timestamps; `gputime` in both. Present 1500, which M6e used with pacing on, is still the loading
  screen with pacing off (`m6h_ps_k`, one command measured).
- **Naming:** the drawlog names a texture only when its handle is in a driver bank; the lit materials read theirs from a
  storage buffer (computed handles) and the drawlog cannot name them. Programs are named below by pass, counts and what their
  code does; "Link", "enemies" and "terrain" could not be told apart from other lit materials by this (not established).

### The fragment harness (`meshlab fs`, `tools/meshlab/fsbench.h`)

`HOS_DBG=meshcap` now keeps every program's stages and records every vertex draw of the frame with its uniform buffers'
bytes (`MC_VDRAW`: the driver banks as the back end made them, the game's buffers read at the frame's end, c1). `meshlab fs`
draws a captured draw's fragment stage, the translator's (A, as captured, with its heap mappings) or a replacement (B, the
same mappings), with:
- the draw's uniform buffers and push data;
- a vertex stage of ours: a 24 × 24 grid of quads over the target, every generic location written with a hash of the corner
  (`--range`, default [0, 1)): smooth inside a quad, varied across;
- noise textures behind every descriptor of every view-type region and linear / repeat samplers (compare: LESS_OR_EQUAL), so
  that any handle the uniform data names samples varied, deterministic texels;
- R32G32B32A32 targets for every output, cleared to a NaN pattern no stage writes, no blending (a discarded pixel keeps the
  pattern in both).

`--compare` reads both targets back and compares them word for word; `--bench` draws the grid 20 times a submission into a
1024 × 1024 target, A and B alternating ABBA over the rounds, GPU timestamps around each submission. What it does not give:
the draw's own vertex stage (its vertex buffers are not captured) and storage buffers (read as zeros: a light loop over a
storage list runs no iterations here).

## 1. The census

Source: `work/m6h/census/stages.jsonl` (local), `tools/shadercensus/report.py`.

| Kind | Stages | Translated | Compiled | Refused |
|---|---|---|---|---|
| vertex | 15,852 | 15,851 | 15,851 | 1 (low level native program form) |
| fragment | 22,356 | 22,343 | 22,343 | 13 (12 "native stage buffer access", 1 as vertex) |
| mesh | 500 | 499 | 499 | 1 (stage buffer) |
| task | 20 | 19 | 19 | 1 (stage buffer: `64ba846654e63ea4`, as M0 found) |
| compute | 4,128 | 4,122 | 4,122 | 6 (native global loads, lowering, one bad branch target) |

**Size and cost** (median / p90 / max; LDS and VGPRs as RADV reports them, in its allocation granules):

| Kind | Native | IR | SPIR-V | ACO | VGPRs | LDS bytes | IT (cycles a wave) | Cost unit (SIMD cycles) | Scratch | Spills |
|---|---|---|---|---|---|---|---|---|---|---|
| vertex | 350 / 697 | 723 / 1,378 / 9,549 | 1,627 / 3,322 / 22,609 | 1,158 / 1,845 / 14,386 | 48 / 72 / 256 | 4,096 (NGG) | 741 / 1,229 / 10,130 | 12 / 19 / 158 a vertex | 12 stages | 0 |
| fragment | 897 / 2,155 | 1,807 / 4,223 / 16,098 | 3,651 / 8,129 / 24,463 | 1,449 / 3,686 / 10,741 | 64 / 128 / 192 | 0 | 1,405 / 5,446 / 142,160 | 44 / 170 / 4,442 a pixel | 8 | 0 |
| mesh | 1,235 / 3,049 | 4,710 / 9,862 / 48,515 | 11,477 / 27,343 / 217,745 | 2,860 / 9,569 / 118,524 | 160 / 256 / 256 | 21,504 / 30,720 / 31,744 | 2,133 / 18,102 / 50,793 | same, a workgroup | 40 | 40 |
| task | 726 / 1,254 | 1,313 / 12,068 | 5,452 / 36,410 | 2,490 / 4,042 | 64 | 2,048 / 4,096 | 906 / 1,710 / 3,904 | same, a workgroup | 0 | 0 |
| compute | 204 / 887 | 574 / 3,221 / 37,619 | 1,099 / 4,548 / 106,535 | 341 / 3,661 / 12,341 | 64 / 96 / 256 | 0 / 0 / 22,528 | 314 / 4,984 / 128,236 | 9.8 / 156 / 4,007 an invocation | 0 | 0 |

**Translation size against the game's own code** (median / p90): IR / native 2.06 / 2.30 (vertex), 2.03 / 2.49 (fragment),
3.00 / 4.18 (mesh), 1.81 / 9.62 (task), 2.98 / 3.81 (compute); SPIR-V / native 3.9-8.3; **ACO / native 3.25 / 4.21 (vertex),
1.56 / 2.29 (fragment), 2.47 / 3.65 (mesh), 3.66 / 6.50 (task), 1.81 / 4.18 (compute)**.

**Inputs and outputs** (median / p90 / max): vertex attributes read 3 / 6 / 12 lines (8 / 16 / 37 components), outputs 9 /
14 / 18 lines (21 / 31 / 49 components); fragment inputs 7 / 12 / 19 lines (16 / 29 / 55 components), 1 / 1 / 8 colour targets;
mesh stages 126 vertices and primitives (160 at most), 18 / 29 / 41 vertex output lines, 32 threads, payload 64 words (800
for the instance families); task stages 32 threads (one of 1), payload 288 / 800 words; compute workgroups mostly 64 × 1
(3,621 stages), then 128, 32, 16 × 16, 8 × 8; shared memory 0 for most (at most 21,600 bytes).

**Texture samples:**

| Kind | Stages with any | Per stage (median / p90 / max) | Total | In a branch | In a loop | In a loop of unknown bound | Distinct descriptors (median / max) |
|---|---|---|---|---|---|---|---|
| vertex | 5,694 of 15,851 | 0 / 2 / 135 | 26,694 | 442 | 3,830 | 20 | 0 / 5 |
| fragment | 22,053 of 22,343 | 13 / 32 / 171 | 358,460 | 151,994 | 56,732 | 56,437 | 9 / 25 |
| mesh | 30 of 499 | 0 / 0 / 12 | 189 | 170 | 0 | 0 | 0 / 2 |
| compute | 784 of 4,122 | 0 / 1 / 123 | 6,735 | 3,155 | 964 | 942 | 0 / 12 |

Fragment samples by instruction: implicit LOD 251,888, shadow compare 70,583, explicit LOD 31,926, gather 2,744, gather
compare 648, compare at LOD 463, gradient 86, fetch 71, with bias 51; by dimension 2D 225,728, 3D 69,980, 2D array 40,433,
cube 22,073, cube array 246.

**Memory and control flow** (sums over the stages of a kind):

| Kind | Uniform loads c1 / driver / game (indexed) | By-address loads (stages) | Stores | Atomics | Shared loads / stores | Branches: const / uniform / memory / input | Loops: const / uniform / memory / input (trips known) | Discards (stages) |
|---|---|---|---|---|---|---|---|---|
| vertex | 8,477 / 247,652 / 1,468,903 (45,943) | 213,519 (13,740) | 33 | 0 | 0 | 3,332 / 73,964 / 723,379 / 101,836 | 270 / 55 / 16 / 4 (262) | 0 |
| fragment | 12,178 / 522,409 / 3,830,548 (81,987) | 216,214 (13,812) | 161 | 980 | 0 | 58,214 / 195,244 / 396,040 / 1,211,321 | 267 / 188 / 12,725 / 163 (110) | 13,482 (9,388) |
| mesh | 892 / 36,759 / 131,566 (10,486) | 39,826 (491) | 40 | 38 | 0 | 44,168 / 91,360 / 20,653 / 88,639 | 46 / 96 / 0 / 58 (8) | 0 |
| task | 0 / 208 / 6,703 (126) | 214 (17) | 0 | 24 | 0 | 428 / 1,560 / 2,548 / 1,470 | 2 / 0 / 34 / 12 (0) | 0 |
| compute | 224 / 143,010 / 77,351 (4,117) | 110,245 (3,892) | 34,031 | 1,966 | 15,116 / 5,397 | 4,265 / 11,825 / 124,969 / 56,824 | 53 / 205 / 109 / 144 (31) | 0 |

Findings of the census (each INFERRED from the IR unless said):
- **Fragment loops are bounded by memory:** 12,725 of 13,343 fragment loops end on a value read from memory (a storage
  buffer: the lit materials' light list, M6d's "one loop each"). Their samples (56,437) cannot be weighted offline.
- **Four mesh stages (`c64d72bbbe965143`, `3868defa…`, `65ca69c7…`, `2658d77e…`) have loops of 2^32 trips by their constants**
  (`while (i + 1 != 0xffffffff)`, writing local memory) and 290-300 KB of scratch: a translation of the game's loops whose
  bound the lowering lost, or code the game never runs that way; none is drawn in these scenes (open).
- Vertex stages read **3.8 game uniform words for every driver word**; every stage reads its uniforms through the address
  table (one indirection), and 46k vertex and 82k fragment uniform loads are indexed (matrix palettes, light tables).

The static top lists (by cost unit, per kind) are in `work/m6h/census/report.md` (local). Statically the most expensive
fragment stages are loop-heavy (IT 69,000-142,000 cycles a wave, 0 samples: ACO's loop weighting); none of them is drawn in
these scenes, which is why the per-scene tables, not the static ones, decide.

## 2. Use per scene

### 2.1 Kokiri (`m6h_ps3_k`, present 4500)

1,087 commands, 351 programs; 204 fragment, 177 vertex, 73 compute, 37 mesh and 5 task stages. Invocations: 0.37 M
vertex, **18.97 M fragment** (14 a pixel of the 1536 × 864 view, shadow maps and post passes included), 5.83 M compute;
5,945 mesh workgroups (capture). GPU busy 7.4 ms a frame (gputime, present 4200-4800 windows), commands 6.26 ms; estimate
1.90 ms (0.30 of measured).

Top 20 programs by in-game command time (indicative; est.: ACO estimate; the description is ours):

| # | Program | What it draws (ours) | Commands | Counts | Est. ms | In-game ms | Share |
|---|---|---|---|---|---|---|---|
| 1 | `fd029a317a9a4b94` | post compute, tiled (kind 3 list): gathers and explicit-LOD samples, shared memory | 1 | 147,456 inv. | 0.051 | 1.031 | 16.5 % |
| 2 | `063b4c21712f159c` | meshlet lit geometry (task `2c56` family) | 2 | 1,068 wg, 0.20 Mpx | 0.091 | 0.302 | 4.8 % |
| 3 | `ab7699ee7f536755` | compute step, 16 × 16 groups, atomics (kind 5 list) | 1 | 24,576 inv. | 0.001 | 0.215 | 3.4 % |
| 4 | `d8e59341cf4f629c` | grass (mesh-only, the leaf textures) | 1 | 713 wg, 0.10 Mpx | 0.048 | 0.123 | 2.0 % |
| 5 | `aba5e6af8d4683eb` | compute step, one 16 × 16 group | 1 | 256 inv. | 0.000 | 0.122 | 2.0 % |
| 6 | `592079442d7129da` | compute step, one 16 × 16 group | 1 | 256 inv. | 0.000 | 0.121 | 1.9 % |
| 7 | `46c14f8ee79c5349` | lit material, main view (52 draws) | 52 | 2,906 vtx, 0.27 Mpx | 0.108 | 0.117 | 1.9 % |
| 8 | `27ee9883e2a20323` | froxel volume compute (8 × 8 × 8 groups, 3D samples: the volumetric fog's grid) | 1 | 884,736 inv. | 0.623 | 0.097 | 1.6 % |
| 9 | `c587820ce7551a01` | lit material, main view | 5 | 877 vtx, 0.35 Mpx | 0.142 | 0.094 | 1.5 % |
| 10 | `20850618fff46b52` | meshlet geometry, depth pass | 2 | 1,068 wg, 1.07 Mpx | 0.010 | 0.092 | 1.5 % |
| 11 | `dba78f81ae692e07` | foliage shadow casters (task `919b`) | 6 | 18 wg | 0.000 | 0.087 | 1.4 % |
| 12 | `d8180bd8c6ecc77b` | full-screen shadow mask (cascade select, gathers: fragment `ce62c81dd5a4d9b0`) | 1 | 1.33 Mpx | 0.071 | 0.085 | 1.4 % |
| 13 | `ffb53ed102dd9908` | 181 small draws in the main view, no pixel shaded (occlusion test boxes, INFERRED) | 181 | 58,622 vtx | 0.000 | 0.085 | 1.4 % |
| 14 | `102f5b0abb0f1e63` | indirect compute (culling or list build, 5,224 native instructions) | 1 | 69,248 inv. | 0.014 | 0.081 | 1.3 % |
| 15 | `abdd8ace9d7b8824` | 128 × 128 and 64 × 64 post draws (a downsample chain) | 16 | 0.04 Mpx | 0.000 | 0.080 | 1.3 % |
| 16 | `435a0138b9013040` | compute step, one group, 65 by-address loads, atomics | 1 | 256 inv. | 0.000 | 0.075 | 1.2 % |
| 17 | `ee7764a6b1ef459c` | compute, 8 × 4 × 4 groups, shadow samples (a lighting volume, INFERRED) | 1 | 110,592 inv. | 0.067 | 0.071 | 1.1 % |
| 18 | `7ef693eac54f8594` | mesh-only shadow casters (1600 × 1600) | 3 | 252 wg, 0.62 Mpx | 0.003 | 0.070 | 1.1 % |
| 19 | `1a7482c0ec9991ea` | full-screen fog and atmosphere composite (fragment `2407c9a14d62a1d6`, decompiled) | 1 | 1.33 Mpx | 0.052 | 0.065 | 1.0 % |
| 20 | `521f8ef53a7fec7f` | indirect compute | 1 | 14,976 inv. | 0.003 | 0.065 | 1.0 % |

### 2.2 Hyrule Field (`m6h_ps3_hf`, present 4500)

2,189 commands, 463 programs; 243 fragment, 276 vertex, 71 compute, 47 mesh, 6 task stages. Invocations: 0.68 M vertex,
**25.86 M fragment**, 6.86 M compute; 10,708 mesh workgroups. GPU busy 8.5 ms, commands 7.34 ms; estimate 1.98 ms (0.27).

| # | Program | What it draws (ours) | Commands | Counts | Est. ms | In-game ms | Share |
|---|---|---|---|---|---|---|---|
| 1 | `fd029a317a9a4b94` | post compute, tiled (as Kokiri) | 1 | 147,456 inv. | 0.051 | 1.019 | 13.9 % |
| 2 | `9a9b21fe398a0bad` | shadow-map casters, vertex draws (1600 × 1600 and 384 × 216) | 368 | 187,396 vtx | 0.001 | 0.277 | 3.8 % |
| 3 | `435a0138b9013040` | compute step, one group | 1 | 256 inv. | 0.000 | 0.213 | 2.9 % |
| 4 | `ab7699ee7f536755` | compute step, 16 × 16 groups | 1 | 24,576 inv. | 0.001 | 0.210 | 2.9 % |
| 5 | `063b4c21712f159c` | meshlet lit geometry | 2 | 868 wg, 0.16 Mpx | 0.073 | 0.188 | 2.6 % |
| 6 | `dba78f81ae692e07` | foliage shadow casters | 6 | 90 wg | 0.000 | 0.161 | 2.2 % |
| 7 | `ffb53ed102dd9908` | occlusion test boxes (INFERRED) | 381 | 166,882 vtx | 0.001 | 0.161 | 2.2 % |
| 8 | `d8e59341cf4f629c` | grass | 1 | 881 wg, 0.07 Mpx | 0.044 | 0.159 | 2.2 % |
| 9 | `592079442d7129da` | compute step, one group | 1 | 256 inv. | 0.000 | 0.129 | 1.8 % |
| 10 | `4a8885a040b23f4a` | lit material, main view (41 draws) | 41 | 8,421 vtx, 0.42 Mpx | 0.177 | 0.127 | 1.7 % |
| 11 | `701ae221d88b2908` | lit material with the heaviest vertex stage (6.9 cycles a vertex: skinned, INFERRED) | 5 | 7,360 vtx, 0.17 Mpx | 0.090 | 0.110 | 1.5 % |
| 12 | `89ca750f9852b7ca` | mesh-only geometry, main view | 1 | 539 wg | 0.013 | 0.101 | 1.4 % |
| 13 | `44511d69b52282ad` | indirect compute | 1 | 56,512 inv. | 0.017 | 0.093 | 1.3 % |
| 14 | `7ef693eac54f8594` | mesh-only shadow casters | 3 | 1,071 wg, 2.30 Mpx | 0.013 | 0.093 | 1.3 % |
| 15 | `27ee9883e2a20323` | froxel volume compute | 1 | 608,256 inv. | 0.428 | 0.087 | 1.2 % |
| 16 | `7650519f88e59b19` | indirect compute | 1 | 71,232 inv. | 0.023 | 0.085 | 1.2 % |
| 17 | `ee7764a6b1ef459c` | lighting volume compute (INFERRED) | 1 | 82,944 inv. | 0.051 | 0.081 | 1.1 % |
| 18 | `abdd8ace9d7b8824` | downsample chain | 16 | 0.04 Mpx | 0.000 | 0.079 | 1.1 % |
| 19 | `3b8c07f2eb4bb7d9` | mesh-only geometry, main view | 1 | 307 wg | 0.007 | 0.076 | 1.0 % |
| 20 | `29e54c2501acc924` | tree impostor shadows (mesh, the impostor textures) | 3 | 555 wg, 3.58 Mpx | 0.028 | 0.073 | 1.0 % |

### 2.3 The market (`m6h_ps3_mk`, present 4500, `HOS_DBG=meshdry`: its mesh draws recorded and not drawn)

The market loses the device on native-vk head (M6c) unless agent/m6d's wait is in the build (not landed); `meshdry` skips
the mesh draws themselves, so the frame lacks its meshlet geometry (and the depth it would have written: later draws shade
pixels the meshlets would have hidden, INFERRED). 2,954 commands, 412 programs; 242 vertex, 212 fragment, 68 compute, 5 task
stages (the mesh stages are not drawn; the capture lists 110 mesh draws of 4,225 workgroups). Invocations 1.08 M vertex,
18.0 M fragment, 6.33 M compute. GPU busy 8.8 ms, commands 7.43 ms; estimate 1.66 ms (0.22).

| # | Program | What it draws (ours) | Commands | Counts | Est. ms | In-game ms | Share |
|---|---|---|---|---|---|---|---|
| 1 | `fd029a317a9a4b94` | post compute, tiled | 1 | 147,456 inv. | 0.051 | 0.853 | 11.5 % |
| 2 | `9a9b21fe398a0bad` | shadow-map casters, vertex draws | 438 | 245,323 vtx | 0.001 | 0.375 | 5.0 % |
| 3 | `ffb53ed102dd9908` | occlusion test boxes (INFERRED) | 621 | 268,369 vtx | 0.002 | 0.266 | 3.6 % |
| 4 | `ee7764a6b1ef459c` | lighting volume compute (INFERRED) | 1 | 82,944 inv. | 0.051 | 0.224 | 3.0 % |
| 5 | `ab7699ee7f536755` | compute step, 16 × 16 groups | 1 | 24,576 inv. | 0.001 | 0.217 | 2.9 % |
| 6 | `4fa5a59f464664e0` | **characters** (the townspeople, INFERRED): a vertex stage of 120 cycles a vertex, 81 by-address loads (bone data through pointers), 256 VGPRs | 54 | 6,316 vtx, 0.01 Mpx | 0.004 | 0.207 | 2.8 % |
| 7 | `435a0138b9013040` | compute step, one group | 1 | 256 inv. | 0.000 | 0.196 | 2.6 % |
| 8 | `dba78f81ae692e07` | foliage shadow casters (task step only: meshdry) | 3 | 197 wg | 0.001 | 0.163 | 2.2 % |
| 9 | `44511d69b52282ad` | indirect compute | 1 | 88,896 inv. | 0.026 | 0.122 | 1.6 % |
| 10 | `27ee9883e2a20323` | froxel volume compute | 1 | 608,256 inv. | 0.428 | 0.115 | 1.5 % |
| 11 | `d76e73cfbe651494` | characters (as #6) | 20 | 360 vtx | 0.021 | 0.104 | 1.4 % |
| 12 | `abdd8ace9d7b8824` | downsample chain | 16 | 0.04 Mpx | 0.000 | 0.078 | 1.1 % |
| 13 | `695ed228eb27e703` | lit material | 28 | 27,920 vtx, 0.13 Mpx | 0.046 | 0.077 | 1.0 % |
| 14 | `dfd1088b001bd2a6` | characters (as #6) | 20 | 620 vtx | 0.000 | 0.071 | 0.9 % |
| 15 | `41cc95376bcfa819` | characters (as #6) | 14 | 2,846 vtx, 0.08 Mpx | 0.029 | 0.069 | 0.9 % |
| 16 | `44ac8f0a5002c123` | lit material | 2 | 0.22 Mpx | 0.091 | 0.066 | 0.9 % |
| 17 | `abccf81b45c5eb50` | indirect compute, 480 threads | 1 | 480 inv. | 0.000 | 0.066 | 0.9 % |
| 18 | `95db514a35353e28` | characters (as #6) | 11 | 470 vtx | 0.004 | 0.065 | 0.9 % |
| 19 | `cb2e06ddfb9d7d27` | indirect compute | 1 | 9,920 inv. | 0.002 | 0.063 | 0.8 % |
| 20 | `679fe5d409d3583b` | lit material | 14 | 4,849 vtx, 0.25 Mpx | 0.104 | 0.062 | 0.8 % |

A second market run with the mesh draws drawn (`m6h_ps4_mk`, after rebasing onto native-vk e8b19db with agent/m6d's
wait; no device lost): 3,050 commands, 485 programs, **671 stages** (287 vertex, 265 fragment, 68 compute, 46 mesh, 5 task),
6,086 mesh workgroups; the meshlet programs enter the top list (`30dcbaf5…` 0.26 ms, `cfebb562…` 0.19 ms, `79ddff6b…`
0.14 ms: in-game command times, indicative only), `fd029a…` stays first (0.85 ms) and the character programs stay (0.19 ms
for `4fa5…`).

The character programs (#6, 11, 14, 15, 18: 0.52 ms together) have vertex stages `a1eb212d…`, `47f68493…`, `500b27d6…`,
`8edb49a3…` of 816-918 native instructions that RADV compiles to 3,575-3,697 (**ACO / native 4.3**, the highest of any stage
drawn) because each of their 81 by-address loads goes through the window table (64-bit address arithmetic a load).

### 2.4 Across the scenes

- **Stages used:** Kokiri 504 (180 vertex, 210 fragment, 73 compute, 36 mesh, 5 task: `m6h_ps2_k`), Hyrule Field 658 (284,
  250, 71, 47, 6); **824 distinct** in the two, 338 in both; of the game's 42,856 stages, **42,032 are drawn in neither**
  (98 %). The market (with its mesh draws, `m6h_ps4_mk`) 671 (287, 265, 68, 46, 5); the three scenes together **927 distinct
  stages** (392 vertex, 388 fragment, 89 compute, 52 mesh, 6 task), 304 in all three; **41,929 of the game's 42,856 stages
  (97.8 %) are drawn in none of them**.
- **The estimate against the measurement:** per program, measured / estimated ranges from 0.16-0.27 (the froxel compute,
  whose two loops ACO weights by 8) to over 1,000 (one-group computes, test boxes). Summed, the estimate is low by a factor of
  3.3 (Kokiri), 3.7 (HF) and 4.5 (market). For the shader-bound draws (lit materials, full-screen passes) measured /
  estimated is 0.66-1.25: there ACO's estimate is a fair cost model.
- **Fragment stages are not where the frame's time is:** the 210 / 250 fragment stages together take an estimated 0.79 /
  0.98 ms; the largest one measured, a lit material, 0.12-0.13 ms. The four largest measured items are a compute pass, small
  computes, shadow casters and the meshlet geometry.

## 3. The rewrites

### 3.1 The fog and atmosphere composite (fragment `2407c9a14d62a1d6`)

Chosen because it is in both scenes' top lists (1.0-2.6 %), is a full-screen pass (the per-pixel cost is the stage's own),
and is small enough to decompile in the time (412 native instructions, 950 IR, 16 samples; of the heavier candidates, the
compute `fd029a…` is 7,289 IR instructions and the lit materials 3,500-3,700, with storage-buffer light loops the harness
cannot feed: section 5). Source: `gpu/direct/shaders/frag/fog_composite.frag` with `common.glsl` (the back end's fragment
interface: banks as word arrays at the program's bindings, the handle's image and sampler ids, `sat`, `rcp`, `guard`) and
`codes.txt`; built by `tools/meshlab/build_shaders.sh` (glslang, `spirv-val`, then `spv_exact_fma.py`). Not used by the game.

What it does (INFERRED from the arithmetic; the comments in the source say more): reads the scene depth and a depth gather,
decides "sky or past the fog's far plane", shifts the coordinate by up to a texel along the depth gradient and reads two
layers there; for the pixels the second test keeps, the sky and atmosphere colour from a 3D table by view ray and logarithmic
distance slice, or a scattering table by sun angle and height (two paths chosen by a uniform), compressed past a luminance of
5 with the game's arctangent polynomial; then the volumetric fog: two 3D lookups (in-scattering, extinction) at the pixel's
froxel blended into the colour (the froxels the compute `27ee9883…` builds, INFERRED).

**Equivalence** (`meshlab fs <draw> --repl <dir> --compare`, 1024 × 1024 float targets):

| Capture | Draw | Inputs | Result |
|---|---|---|---|
| Kokiri `m6h_ps2_k` | #1057 | varyings in [0, 1) | **EQUAL**: 4,194,304 of 4,194,304 words (VERIFIED) |
| Hyrule Field `m6h_ps2_hf` | #2125 | [0, 1) | **EQUAL**: 4,194,304 words (VERIFIED) |
| Hyrule Field `m6h_ps2_hf` | #2125 | [-4, 4) (more sky and both fog paths) | **EQUAL**: 4,194,304 words (VERIFIED) |

Getting there took three findings, each a rule for the next rewrite (VERIFIED by the compare and by RADV's ISA of both):
1. The translator marks every float operation NoContraction; glslang carries `precise` back from the outputs through the
   arithmetic but **not into texture coordinates nor into functions**: every temporary is `precise` here, a function's
   result explicitly so.
2. The translator leaves its reciprocals (`OpFDiv 1, x`) contractible: `rcp()` is a function so that `precise` does not reach
   it (the first build, with them precise, differed in 23,954 words by up to 2 ulp).
3. **glslang does not decorate a precise `fma()`, and Mesa then splits it** into a multiply and an add (0 of 83 fused, against
   87 fused in the translator's): `tools/meshlab/spv_exact_fma.py` decorates every Fma NoContraction after glslang. With it,
   RADV's ISA of both has 82 fused multiply-adds and the result is equal.

**Cost** (RADV, pipeline statistics: translator → rewrite): 656 → 643 instructions, code 3,788 → 3,688 bytes, VGPRs 64 → 64,
16 waves a SIMD both, IT 463 → 451, latency 2,626 → 2,681, VALU 380 → 368, SALU 116 → 113. **Timed** (`--bench`, 20 grids of
1024 × 1024 a submission, 31 ABBA rounds): translator 38.05 µs a million pixels, rewrite 38.73; **ratio 1.008** (p10 0.999,
p90 1.018). Rerun after the session restart on a quiet GPU (41 rounds, Kokiri draw): translator 37.07 µs a million pixels, rewrite 37.30,
**ratio 1.007** (p10 1.000, p90 1.014): the rewrite is not faster (harness). (An HF series gave 1.008 but a game process
was running during it: not a result.) On the frame: 1.33 Mpx × 38 µs/Mpx = 0.051 ms, in-game 0.065 ms (Kokiri, indicative); a 2 % gain would be about 1 µs a frame.

### 3.2 M6e's mesh rewrites, timed

`meshlab bench <draws> --repl <dir>` (new): each captured draw drawn `--reps` times in a rendering of its pass's targets
with the simple fragment stage, A (translator) and B (replacement) alternating ABBA over 31 rounds, GPU timestamps around each
submission; `--discard` sets rasterizer discard (the mesh stage and primitive setup alone); `--mult K` repeats each draw's
mesh workgroups K times (a task+mesh draw's map repeated: the extra workgroups draw the same meshlets again), so that the
stage, not the draw's fixed cost, is what is timed. Market capture `m6e_cap2`, replacements built from `gpu/direct/shaders/mesh`.

| Stage | RADV (pipeline): translator → rewrite | Draws | Mode | Translator | Rewrite | Rewrite / translator (p10-p90) |
|---|---|---|---|---|---|---|
| depth meshlet (`26fa8419…`, `6942222e…`, `ef1f1c50…`) | ACO 839 → 745, VGPRs 64 → 32, LDS 6,144 → 3,072, waves 16 → 16, IT 645 → 499, latency 1,379 → 2,045 | 38 | 20 reps | 30.0 ns a workgroup (0.7 µs a draw) | 29.8 | **1.000** (0.993-1.008) |
| | | 38 | + discard | 28.6 | 28.7 | 1.001 (0.993-1.007) |
| | | 38 | × 32 workgroups (29,248 a rep) | 9.4 ns (7.3 µs a draw) | 9.5 | **1.001** (0.990-1.012) |
| | | 38 | × 32, discard | 9.0 | 9.1 | 1.001 (0.991-1.011) |
| lit meshlet (`e67ab188…`) | ACO 1,509 → 1,258, VGPRs 160 → 48, LDS 18,432 → 9,216, waves 6 → 14, IT 1,244 → 962, latency 2,648 → 2,837 | 5 | 100 reps | 111.1 ns a workgroup (0.7 µs a draw) | 110.9 | **1.000** (0.993-1.007) |
| | | 5 | × 64 workgroups (1,920 a rep) | 33.0 ns (12.7 µs a draw) | 33.0 | **1.000** (0.991-1.006) |
| | | 5 | × 64, discard | 32.8 | 32.8 | 0.999 (0.993-1.005) |

All timings in this table: the isolated harness, A/B alternated, the GPU lock held and no game process running (checked
by `gpu_run.sh` at each run: "quiet GPU"; rerun 2026-11-13 after the session restart, same results as the first series).
(RADV's numbers here are the pipeline's, linked with a fragment stage: M6e's 22,218 / 12,490 and 5,994 / 2,410 bytes of LDS
were the shader objects'.)

## 4. Size and speed

**No.** VERIFIED for M6e's two mesh rewrites, by the harness (quiet GPU, alternated A/B, 31 rounds each): halving LDS and
VGPRs (lit: 160 → 48 VGPRs, waves a SIMD 6 → 14; depth: 64 → 32 VGPRs) and cutting ACO's throughput estimate by 23 % changed
the time of the captured draws by 0.0 ± 1 % (ratios 0.999-1.001, p10-p90 within ±1.4 %), at the captured sizes (0.7 µs a
draw, fixed cost), at 32-64 times the workgroups (9.4 and 33 ns a workgroup across the GPU, where the stage's work should
show) and with rasterization off. So the stage is not what bounds these draws: at about 9 ns a depth workgroup (≈3,300 SIMD
cycles each across 160 SIMDs, against ACO's 645) the limit is outside the shader's ALU and occupancy, most likely the rate
at which the geometry engines launch mesh workgroups and take their primitives (INFERRED; RADV's statistics cannot show it).
The fog composite (fragment, size −2 %) is the same: 1.008 (p10 0.999, p90 1.018). Size and register savings matter for what
they remove (the mesh scratch ring past 30 KB of LDS, spills, the 2^32 loops' scratch), not for speed at these sizes on
this GPU. For the one stage where the translator's code is clearly slow per ACO (`fd029a…`, the CAS loops: −39 % IT with
fix 1 of section 5b), the speed effect is still unmeasured.

## 5. What is worth rewriting, and what recurs

Ranked by in-game command time a frame (Kokiri / HF; indicative), with what a rewrite could save (ESTIMATED):

| # | Stage(s) | In-game a frame | What a rewrite can win | Confidence |
|---|---|---|---|---|
| 1 | compute `fd029a317a9a4b94` (post, tiled) | 1.03 / 1.02 ms | most of it if the time is a translation artefact: it stores 16-bit values to shared memory 16 times a thread, each through a **compare-and-swap loop on a 32-bit word** whose other half a neighbouring thread writes at the same time (`WriteSharedU16` → `OpAtomicCompareExchange` loop, VERIFIED in its SPIR-V), between 6 barriers; ACO estimates 0.05 ms. A compute harness and a test with native 16-bit shared stores (or a 32-bit layout) would tell. Up to ~1 ms | open: the cause is INFERRED |
| 2 | the small compute steps (`435a…`, `ab76…`, `aba5…`, `5920…`, 256-25k threads) | 0.55 / 0.68 ms | nothing in their code (75-505 native instructions); their time is the barriers before them (the flushes RADV emits with the dispatch) and launch latency: a back-end question (fewer, merged barriers), not a rewrite | INFERRED |
| 3 | per-draw cost of many small draws (shadow casters 368 draws, test boxes 181-381) | 0.17 / 0.44 ms | nothing per stage: 0.75 µs a draw | measured |
| 4 | meshlet lit geometry `063b4c21…` (task `2c56` + lit mesh family) | 0.30 / 0.19 ms | the mesh stage's LDS (the translator's stage buffer) and its VGPRs: a 2-3× occupancy gain that, by section 3.2, does not shorten these draws | measured: no gain at the draw sizes |
| 5 | the lit materials (fragment `a83142e5…`, `91f5b9d7…`, `f8294162…`, `7d1fd77e…`, `db42548a…`, `e44eaa18…`) | 0.12-0.23 / 0.11-0.20 ms each, ~0.6-0.8 ms together | their light loop reads a storage list; ACO/native 1.56: a rewrite saves a few per cent of ALU at most (as the fog composite did: 2 %) | ESTIMATED |
| 6 | full-screen post fragments (shadow mask `ce62…`, fog `2407…`, …) | 0.05-0.09 ms each | 0-2 % (measured on the fog composite) | measured for one |
| 7 | froxel compute `27ee9883…` | 0.10 / 0.09 ms | ACO overestimates it 6× (loop weighting); small | measured |

**Recurring translator overheads** (counts from the census; effect where measured):

| Overhead | Where | How much | Does RADV remove it? |
|---|---|---|---|
| Mesh stage buffer (ISBE) in workgroup memory, copied to the outputs at the end | every mesh stage | 24,552 stage-buffer ops; median 21.5 KB LDS, 31.7 KB max; 40 stages in scratch / spilling | no: LDS 2× the rewrite's, VGPRs 2-3×; but no speed effect at the drawn sizes (section 3.2) |
| 16-bit shared stores as a CAS loop | compute stages with `WriteSharedU16` (`fd029a…` among them) | one loop a store | no (a loop in the ISA); the heaviest pass of the frame has 16 a thread |
| Condition codes as `SelectU1` | task 16.7 %, mesh 7.9 % of IR instructions | 212k in mesh stages | mostly (dead after folding) |
| Predication lowered to one `if` an instruction (an empty block and a phi a predicated instruction: the fog composite's arctangent is 13 of them) | all kinds | not counted separately | yes (ISA has 7 branches for the fog composite) |
| NaN guard of float outputs applied twice (`GuardFragmentOutputs` and `frag_nan_guard`) | every fragment stage | 8 ops a component | partly (the second is idempotent; both survive as clamps) |
| Uniform-register loads as ballot + shuffle | 0.2-0.9 % of IR | 140k ops | no (subgroup ops) |
| Every uniform through the address table (INDIRECT_ADDRESS) | all | one indirection a bank | n/a (the back end's design) |
| XMAD expansion of 32-bit multiplies | all | 3 multiplies a product | yes (folded to one) |

Fixing the translator once would cover all 500 mesh stages (the stage buffer) and every compute stage with 16-bit shared
stores; hand rewrites cover one stage each and, for fragment stages, win almost nothing because RADV already removes the
translation's bulk (ACO / native 1.56).

## 5b. The translator: what recurs, what to change

The user's question: dependent on these results, can the converter be improved in general? For each overhead the census finds
in the translated stages: how many stages carry it, the time of the drawn stages that carry it per scene (estimate / in-game
command time, the latter **indicative only**: taken in the game while other agents ran; not a harness result), what to change
in `gpu/direct/eden`, and the gain (ESTIMATED unless said).

| Overhead | Stages in the game | Kokiri: est. / in-game ms (stages drawn) | HF | market (meshdry) |
|---|---|---|---|---|
| mesh stage buffer (ISBE) in workgroup memory | 499 | 0.05 / 0.43 (32) | 0.09 / 0.69 (38) | 0.05 / 0.27 (17) |
| sub-word shared stores as a CAS loop | 15 (compute) | 0.05 / 1.03 (1) | 0.05 / 1.02 (1) | 0.05 / 0.85 (1) |
| sub-word shared loads (word load + extract) | 70 (compute) | 0.05 / 1.03 (1) | 0.05 / 1.02 (1) | 0.05 / 0.85 (1) |
| by-address loads through the window table | 31,952 | 1.44 / 4.04 (204) | 1.45 / 4.76 (281) | 1.27 / 5.07 (292) |
| of them, 16 or more by-address loads | 9,077 | 0.54 / 1.82 (89) | 0.68 / 2.15 (78) | 0.44 / 2.55 (103) |
| NaN guard applied twice (fragment) | 22,284 | 0.79 / 2.09 (115) | 0.97 / 2.16 (101) | 0.78 / 2.49 (134) |
| uniform-register loads as ballot + shuffle | 5,034 | 1.30 / 3.28 (89) | 1.21 / 3.50 (78) | 0.98 / 3.36 (85) |
| condition codes as SelectU1 | 6,710 | 1.20 / 1.67 (63) | 1.08 / 1.71 (56) | 0.84 / 1.78 (76) |
| indexed uniform loads (through the address table) | 16,301 | 1.44 / 2.76 (145) | 1.42 / 2.96 (130) | 1.17 / 3.44 (149) |

(A stage counts in every row it has; the columns overlap. Not found, or removed by RADV: integer/float bit-cast chains
(11.8 M bit-casts in the fragment IR, none left in the ISA: no-ops), per-component stores (merged), dead vertex outputs (the
fragment partner reads every component the vertex stage writes in 15,835 of 15,851 programs), predication's one-if-an-
instruction (folded: the fog composite's ISA has 7 branches), XMAD multiplies (folded to one).)

**The changes, ranked by reach × gain:**

1. **Sub-word shared memory through an explicit layout** (`backend/spirv/spirv_emit_context.cpp DefineSharedMemory`; the
   profile flag `support_explicit_workgroup_layout` and its 8/16-bit access flags, which `direct_shader.cpp make_profile`
   never sets, with `VK_KHR_workgroup_memory_explicit_layout` enabled on the device). Eden already emits the typed 16-bit
   and 8-bit views when the flag is set; without it every `WriteSharedU16` is a compare-and-swap loop on the 32-bit word.
   **Prototyped** (`censustool --explicit-shared`, RADV's statistics; VERIFIED): on the 15 stages with sub-word stores, ACO
   instructions −22 to −30 %, branches halved, IT −33 to −44 %; `fd029a317a9a4b94` (the frame's heaviest pass) 2,619 → 1,932
   instructions, IT 4,096 → 2,484, latency 10,752 → 4,624. Of the other 55 stages that only read 16-bit
   shared values, 15 get faster (IT −5 % or more) and **12 get much worse**: their shared memory RADV otherwise removes (LDS
   0 without the flag), the explicit (Aliased, Block) layout keeps it alive and makes them up to 13× larger (e.g. `a41c78fa…` 583 → 4,421 instructions). So the change is: the explicit layout only when the program has
   8- or 16-bit *stores* to shared memory (`info` gains that bit in `collect_shader_info_pass.cpp`, set by
   `WriteSharedU8/U16`). Gain: up to the ALU share of `fd029a…`'s ~1 ms (in-game, indicative): ALU estimate −39 %; how
   much of that pass is ALU is not known. Not measured: the harness has no compute mode (left undone).
2. **The window translation of by-address loads** (`backend/spirv/bda_emit.h Translate`, used by every global load in
   `spirv_emit_context.cpp define_body`): per load a shift, a compare, a select, a 64-bit address, a **dependent load of the
   window table entry**, a mask and an add, and the virtual-range compare and select: about 20 instructions and one extra
   memory round trip a load (ESTIMATED from the SPIR-V; the market's character vertex stages, 81 such loads, are ACO / native
   4.3 against the vertex median 3.25). 119 of 139 addresses in a pointer-reading mesh stage are 64-bit sums built in 32-bit
   halves, so the IR cannot split base and offset to share one lookup. The general fix is in the back end's layout, not the
   emitter: if layer 0 placed its windows **contiguously in device address space** (one sparse buffer, window k at
   base + k × 4 GB) the translation is `base + g + (g >> 31 << 31)`: three ALU ops, no table load, and the per-draw virtual
   range only where a draw has one (`Pushed::delta` invalid otherwise, so the select is not emitted). Reach: 31,952 stages;
   the drawn ones carry 4-5 ms of the frame's command time (indicative), most of it not this overhead. Gain: not estimable
   without a measurement; the character draws (0.52 ms in the market, indicative) are the place to measure it.
3. **The NaN guard once** (`direct_shader.cpp translate_*_variant` calls `GuardFragmentOutputs` and the emitter's
   `frag_nan_guard` clamps again). **Prototyped** (`censustool --no-ir-guard`, the fragment harness): **bit-exact** on the three captured draws tried (fog
   composite, shadow mask `ce62…`, lit material `a83142e5…`: 4,194,304 words each, VERIFIED; the offline translation with the
   guard was first checked equal to the captured stage, so the compare is the fix's alone). Timing: two A/B series (41 rounds
   each) gave 1.000 / 0.999, 0.992 / 0.991 and 0.998 / 1.001 (fix / translator) for the three, but **a game process was
   running during both** (the quiet check of `gpu_run.sh` said so), so they are not results; they agree with the static
   count.
   RADV: 3-4 instructions fewer a stage (656 → 653, 734 → 731, 2,939 → 2,935), IT −0.1 to −1.3 %. Reach: every fragment
   stage with a float target (22,284); gain under 1 %.
4. **The mesh stage buffer** (`backend/spirv/emit_spirv_mesh.cpp`, the ISBE in workgroup memory copied to the outputs at the
   end): M6e's rewrites without it halve LDS and VGPRs; section 3.2 measured **no speed difference** on the captured draws,
   also with 32-64× the workgroups and with rasterization off (the stage is not the bound: about 9 ns a workgroup either way).
   Worth doing for the LDS limit (M6e: 11 programs past 30 KB use RADV's mesh scratch ring) and register pressure, not for
   speed on this GPU.
5. Uniform-register loads as ballot + shuffle (`frontend/ampere/lower_to_maxwell` lowering of uniform registers; 5,034
   stages, 0.2-0.9 % of IR): a uniform value loaded once instead of a subgroup shuffle; RADV keeps the shuffles. Small.

**Translator fixes against hand rewrites.** One hand rewrite (the fog composite) gained 2 % of one 0.05 ms pass; M6e's two
mesh rewrites gained nothing measurable. Fix 1 reaches the single heaviest pass of every scene and 14 more stages with one
condition; fix 2 reaches 32,000 stages. Both beat rewriting six stages; neither is measured yet (the next step: a compute
harness that replays a captured dispatch, then fix 1 there, bit-exact on its outputs).

## 6. Tools, code and tests

Committed (nothing of the game: our sources and the decompiled stage, a transcription of the game's logic as M6e's are):

| What | Where |
|---|---|
| The offline translation, shared by meshtool and the census | `tools/meshlab/offline.h` (meshtool now uses it) |
| The census | `tools/shadercensus/censustool.cpp`, `build.sh`, `census.py` (shards, resumable), `report.py` (aggregates), `scene.py` (per scene), `scene_run.sh` (a game run under the lock with the hang rules) |
| `HOS_DBG=pstats:<present>` (statistics and timestamps a command) | `gpu/direct/direct_pstats.{h,cpp}`; calls in `direct_draw.cpp`, `direct_compute.cpp`, `direct_task.cpp`, `direct_present.cpp`, `direct_main.cpp`; `meshShaderQueries` asked for when the device has it (`direct_heap.cpp`, `DrawCaps::mesh_queries`) |
| `HOS_DBG=meshcap` with vertex draws (`MC_VDRAW`) and every program's stages | `direct_meshcap.{h,cpp}`, `direct_meshcap_format.h`, `direct_draw.cpp` (`meshcap_ubos`, `meshcap_vertex`), `direct_shader.cpp` |
| The fragment harness, the mesh bench | `tools/meshlab/fsbench.h`, `meshlab.cpp` (`fs`, `fsfind`, `bench`, `--mult`, `--discard`), `capture.h` |
| The decompiled fragment stage | `gpu/direct/shaders/frag/` (`common.glsl`, `fog_composite.frag`, `codes.txt`); `tools/meshlab/build_shaders.sh` (any stage kind), `spv_exact_fma.py` |

With `HOS_DBG` unset, one load and a branch per command.
Tests: `HOOKSHOT_VULKAN_HEADERS=/usr/include work/venv/bin/python -I -B tests/run_all.py`: 296 passed, 5 failed (the baseline
five), 19 skipped.

Local, not committed: the seed (`work/m6h/hookshot_shaders.bin`), the census (`work/m6h/census/`: `stages.jsonl`,
`stages.csv`, `report.md`), IR prints (`work/m6h/ir/`), the runs `work/runs/m6h_ps_k`, `m6h_ps2_{k,hf}` (drawlog, meshcap),
`m6h_ps3_{k,hf,mk}` (timestamps; mk with meshdry), `m6h_ps4_mk` (the market with its mesh drawn), the per-scene tables (`work/m6h/scene3_*`), the benches (`work/m6h/bench/`).

**Left undone:** decompiling the heavy compute `fd029a…` and a lit material (too large for the time; the harness has no
compute mode and no storage buffers for fragment stages); a second fragment rewrite (the shadow mask `ce62…` was read, not
written); naming Link, enemies and terrain (their textures' handles are computed and the drawlog cannot name them); the
title scene; a compute harness (to time `fd029a…` and fix 1 of section 5b, and to check that fix bit-exact); fix 2 of
section 5b (a contiguous window layout) needs a back-end change and was only specified. No labels were added to
`nx2/oot_labels.txt` (no game function was identified).
