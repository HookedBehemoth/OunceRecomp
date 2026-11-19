# The register-pressure pass

This page explains the pass that reorders translated mesh stages so fewer values are live at once, how to check it
after a change, and its limits. Read it before you change `renderer/vk_spv_late.cpp`, the translator, or the
pass's defaults, and after a driver update.

## Why it exists

The translator emits a stage in the native code's order, and the game's compiler loads early: a mesh stage that reads
many rows of a table holds all of them in registers before it uses the first. RADV's compiler, ACO, does not reorder
instructions, so the translator's order decides the register demand. Past 256 vector registers ACO spills to scratch
memory, and a spilling mesh stage lost the device on this GPU; see [task and mesh shaders](task-mesh.md).

## What it does

The pass takes SPIR-V and returns the same instructions in another order, block by block. The back end runs it on every
translated mesh stage, right after translation.

- **What may move:** instructions that only compute, loads of read-only memory (uniform, input, push constants) and
  loads by address. Stores, atomics, barriers, image instructions, mesh outputs and loads of writable memory keep their
  order.
- **Loads by address never cross a write that may alias them.**
- **Subgroup operations don't move** in a stage that can demote or end an invocation.
- **A budget, not "as late as possible".** A block that holds at most 160 32-bit values at once is left as it is. A
  block past the budget holds back movable instructions while it is at the budget, so loads stay ahead of their uses by
  what the budget allows.
- **Never worse.** A block's new order is kept only when it holds fewer values than before, and a function is changed
  only when its worst block holds fewer.
- **Checked and deterministic.** Every new order is checked for soundness before it is taken; a failure leaves the
  whole module as the translator emitted it. The same input gives the same output.

By default nothing moves from one block into another. Sinking into a branch is built (`--sink-peak` on the `spvlate`
command) but off: it made some stages spill more.

## Switches and caches

| Switch | Effect |
|---|---|
| `debug.nolate` | the translator's order, unchanged (a diagnosis) |
| `debug.latevertex` | the pass on vertex stages too (a trial; off by default) |

The pass runs before the driver compiles a stage, so its result is in the driver binaries the caches keep. The pass's source is part of the shader stamp, and both
switches are part of the cache's stamp. So a build that changes the pass, or a run that flips a switch, translates
every program once more on its first start. You don't need to delete anything.

## Check a change: the gate

The pass's count of live values is not ACO's register allocation. `tools/shaders/late_gate.py` holds one against the
other. Run it before you change the pass's defaults, after a translator change and after a driver update:

```
python3 -I tools/shaders/late_gate.py seed --censustool <censustool> --spvlate <spvlate> --seed <seed> --out <dir> --kinds mesh
```

It compiles every mesh stage of the game's shader seed before and after the pass. The gate fails when a stage starts
to spill, spills more, or needs noticeably more registers. `tools/shaders/build.sh` builds `spvlate`, and
`tools/shadercensus/build.sh` builds `censustool`.

The pass has a unit test: `renderer::spv_late` in `cargo test`.

## Limits

- **It reorders; it does not recompute.** The mesh stages that still spill hold their values across blocks, often as
  loop-carried registers. Bringing those down needs a change of the translation, not of the order.
- **Its count is not ACO's.** It knows nothing of variables the driver turns into registers or of values ACO keeps in
  scalar registers. That is why the budget is 160, not 250, and why the gate exists.
- **Loads of uniform buffers move past stores by address**, as the driver's own compiler allows.
