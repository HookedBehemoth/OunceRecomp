# Task and mesh shaders

This page explains how the back end draws the game's task+mesh and mesh-only programs and what to check before you
change anything in that path. Read it before you touch `renderer/vk_task.cpp` or the translator's handling of
mesh stages. Every mesh stage is drawn as the translator emits it.

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
`{x, y, z}`. The culling lists' resets, buffer clears at the culling pass (`games/oot/patches/cull_lists.c`, log line
"direct patch culling lists"), write `{0, 1, 1, 0}` where the game wrote zeros, so the culling shaders leave a valid
Vulkan record in place and no conversion copy is needed. The records the CPU writes
(`actor_records_read_b`) get y = z = 1 where the game leaves both 0.

## What the task stages do

The game has about 20 task stages; six appear in the scenes tested. They only read memory and write a payload and a
group count. Some meshlet-culling families walk a depth grid whose width and height come from a uniform buffer, and
some instance families loop over an item count from memory. Both loops are bounded on the console's data, but run
very long on zeros or garbage. If a task stage hangs the GPU, look at what it read first.

## Defenses against a mesh stage that hangs the GPU

A mesh stage that spills registers to scratch memory can lose the device while RADV, Mesa's Vulkan driver for AMD GPUs,
compiles or runs it. The translator emits the native code's order, which can load many values long before their use.

- **The register-pressure pass** reorders every translated mesh stage so fewer values are live at once; see
  [the register-pressure pass](spv-late.md).
- **A wait before task+mesh draws.** `debug.tmwait` records a framebuffer-local barrier before every task+mesh draw.
  It is off by default.

## Check your change

1. Run the register gate (`tools/shaders/late_gate.py`, see [the register-pressure pass](spv-late.md)). No mesh stage
   may start to spill.
2. Walk from Kokiri to Hyrule Field and stand in the market. Save the kernel log into the run folder after any device
   loss, and use `debug.marks` to see which command was running.
