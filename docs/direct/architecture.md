# How the direct back end works

This page explains how the game's NVN commands become Vulkan work: where the back end takes over, what each kind of
command list becomes, how a draw finds its data, and how a frame is submitted. Read it before you work in `gpu/direct`.
The rules the back end keeps and the open work are in [the direct back end plan](../direct-backend-plan.md).

## The seam

The host's NVN layer, `host/nvn_native.c`, keeps the NVN objects: device, memory pools, buffers, textures, samplers,
syncs and events. Its command functions append a record to the command buffer. That append is the seam: at a flush
point, on the calling thread, the record goes to the back end in the format of `gpu/api/rx_nvn.h`. The back end's
interface is `gpu/direct/direct_api.h` on its side and `host/direct.h` on the host's.

NVN alone does not say everything the back end needs. For example, a render job's command list is anonymous. Each
missing fact comes from a hook on the engine function that has it:

| NVN lacks | Taken from |
|---|---|
| which pass a render job belongs to | the job recorder's pass key ([render jobs](jobs.md)) |
| the job's target formats | a table keyed by the pass key, filled where the layer lists call their jobs |
| what kind of list a recording is | the caller of `BeginRecording` |
| final deferred texture handles | a copy at the guest's submit |
| whether a texture's image exists | the texture loader and the render-target allocator hooks, at registration |
| report values | the GPU, written to guest memory |

When a command needs something NVN does not carry, hook the engine function that has it. Never infer it at submit.

## What a command list becomes

The kind of a list is decided at `BeginRecording` from its caller (`HSD_LIST_*` in `direct_api.h`).

| NVN list | About this many a frame | Becomes |
|---|---|---|
| render job | 102 | one rendering secondary command buffer that inherits its pass's formats |
| layer list, gate primary | 10 | primary command buffers, cut at cross-queue event waits; it begins and ends rendering and executes its jobs |
| async-compute list, queue head | 12 | primary command buffers |
| immediate list (a one-shot list the game submits and waits for, such as the save picture's copy) | 0, a few at a save | primary command buffers |
| load-time list (vertex, draw, shape, material) | tens of thousands live, hundreds of calls a frame | a packet: a compact op array, expanded into the caller's command buffer where it is called |
| a list not classified | 0 | a packet, counted |

A packet runs only where a recording list calls it. A packet submitted by itself has nothing to run: it is counted
and logged as "submitted but never recorded", and it is an event in strict mode. When a shared engine function begins
lists for several callers, each caller names the kind at its own call.

The packet expander is the only interpreter, and it runs on the calling thread at record time.

## Passes

`SetRenderTargets` notes the targets. The back end begins dynamic rendering lazily, at the first draw, job call or call
of a list that draws, so the clears in between become load operations. A barrier, a dispatch, a copy or a texture
clear ends the rendering; the next draw begins it again and loads. The next `SetRenderTargets` or the list's end ends
the pass. A barrier is the game's NVN barrier as one pipeline barrier. Images stay in `VK_IMAGE_LAYOUT_GENERAL`.

RADV has no dynamic-state inheritance and no inherited conditional rendering. A secondary command buffer therefore sets
all its state, and a render condition is begun and ended inside it.

## Data a draw reads

| Data | How it reaches the GPU | CPU work per draw |
|---|---|---|
| uniform, storage, vertex and index buffers | the guest address, through a window buffer or a device address | arithmetic |
| textures and samplers bound by handle | descriptor-heap index = the NVN pool id, written at registration | none |
| deferred handle arrays | the bind reserves words in the frame arena; one copy from the game's array runs at the guest's submit | none |
| handles and addresses inside uniform buffers | read by the shader from the buffer | none |
| inline uniform updates | the bytes go to the frame arena; later draws of the list bind that copy | a range lookup |
| GPU-made draw arguments | indirect draws; the game's culling writes mesh arguments in Vulkan's layout (a patch) | none |

The deferred arrays cannot be read in place: the game keeps two copies, flips them each frame, and rewrites most words
one present after the submit that read them.

Every memory pool the GPU can read is bound in full when it is made, including pools the CPU cannot access. Shaders
follow pointers the game stores in memory, so a pool reachable only through a stored pointer must still be visible.

What a translated program expects at a draw is in [the shader contract](shader-contract.md).

## Programs

All programs are in the romfs. The back end translates them with its fork of Eden's recompiler (`gpu/direct/eden`) and
keeps shader objects in caches beside the executable. A table keyed by the NVN program's serial holds the variants and
is read without locks. A program missing at load is compiled then, and the load waits. A program the game set but the
workers have not read yet is read at its first draw or dispatch, which waits for it: a draw the game makes once, such
as the pause picture's, has no later frame. A variant missing in play skips its draw, is counted, and is compiled in
the background.

## Textures

- **Asset textures** are images made at registration from the prepared pack (`hookshot-builder textures`). The loader
  hook skips the game's texel copy, so guest texel pages stay unbacked. The back end follows the game's own texture
  events and frees an image when the game destroys its texture. See [texture streaming](texture-streaming.md).
- **Render targets** have an image per stable slot of the game's allocator (`host/rt_slots.c`). Transient targets live
  in one constant allocation; see [the transient pool](transient-pool.md) and [render targets](render-targets.md).
- **Terrain tiles and other CPU-written texels** arrive linear and are copied into the image at the call; see
  [terrain tiles and CPU texels](terrain-texels.md).

The back end has no block-linear code and no ASTC decoder; both end at the build-time [converters](converters.md). A
texture with no prepared record draws a 1x1 zero placeholder and is counted. There is no texture cache keyed by guest
bytes and no page tracking.

## Task and mesh shaders

The mesh stage runs natively. The task stage runs as a compute step before the pass, and the mesh stage is drawn with
`vkCmdDrawMeshTasksIndirectEXT`, reading the task's payload from a buffer. See [task and mesh shaders](task-mesh.md).

## Reports and render conditions

Occlusion queries are real Vulkan queries. At the end of the rendering they were recorded in, their results are copied
and one compute step writes each report's 16 bytes (counter and timestamp) at its guest address. The game's CPU readers
then read plain memory. A render condition is Vulkan conditional rendering on the low word of the report's first
counter; `HOS_DBG=condcheck` counts conditions that need more than that (none seen). The details are in
`gpu/direct/direct_query.h`.

## Submission

The guest submits on its Presentation Thread. By then every list is recorded Vulkan, in chunks cut at each event signal
and wait. The submit orders the chunks of the guest's two queues so each wait follows its signal, and a sender thread
sends them with `vkQueueSubmit2` on one Vulkan queue with one timeline semaphore. Timeline values are given in order on
the submitting thread, so the game's thread does not wait in the kernel's submit. A wait whose signal comes from the CPU
is held on the host. An NVN sync is the timeline value of its submission's last chunk.

Each recording thread has a command pool, a frame arena, query pools and a task pool per command slot, three slots in
rotation. A slot is reset as a whole once the GPU is past its last submission. A list kept alive longer (a pause, for
example) sets its slot aside, and the thread gets one slot more (`gpu/direct/direct_cmd.h`).

## Engine code replaced above NVN

| What | Why NVN cannot stay |
|---|---|
| texture loading | it keeps a resident CPU copy of every texel |
| render-target placement (`host/rt_slots.c`) | the game makes new objects at new addresses every frame |

The game patches the back end relies on are declared in the `host/direct_*.c` files and `host/rt_slots.c`. To list
every patch and the site it applies to, run `python -m nx2.patch --game games/oot report`.
