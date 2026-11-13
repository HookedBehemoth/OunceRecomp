# The register-pressure pass in the translator (2026-11-13)

Branch `agent/spvlate` (from native-vk 87c21f8). RX 6950 XT (Navi 21), RADV Mesa 26.2.4, kernel 7.2.11-1-cachyos. Game
runs in `work/runs/spvl_*`; gate tables, compiler outputs, censuses and harness runs in `work/spvlate/` (all local: they
hold game data or are made from it). Follows `hf-fix-report.md` (the cause; its section 4 specified this pass). Every
statement is **measured** (a run or a file says so) or **inferred**.

## Summary

@@SUMMARY@@

## 1. The pass

`gpu/direct/direct_spv_late.{h,cpp}` (`hsd::spv_schedule_late`): SPIR-V in, SPIR-V out, the same instructions in another
order. It stands alone (no header of the back end), so the harness, the census and the test build the same source. The
back end calls it on a mesh stage's SPIR-V right after `EmitSPIRV` (`direct_shader.cpp schedule_late`, both mesh
translations). Commit 79f5ee5's file was the starting point; what its first version lacked is what made it raise two
stages' demand (`hf-fix-report.md` section 4), and all of it is here:

- **What may move.** Instructions that only compute (arithmetic, composites, conversions, access chains, `OpExtInst`
  but for Modf and Frexp, which write through a pointer), loads of uniform, input, push-constant and uniform-constant
  memory, loads by address, subgroup operations, calls of functions that only compute. Everything else keeps its order:
  stores, atomics, barriers, image instructions, mesh outputs, loads of memory the stage can write (function, private,
  workgroup, output, storage buffer), calls of functions with effects; a block's label, phis and variables stay first,
  its merge and terminator last.
- **Loads by address never cross a write that may alias.** A load by address (or a call of a function that reads by
  address) stays between the two instructions around it after which memory read by address may hold other values: a
  store or copy by address, any atomic, a control or memory barrier, a call of a function with effects. Loads of a
  uniform buffer are not held that way (the driver's own compiler treats them as read-only too).
- **Subgroup operations** do not move at all in a module that can demote or end an invocation, and never leave their
  block.
- **A budget, not "as late as possible".** A block that holds no more than 160 32-bit values at once
  (`LateOptions::budget`) is left word for word. A block past it is emitted in the translator's order with the movable
  instructions held back while the block is at the budget, and placed as soon as there is room again or something needs
  them (operands first, the larger operand tree first). So the loads stay ahead of their uses by what the budget allows.
  Only where that does not bring the block near the budget (values held on entry, operands that must come early) is
  every movable instruction emitted at its first use, as the prototype did (and what frees at least as many values as
  it makes follows at once: a row is consumed as it is loaded). Why not the prototype's order everywhere: section 6.
- **Kept only when the count is lower.** The count is the most 32-bit values a block holds at once: what it holds on
  entry and at its end comes from the function's control flow (each value followed from its uses back to the block
  that makes it; a phi's operand is held to the end of the block it comes from), which a block's own order does not
  change. A block's new order is kept only when it holds fewer than the order it had, and a function is changed only
  when its worst block then holds fewer. So the count never rises in any block, which is what the first version could
  not say.
- **A soundness check on every new order** before it is taken: every instruction once, the first and last ones in
  their places, the instructions with effects in their order, each value made before it is used, each load by address
  between its writes. A failure leaves the whole module as the translator emitted it. (It has not failed on any stage
  of the game; `spirv-val` found the one bug of this stage's development, a merge instruction parted from its branch, and
  that case is now in the check.)
- **Deterministic:** no order is taken from a hash table; the test runs a module twice and compares.

**Control flow: nothing moves from one block into another by default.** Not into a loop, not into a branch, not out of
one. **Sinking into a branch is built and is off** (`LateOptions::sink_peak`, `spvlate --sink-peak N`; 0 = never): an
instruction that only computes, or loads from read-only or by-address memory, goes down into the one other block that
holds every use of its value (a phi's use counts in the block the value comes from) when that block cannot be reached
again from itself without passing the instruction's own block (so it runs at most as often as before: never into a
loop), when holding its operands that far costs fewer values than not holding its result, and, for a load by address,
when no instruction that may write by address lies on any way between the two. Why it is off, **measured** on the
whole seed's mesh stages with the pass's earlier at-first-use scheduling (`work/spvlate/gate1_seed_local`, `gate1_seed_sink`):
the same 17 stages spill with it and without it, and with it two of them spill more (`4ca31d6e` 182 -> 195 scratch
words, `c3a3d76f` 188 -> 201). What holds the remaining stages up is not movable into one block (section 7).

## 2. The gate

The count is the pass's; whether a stage spills is ACO's register allocation. `tools/meshlab/late_gate.py` holds one
against the other, and is what to run before the pass's defaults change, after a translator change, on a capture of a
new scene, and after a driver update:

- `late_gate.py captures`: every mesh program of the given `HOS_DBG=meshcap` captures made as the game's shader objects
  (`meshlab stats`, now one process a capture; nothing drawn), as captured and with the pass (`meshlab --late mesh`,
  the same source the game runs), twice each: with the driver's scheduler on (the spills the game gets) and with
  `ACO_DEBUG=nosched` (register demand). With `--spvlate`: every captured stage's SPIR-V through the pass must keep its
  instructions and pass `spirv-val`. With `--compare`: every captured mesh draw's geometry with the pass against the
  translator's, bit for bit (`meshlab compare --late mesh`, under the GPU lock; one draw each, now with the rasterizer
  off, so a stage that spills is not followed by pixel work).
- `late_gate.py seed`: every stage of a kind in the game's shader seed (`nx2/shader_census.py`: all 42,856 stages of
  the game, visited or not) translated offline (`censustool`, as M6h's census) and made as the game's shader object
  (`censustool --objects`: unlinked, mesh stages with the no-task flag, 32 lanes), before and after. Compile only.
- It fails when a stage that did not spill spills with the pass, a stage that did spills more (by more than 4 words or
  an eighth), a stage needs more than 8 registers more than before and is then within 32 of the 256, or a stage's SPIR-V
  does not keep its instructions or stops being valid.
- `tests/test_direct_spv_late.py` (in `tests/run_all.py`; no game, no GPU) holds the pass's rules on modules it builds:
  within the budget nothing changes; past it the loads follow their uses, effects keep their order, the phi stays
  first; a load by address stays between two stores by address; with the sinking step on, loads only a branch uses go
  into it, nothing goes into a loop, a subgroup operation stays; the same output twice; the five hand-written mesh
  stages and the harness's synthetic spilling stage (`probes/spill.mesh`: 324 values held -> 162) come out with the
  same instructions and valid.

### Captures (measured: `work/spvlate/gate3_captures/`)

Market `m6e_cap1` and `m6e_cap2`, Hyrule Field `m6e_cap_hf` and `hang_hf_cap`, Kokiri `m6e_cap_k`, the losing frames
`hfh_f_plainfs1:1897`, `hfh_t_losttest:1949` and `hfh_a_lost:3596`: 53 mesh stage codes, 6 task stage codes.

| | Before | With the pass |
|---|---|---|
| Mesh stages with scratch spills | 1 (`40d0`) | **0** |
| Mesh stages at 240 registers or more (scheduler off) | 12 | 2 |
| Mesh stages whose demand fell / rose | | 10 / 0 |
| Task stages (not touched) with spills | 0 | 0 |

The ten stages the pass changes (the other 43 are word for word what the translator emitted):

| Mesh stage | Spilled words before -> after | Registers before -> after |
|---|---|---|
| `40d0c0768e874600` | 16 -> 0 | 258 -> 163 |
| `463f42630fce83a0` | 0 -> 0 | 276 -> 183 |
| `3d33bb73d3deee34` | 0 -> 0 | 272 -> 190 |
| `e3fc8fcf540f7a48` | 0 -> 0 | 269 -> 181 |
| `b86297c24c8d8c7a` | 0 -> 0 | 257 -> 167 |
| `b509ae330829ce02` | 0 -> 0 | 256 -> 164 |
| `55e5c68554e506d6` | 0 -> 0 | 253 -> 183 |
| `072e452fb9b68991` | 0 -> 0 | 248 -> 165 |
| `6eaef39e69dd118b` | 0 -> 0 | 248 -> 182 |
| `d38b0d52a94ab5a8` | 0 -> 0 | 245 -> 159 |

`b86297c2` is one of the two stages the first version made spill (0 -> 29); the other, `354dde83`, and `66c494fe` are
the two left at the limit (257 and 259, neither spills, the pass leaves both as they are: section 7).

@@COMPARE@@

### The whole seed's mesh stages (measured: `work/spvlate/gate2_seed/`)

All 500 mesh stages of the seed (499 distinct codes), translated offline and compiled as the game's shader objects:

| | Before | With the pass |
|---|---|---|
| With scratch spills | **40** | **17** |
| At 240 registers or more | 82 | 19 (the 17 and the two above) |
| Stages the pass changes | | 108 |
| Demand fell / rose | | 71 / 4 (by 4 at most; two of the four spill either way, the others are at 177 and 168) |

The 23 stages it frees of spills are `40d0`'s kind: 258-259 registers and 4-20 spilled words before, 163-188 and none
after. 42 more stood at 240 or more without spilling; 40 of them are now at 190 or less. **The 17 that still spill are
not `40d0`'s kind** (section 7), and each is a stage to expect the Hyrule Field loss from when its scene is visited:
`2658d77e`, `65ca69c7`, `c64d72bb`, `3868defa`, `18341da3`, `41701590`, `11a1afbd`, `3af1ac7c`, `60f28d80`, `fbbc9cba`,
`c3a3d76f`, `4ca31d6e`, `25ae727d`, `cf139894`, `53319ee7`, `c48ceed6`, `209e3c2e` (the table has the full keys). None
of them is in the three scenes' captures.

The pipeline census (M6h's method: RADV's own statistics, mesh linked with its fragment stage; `work/spvlate/cf_all`
against `census_before`) agrees: 40 -> 17 mesh stages with spilled vector registers, 82 -> 19 at 240 pre-scheduling
registers or more, none newly spilling, none spilling more.

## 3. The other stage kinds

**Measured** over all 42,856 stages of the seed, compiled as pipelines with the pass on every kind (`censustool --late
all`, `work/spvlate/cf_all`) against without (`census_before`); "registers" is ACO's own count before its scheduler:

| Kind | Stages | Spilling before | Most registers before | Stages the pass would change | Decision |
|---|---|---|---|---|---|
| task | 19 | 0 | 59 | 0 | off: nothing to do |
| compute | 4,122 | 0 | 176 | 2 | off: nothing to help |
| fragment | 22,343 | 0 | 179 | 10 | off: nothing to help |
| vertex | 15,851 | 0 as pipelines, **21 as the game's shader objects** | 257 | 781 | off, with a switch: see below |
| mesh | 499 | 40 | 335 | 108 | **on** |

- **Fragment stages are scanned** this way (every fragment stage of the game, with RADV's statistics): none spills a
  vector register and none needs more than 179. The capture scan can compile the captured fragment stages too
  (`spill_scan.py --fs-real`, `late_gate.py captures --kinds mesh,fragment`).
- **Vertex stages are the finding.** 769 vertex stages need 200 registers or more. Made as the game makes them
  (unlinked shader objects; `work/spvlate/gate2_seed_vertex`): all 769 are at 242-258 registers and **21 spill** 2-8
  words to scratch; with the pass none spills and all are at 152-186. ACO's own cost estimate of the 781 changed
  stages falls to 0.61 of what it was (median; the loads no longer wait behind 250 held values). One of the 21,
  `a6653e1e9111ba7d`, is drawn in the Hyrule Field frame of `m6h_ps3_hf`, a scene that runs for minutes; **inferred:**
  a vertex stage with a few spilled words has not lost the device there (the offline translation of it was not held
  against the game's own, and whether a spilling vertex stage can lose the device as a spilling mesh stage does is not
  known). It is **not on**: the gate's second half cannot be run, since no harness draws a captured vertex draw with
  its own vertex buffers (M6h's fragment harness draws a grid of its own). `HOS_DBG=latevertex` turns it on for a
  trial (the cache is stamped by it). @@VERTEXRUN@@

## 4. Caches

The translated SPIR-V lives in the program cache (`hookshot_direct_programs.bin` beside the executable). Its stamp is
the build's shader stamp (`HOOKSHOT_DIRECT_SHADER_STAMP`, `nx2/build.py`: a hash of the translator's sources, now with
`direct_spv_late.cpp` and `.h` in it) and the back end's flags (`direct_shader.cpp disk_stamp`: now with "the pass on
mesh stages" and "on vertex stages"). A cache with another stamp is not read and is written anew, so a build with
this change translates every program once more on its first start (the scene is complete later on that start:
`hf-fix-report.md` saw 97 s against 40 s), and `HOS_DBG=nolate` or `latevertex` does the same each time it is switched.
**A user deletes nothing.** The shader objects' binaries (`hookshot_direct_shader_objects.bin`) are keyed by each
stage's SPIR-V: the stages the pass leaves alone find theirs, the changed ones are compiled once.

## 5. In the game

@@GAME@@

## 6. Translation time, compile count, frame time

- **The pass's time** (measured, `work/spvlate/cf_all`, with five jobs at once on a shared machine, so high): a mesh
  stage 2.5 ms (median over all 499), a changed one 5.3 ms (median), 10 ms at most; the two stages of 118,000 ACO
  instructions, which it does not change, 288 ms. All 499 mesh stages together: 2.7 s. A stage of 17,000 blocks needs
  80 MB while it is looked at (the first version of the liveness, as bit sets, needed 1.5 GB for it).
  @@GAMETIME@@
- **Shader compiles:** the pass makes no stage and removes none. @@COMPILES@@
- **Frame time: no claim from the game** (the machine was shared with two other agents' builds and runs all day).
  What is **measured** is the changed stages' own GPU time in the harness (`meshlab bench --late mesh`: the captured
  draws of the nine changed stages that do not spill, the translator's against the pass's, alternating, GPU timestamps,
  under the GPU lock with no game process; `work/spvlate/bench/`):

@@BENCH@@

  **Inferred:** the price of the margin is a few microseconds a frame. ACO's cost estimate says the same and says why
  the budget is there: over the seed's mesh stages its sum rises 2.6 % with the pass as it is, and rose 8 % with
  everything emitted at its first use (the `40d0` family's by 30 %: a row loaded and consumed at once makes each load
  wait for the one before); with a budget of 128 it rose 4.4 %, with 192 1.5 % (those three with an earlier build,
  `work/spvlate/cm_*`). 160 leaves ACO at 159-190 registers on the stages that were past 256.

## 7. Limits

- **It reorders; it does not recompute.** The 17 mesh stages that still spill hold their values across blocks.
  **Measured** on two of them and on the two at the limit: in `66c494fe`'s worst block 263 values are held at its end,
  of which 179 are used in that block and again in one later block (so they cannot go down into it: they would have to
  be computed twice); in `3af1ac7c` 370 of the values held are phis of a loop (the game's registers carried around
  it). Bringing those down needs values computed again where they are used, or the translator not carrying every
  register through a loop: a change of the translation, not of the order.
- **The count is not ACO's.** It counts SPIR-V values as 32-bit words. It knows nothing of variables the driver turns
  into registers (`53319ee7`: 145 by the count, 314 by ACO), of values ACO keeps in scalar registers or as lane masks,
  or of ACO's own scheduler. Over the seed's mesh stages the count is 30 above ACO's in the median and between 169
  below and 135 above. That is why the budget is 160 and not 250, and why the gate exists.
- **A block within the budget is never touched,** so a stage whose demand comes from many values held on entry of
  small blocks is left alone even where an order within its large blocks would lower other blocks' counts.
- **Loads of uniform buffers move past stores by address.** A stage that wrote by address into memory it also reads as
  a uniform buffer would see the old value where it saw the new one; the driver's compiler takes the same liberty.
- **The gate's captures must be the translator's own stages:** a capture taken with the pass on holds the pass's
  output, and "before" is then not before. Take them with `HOS_DBG=nolate`.
- **Vertex stages:** section 3.

## 8. Device losses

@@LOSSES@@

## 9. Code

| What | Where |
|---|---|
| The pass | `gpu/direct/direct_spv_late.{h,cpp}` |
| Its call on mesh stages, `HOS_DBG=nolate` (off), `HOS_DBG=latevertex` (vertex stages too), the cache stamp, the statistics line | `gpu/direct/direct_shader.cpp` (`late_mesh`, `late_vertex`, `schedule_late`, `disk_stamp`, `shader_stats`); `nx2/build.py` (the shader stamp) |
| The pass as a command (`--budget`, `--sink-peak`, `--blocks`: a line a block, `--dir`) | `tools/meshlab/spvlate.cpp`, `tools/meshlab/build.sh` |
| The gate | `tools/meshlab/late_gate.py` |
| The scan: one process a capture, `--late`, `--fs-real`; `scan()` for the gate | `tools/meshlab/spill_scan.py` |
| meshlab `--late mesh[,task,fragment]` for stats, compare, dump, run and bench; compare with the rasterizer off; `@@object` lines | `tools/meshlab/meshlab.cpp` |
| censustool `--late <kinds>`, `--budget`, `--sink-peak`, `--objects`, `--list`, `--spv-only`; the pass's count in every line; census.py `--tool-args`, `--kinds` | `tools/shadercensus/censustool.cpp`, `census.py`, `build.sh` |
| The prototype, marked as replaced | `tools/meshlab/spv_sink.py` |
| Test | `tests/test_direct_spv_late.py`, `tests/run_all.py` |

Tests: `HOOKSHOT_VULKAN_HEADERS=/usr/include work/venv/bin/python -I -B tests/run_all.py`: 298 passed, 5 failed (the
baseline five: `test_texture_init_barrier`, `test_lift_cleanup_error`, `test_make_game_cache`,
`test_parallel_init_order`, `test_recomplauncher_json`), 19 skipped.

The gate as run here (the tools built with `MESHLAB_OUT=... tools/meshlab/build.sh` and `CENSUS_OUT=...
tools/shadercensus/build.sh`):

```
python3 -I tools/meshlab/late_gate.py captures --meshlab <meshlab> --spvlate <spvlate> --out <dir> --compare \
    work/runs/m6e_cap1/meshcap/frame.bin work/runs/m6e_cap2/meshcap/frame.bin work/runs/m6e_cap_hf/meshcap/frame.bin \
    work/runs/m6e_cap_k/meshcap/frame.bin work/runs/hang_hf_cap/meshcap/frame.bin \
    work/runs/hfh_f_plainfs1/meshcap/roll.bin:1897 work/runs/hfh_t_losttest/meshcap/roll.bin:1949 \
    work/runs/hfh_a_lost/meshcap/roll.bin:3596
python3 -I tools/meshlab/late_gate.py seed --censustool <censustool> --spvlate <spvlate> \
    --seed work/m6h/hookshot_shaders.bin --out <dir> --kinds mesh
```

(2 minutes and 5 minutes of compiling; the compare is as long as the GPU lock is busy.)

## 10. What remains

@@REMAINS@@
