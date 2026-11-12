# The direct back end: plan (2026-11-12)

This replaces `work/assets/native/engine-cut-plan.md` and the E1-E9 steps. Those notes, the `e*-impl.md` files,
`fallbacks.md` and `docs/native-vk.md` are frozen: they stay as the record of what was measured and reverse-engineered,
and this plan cites them, but nothing here continues their steps.

Status of this document: accepted by the user 2026-11-12; M0's answers (`docs/direct/m0-*.md`) are folded in below,
M1 is being built (`docs/direct/m1-interface.md`). Items marked **(verify)** are assumptions taken from the frozen notes or
from a first reading of the code, not yet checked.

## Why a new back end

The engine cut was built inside the replay so each piece could be compared with it. Every piece therefore took the
replay's shape: secondaries inside passes the replay opens, programs the replay picked first, textures the replay's
draws made resident. The serial Presentation Thread still walks every record (about 11 ms a frame in Hyrule Field with
everything on), and the new path cannot run without the old one.

The decision (user, 2026-11-12): the record/replay renderer goes. A new back end is written beside it, the old one is
frozen and used only as an oracle, and it is deleted when the new one reaches parity.

## Goals and how each is measured

| Goal | Means | Measured by |
|---|---|---|
| No reliance on NVN's memory coupling beyond what the GPU can honour | pools are Vulkan memory at the guest address (layer 0); everything else a command needs is known when it is recorded, or is read by the GPU, or is snapshotted once at submit | 0 CPU reads of GPU results in the back end; 0 submit-time resolves except the handle snapshot |
| No memory or Vulkan-object churn per frame | per-thread command pools and bump arenas per frame slot, reset when the frame's timeline value has passed; images, views, heap entries and shader objects made at load | 0 object creations a frame in steady play (counter) |
| No resident copies between CPU and GPU | prepared assets go from the pack to their image; guest texel pages are never written | pool 127 resident pages about 0; guest memory about 7.4 GB (asset-path.md) |
| Render work spread over the game's threads | each NVN command-buffer call emits Vulkan on the thread that makes it | Presentation Thread under 0.5 ms a frame; the workers' added time below the old replay's total |
| A smooth 60 fps everywhere | all of the above; programs built and textures in VRAM before they are drawn | 99th-percentile frame time under 16.7 ms on every scene of the cycle, area changes included |

Target (user, 2026-11-12): this PC only, the RX 6950 XT on RADV, with every extension Mesa offers; no other vendor or
platform is considered. Texture-replacing mods are ignored. The menu may be changed or rewritten.

## Rules

1. **One path.** One boot-time selector chooses the frozen back end or the direct one. No per-feature switches inside
   the direct back end; a debug aid is a build option or one `HOS_DBG=<list>`.
2. **No fallback.** Every miss has a defined behaviour: wait (load time only), or skip and count (play). A strict mode
   stops at the first one. Steady play must count zero.
3. **Known when recorded.** If a command needs data that is not final when it is recorded, the data moves: to load
   time, to a GPU read, or to one flat copy at submit. Never to a per-draw step at submit.
4. **Prepare offline.** The romfs can be changed. A problem that a build-time tool can remove is not solved at run time.
5. **Cut at the NVN call.** Engine code above NVN is replaced only where NVN's semantics force a CPU readback, a copy
   or a serial order. The list is in "Engine replacements" below and is closed until a measurement adds to it.
6. **Checks need no second renderer in the process.** Pictures against stored references, the validation layers,
   counters, GPU asserts.
7. **Slices are whole frames.** Each milestone shows a complete frame of reduced scope on screen.
8. **Patch the game, not the back end** (user, 2026-11-12). The back end implements what Vulkan needs, plainly. When
   the game does something awkward for it, the fix is a patch of the game: a hook or splice in the lifted code, a
   replaced shader, or changed romfs data. The back end gets no special case for one pass, one program or one call
   site. Only this one game build is targeted, so a patch may name addresses.

## Architecture

### The seam

`host/nvn_native.c` keeps its object layer (device, pools, buffers, textures, samplers, syncs, events). Its command
functions all append a record through `cmd_alloc` / `put_hdr` (`host/nvn_native.c:1601`). That append is the seam: in
direct mode the record is handed to the back end at once, on the calling thread, as `emit(list, op, payload)`, using the
record structs of `gpu/render/rx_nvn.h` as the call format. The host's 3,765 lines are not rewritten. Confirmed
(m0-seam.md): `cmd_alloc` is the one write path. The record is handed over at a flush point, not at the append (its
payload is filled after the append returns), and direct mode rides on the host's lists-by-reference handle table.
The exact interface is `docs/direct/m1-interface.md`.

The NVN call alone is not enough, as the frozen notes found: at that level a job's list is anonymous. The seam is the
NVN call for the commands, plus an engine hook for each thing NVN does not carry:

| NVN lacks | Taken from |
|---|---|
| which pass a job belongs to | the view record at the job recorder's BeginRecording (`host/eng_jobs.c` reads it today) |
| the job's target formats | the pass table, filled where the layer lists set targets (the weak point: see Jobs) |
| what kind of list a recording is | the caller of BeginRecording |
| final deferred handles | the snapshot at the guest's submit |
| whether a texture's image exists | the loader and allocator hooks, at registration |
| report values | the GPU, written to guest memory |

Rule: when a command needs something NVN does not carry, hook the engine function that has it. Never infer it at
submit. If M0 finds a datum no hook can supply in time, the cut moves up at that place and only there.

### What an NVN command buffer becomes

The kind is decided at BeginRecording from the caller (the return addresses are in engine-frame.md section 1).

| NVN list | A frame | Becomes |
|---|---|---|
| render job (`f_80084de0`) | about 102 | runs of secondaries: a rendering run (inherits the pass's formats) or a work run (dispatch, barrier, copy), in order |
| layer list (9), gate primary | 10 | primaries, cut into chunks at cross-queue waits; owns begin/end rendering; executes job runs at its job-call sites |
| async-compute list (7), queue heads | 12 | primaries, one chunk each |
| load-time list (vertex, draw, shape, material) | 37,000 live, 835 calls a frame | a packet: a compact pre-resolved op array, expanded into the caller's command buffer where it is called |
| any list not classified | should be 0 | a packet, counted |

The packet expander is the only interpreter left, and it runs on the calling thread at record time.

A top-level `CallCommands` (the gate calling the 9 layer lists) is not nested: the submit takes the chunks in order.

### Passes

SetRenderTargets in a layer list notes the targets; `vkCmdBeginRendering` is recorded lazily at the first draw or job
call, so the clears that follow SetRenderTargets become load ops. A dispatch, copy or next SetRenderTargets ends it.
Barriers come from the NVN barrier bits. Image layouts need an explicit policy: this driver does not expose
`VK_KHR_unified_image_layouts`, so targets are moved between attachment and read layouts at pass boundaries, driven by
the pass's targets and the game's barriers. **(verify:** that the game's 141 barriers a frame cover every
attachment-to-sampled change; the frozen S6 census is the check.**)**

### Jobs

A job records 0 to 4 ms before its pass sets targets, and the targets are allocated inside the layer list, so they
cannot be known earlier. A rendering secondary inherits formats only (m0-job-formats.md).

- **Pass identity** is the recorder's pass key pointer (x3 of `f_80084de0`), a fixed address in the pass's owner, which
  the two job-call sites read too. A view record is a slot handed out in arrival order and is not an identity.
- **Formats** come from a table keyed by that pointer, written at the job-call sites (`0x80668a9c`, `0x8028a3c4`) from
  the targets just set.
- **Miss or mismatch:** the job is not recorded (the recorder takes its own early return) and the layer list draws the
  pass through the game's own inline path (`f_80681320`), after its own SetRenderTargets. No job is skipped and none
  is drawn with wrong formats. Expected on a key's first frame (about 23 keys a scene) and after a settings change.
  **(verify by a run:** that the inline path draws a complete pass, and that keys and their formats are stable over
  the scene cycle.**)**
- **A job is one rendering secondary.** It inherits nothing it reads except the task and mesh storage buffers of
  GPU-culled mesh draws (m0-jobs-state.md); a patch makes those jobs bind them. The three waits and the one dispatch
  recorded inside jobs move to the layer list by a patch at the job-call site (the water draw's wait only to
  `0x80668c84`: earlier deadlocks). Both patches are inferred from static analysis and need a confirming log.
- RADV has no dynamic-state inheritance, so each secondary sets all its state; and no inherited conditional
  rendering, so a condition is begun and ended inside it. Inherited queries are available.

### Data a draw reads

| Data | How it reaches the GPU | CPU work per draw |
|---|---|---|
| uniform, storage, vertex, index buffers | guest address to window buffer and offset, or device address (layer 0, BDA) | arithmetic |
| textures and samplers bound by handle | descriptor-heap index = NVN pool id, written at registration | none |
| deferred handle arrays (about 924 handles a frame; the 30-slot system-texture table per view) | the bind reserves words in the frame arena and queues one copy from the game's array; all copies run once at the guest's submit; shaders read the handles from the arena | none |
| handles and storage addresses inside uniform buffers | read by the shader from the buffer | none |
| inline uniform updates (about 80 a frame) | the bytes go to the frame arena and later draws of the same list bind that copy | a range lookup in the list |
| GPU-made draw arguments | indirect draws; mesh arguments converted by one batched copy before the pass | none |

The deferred arrays cannot be read in place: the game keeps two copies and flips them each frame, and in Hyrule Field
97% are rewritten one present after the submit that read them (m0-gpu-experiments.md). About 4.5 words a frame
change, all on six render-target slots whose textures get a new id every frame; target parking keeps their memory but
not their ids. Preferred fix, a patch: parked targets keep their id, which makes the tables constant from frame to
frame and also ends a per-frame rewrite of descriptor-heap entries. **(open:** where the ids are handed out.**)**
Until that patch exists, the snapshot at submit stands: a few kilobytes.

Mesh draw arguments need no conversion copy: the game zeroes each 16-byte argument record every frame and the culling
shaders write only its count words, so a patch of that zeroing writes Vulkan's layout in place (m0-task-mesh.md; the
writer of five other indirect addresses is still to be identified).
Inline updates assume no list reads a range another list updated in the same frame: counted, and 0 in the frozen
measurement (e6-impl.md section 6).

The translator changes this needs (handles read from memory, storage arrays by address, every stage by address) are
made together, so the shader caches are invalidated once.

### Programs

All programs are in the romfs. They are translated offline and their shader objects are kept in the cache; a table
keyed by program serial holds the variants (in practice one per program: e7-design.md) and is read without locks.
A program missing at load is compiled then, and the load waits. A program missing in play skips its draw, is counted
and is compiled in the background.

### Textures

- **Asset textures:** images made at registration from the prepared pack (`nx2/prepare_textures.py`, done on
  `agent/assets1`), BCn in Vulkan layout. The loader hook skips the texel copy, so the guest pages stay unbacked.
  Residency follows the game's registrations and its own streaming; there is no eviction in the first version. VRAM use
  on Hyrule Field is a gate for M2.
- **Render targets:** an image per stable target slot (`host/rt_slots.c`), made at allocation, never uploaded.
- **Terrain tiles** are the one other source of pre-swizzled texels (51,233 TARB files, 25.5 GB of texels). A
  build-time converter, a sibling of the texture tool, deswizzles them. The game's own linear branch cannot be used
  (it refuses compressed formats, 90% of tiles, and uploads a different CPU copy: m0-terrain-texels.md), so one patch
  turns the single memcpy at `f_80c17040+0x544` into a host copy of the converted record into the image. A file that
  was not converted is recognised by its size and draws the placeholder.
- **CPU-written textures** that go through WriteTexels (primitive and procedural textures, the JPEG initializer, the
  terrain polish step) and movie frames arrive linear: a region copy into the image at the call.
- **The font glyph cache** is the exception: the CPU stores glyphs straight into the texture's memory after a GPU
  clear, with no call to hook. It needs a patch at the glyph writer. **(open:** the writer and its layout, from one
  logged run.**)**
- **The direct back end contains no block-linear code and no ASTC decoder.** Both formats end at the build-time
  tool. A texture with no prepared record (the 3 damaged dump files until they are re-dumped) draws a placeholder and
  is counted; it is never converted at run time. Besides the CPU and memory, this removes the GPU's decode work and
  the larger decoded formats from VRAM.
- There is no texture cache keyed by guest bytes and no page tracking for textures.
- The SMAA edge list is a texture allocated only for its memory and never bound: a patch gives it a host-owned
  buffer. The one stray buffer write into texture storage seen before is most likely the same buffer.

### Task and mesh shaders

They must work natively (user, 2026-11-12); the compute emulation of both stages is not carried over.

What is recorded (docs/native-vk.md, "The RDNA2 hang with faster timing"): task shaders can be drawn on the graphics
queue, but RADV runs the task stage on an async-compute ring, gang-submitted with the graphics ring. The device losses
named that compute ring (`comp_1.x`), at the map load, only under faster timing. Mesh-only programs (29 of 63 in
Hyrule Field) ran natively throughout. The cause was never found: a gang-submit ordering fault in the driver, or a task
shader looping on data it read too early from the replay's stream. A second hang (one task shader's depth-grid walk,
graphics ring) is worked around by a switch and equally undiagnosed.

**Measured 2026-11-12 on the frozen back end** (m0-gpu-experiments.md): native task shaders still lose the device.
Hyrule Field 6 of 6 runs at the map load, at default timing too; Kokiri 0 of 6 with one faster-timing setting, 1 of 1
with all three. The kernel names the graphics ring, an async-compute ring, or both, and the compute ring's reset
fails. `RADV_DEBUG=syncshaders` did not help (2 of 2 lost). The compute emulation lost 0 of 6. After 13 losses in one
boot the GPU no longer came back and the PC had to be restarted.

**What the task stages are** (m0-task-mesh.md): six task shaders in these scenes; they only read memory and emit a
payload and mesh group counts. Two loops are bounded by data read from memory (an item count with an unsigned
compare, 2^27 trips when it reads 0; a grid width and height). Clamping both did not stop the losses (3 of 3 lost),
so they are not the cause, or not alone.

**Decision.** The mesh stage stays native. The task stage moves off the async ring: a compute step per task
workgroup writes its mesh group count and payload (count and payload belong to the task workgroup, not the draw; a
draw starts up to 4096 of them), and the mesh stage, drawn with `vkCmdDrawMeshTasksIndirectEXT` and no task stage,
reads its payload from that buffer. Folding the task into the mesh stage is not possible (one task workgroup feeds up
to 32 meshlets, past the mesh output limits). This is untested as a whole: its task half ran as compute in the old
emulation without a loss, and mesh-only programs ran natively without a loss.

**The dump of one loss** (m0-hang-dump.md, 2026-11-12) changes the picture: the graphics ring is the one stuck, in
our own command buffer, with draws in the 3D pipe that never finish. The compute side had finished and was idle; no
task shader was running and no gang semaphore was being waited on. So the hang is most likely a native mesh draw of a
task+mesh program: either its mesh stage or data (outputs past the declared limits, a loop bounded by the payload) or
a firmware mismatch between the task and mesh halves. Which of the two is not known, and the dump cannot tell (it
holds no wave state and the user command buffers read back as zeros). The decision above stands as provisional: the
compute step hands the mesh stage the same payload the old emulation produced without a loss, and removes the
task-to-mesh hand-over. Before M4 builds it, one run with the frozen back end's draw markers (`RX_VK_CHECKPOINTS=1`)
names the program whose draw does not finish; that costs one device loss.

**Not done, and worth doing once under the hang rules below:** the same native run on Mesa main, and a reading of the
devcoredump (it needs root). If either shows a driver fault with a fix, native task shaders come back and the compute
step goes.

**Hang rules** (after the restart): commit everything first; at most three device losses per boot; no other agent
holding uncommitted work; the kernel log saved into the run folder after each loss.

### Reports and conditions

Occlusion queries are real. `vkCmdCopyQueryPoolResults` writes into the report buffer at its guest address, which is
what the console's GPU does, so the game's CPU readers (`f_812342c0`, `f_80365f00`, `f_80362ff0`, `f_80357060`, the
timestamps) read plain memory in pools placed in host-cached memory. A render condition is conditional rendering on a
predicate word that a small compute step derives from the two counter words at the end of the producing pass
(fallbacks.md section 6).

### Submission

The guest submits on its Presentation Thread. By then every list is recorded Vulkan in chunks cut at SignalEvent and
WaitEvent. The submit orders the roughly 25 chunks of the guest's two queues so each wait follows its signal (the graph
is acyclic: engine-frame.md section 5) and sends them in one `vkQueueSubmit2` on **one** Vulkan queue. Waits whose
signal comes from the CPU are held on the host, as today (timelines-impl.md). An NVN sync is the timeline value of the
submission's last chunk.

A second Vulkan queue for async compute is a GPU experiment for later. It buys no CPU time and it uses the ring the
kernel blamed for the RDNA2 hangs.

### Frame resources

Each recording thread has a command pool and a bump arena per frame slot. A slot is reset as a whole when its frame's
timeline value has passed. There is no per-job retirement and no wrapping ring.

## What is taken from the frozen tree

Copied into the new directory, not shared, so the frozen back end is not disturbed and the source-reading tests in
`tests/` keep passing against it. **(verify each for how cleanly it separates; M0.)**

| Taken | Left behind |
|---|---|
| the host: lifted code, HOS services, hooks, `nvn_native.c` object layer, `rt_slots.c` | `vk_replay.cpp`, `vk_draw.cpp`, the `eng_*` files |
| Eden's shader recompiler, shader objects, their cache and prefetch | `vk_texture.cpp`'s cache, uploads and decoders |
| layer 0 pools, the descriptor heap, BDA, timelines | the `RX_VK_*` and `HOS_ENG_*` switches |
| device, swapchain and present | the menu (rewritten small when needed), upscalers and frame generation |

## Engine replacements above NVN

| What | Why NVN cannot stay | State |
|---|---|---|
| texture loading (`f_80798690` and the compaction and Replace copies) | a resident CPU copy of every texel | hooks designed and traced (asset-path.md) |
| render-target allocation | new objects every frame | done (`rt_slots.c`) |
| nothing else | | |

## Game patches

Rule 8 changes several places where the sections above still describe machinery in the back end. Each is a candidate
to settle in M0; where the patch is feasible, the back-end mechanism named in the middle column is not built.

| Awkward for the back end | Back-end mechanism it would need | Patch instead |
|---|---|---|
| a job records before its pass's targets are known | the pass table as a guess, skipped jobs on a mismatch | give the job its formats: patch the pass or the job kick so the targets are chosen before the jobs record, or find them from the job group (engine-frame.md section 3) |
| 3 waits and 2 dispatches a frame sit inside jobs | jobs as mixed runs of rendering and work | move them to the layer list that calls the job, so a job is one rendering secondary |
| jobs may inherit vertex state from their caller | state handed down at the job-call site | make those jobs set it themselves |
| GPU culling writes mesh draw arguments in NVN's layout | a batched conversion copy before the pass | replace the culling compute shader so it writes Vulkan's layout |
| a task shader may loop on guest data | a clamp added in translation | replace that shader |
| the system-texture table (30 engine textures per view; only this frame's transient targets change, because they get new ids each frame: engine-materials.md) is final after the jobs bind it | the handle snapshot at submit | first check whether target parking (`rt_slots.c`) already keeps those ids, and so the table, constant from frame to frame; if so the bind is read when recorded and nothing else is needed. Otherwise patch its only writer (`f_80667d20`). The tables are kept per buffer index, so reading them in place may also be safe **(verify)** |
| inline uniform updates in the command stream | arena copies and a range lookup per list | patch the roughly 80 call sites a frame to write the buffer directly, if their order allows |
| render conditions compare two counter words | a compute step per producing pass | patch the gating sites to test one predicate word |
| attachment-to-sampled changes without a barrier, if any | a hazard tracker | add the barrier in the pass that lacks it |
| pre-swizzled terrain tiles | a run-time deswizzle | changed romfs data (already the plan) |

Patches live in one place, one file per patch, each stating the function, why it exists and the check that shows it
still applies. A patch that needs a reverse-engineering answer first is listed in M0 with that question.

Later, by measurement only: the occlusion and culling readers as GPU-native designs (readback2.md), and the job
binder as load-time draw records with a view and a root pointer (the frozen plan's root/view proposal), which would
make job command buffers nearly frame-invariant.

## Checks

- **Reference pictures per pass.** The frozen back end dumps its targets per pass on the standing scenes
  (`RX_VK_DUMP`); the direct back end dumps the same passes, named by the pass driver and order. They are compared
  offline with the run-to-run noise of the frozen back end as the tolerance.
- **Determinism.** One task in M0: find why a scripted route on the frame clock still drifts. If it can be removed,
  the comparison is exact and routes can be tested, which is where the last picture bug hid.
- **Validation.** The direct back end runs clean under the Vulkan validation layer with synchronization validation
  in a dedicated smoke run, from M1 on. Any message is a failure.
- **Counters.** Skipped draws, skipped jobs, unclassified lists, objects created a frame, cross-list uniform overlaps:
  all 0 in steady play. Strict mode stops at the first.
- **GPU asserts** behind `HOS_DBG`: converted mesh arguments, event values at waits, out-of-bounds address reads.
- **Hangs.** A script that saves the devcoredump and the kernel log at a device loss; every hang is recorded.
- **Timing.** Frame cap off, idle machine, one game process: Presentation Thread ms, workers' ms, frame-time
  percentiles. Fps at the cap is not a result.

## Milestones

Sizes are relative (S, M, L); I have no basis for calendar estimates.

| # | Slice | Contents | Done when | Size |
|---|---|---|---|---|
| M0 | decisions | the seam and list kinds; what a job inherits; how the taken parts separate; the task-shader diagnosis (step 1 of "Task and mesh shaders"); device checks (heap index read from memory, sampler-heap size against NVN sampler ids); the drift | each **(verify)** and open item above has an answer written here | S |
| M1 | boot with no picture | new DLL and selector; device, swapchain, pools, syncs and events, the submit; every list recorded as counted no-ops | the game runs to Kokiri with a cleared window and correct syncs; this gives the game's own frame time with a free renderer, the ceiling for everything after | M |
| A | prepared assets (beside M1) | pack reader, images at registration, the loader hook; the terrain-tile conversion; then texel pages unbacked | every registered asset texture has its image before first use; pool 127 not resident; 0 ASTC and 0 block-linear bytes reach the back end (counted at the host's texture calls) | M |
| M2 | the UI frame | passes, clears, direct draws, programs table, textures by heap index; UI layers and the final composite | title screen and menus match the reference; validation clean | M |
| M3 | the opaque scene | jobs as runs, packets, the pass table, the handle snapshot, inline uniform updates, translator changes | Kokiri and Hyrule Field opaque passes match per pass; mesh draws and compute still skipped and counted | L |
| M4 | the whole frame | compute lists, chunks and ordering, copies, indirect and mesh draws as M0 decided | all passes match with conditions forced on; 0 skips | L |
| M5 | queries and conditions | real queries, results in guest memory, conditional rendering | the CPU readers see real values; no pops on turns | M |
| M6 | parity | scene cycle, soak, hang reproducer, timing; then the frozen renderer, its switches and its tests are deleted | goals table met; one renderer in the tree | M |

Order: M0 first and alone. M1 and A in parallel (A needs no game slot for most of it). M2 needs A's textures, M3
needs its terrain tiles. Then in sequence.

## How the work is run

- One integration branch and one owner of this document. Delegated work is a leaf task with its interface written
  here first.
- Plans and findings live in `docs/` and are committed. Nothing new is written under `work/assets/native/`.
- A milestone's status is a line in the table above, with the run that showed it.
- One game process at a time; no background job on the machine during a timing run.

## Open questions for the user

1. Where the code lives and what it is called (proposal: `gpu/direct/`, selected by `RX_BACKEND=direct`).
2. Whether a locally built or patched Mesa is acceptable as the driver the game runs on, if the task-shader fault is
   the driver's.
3. Whether upscalers and frame generation are wanted at all on this card at a native 60 fps.

Answered 2026-11-12: the menu can be changed; texture mods are ignored; the only target is this card on RADV.
