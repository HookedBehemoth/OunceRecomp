# Render jobs

This page explains how the game's render jobs become Vulkan secondary command buffers, and which game patches make
that possible. Read it before you change the job recorder hook, the pass format table or any of the job patches.

## What a render job is

A render job is a command list that one of six threads records for one pass, in calc stage C of the frame (see
[the frame loop](frame-loop.md)). About 102 are recorded a frame. Later, the pass's layer list calls the pass's jobs
with `CallCommands`, after its own `SetRenderTargets`.

In the back end each job is one rendering secondary command buffer. A secondary that continues a dynamic rendering
must know the pass's attachment formats when it is begun. The job, however, records up to a few milliseconds before
its pass even allocates its targets. Everything on this page follows from that.

## Pass identity and formats

- **The pass key pointer is the pass's identity.** The recorder (`render_job_record` in `games/oot/labels.txt`) gets a
  pointer to the pass's key, a fixed address in the pass's owner object. It is the same in every boot and scene. The
  job's view record is not an identity: the game hands records out in arrival order, so they swap between passes.
- **Formats come from a table keyed by that pointer.** The table is written where the layer lists call their jobs,
  from the targets the list has set. A job reads it when it begins.
- **A miss draws the pass inline.** When the table does not know the key (a key's first frame, or after a settings
  change), the recorder hook returns at entry, as the game's own early return does. The layer list then finds no job
  and records the pass itself through the game's inline path, on its own thread. The next frame records jobs. No job is
  ever recorded against a guessed format.

Code: `host/eng_jobs.c` (the recorder hook), `host/direct_passes.c` (the key and the inline path), the job half of
`gpu/direct/direct_pass.cpp`. Log lines: "direct patch passes" and the back end's "jobs a frame" line.

## What a job inherits

On the console, `CallCommands` keeps the GPU state, so a job can read state its caller or an earlier job left behind. A
Vulkan secondary starts from nothing. The game's jobs set almost everything themselves:

- Ordinary draws bind their own vertex state. Fixed-function state that a job does not set is at its defaults whenever
  the game relies on it.
- **GPU-culled mesh draws read task and mesh storage buffers they did not bind.** Two patches make each job bind them:
  - `host/direct_mesh_system.c` runs the game's system binder at the start of every job, so storage slots 0-3 of the
    task and mesh stages start at the system values.
  - `host/direct_mesh_binds.c` makes three culled-mesh drawers bind the two buffers that the culled-instance drawer
    binds for itself. A binder binds nothing when the program has no slot for the name, so other programs don't change.

## Work moved out of jobs

A few jobs record work that a rendering secondary cannot hold: event waits, barriers and the occlusion-grid dispatch.
`host/direct_job_sync.c` keeps that work out of the job and records it on the layer list that calls the job:

| Work | Where it goes |
|---|---|
| two culling waits and their barriers | before the job call, on the layer list |
| the water pass's wait for its FFT compute | at the colour pass's job-call site only; earlier positions deadlock |
| the occlusion-grid dispatch | before the job's commands at the job-call site |

The occlusion-grid dispatch must run before the job's draws. If it runs after them, the grid's corner sampler reads the
finished depth, every corner counts as unchanged, and the CPU culling that reads the grid one or two frames later culls
wrongly.

Log line: "direct patch job sync" (work kept and placed a frame, by kind).

## The long layer runs ahead

Stage C's task graph opens the layer-list phase only after every render-job loop has ended. The 3D layer (the scene
pipeline and post effects) then records alone for about 2 ms in the market. `host/direct_layer_ahead.c` starts that
layer on the first thread that enters a job loop:

1. The job list is reordered into the order the layer called its passes the frame before.
2. One thread claims the layer and runs it before its own job loop.
3. At each job-call site, that thread waits until every job of the pass has gone through the recorder. The game's own
   sites don't wait; without the wait they would draw the pass inline.

It is on by default; `HOS_LAYER_AHEAD=0` turns it off. Log line "direct patch layer ahead": run ahead 1.00 a frame, waits
given up 0.

## Check that the job patches apply

Run with `HOS_DBG=logpatch`. Every 600 presents each patch logs one line ("direct patch passes", "mesh system", "mesh
binds", "job sync", "layer ahead"). In steady play, the "passes" line shows no inline passes of keys a recorder asked
for, except on a key's first frame.

## Things that will surprise you

- Five lists a frame are begun for layers that have no items and record nothing. The back end creates no Vulkan object
  until a list's first record, so they cost nothing.
- The game already draws about 20 local passes a frame inline (passes with no jobs). They are not misses.
- Recording order is not GPU order: a wait recorded before its signal on the host is still correct, because the job's
  commands run at the call's position in the queue.
