# The Hyrule Field hang: the fix (2026-11-13)

Branch `agent/hffix` (from native-vk 30e3366). RX 6950 XT (Navi 21), RADV Mesa 26.2.4, kernel 7.2.11-1-cachyos. Game runs
in `work/runs/hffix_*`, harness runs and compiler outputs in `work/hffix/` (`runs/`, `repl/`, `late2/`, `scan_tr/`); all
local: they hold game data. Follows `hf-hang-report.md` (the cause) and `hf-mesh-program.md` (the stage read statically).
Every statement is **measured** (a run or a file says so) or **inferred**.

## Summary

- **Fixed and proven:** the game draws mesh stage `40d0c0768e874600` (programs `cfebb562ec378f87`, `f69141e92f08bc7b`,
  `4ce46adcc32922a4`, `7bb29ab9ea165bbe`) with a hand-written stage (`meshlet_lit.mesh -DMODEL_SLOT=2 -DPROBE_LIGHT`). It
  emits the translated stage's geometry bit for bit in all 28 output components on 10 of 10 captured draws, RADV compiles
  it without scratch, and the Kokiri-Hyrule Field walk that lost the device 5 of 5 times survived 3 of 3 runs of 240 s
  (**measured**, sections 1-2).
- **Spill scan:** of the 53 mesh stages and 6 task stages the three scenes' captures hold, **one** spills registers to
  scratch as RADV compiles the translator's SPIR-V: `40d0` (**measured**, section 3). Eleven more mesh stages are at the
  register limit by this report's count without spilling.
- **The translator:** `40d0` spills because the translator emits the native code's order, in which all 112 probe rows are
  loaded before the first is used: ACO needs 258 vector registers for it and has 256. Emitting each instruction just
  before its first use brings the same translated stage to 103 and it then survives the reproducer (**measured**,
  section 4). That pass is **specified and prototyped, not in the game**: its first version, applied in the game, made
  two other mesh stages spill and the market lost the device with it (a loss of this stage's budget; section 4).
- **Market:** with `40d0` drawn by the hand-written stage the market ran without M6d's wait before task+mesh draws: 240 s
  twice and 15 minutes, 3 of 3 survived (13 of 13 lost without the wait before). The wait is **off by default now**
  (`HOS_DBG=tmwait` keeps it); its frame cost was not measurable (section 5).
- **Open question settled:** the reproducer does not need two overlapping draws: with the wait between the depth draw and
  the depth-equal draw the translated stage still hung (section 6).
- **Device losses used:** 2 of 4 (section 7).

## 1. The hand-written stage

`gpu/direct/shaders/mesh/probe_light.glsl` (new) and `meshlet_lit.mesh` with `PROBE_LIGHT`: the lit meshlet stage plus the
12 light components a vertex of the game's probe grid (output locations 14-16), transcribed from the stage's IR
(`hf-mesh-program.md` section 1.2): the cell and leaf lookup, the 8 probes' trilinear weights (a probe that is not valid
counts 1e-5), rows 1-7 of each probe summed in the game's order, mixed with the scene's own coefficients. One address
translation a probe instead of one a row; nothing else differs from what the translated stage reads.

`meshlab compare` (the translator's stage against the replacement, every triangle's corners' attributes, exact), all
**measured**:

| Capture | Draws of `40d0` | Result | Output |
|---|---|---|---|
| `hang_hf_cap` (sane frame, draw #230, 262 groups) | 1 | equal, 13,344 primitives | `cmp_sane_230.txt` |
| `hfh_f_plainfs1` present 1897 (a losing frame, 525 groups) | 1 | equal, 26,829 primitives | `cmp_f1897.txt` |
| `hfh_t_losttest` present 1949 (a losing frame, 527 groups) | 1 | equal, 27,019 primitives | `cmp_t1949.txt` |
| market `m6e_cap2` | 5 | 5 equal | `cmp_m6e_cap2.txt` |
| Hyrule Field `m6e_cap_hf` | 1 | equal | `cmp_m6e_cap_hf.txt` |
| Kokiri `m6e_cap_k` | 1 | equal | `cmp_m6e_cap_k.txt` |

The other four replacements of `codes.txt` were compared in the same runs: 43, 24 and 2 draws equal, none different. A
`dump` of the replacement shows the light lines hold values (`dump_sane_230.txt`): the compare did not pass on absent
outputs.

RADV's compile (`RADV_DEBUG=shaders meshlab ... stats 230 [--repl]`, `stats_230_*.txt`), **measured**:

| Stage `40d0` as | Instructions | `scratch_store_dword` / `scratch_load_dword` | Register demand (scheduler off) | Mesh scratch ring stores |
|---|---|---|---|---|
| translated | 8,172 | 8 / 8 | 258 | 8 |
| hand-written | 2,228 | 0 / 0 | 68 | 0 |

The reproducer (`run --present 1897 --fs simple --prepass`, the losing draw of `hfh_f_plainfs1`): with the hand-written
stage **survived, 542,664 draws in 40 s** (`run_f1897_prepass_repl28.txt`; the translated stage hung at submission 5,
`hf-hang-report.md`).

## 2. In the game

`gpu/direct/direct_meshrepl.{h,cpp}` (new): a table of hand-written mesh stages by the game stage's code key and the
program's shape, built from `gpu/direct/shaders/mesh/swap.txt` (the stages swapped in: now `40d0` alone; the other four of
`codes.txt` stay out, the user's decision of M6e). The translator still translates the stage: its `Info` drives the heap
mappings and the fragment stage, and the program cache and the capture keep its SPIR-V. The replacement takes its place
where the shader object is made (`direct_shader.cpp finish`), after a check against the translated stage: the same
execution modes, the same output locations, no output built-in of its own, and the stage's uniform buffers in the order
the source declares them (`c0, c3, c4, c5, c6`). A mismatch refuses the replacement (logged, counted, drawn as
translated). `HOS_DBG=nomeshrepl` draws with the translator's stages. Its SPIR-V is a generated header beside the
sources (`40d0c0768e874600_spv.h`, written by `tools/meshlab/build_shaders.sh`); `tests/test_direct_mesh_repl.py` holds
swap.txt, codes.txt, the table and the header together and compiles the source again when glslang is there.

The walk (`OVERLAY=work/overlay/chisel_hf RUN=timing scripts/run-vk.sh <name> 240 -- RX_BACKEND=direct
HOS_DBG=marks,tmlog:cfebb562ec378f87 HOS_LOG_TIME=1 HOS_INPUT_SCRIPT=<hfh_a_lost's> HOS_INPUT_EXCLUSIVE=1`), **measured**:

| Run | Result | The ivy's draw |
|---|---|---|
| `hffix_walk1` (the program cache made anew: the scene complete from 97 s) | survived 240 s, 9,917 presents | 7,912 frames, 427 of them with 500 groups or more, 593 at most |
| `hffix_walk2` | survived 240 s, 14,061 presents | 10,896 frames, 410 with 500 or more, 593 at most |
| `hffix_walk3_nowait` (the final build: M6d's wait off, section 5) | survived 240 s, 14,047 presents | 10,898 frames, 416 with 500 or more, 593 at most; no wait recorded |

Before: 5 of 5 lost, within a few frames of 515-567 groups (`hf-hang-report.md`). Both runs logged the four programs made
with the hand-written stage, none refused.

**The picture.** Kokiri standing (`chisel_kokiri`, present 1800 dumped, `HOS_GAME_STEP_PER_FRAME=1`, 150 s; Kokiri draws
the ivy with 86 groups and does not lose the device with either stage), two runs with the hand-written stage
(`hffix_pic_k_repl`, `_repl2`) and two with the translator's (`HOS_DBG=nomeshrepl`: `hffix_pic_k_trans`, `_trans2`),
compared pass by pass (`direct_picture_check.py compare`, the frozen goldens' noise table), **measured**:

| Pair | Failing states |
|---|---|
| translated against translated | 40 |
| hand-written against hand-written | 40 |
| translated against hand-written (four pairs) | 40, 39, 33, 27 |

The check cannot tell the stages apart: two runs of the same build differ as much as the two builds (Navi's light and the
camera's spot differ between runs: M6d's "bimodal start"). The screenshots of present 1800 of both builds show the ivy
around the house lit alike (looked at: `hffix_pic_k_repl/shots/vk_001800.png`, `hffix_pic_k_trans/shots/vk_001800.png`).
What says the ivy is right is section 1's bit-equal compare; the picture check adds only that nothing else broke.

## 3. The spill scan

`tools/meshlab/spill_scan.py` (new): for every mesh program of a capture, `RADV_DEBUG=shaders meshlab <frame> stats <seq>`
makes its shader objects (nothing is drawn) and the script counts ACO's `scratch_store_dword` / `scratch_load_dword`
(`storage:vgpr_spill`) in each stage after register allocation. Captures: market `m6e_cap2` (54 mesh programs), Hyrule
Field `m6e_cap_hf` (51) and `hang_hf_cap` (39), Kokiri `m6e_cap_k` (35), the losing frame `hfh_f_plainfs1:1897`.
**Measured** (`work/hffix/runs/scan_translated.txt`, each program's RADV output in `work/hffix/scan_tr/`):

| Kind | Stage codes | With scratch spills |
|---|---|---|
| mesh | 53 | **1: `40d0c0768e874600`** (8 stores, 8 loads; programs `cfebb562ec378f87`, `f69141e92f08bc7b`; in all three scenes) |
| task (as the compute shader the back end makes) | 6 | 0 |

Fragment stages were **not scanned**: the harness compiles its constant fragment stage unless `--fs real` is given (M6h's
census has `cfebb562`'s fragment stage at 112 registers, no scratch).

Register demand, a second count (`--demand`: compiled with `ACO_DEBUG=nosched`, each ACO temporary counted from its first
to its last line after register allocation; exact for straight-line code, too high where branches hold values apart, so
a stage can show more than 256 and not spill). **Measured** (`scan_translated_demand.txt`): `40d0` 258; eleven more mesh
stages between 245 and 276 without a spill (`463f4263` 276, `3d33bb73` 272, `e3fc8fcf` 269, `66c494fe` 259, `354dde83`
257, `b86297c2` 257, `b509ae33` 256, `55e5c685` 253, `072e452f` 248, `6eaef39e` 248, `d38b0d52` 245); the next is at
143. **Inferred:** those eleven are at the edge: a small change of the driver's compiler, or of their SPIR-V, can make
them spill (section 4 shows two of them doing so).

So there is no second spilling stage to run through `run --prepass`, and no list of hand rewrites to grow now. The scan
is the check to run on a capture of any new scene, and after a driver update.

## 4. The translator: why `40d0` spills, and a pass that removes it

**Why (measured).** The translator emits a stage in the native code's order. The game's compiler loads early: `40d0` reads
8 probes x 7 rows x 2 vertices = 112 rows of 16 bytes, and nearly all of them are loaded before the first weighted sum is
taken. With ACO's scheduler off (`ACO_DEBUG=nosched`: the scheduler moves loads further up until the registers are full,
which hides the demand) the translated stage holds 258 vector registers' worth of values at its worst point, 120 of them
loaded rows (`stats_230_tr_nosched.txt`); RADV has 256 a thread, so ACO spills 8. It is not the memory-window
translation, the 64-bit address arithmetic or the stage buffer: those are short-lived or in scalar registers. The
hand-written stage sums a row as it loads it and needs 68.

**The pass (prototype: `tools/meshlab/spv_sink.py`, not in the game).** Within each block, instructions with effects keep
their order (stores, barriers, atomics, loads of workgroup and private memory, calls of functions that write); every other
instruction is emitted just before the first instruction that needs it, operands first, the larger operand tree first;
and once an instruction is emitted, every instruction that is ready and frees at least as many values as it makes follows
at once, so a row is consumed as it is loaded. A read by address stays between the writes by address around it. The
result is kept only when it holds fewer values at once than the input, by the pass's own count. Values do not change:
the same instructions in another order.

**Measured with its first version** (`work/hffix/late/`; loads by address not moved at all in a stage that writes by
address, no acceptance rule):

- `40d0` translated and scheduled late: register demand 258 -> 103, **0 scratch** (`stats_230_sink_nosched.txt`),
  geometry equal to the translator's on the sane and the losing frame, and the reproducer **survived: 206,523 draws in
  40 s** (`run_f1897_prepass_late.txt`). So the translated stage without a hand rewrite does not hang once it does not
  spill: the spills are the trigger, not the stage's other properties (it still keeps outputs in RADV's mesh scratch
  ring: 8 stores).
- Every captured mesh draw with its stage scheduled late against the translator's (`meshlab compare`): market 110 of 110
  equal, Hyrule Field 82 of 82 and 68 of 68, Kokiri 47 of 47 (`cmp_late_*.txt`): the reorder changes no output.
- **It made two stages worse.** In stages that write by address (statistics atomics, stores) the loads stayed in place
  while their uses moved down: `b86297c24c8d8c7a` (program `30dcbaf546843b46`) went from 0 to 29 spills and
  `354dde838352c262` (`d905f52b7517c681`) from 0 to 33 (`st_late_2397.txt`, `st_late_2400.txt`). With that version applied
  in the game at translation and M6d's wait off, the market **lost the device** at present 564, the first second those
  draws were drawn: open, the task+mesh draw of `30dcbaf546843b46` (1,019 groups) and the five mesh draws after it,
  `d905f52b7517c681` first (`hffix_mk_nowait1`). **Inferred:** two more stages that hang once they spill, which supports
  the cause and shows the cost of a pass that can raise register demand. The pass was taken out of the game.

**The second version** (the file as committed: reads by address movable between writes, subgroup operations movable, the
acceptance rule) by the pass's own count: `40d0` 260 -> 110, `b86297c2` 254 -> 84, `b509ae33` 257 -> 94, `8d46218c`
139 -> 119; 18 of 53 stages left as they were. RADV's compile of all 53 with it (`scan_late2.txt`,
`scan_late2_demand.txt`, **measured**): **no stage spills**; register demand `40d0` 103, `b86297c2` 257 -> 98, and of the
twelve stages at 245 or more two are left (`66c494fe` 259 and `354dde83` 257, both unchanged by the pass). It has not
been drawn with, and its output was not compared draw by draw (the first version's was).

**Specification, if it is to go into the translator** (`gpu/direct/eden`, after `EmitSPIRV`, or as an IR pass before it):
the pass as above; applied to mesh stages only at first; the result kept only when its count is lower; the program cache
stamped by it; and gated by the scan: `spill_scan.py --repl <scheduled>` over the scenes' captures must report no spill
and `meshlab compare` every draw equal before it is turned on, since the count is this pass's and not ACO's. A switch
to turn it off. Not done here: the first version's loss shows it needs that gate, and the hand-written `40d0` covers the
one stage that spills today.

## 5. The market

`HOS_DBG=notmwait` (then a switch, now the default) turned M6d's wait off. Market standing (`chisel_hf_market`,
`RX_BACKEND=direct HOS_DBG=marks,notmwait`), the hand-written `40d0` in, no late scheduling. **Measured:**

| Run | Result |
|---|---|
| `hffix_mk_nw_a` (program cache made anew) | survived 240 s, 9,000 presents |
| `hffix_mk_nw_b` | survived 240 s, 13,200 presents, 60.1 fps |
| `hffix_mk_nw_long` | survived 900 s, 51,600 presents, 60.0 fps |

No wait was recorded in any of them (the counter "after a wait for the work before it" absent from their logs; 55
task+mesh draws a frame drawn). Before: 13 of 13 lost within 22-111 s without a separation (M6c, M6d). **Inferred:** the
market's loss was the same stage: `f69141e92f08bc7b` draws `40d0` there and was open at M6d's control loss. So the wait
was a mitigation of this cause, and it is **removed as the default** (`gpu/direct/direct_draw.cpp`; `HOS_DBG=tmwait`
turns it on). Three runs against thirteen losses is the proof asked for; it is not a proof for other scenes, where the
scan (section 3) is the check.

**In the harness the market's job does not reproduce the loss** (`run --job 1440 --barrier none --fs simple --prepass`,
the 22 task+mesh draws of M6e's job, all depth-equal, each drawn first with depth written), **measured:** with the
translator's stages **survived, 4,161,784 draws in 40 s** (`run_mkjob_prepass_translated.txt`); with every stage scheduled
late (the first version) survived, 4,140,730 draws (`run_mkjob_prepass_late.txt`). The captured frame's `40d0` draws have
16-58 groups; the Hyrule Field draw that hangs in 2 s has 525. **Inferred:** the loss needs more of the stage's waves
at once than that frame's draws give in the harness; not shown what the market's losing frames had.

**The wait's frame cost** (market, pacing off: `HOS_NO_PACING=1 HOS_FPS_TARGET=1000 HOS_FPS_MAX=240`, 100 s, the mean of
the last six 600-present windows, alternated), **measured:**

| Run | Wait | Frame time mean |
|---|---|---|
| `hffix_t_mk_nowait_1` | off | 13.58 ms |
| `hffix_t_mk_wait_1` | on (61.3 waits a frame) | 13.92 ms |
| `hffix_t_mk_nowait_2` | off | 13.61 ms |
| `hffix_t_mk_wait_2` | on (61.4 a frame) | 13.39 ms |

No difference above the runs' own spread (0.5 ms between the two runs with the wait). The machine was this stage's
alone for game runs under the lock, but a second agent's builds and short runs shared it by then: indicative only.

## 6. Open questions

- **Does the reproducer need two overlapping draws? No (measured, one run).** The losing draw with the translated stage
  and the back end's wait (framebuffer stages, by region) between the depth draw and the depth-equal draw
  (`run --present 1897 --fs simple --prepass-barrier`): **hung at submission 24, 2.1 s**
  (`run_f1897_prepass_barrier_translated.txt`). So the depth-equal draw alone, its own mesh waves against its own pixel
  work, is enough at 525 groups. **Inferred:** M6d's wait helped the market by lowering how many of the stage's waves ran
  beside pixel work, not by keeping two draws apart; it did not end the Hyrule Field loss (`hang_hf_fix1`, `fix2`).
- **Why did `HOS_DBG=tmdiscard` avoid the loss? Inferred, no new run:** with the rasterizer off the stage runs (RADV
  26.2.4 does not skip a mesh stage's work under rasterizer discard: the coordinator's reading, section 8 (c)) and no
  pixel work follows; the reproducer without the first draw is the same case (every fragment fails; 235,573 draws
  survived, `hf-hang-report.md`). The loss needs the spilling stage's waves and passing fragments together.
- Not known: what the hardware does (section 8); the group count at which it starts; whether fragment stages anywhere
  spill (not scanned).

## 7. Device losses

Budget 4, used **2**. No configuration lost twice; each was followed by the kernel log saved, 3 minutes and `vulkaninfo`.

| # | Time | Run | What | Recovery |
|---|---|---|---|---|
| 1 | 10:34:54 (reset 36) | game, `hffix_mk_nowait1` | market, the wait off, **with the first late-scheduling pass in the game**: it made `b86297c2` and `354dde83` spill; lost at present 564 (section 4). Caused by this stage's own change | vulkaninfo at 10:37:54; `kernel.txt` in the run |
| 2 | 14:03:35 (reset 37) | harness, `run_f1897_prepass_barrier_translated` | section 6's one run | vulkaninfo at 14:06:39; `.kernel.txt` beside the output |

## 8. For a later Mesa report

Not filed; for the user to review. No game data may go into it: the reproducer below uses a captured frame and stays
local, and a synthetic one is written but **not run** (below).

- **System:** AMD Radeon RX 6950 XT (Navi 21, GFX10.3), RADV, Mesa 26.2.4 (Arch package `mesa 3:26.2.4-1`), kernel
  7.2.11-1-cachyos, amdgpu. Shader objects (`VK_EXT_shader_object`) with descriptor heaps, `VK_EXT_mesh_shader`, mesh
  stages made with `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`, required subgroup size 32.
- **Symptom:** `ring gfx_0.0.0 timeout` about 2 s after the frame, GPU reset, VRAM lost; no VM fault in the kernel log
  at any of these losses.
- **Reproducer (local, 2 s):**
  `MESHLAB=work/hffix/bin/meshlab tools/meshlab/gpu_run.sh <out.txt> work/runs/hfh_f_plainfs1/meshcap/roll.bin run --present 1897 --fs simple --prepass --seconds 40`
  (harness: `tools/meshlab/build.sh`; the capture: `work/runs/hfh_f_plainfs1/meshcap/roll.bin`; the stage's SPIR-V:
  `work/hffix/trans_f1897/40d0c0768e874600.spv`). One mesh draw of 525 workgroups (32 invocations, 126 vertices and
  primitives declared, about 27,000 small triangles, 0.1-0.2 of a 1536 x 864 target) drawn twice in one rendering: first
  with depth written (less-or-equal, colour masked), then with depth equal and colour written, a constant fragment
  stage. Hung at submission 5 (`work/meshlab_hf/runs/run_f1897_prepass_simple.txt`, kernel log beside it).
- **The compiled stage** (`work/hffix/runs/stats_230_tr.txt`): HW stage NGG, 8,172 instructions, 256 VGPRs, 8
  `scratch_store_dword` and 8 `scratch_load_dword` (`storage:vgpr_spill`, offsets -2048..-2020 from an offset register
  of 0x800; `FLAT_SCR` set up in the prologue from the ring offsets and the scratch offset argument), 8 stores to the
  mesh scratch ring (outputs past the NGG LDS budget), 343 LDS operations.
- **What makes it go away (each measured, the same draw and state):** the same SPIR-V with its instructions reordered so
  that it needs 103 registers and no scratch, still with the mesh scratch ring: 206,523 draws in 40 s; a hand-written
  stage with the same outputs (no scratch, no ring): 542,664 draws in 40 s; no first draw, so that every fragment of the
  depth-equal draw fails: 235,573 draws in 30 s. In the game: a 128 x 128 scissor with depth test off on the program's
  draws (240 s).
- **What it is not (measured):** not the geometry (three losing frames captured: finite, ordinary, the task stage run
  again gives the same groups); not the game's fragment stage (lost with a constant one); not a draw class (a larger
  depth-equal task+mesh draw of 1,102 groups whose stage does not spill ran 156,740 times in 40 s under the same
  conditions).
- **Also seen (measured, this stage):** two other mesh stages, made to spill by a reorder of ours (29 and 33 spills; one
  has an atomic add by address), lost the device in the game in the first second they were drawn (`hffix_mk_nowait1`).
- **One draw is enough (measured):** with a framebuffer-local barrier (RADV: a PS partial flush) between the two draws
  it still hung, at submission 24.
- **Leads from reading `~/source/mesa` (27.0-devel, e04357c4642), all UNVERIFIED as causes:**
  - (a) `ac_compute_late_alloc` (`src/amd/common/ac_shader_util.c`) turns late alloc off only for the stage that itself
    uses scratch, with the comment "If scratch is used with late alloc, the GPU could deadlock if PS uses scratch too. A
    more complicated computation is needed to enable late alloc with scratch (see PAL)" (the comment is there, line 799
    of the local clone: read). The non-spilling stage drawn just before (the game's depth prepass; in the reproducer the
    first draw is the same stage) keeps late alloc on, with another CU mask.
  - (b) RADV's scratch wave count is `MIN2(device->scratch_waves, 4 * num_cu * shader->max_waves)`
    (`radv_get_max_scratch_waves`), radeonsi uses `info.max_scratch_waves` (`ac_gpu_info.c`).
  - (c) Mainline after 26.2.4 makes a mesh stage skip its outputs under rasterizer discard
    (`ac_nir_lower_ngg_mesh.c`); 26.2.4 does not, so the game's `tmdiscard` runs did execute the spilling stage and
    survived: consistent with "it needs pixel work".
  - (d) Experiments proposed, with a locally built RADV chosen by `VK_ICD_FILENAMES` (**not built, not run**):
    unmodified mainline; late alloc forced off for all NGG stages; radeonsi's wave count.
- **A synthetic stage, written and compiled, never drawn:** `tools/meshlab/probes/spill.mesh` (320 values loaded, then
  folded from both ends: RADV compiles it with 70 scratch stores and 70 loads; `-DVALUES=64`: none, the control; each
  invocation checks its sum and flags a value that came back changed) and `meshlab probe <spv> --run [--groups N]
  [--seconds T] [--no-prepass | --never] [--prepass-barrier]` (the reproducer's two draws with that stage, a table of
  ones, no game data). Whether it hangs is not known: the step was dropped before a run.

## 9. Code

| What | Where |
|---|---|
| The lit meshlet stage with probe light, the probe lookup | `gpu/direct/shaders/mesh/meshlet_lit.mesh` (`PROBE_LIGHT`), `probe_light.glsl`, `codes.txt` |
| Hand-written mesh stages swapped in by code key; `HOS_DBG=nomeshrepl` | `gpu/direct/direct_meshrepl.{h,cpp}`, `shaders/mesh/swap.txt`, `shaders/mesh/40d0c0768e874600_spv.h` (generated), `direct_shader.cpp` (`finish`, the statistics line) |
| M6d's wait before task+mesh draws off by default; `HOS_DBG=tmwait` | `gpu/direct/direct_draw.cpp` |
| The headers of swap.txt written by the shaders' build | `tools/meshlab/build_shaders.sh` |
| The spill scan (`--demand`: register demand with ACO's scheduler off; `--repl`) | `tools/meshlab/spill_scan.py` |
| The late-scheduling prototype (`--stats`) | `tools/meshlab/spv_sink.py` |
| The synthetic stage and `probe --run` (not run) | `tools/meshlab/probes/spill.mesh`, `tools/meshlab/meshlab.cpp` |
| Test | `tests/test_direct_mesh_repl.py` |

`HOS_DBG=tmwait` replaces the default wait in `gpu/direct/direct_draw.cpp`. Tests:
`HOOKSHOT_VULKAN_HEADERS=/usr/include work/venv/bin/python -I -B tests/run_all.py`: 297 passed, 5 failed (the baseline
five: `test_texture_init_barrier`, `test_lift_cleanup_error`, `test_make_game_cache`, `test_parallel_init_order`,
`test_recomplauncher_json`), 19 skipped.

## 10. What remains

- The translator pass (section 4) is a prototype with a specification; whether to build it into the translator is the
  user's call. Eleven stages sit at the register limit meanwhile.
- The scan covers three scenes' captures and no fragment stage; a new scene needs a capture and a scan.
- The final build (the wait off by default) ran the walk once (`hffix_walk3_nowait`, survived) and the market in the
  timing runs (100 s each, no loss); the first two walks and the Kokiri picture runs were made with the wait still on.
- The four `codes.txt` stages besides `40d0` are still not swapped in.
- The Mesa report (section 8) is not written or filed; the synthetic stage has not been drawn.
