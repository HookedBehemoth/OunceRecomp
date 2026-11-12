# M3: the opaque scene (structure and stages, 2026-11-12)

M2 landed (3a5f8cd; `m2a-report.md`, `a-rt-report.md`, `m2b-report.md`). The UI and the final composite are drawn
and sit where the frozen back end puts them; the scene behind them is black because jobs are not executed yet. Asset
textures come from the prepared store, the game's texture pool is unbacked, uploads never run on the Presentation
Thread. With pacing off a frame takes 10.7 ms in Kokiri and 11.6 ms in Hyrule Field (9.2 / 9.4 with nothing drawn),
1.9 / 2.9 ms of it inside `hsd_emit` summed over the recording threads.

M3's goal (plan, Milestones): the opaque passes of Kokiri and Hyrule Field match the frozen back end per pass. Mesh
draws and compute are still skipped and counted (M4); render conditions are not evaluated: a gated draw is drawn
(M5 makes them real).

Two stages run in parallel on separate modules.

## M3a: jobs, packets, and the game patches (lists, passes, host)

- **A job is one rendering secondary** (plan, Jobs; `m0-job-formats.md`, `m3-patch-sites.md` 1-2): begun with the
  formats of its pass, executed by the layer list at its job-call site inside the rendering the layer list opened
  (secondary and inline commands mixed in one rendering).
- **The format table** is keyed by the pass identity as `m3-patch-sites.md` 1 corrects it (the recorder's key
  pointer; through the view record at the `0x8028a3c4` site). The back end fills it when a layer list calls a job
  while its pass is known. The recorder's entry hook asks the back end whether the key is known: if not, the game's
  recorder takes its own early return and the layer list draws that pass through the game's inline path, which the
  back end records as ordinary layer-list commands. A job whose formats differ from the pass at its call is not
  executed, is counted, and its key is forgotten (measured: never).
- **Load-time lists** (vertex, draw, shape, material lists called from jobs and layers) are packets: a compact,
  pre-resolved op array made once when the list ends, expanded into the caller's command buffer and state at the
  call, on the calling thread.
- **Game patches** (each its own host file, active in direct mode only, each with a log line that shows it applies):
  the waits, barriers and dispatches recorded inside jobs moved to the job-call site (`m3-patch-sites.md` 4); the
  barrier around the two depth copies (`m2a-report.md`); the render-target id kept when texture and view are
  unchanged (`m3-patch-sites.md` 5), after which the deferred handle tables are checked for how many words still
  change a frame.
- **Deferred binds:** the bank copy at the guest's submit stays (it exists since M2b).

Done when, in Kokiri and Hyrule Field: jobs executed a frame against jobs recorded (all, after the first frames);
passes drawn inline a frame (0 in steady play) and the keys that went inline; jobs not executed for a format mismatch
(0); the patches' counters; `scripts/direct_dump_compare.py` on the opaque colour and depth passes against the
frozen back end, per pass, within the frozen back end's own noise wherever the pass has no mesh draw, compute input
or handle-program texture (those are listed); the final composite shows the scene; validation: every message
reported, the three depth-copy hazards gone; frame time with pacing off, time in `hsd_emit` by list kind.

## M3b: the translator interface and the draw path's cost (shaders, heap, draw)

- **The contract's step B** (`m2-shader-contract.md` 6): Eden forked into `gpu/direct/eden` (the frozen objects are
  no longer linked), every stage reads its buffers by address, texture handles are read by the shader (handle
  programs and handles inside uniform buffers draw), no CPU-built pointer tables, translation without a bound state
  where the contract allows. One invalidation of the direct back end's program cache.
- **Storage images** bound by draws: heap entries as textures have them.
- **The draw path's cost:** profile `hsd_emit` (the cheap sampler or Superluminal: docs/native-vk.md "Measure"), name
  the top costs, and remove what is not arithmetic: per-draw copies of constant banks, lookups under a lock,
  redundant dynamic state. Target: the draw path under half of today's 1.10 / 1.85 ms a frame.
- **The heap entry written before a prepared image is linked** (three title textures sample the placeholder:
  `m2b-report.md`): fixed by ordering, not by a retry at the draw.

Done when: the skip table has no "handle program", "not by address" or "storage image" rows left; the translator
self-check (the frozen and the forked translator give the same picture on the UI passes; any intended difference
named); cold-cache start time and warm start time; the draw path's ms a frame before and after with the profile's top
entries; validation unchanged; the three title textures drawn.

## Rules (as M2)

No edits under `gpu/vk`, `gpu/render`, `tests/`. No `getenv` in `gpu/direct` but `HOS_DBG`. No fallback, no special
case for one pass, program or site in the back end: the game is patched instead. Nothing a recording thread touches
is shared without a lock, and no global lock is taken per draw. Zero Vulkan objects created a frame in steady play.
With `RX_BACKEND` unset the frozen back end and the host behave as before.
