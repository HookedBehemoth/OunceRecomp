# The direct back end

The direct back end (`gpu/direct`, loaded by `host/rx_backend.c` as `hookshot_direct.so`) replays the game's NVN2 command
buffers on Vulkan. It is the only renderer in the tree; the target is one PC, an RX 6950 XT on Mesa RADV, with every
extension Mesa offers. Texture-replacing mods are ignored. This document is its design, its rules and its open problems;
what is known about the game itself is in [direct/](direct/README.md).

## Why it is built this way

Commands are recorded as Vulkan on the thread that makes each NVN call, not walked again by a serial replay thread. A
pass, a job or a program is never decided by a later step that has to look back: if a command needs data that is not
final when it is recorded, the data moves (to load time, to a GPU read, or to one flat copy at the guest's submit).

## Goals and how each is measured

| Goal | Means | Measured by |
|---|---|---|
| No reliance on NVN's memory coupling beyond what the GPU can honour | pools are Vulkan memory at the guest address (layer 0); everything else a command needs is known when it is recorded, is read by the GPU, or is snapshotted once at submit | 0 CPU reads of GPU results in the back end; 0 submit-time resolves except the handle snapshot |
| No memory or Vulkan-object churn per frame | per-thread command pools and bump arenas per frame slot, reset when the frame's timeline value has passed; images, views, heap entries and shader objects made at load | 0 object creations a frame in steady play (counter) |
| No resident copies between CPU and GPU | prepared assets go from the pack to their image; guest texel pages are never written | guest memory use, `direct: memory` log lines |
| Render work spread over the game's threads | each NVN command-buffer call emits Vulkan on the thread that makes it | the Presentation Thread's ms a frame; the workers' added time |
| A smooth 60 fps | all of the above; programs built and textures in VRAM before they are drawn | 99th-percentile frame time under 16.7 ms on every scene, area changes included |

## Rules

1. **One path.** No per-feature switches inside the back end; a debug aid is a build option or one `HOS_DBG=<list>`.
2. **No fallback.** Every miss has a defined behaviour: wait (load time only), or skip and count (play). `HOS_DBG=strict`
   stops at the first one. Steady play must count zero.
3. **Known when recorded.** Data that is not final at recording moves to load time, a GPU read, or one flat copy at submit.
   Never a per-draw step at submit.
4. **Prepare offline.** The romfs can be changed. A problem a build-time tool can remove is not solved at run time.
5. **Cut at the NVN call.** Engine code above NVN is replaced only where NVN's semantics force a CPU readback, a copy or a
   serial order (see Engine replacements).
6. **Checks need no second renderer in the process.** Pictures against stored references, the validation layers, counters,
   GPU asserts.
7. **Patch the game, not the back end.** The back end implements what Vulkan needs, plainly. When the game does something
   awkward for it, the fix is a patch of the game: a hook or splice in the lifted code, a replaced shader, or changed romfs
   data. The back end gets no special case for one pass, one program or one call site. Only this one game build is
   targeted, so a patch may name addresses.

## Architecture

### The seam

`host/nvn_native.c` keeps its object layer (device, pools, buffers, textures, samplers, syncs, events). Its command
functions append a record through `cmd_alloc` / `put_hdr`; that append is the seam. The record is handed to the back end at
a flush point (its payload is filled after the append returns), on the calling thread, using the record structs of
`gpu/api/rx_nvn.h` as the call format. The interface is `gpu/direct/direct_api.h` and `host/direct.h`.

NVN alone does not say everything: a job's list is anonymous. Each thing NVN does not carry comes from an engine hook:

| NVN lacks | Taken from |
|---|---|
| which pass a job belongs to | the recorder's pass key (`host/eng_jobs.c`) |
| the job's target formats | the pass table, filled where the layer lists set targets |
| what kind of list a recording is | the caller of BeginRecording |
| final deferred handles | the snapshot at the guest's submit |
| whether a texture's image exists | the loader and allocator hooks, at registration |
| report values | the GPU, written to guest memory |

When a command needs something NVN does not carry, hook the engine function that has it. Never infer it at submit.

### What an NVN command buffer becomes

The kind is decided at BeginRecording from the caller.

| NVN list | A frame | Becomes |
|---|---|---|
| render job (`f_80084de0`) | about 102 | runs of secondaries: a rendering run (inherits the pass's formats) or a work run (dispatch, barrier, copy), in order |
| layer list (9), gate primary | 10 | primaries, cut into chunks at cross-queue waits; owns begin/end rendering; executes job runs at its job-call sites |
| async-compute list (7), queue heads | 12 | primaries, one chunk each |
| load-time list (vertex, draw, shape, material) | 37,000 live, 835 calls a frame | a packet: a compact pre-resolved op array, expanded into the caller's command buffer where it is called |
| any list not classified | should be 0 | a packet, counted |

The packet expander is the only interpreter, and it runs on the calling thread at record time. A top-level `CallCommands`
(the gate calling the layer lists) is not nested: the submit takes the chunks in order.

### Passes

SetRenderTargets in a layer list notes the targets; `vkCmdBeginRendering` is recorded lazily at the first draw or job call,
so the clears that follow SetRenderTargets become load ops. A dispatch, copy or next SetRenderTargets ends it. Barriers come
from the NVN barrier bits. RADV does not expose `VK_KHR_unified_image_layouts`, so targets move between attachment and read
layouts at pass boundaries, driven by the pass's targets and the game's barriers.

### Jobs

A job records 0 to 4 ms before its pass sets targets, which are allocated inside the layer list; a rendering secondary
inherits formats only ([job-formats.md](direct/job-formats.md)).

- **Pass identity** is the recorder's pass key pointer (x3 of `f_80084de0`), a fixed address in the pass's owner. A view
  record is a slot handed out in arrival order and is not an identity.
- **Formats** come from a table keyed by that pointer, written at the job-call sites (`0x80668a9c`, `0x8028a3c4`).
- **Miss or mismatch:** the job is not recorded and the layer list draws the pass through the game's own inline path
  (`f_80681320`). Expected on a key's first frame and after a settings change.
- **A job is one rendering secondary.** It inherits nothing it reads except the task and mesh storage buffers of GPU-culled
  mesh draws ([jobs-state.md](direct/jobs-state.md)); a patch makes those jobs bind them. The waits and the dispatch
  recorded inside jobs move to the layer list by a patch at the job-call site (the water draw's wait only to `0x80668c84`:
  earlier deadlocks). The occlusion-grid dispatches kept from a job run before the job's commands, not after; the exact
  place needs the job cut in two and is not built.
- RADV has no dynamic-state inheritance, so each secondary sets all its state, and no inherited conditional rendering, so a
  condition is begun and ended inside it.

### Data a draw reads

| Data | How it reaches the GPU | CPU work per draw |
|---|---|---|
| uniform, storage, vertex, index buffers | guest address to window buffer and offset, or device address (layer 0, BDA) | arithmetic |
| textures and samplers bound by handle | descriptor-heap index = NVN pool id, written at registration | none |
| deferred handle arrays (about 924 handles a frame; the 30-slot system-texture table per view) | the bind reserves words in the frame arena and queues one copy from the game's array; all copies run once at the guest's submit; shaders read the handles from the arena | none |
| handles and storage addresses inside uniform buffers | read by the shader from the buffer | none |
| inline uniform updates (about 80 a frame) | the bytes go to the frame arena and later draws of the same list bind that copy | a range lookup in the list |
| GPU-made draw arguments | indirect draws; mesh arguments converted by one batched copy before the pass | none |

The deferred arrays cannot be read in place: the game keeps two copies and flips them each frame, and in Hyrule Field 97%
are rewritten one present after the submit that read them. About 4.5 words a frame change, all on six render-target slots
whose textures get a new id every frame. Inline updates assume no list reads a range another list updated in the same
frame: counted, and 0 so far.

### Programs

All programs are in the romfs. They are translated (the forked Eden recompiler in `gpu/direct/eden`) and their shader
objects kept in a cache; a table keyed by program serial holds the variants (in practice one per program) and is read
without locks. A program missing at load is compiled then, and the load waits. A program missing in play skips its draw, is
counted and is compiled in the background. What a translated program expects at a draw is in
[shader-contract.md](direct/shader-contract.md).

### Textures

- **Asset textures:** images made at registration from the prepared pack (`nx2/prepare_textures.py`), BCn in Vulkan layout.
  The loader hook skips the texel copy, so the guest pages stay unbacked. The game streams whole textures and never mip
  ranges; a load with no data is a UnifyTex alias; a released texture stays registered in an LRU cache until the game's
  2,400 MB heap is nearly full ([texture-streaming.md](direct/texture-streaming.md)). Asset textures' state comes from hooks
  on the game's own texture events, an image is freed when the game destroys its texture, and a patch makes the game's
  per-frame eviction look at free VRAM instead of its heap bytes. The back end holds no eviction policy.
- **Render targets:** an image per stable target slot (`host/rt_slots.c`), never uploaded. The game's transient targets
  (shadow maps and the rest of the allocator's pool) live in one constant device allocation made at boot, twice the game's pool
  (1 GB at the 2048x1152 and 2560x1440 outputs, 1.34 GB at 3840x2160); images are
  bound in it at their storage offsets, so no device memory is made or freed for them in play
  ([transient-pool.md](direct/transient-pool.md)). The allocator's hooks tell the back end each target by its own texture
  object (`hsd_target_given`).
- **Terrain tiles** are the other source of pre-swizzled texels (51,233 TARB files). A build-time converter deswizzles them
  ([converters.md](direct/converters.md)); one patch replaces the memcpy at `f_80c17040+0x544` with a GPU upload, because
  the game's own linear branch refuses compressed formats and uploads a different CPU copy
  ([terrain-texels.md](direct/terrain-texels.md)). A file that was not converted is recognised by its size and draws the
  placeholder.
- **CPU-written textures** that go through WriteTexels (primitive and procedural textures, the JPEG initializer, the terrain
  polish step) and movie frames arrive linear: a region copy into the image at the call. The font glyph cache is the
  exception: the CPU stores glyphs straight into the texture's memory after a GPU clear, with no call to hook, so a patch at
  the glyph writer hands the bytes over.
- The back end contains no block-linear code and no ASTC decoder; both end at the build-time tool. A texture with no
  prepared record (the dump's three damaged files) draws a 1x1 zero placeholder and is counted. There is no texture cache
  keyed by guest bytes and no page tracking for textures.
- The SMAA edge list is a texture allocated only for its memory and never bound: a patch gives it a host-owned buffer.

### Task and mesh shaders

The mesh stage is native. The task stage is a compute step per task workgroup that writes its mesh group count and payload
(they belong to the task workgroup, not the draw; a draw starts up to 4096 of them); the mesh stage is drawn with
`vkCmdDrawMeshTasksIndirectEXT` and no task stage and reads its payload from that buffer. Folding the task into the mesh
stage is not possible: one task workgroup feeds up to 32 meshlets, past the mesh output limits. There are six task shaders
in the scenes tested; they only read memory and emit a payload and group counts ([task-mesh.md](direct/task-mesh.md)).

The Hyrule Field and market device loss was the translated mesh stage `40d0c0768e874600` as RADV compiles it: 8 scratch
spills (258 vector registers wanted, 256 available), the one of 53 mesh stages in the three scenes' captures that spills; it
hangs once its fragments pass ([hang-cause.md](direct/hang-cause.md), [hang-fix.md](direct/hang-fix.md)). In the game:

- that stage is drawn by a hand-written one (`meshlet_lit.mesh -DPROBE_LIGHT`), swapped in by code key
  (`gpu/direct/direct_meshrepl.cpp`, `shaders/mesh/swap.txt`; `HOS_DBG=nomeshrepl` draws the translator's), bit-equal to the
  translated stage in all 28 output components on 10 of 10 captured draws;
- a framebuffer-local barrier before every task+mesh draw ended the market's losses (13 of 13 runs lost without it); it is
  off by default now (`HOS_DBG=tmwait` keeps it) after the market ran 240 s twice and 15 minutes without it;
- mesh stages go through a register-pressure pass at translation (`gpu/direct/direct_spv_late.cpp`; `HOS_DBG=nolate` turns it
  off, the program cache is stamped by it): a block past 160 held values is re-emitted with what only computes held back to
  that budget, kept only when the block and the function then hold fewer, and every new order is checked for soundness
  ([spv-late.md](direct/spv-late.md)). On vertex stages it is a trial, off (`HOS_DBG=latevertex`).

### Reports and conditions

Occlusion queries are real. `vkCmdCopyQueryPoolResults` writes into the report buffer at its guest address, as the console's
GPU does, so the game's CPU readers (`f_812342c0`, `f_80365f00`, `f_80362ff0`, `f_80357060`, the timestamps) read plain
memory in pools in host-cached memory. A render condition is conditional rendering on a predicate word that a small compute
step derives from the two counter words at the end of the producing pass. The occlusion-grid dispatches (`f_8067c310`) must run
before their job's draws: run after them, the corner sampler reads the finished depth, all 627 corners count as unchanged
(-1.0) and the grid compute writes its tiles negated; run before, Kokiri draws 684 meshes a frame.

### Submission

The guest submits on its Presentation Thread. By then every list is recorded Vulkan in chunks cut at SignalEvent and
WaitEvent. The submit orders the roughly 25 chunks of the guest's two queues so each wait follows its signal and sends them
in one `vkQueueSubmit2` on one Vulkan queue, made by a sender thread: the timeline values are given in order on the
submitting thread and the work is handed over, so the game's thread does not wait in the kernel's submit. Waits whose
signal comes from the CPU are held on the host. An NVN sync is the timeline value of the submission's last chunk. Each recording thread has a command pool and a bump arena per frame slot,
reset as a whole when its frame's timeline value has passed (`cmd_slot_take` sets aside a slot with a list alive).

## Engine replacements above NVN

| What | Why NVN cannot stay |
|---|---|
| texture loading (`f_80798690` and the compaction and Replace copies) | a resident CPU copy of every texel |
| render-target allocation (`rt_slots.c`) | new objects every frame |

## Game patches

The awkward things and the patch each has (the patch sites and their checks are in [patch-sites.md](direct/patch-sites.md)):

| Awkward for the back end | Patch |
|---|---|
| a job records before its pass's targets are known | the pass table, keyed by the recorder's pass key |
| waits and dispatches sit inside jobs | moved to the layer list that calls the job, so a job is one rendering secondary |
| jobs may inherit vertex state from their caller | those jobs set it themselves |
| mesh draw arguments in NVN's layout | the zeroing of each 16-byte argument record writes Vulkan's layout in place |
| a task shader may loop on guest data | that shader is replaced |
| pre-swizzled terrain tiles | changed romfs data, one upload patch |
| the font glyph writer | hands the bytes to the back end |
| the SMAA edge list | a host-owned buffer |
| the eviction at the heap's byte count | looks at free VRAM |

Patches live in `host/direct_patches.c` and the `host/direct_*.c` files, each stating the function, why it exists and the
check that shows it still applies (`HOS_DBG=logpatch`).

## Checks

- **Reference pictures per pass.** `HOS_DBG=dump:<present>` dumps the targets per pass; `scripts/direct_dump_compare.py` and
  `scripts/direct_picture_check.py` compare them against stored references.
- **Validation.** `HOS_DBG=validation` runs the Vulkan validation layer with synchronization validation; any message is a
  failure. `HOS_DBG=strict` ends the run at the first warning or error.
- **Counters.** Skipped draws, skipped jobs, unclassified lists, objects created a frame, cross-list uniform overlaps: all 0
  in steady play (the `direct:` and `vk:` log lines).
- **Hangs.** `HOS_DBG=marks` (GPU breadcrumbs for a lost device) and the kernel log. At most three device losses per boot;
  commit before a run that may hang; save the kernel log into the run folder after each loss.
- **Walks.** `scripts/direct_walk_check.sh`, `scripts/direct_scene_cycle.sh` and `scripts/direct_check.sh` run the scene
  routes; `scripts/smoke.sh` is the quick one.
- **Timing.** Frame cap off, idle machine, one game process: Presentation Thread ms, workers' ms, frame-time percentiles.
  Fps at the cap is not a result; timing taken while other agents run is not a result.

## Open problems

- **Pictures.** The per-pass picture checks fail on the current head; failing pass states Kokiri 8, Hyrule Field 28,
  market 15 (21 / 45 / 21 on the float-aware metric). Work them through, earliest pass first, and close the Kokiri
  check's spawn-spot blind spot.
- **Eviction destroys textures still drawn.** The eviction patch at the VRAM margin destroys a texture released in the same
  present (416 "texture with no image sampled" in one run); the patch must not, or the margin must be lowered until it does
  not fire (the margin, 1.5 GB, was sized before targets cost 0 of it).
- **The transient pool's size** (twice the game's pool) is measured on the HF market walk: peak held 534-565 MB at 2560x1440
  and 1,004 MB at 3840x2160, no target outside the slots; the full 26-stop scene cycle and a long soak under strict are not
  run; persistent targets of the same allocator are outside the slots.
- **A frame at each step up of the dynamic resolution** (`HOS_DYNRES=1`, off by default). The first frame presented at a
  larger 3D scale shows the previous frame's picture, at the old scale, in the top-left part of the screen; the rest is
  fog-white or dark blue. The next frame is right. Steps down show no such frame. It happens at every step up from 0.333 to
  0.75 at 2560x1440, with either slot range (1024 or 512 MB) and with no target outside the slots, so the slot range does not
  cause it. Open: which image the final upscale reads on that frame. Either the game's own one-frame lag (the new scale
  reaches the final pass a frame before the 3D is drawn at it), or our identity of a target made again at the larger size
  over the old one's storage, with the pass still reading the old image. To force the steps, make the GPU timestamps run
  fast and then slow (direct_query.cpp's resolve push: host0 and scale), and take shots from the present at which +0xe5c
  changes.
- **Memory accounting per owner** in the periodic log; the per-slot arenas, task pools and query pools never shrink.
- **The mesh-stage fix rests on few runs** (3 walks, 3 market runs against 5 and 13 losses before); eleven other mesh stages
  sit at the register limit without spilling; the four other stages in the capture list are not swapped in; the scan covers
  three scenes and no fragment stage. Why a spilling mesh stage hangs the GPU is inferred, not known. A report to Mesa is not
  written (the synthetic spilling stage is not drawn).
- **The render-target id patch** (`host/direct_rt_ids.c`, logged as "direct patch rt ids" every 600 presents): its premise,
  that the system-texture entry keeps its id while nothing changes, does not hold. The entry arrives with a stale id every
  frame, so the patch keeps 0 calls and passes every call to the game's path; the submit-time snapshot of the deferred
  tables covers it. Open: find what resets the id (a watch on the table's source entry).
- **Frame time.** The serial layer list is split by nothing yet (a patch in calc stage C of `moduleCalc_`); Hyrule Field
  has bursts. Render jobs and layer lists are recorded in the calc phase, not the draw phase; a frame has four job stages ([boot-to-main-loop.md](direct/boot-to-main-loop.md)).
- **Programs translated offline;** translator improvements and stage replacements as the shader cost census ranks them
  ([shader-costs.md](direct/shader-costs.md)): the heavy compute pass `fd029a317a9a4b94` first, then per-command overhead,
  then translator-wide costs; not stage-by-stage rewrites.
- **The dump's three damaged files** draw a placeholder and stop strict mode in the market; a neutral record written by the
  prepare tool is the fix, not built.
- **A per-draw trace compared offline** (program, vertex layout, buffer addresses, texture ids, fixed state per draw, lined
  up per pass): not built.
- **Movie frames** are copied three times; once would do.
- **Native task shaders** on the graphics queue are not used: RADV runs the task stage on an async-compute ring, gang-submitted
  with the graphics ring. The same native run on Mesa main, and a reading of an amdgpu devcoredump, would show whether the
  compute step could go.
