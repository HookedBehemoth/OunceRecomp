# M0: what a render job inherits, and the work recorded inside jobs (2026-11-12)

Questions: (A) which state do a job's draws read that the job's own records do not set, who sets it, and can a game patch
make the job self-contained; (B) what are the three WaitEvents and the two dispatches recorded inside jobs, and can a patch
move them to the layer list that calls the job. Static analysis plus existing traces and logs. Nothing was run.
Addresses are guest VAs. Scripts: `work/proto/direct-m0/s*.py`, which walk the flattened submit stream of
`ct2_streams.bin` (Kokiri, frames 2200/2205/2210) and the call trace `ct2.bin`. A job's region in the stream starts at its
prologue (`SetScissor` `0x8008556c`, `SetViewport`, `SetDepthRange`, then default state). That is one scissor per job:
102 a frame, `engine-jobs/stage_1802.txt`. Decompiled functions: `work/proto/direct-m0/dec1/`.

## A. Job inheritance

**A1. Vertex state: ordinary draws inherit none.** CONFIRMED.
- In all three frames, every non-mesh job draw has `BindVertexAttribState`, `BindVertexStreamState` and `BindVertexBuffer`
  recorded inside its own job (`s5.py`, `s6.py`). The 24 to 25 draws a frame that do not are all in layer lists: fullscreen
  `DrawArrays` and post-process draws.
- Every stream that an enabled attribute names also has its buffer bound in the job. The only exception is 20
  layer-list draws.
- The vertex binds come from the load-time shape lists (`CallCommands` at `0x80093ca4`, `0x80087618`, `0x8008d978`) and
  from `f_802a0390` / `f_812902e0`. Drawers such as `f_80435bc0` issue draws with no vertex bind of their own and use the
  state the shape list just set in the same job (`seq_frame.txt` 4023-4053).
- The E3 check compares, for each job draw, the vertex attributes its program loads and their streams and buffers,
  against the replay, which carries inherited state. It found 0 vertex mismatches in e3_chk3_k, e3_chk_hf, e3_d4_*,
  e6_chk_hf, e6r_chk_hf and e6r_chk2_hf (`grep MISMATCH work/runs/*/log.txt`).
- The e6-impl.md §6 finding (vb1 = 0x104290c000 in the replay, 0 in the job) is real but harmless: no attribute of those
  draws names stream 1. In Kokiri the leftover streams 1-6 are a layer list's 7-buffer bind, and every attribute uses
  stream 0 (`s4.py`).
- The ~98 refusals under `HOS_ENG_JOBS_VTX_CHECK=2` are attributes that the job's own `BindVertexAttribState` disables
  (fmt 0, e.g. entries 3-7 of `0x22,0x1c69,0x2069,0,...`), or that lie past its count. INFERRED: the check above sees the
  same value on both sides. To confirm, log nvattr and fmt at each refusal.
- Mesh draws do inherit the attribute layout, and that is harmless because they read none (CONFIRMED, commit 0d931cf).

**A2. Fixed-function state: inherited, but always at the defaults.** CONFIRMED for Kokiri; HF not walked.
- The job re-sets its viewport, scissor, depth range, line width, point size, colour state, channel mask, depth-stencil,
  polygon state, depth clamp, blend colour and program (prologue `0x8008556c..0x80089b98`, and per item).
- Four things are not re-set: multisample state, stencil masks and reference, rasterizer discard and primitive restart.
  The jobs inherit them at the following values (`s8.py`):

| inherited state | value at the inheriting job draws |
|---|---|
| multisample | disabled, all zero |
| stencil | masks 0xff, reference 0 |
| rasterizer discard | 0 |
| primitive restart | 0 |
| effective render enable | true (last of `SetRenderEnable` / `SetRenderEnableConditional`; `s9.py`) |

- Blend target state is inherited only for targets whose blending is disabled. Every target with blend enabled has its
  blend state set inside the job (`s9.py`).
- E3, which starts every job from `NvnState{}` (`eng_job.cpp:1650`), matched the replay in every one of these groups in
  every check run above.

**A3. The one inherited state that is read: task/mesh storage buffers of GPU-culled mesh draws.** CONFIRMED.
- Every E3 check mismatch other than render enable and stale pass formats is "storage buffers" on `NOP_DRAW_MESH`
  (op 47), in views 0x11371c10c0 (the first 1536x864 colour pass, the one with the culling waits) and 0x113715cf80
  (logged mismatch lines: e3_d4_k 18, e3_d4_hf 15, e6_chk_hf 20, e6r_chk_hf 18 + 2; logging is budget-limited).
- e6r_chk2_hf: about 7 draws a frame.
- The slots involved are task stage 6 slots 0 and 1 and mesh stage 7 slot 1 (`s14.py`-`s16.py`). They are bound by
  three kinds of code:

| who binds them | where | relation to the job |
|---|---|---|
| `f_82c02bc0` / `f_82c024f0` (culled-instance drawers, via `f_8008f8e0`) | names 4 and 5 through `f_801a2d70` (`0x801a2f44`) and `f_801a6a40`, from `[param_1+0x18]+0x338` +0x1b0 / +0xd70 | an earlier job of the same pass, or earlier in the same job |
| system binder `f_80101ec0` <- `f_8027c9e0` | `f_801060f0` `0x80106338` (stage 6), `0x80102450` / `0x80102470` (stage 7); 0x113cb2e000 / 0x113b796000 for every stage | the layer list (`f_8007ad40`), 2-3 passes earlier; 4 calls a frame inside jobs |
| `f_801a60f0`, `f_801a6410`, `f_801a6720` | stage 7 slot 1 | earlier job of the pass |

- The drawers that bind neither task slot are `f_801a43e0`, `f_801a46a0`, `f_801a60f0`, `f_801a6410` and `f_801a6720`.
  Each binds only the resource names its own code lists (e.g. `f_801a6720`: 9, 3, 10, 11, 7). `f_82c02bc0` binds
  9, 11, 4, 5, 7.
- So job k's mesh draws read whatever job k-1 (or the layer list) left in those slots. That depends on submit order. On
  NVN it works because `CallCommands` keeps the GPU state.
- Stage 6/7 slots 2 and 3 (0x108c6d6540, 0x11371e7f00) are also only bound by the system binder. Whether they are read is
  not separated from slots 0/1 in the check. INFERRED: they are, by the same programs.

**A4. Patch: make the mesh drawers bind what they read** (INFERRED; it needs the confirmation below).
1. At the job prologue, right after `SetDepthRange` (`0x800855ac` in `f_80084de0`), call `f_8027c9e0`'s system bind for
   stages 6 and 7. The game already does this inside 4 jobs a frame. It is idempotent, and it removes the dependence on
   the layer list.
2. In `f_801a43e0`, `f_801a46a0`, `f_801a60f0`, `f_801a6410` and `f_801a6720`, add the two binds `f_82c02bc0` makes:
   `f_801a2d70(ctx, item, 4, o+0x1b0, 0)` and `f_801a6a40(ctx, item, 5, o+0xd70, 0)`, with `o = [[param_1+0x18]+0x338]`.
   Every drawer has that object (`f_801a6720` loads it as `lVar19`). Both binders return without binding when the
   program's name table has no slot for the name (`f_801a2d70`: all four table bytes 0xff).

Together these remove the dependence on the previous job. The result differs from the game only where a draw reads the
system buffer in a slot that an earlier `f_82c02bc0` overwrote, which depends on item order. **To confirm:** for each
op-47 mismatch, log the drawer (the LR of `DrawMeshTasksIndirect`) and the program's name-table bytes for names 0-15. That
shows which name each read slot is.

**Fallback if the patch is refused:** the job-call site must hand job k the stage 6/7 storage slots 0-3 as they stand after
jobs 0..k-1 of its pass. This can be known only at the call site, after all six jobs have recorded (E3's walk knows each
job's last binds). Under BDA=2 these are driver-bank words, so the secondary would have to read them from a per-call
buffer rather than having them baked in.

## B. Work recorded inside jobs (all in Kokiri frame 2205's first colour pass unless noted; `s17.py`, `s18.py`)

| op | site | what | moves to | safe position |
|---|---|---|---|---|
| WaitEvent b00 (value 0x7eab) + Barrier 0x28 | `f_82c02f20` `0x82c0301c` / `0x82c030c4`, a pre-draw hook (vtable `0x84b423a0`) called by `f_8008f8e0` `0x8008fb6c` for items of kind 2 | waits for GPU culling #241 (signal `0x80291440`), then makes the culled indirect arguments visible | the layer list `f_806688c0` | before the pass's `CallCommands` `0x80668c84`; #241 waits for nothing, so any earlier point is deadlock-free too (fallbacks.md §4, CONFIRMED) |
| WaitEvent b48 (0x7eac) + Barrier 0x28 | `f_82c03170` `0x82c0326c` / `0x82c03314` (vtable `0x84b42418`); the same code with fields +0x121c/+0x1220 | the second culling group (#241, `0x80291504`) | the same | the same |
| WaitEvent b90 (0x7ead) | `f_80279400` `0x8027944c`: the return of `blr [renderer+0x150]` at `0x80279448` (water drawer, reached by a tail call from `f_80279b30`) | waits for water/FFT compute #239 (`0x80660f34`), which itself waits on bd8 (signalled by #233 at `0x80660adc`) | the same | at the job-call site `0x80668c84` only. Never at the layer list's start, and never at `0x8028a538`: those calls (depth probes) come before bd8, so the wait would deadlock (fallbacks.md §4) |
| DispatchCompute 20x1x1, 2 a frame (views c10c0 and 5cf80) | `f_8067c310` `0x8067c488`, tail-called (`b` at `0x8067c1c8`) by `f_8067c110`, an item's draw callback (called at `0x800876f8` in `f_80084de0`, and from `f_8008d640` near `0x8008da50`) | builds the agl::utl::DepthBufferEx occlusion grid (32x18), program `3018813b775edcb6` (readback.md §2): UBO 0x113b620000, writes SSBO 0x113b62d000 (23,360 B), sets the obj+0x10 bit that the CPU culler `f_80365f00` tests | the layer list; the game already calls `f_8067c310` from layer lists (`f_806685b0` `0x806687c8`, `f_80a47660` `0x80a48ff0`) | after the pass's `CallCommands` `0x80668c84`. No draw of the pass reads the SSBO; the first readers are 4-5 passes later: a fragment bind, then compute (`s19.py`; e6-impl.md §7 CUTLOG) |

- **How the waits move.** CONFIRMED, decompile. The hooks assign the event slot and value lazily, under the mutex at
  `[[0x84c9e740]+0x1e8]+0x11f0`, from a global counter. The patch keeps that CPU side, drops only the WaitEvent and
  Barrier calls, and marks the view record (the job's `x25`) as "wait group g".
- `f_806688c0` already waits until all six job slots are in before it calls them (engine-jobs.md §5). So the mark is final
  at `0x80668c84`, and the site records `WaitEvent(table + idx*0x48, 0, value)` plus `Barrier(0x28)` before its
  `CallCommands`.
- What `[renderer+0x150]` holds for b90 is INFERRED (a thunk that tail-calls WaitEvent). To confirm, read it at
  `0x80279418` with the probe.
- **The dispatch.** Patch `f_8067c110`'s tail call at `0x8067c1c8` to queue (obj+0x1c0 entry, flags) on the view record and
  set the obj+0x10 bit now. Then `f_806688c0` calls `f_8067c310` with its own command buffer after `0x80668c84`.
  INFERRED safe: the grid's CPU reader runs in a later frame.
- Not checked: the input texture (deferred bind, stage 5, slot 31) is written before the pass, and HF has the same reader
  positions. Placing the dispatch after the pass removes the first question.
- The stream walk in frame 2205 found no other in-job ops (its other hits are layer-list barriers after the last job).
  This matches the census: 3 waits, 2 dispatches, 2 barriers.

## Decision for the plan

A job is self-contained except for one case: the task/mesh-stage storage buffers of GPU-culled mesh draws, which come from
an earlier job of the same pass or from the layer list's system binder. Ordinary draws inherit no vertex state; the
e6-impl.md §6 note is resolved. All other fixed-function state may be set to NVN defaults at the start of each run.

So M3 builds no state hand-down at the job-call site. It takes two patches instead:
1. The A4 mesh-binding patch, confirmed first by the one name-table log described there.
2. One job-call-site patch in `f_806688c0` that records, before `0x80668c84`'s `CallCommands`, the culling waits and
   barriers that the hooks flagged (b00, b48, b90; never at `0x8028a538`), and after it, the occlusion-grid dispatch that
   `f_8067c110` deferred.

With both patches every job is one rendering secondary, and the plan's "jobs as mixed runs" mechanism is not built.
