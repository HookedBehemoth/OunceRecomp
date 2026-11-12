# M4: the whole frame (structure and stages, 2026-11-12)

M3 landed (668d9ef; `m3a-report.md`, `m3b-report.md`). Every job is one rendering secondary executed in its pass;
load-time lists are packets; shaders read their buffers by address and their texture handles themselves, through the
translator forked into `gpu/direct/eden`. Kokiri's depth, shadow and probe passes match the frozen back end. The lit
colour pass and the final composite are still black: the scene reaches them only through compute. With pacing off a
frame takes 10.8 ms in Kokiri and 11.7 ms in Hyrule Field; the draw path costs 1.7 / 3.0 ms a frame summed over the
recording threads.

M4's goal (plan, Milestones): every pass of the frame matches the frozen back end with render conditions forced on;
no draw or dispatch is skipped.

## M4a: compute

- **Dispatches** (direct and indirect) in every list kind: compute programs through the forked translator with the
  step-B interface, push compute constants, the workgroup counts of an indirect dispatch read by the GPU.
- **Compute lists** are primaries like the layer lists; a dispatch inside a layer list ends the rendering, as the
  pass rules already say. The queue order and the host-held waits stay as they are (events and reports stay
  CPU-written until M5).
- **Copies and clears** not yet handled by the passes (buffer to buffer, texture clears inside compute lists).
- **Uniform buffers read past their bound size** (`m3b-report.md`, open item): find what the game's shaders rely on
  there, by evidence, and settle it once in the translator's interface (not per program).

Done when, in Kokiri, Hyrule Field and on the title screen: dispatches drawn a frame against recorded (all); the lit
colour pass and the final composite show the scene, and the title shows its movie; `scripts/direct_dump_compare.py`
per pass against the frozen back end, with the passes that still differ listed with their cause (mesh draws: M4b;
conditions: M5); validation: every message reported; frame time with pacing off and time in `hsd_emit` by list kind.

## M4b: indirect and mesh draws

- **Indirect draws** (arrays, elements, with count buffers) from their records in layer 0.
- **Mesh-only programs** drawn natively (they have run natively on the frozen back end without a device loss).
- **Mesh draw arguments in Vulkan's layout**: the patch of the game's zeroing of the argument records
  (`m0-task-mesh.md` 3), so no conversion copy exists; the writer of the five other indirect addresses identified.
- **The inherited task and mesh buffers**: the patch of `m3-patch-sites.md` 3 (the system binder at the job
  prologue, the two binds in the drawers that lack them).
- **Task+mesh programs are NOT drawn in this stage**: they stay counted and skipped. Their native draws lose the
  device on this GPU (plan, "Task and mesh shaders"); they are M4c, run alone under the hang rules.

Done when: indirect and mesh-only draws drawn a frame against recorded (all); task+mesh draws counted and named by
program; Hyrule Field's depth and shadow passes compared again (what terrain is drawn by which kind of program);
validation; no device loss.

## M4c (after a and b, alone): the task stage

The task stage as a compute step per task workgroup feeding a native mesh stage (plan), built and tried under the
hang rules: everything committed first, no other agent working, at most three device losses per boot, the kernel log
saved after each. The parked experiments on the frozen back end belong here too.

## Rules

As `m3-interface.md`.
