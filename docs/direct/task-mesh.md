# Task and mesh shaders

This page explains how the back end draws the game's task+mesh and mesh-only programs, which mesh stages it replaces
with hand-written ones and why, and what to check before you change anything in that path. Read it before you touch
`gpu/direct/direct_task.cpp`, `gpu/direct/direct_meshrepl.cpp`, the shaders in `gpu/direct/shaders/mesh` or the
translator's handling of mesh stages.

## How a task+mesh draw runs

The device runs with the task stage off: native task shaders lost the device on this GPU. A task+mesh draw is two
pieces of work:

1. **The task stage runs as a compute shader**, one workgroup per task workgroup of the draw. It writes the draw's task
   header in the frame arena: a map entry for each mesh workgroup it starts, its payload once, and the total count of
   mesh workgroups.
2. **The mesh stage is drawn natively** with `vkCmdDrawMeshTasksIndirectEXT` on that header. Each mesh workgroup reads
   its map entry and its task's payload by address.

A compute step cannot run inside a rendering. In a job, the step is recorded into two plain secondaries that the layer
list executes at the job's call, before the job; in a layer list, the rendering is ended before the step and begun
again after it. For an indirect draw the task count is known only on the GPU, so a one-invocation compute step
(`shaders/task_prep.comp`) reads the game's record first.

Folding the task stage into the mesh stage is not possible: one task workgroup feeds up to 32 mesh workgroups of up to
126 vertices each, past the mesh output limits.

### The culling's indirect records

The game's GPU culling writes the indirect mesh-draw records. NVN reads a record as `{count, first}`, Vulkan as
`{x, y, z}`. Two patches make the game's CPU reset write `{0, 1, 1, 0}` instead of zeros, so the culling shaders leave
a valid Vulkan record in place and no conversion copy is needed: `host/direct_mesh_args.c` and
`host/direct_mesh_lists.c` (log lines "direct patch mesh args", "direct patch mesh lists").

## What the task stages do

The game has about 20 task stages; six appear in the scenes tested. They only read memory and write a payload and a
group count. Some meshlet-culling families walk a depth grid whose width and height come from a uniform buffer, and
some instance families loop over an item count from memory. Both loops are bounded on the console's data, but run
very long on zeros or garbage. If a task stage hangs the GPU, look at what it read first.

## Replaced mesh stages

One of the game's mesh stages is drawn with a hand-written stage that emits the same geometry bit for bit:

- The sources are in `gpu/direct/shaders/mesh` (`meshlet_depth.mesh`, `meshlet_lit.mesh`, `probe_light.glsl`).
- `gpu/direct/shaders/mesh/codes.txt` maps a game stage's code key to its source and defines;
  `tools/shaders/build_shaders.sh` compiles them into `<code key>_spv.h`.
- `gpu/direct/shaders/mesh/swap.txt` lists the stages the back end actually swaps in. The set is closed: the stage
  listed stays, and no new ones are added.
- The translator still translates the stage: its interface information drives the heap mappings, the push data and the
  fragment stage's inputs. The replacement takes its place when the shader object is made.

`HOS_DBG=nomeshrepl` draws with the translator's stages instead.

### Why: the Hyrule Field hang

One mesh stage, `40d0c0768e874600` (the lit meshlet stage plus per-vertex light probes, drawn for ivy on the way from
Kokiri to Hyrule Field), lost the device every time as RADV compiled the translator's SPIR-V for it. RADV is Mesa's
Vulkan driver for AMD GPUs. That stage was the only mesh stage of the captured scenes that spilled registers to scratch
memory: the translator emits the native code's order, which loads all 112 probe rows before using the first, and RADV's
compiler (ACO) needs 258 vector registers where it has 256.

The hand-written stage needs far fewer registers and draws the same triangles. With it, the walk that lost the device
every time survives. Why a spilling mesh stage hangs the GPU is inferred, not known; the open work is listed in
[the direct back end plan](../direct-backend-plan.md).

Two more defenses exist:

- **The register-pressure pass** reorders every translated mesh stage so fewer values are live at once; see
  [the register-pressure pass](spv-late.md).
- **A wait before task+mesh draws.** `HOS_DBG=tmwait` records a framebuffer-local barrier before every task+mesh draw.
  It stopped the market's device losses before the stage was replaced; it is off by default.

## Check your change

1. Run the register gate (`tools/shaders/late_gate.py`, see [the register-pressure pass](spv-late.md)). No mesh stage
   may start to spill.
2. Walk from Kokiri to Hyrule Field and stand in the market. Save the kernel log into the run folder after any device
   loss, and use `HOS_DBG=marks` to see which command was running.
