# M4b report: indirect and mesh draws (2026-11-12)

Branch `agent/m4b` (from native-vk a79076a; M4a has not landed, so not rebased). Runs `work/runs/m4b_*` (local), RX 6950 XT,
RADV, `RUN=timing`, pacing off (`HOS_FPS_MAX=240`). Kokiri `chisel_kokiri`; Hyrule Field (HF) `chisel_hf` (dump) and
`chisel_hf_market` (validation). **No run lost the device. No task stage was ever enabled.** Tests: 291 pass / 5 fail
(the baseline).

## What was built

| Part | Where |
|---|---|
| Indirect draws (arrays, elements, with or without a count buffer) from their records in layer 0: `vkCmdDraw[Indexed]Indirect[Count]`; mesh draws: `vkCmdDrawMeshTasks[Indirect[Count]]EXT` | `direct_draw.cpp` (`draw_emit` rewritten around the draw kind; new skip reasons; first-use log; `HOS_DBG=meshdry`, `meshlog:<present>`) |
| Mesh-only programs: slot scan classifies vertex, mesh-only and task+mesh; mesh + fragment translated as the frozen `translate_mesh_into` does without the fallback (one-exit CFG, mesh registers, per-primitive and point-coordinate generics); shader objects with `NO_TASK_SHADER`; key `mesh_key`; cache records tagged by stage | `direct_shader.{h,cpp}` |
| **Task+mesh programs**: any stage 6 makes the slot state 4 at creation, never translated (checked again in the job); draws counted per (task, mesh, fragment) code | `direct_shader.cpp` |
| Device: `VK_EXT_mesh_shader` with `meshShader` only; **`taskShader` forced off**, so no task shader object can exist; mesh/indirect procs | `direct_heap.{h,cpp}` |
| Banks for every NVN stage (`kBankStages = kStages`: the mesh stage's c0/c2) | `direct_state.{h,cpp}` (shared, small; M4a's compute stage 5 gets the graphics layout here and must replace it with its packed bank) |
| Patches (each its own file, direct mode only, 600-present line): `host/direct_mesh_args.c`, `direct_mesh_binds.c`, `direct_mesh_system.c` | `direct_patches.c` +6, `eng_jobs.c` +3 (`eng_jobs_drawctx`) |

Bring-up as asked: `m4b_dry_k` (meshdry: everything recorded but the draw call; 24 mesh programs' first use logged, every
one "mesh + fragment, no task stage"), then `m4b_mesh_k` with the draws on.

## Done-items

| Item | Kokiri (`m4b_p_k`) | HF (`m4b_dd_hf`) |
|---|---|---|
| Indirect vertex draws | 0 recorded (none in these scenes; the path is built) | 0 |
| Mesh-only draws drawn / recorded a frame | 34 / 34 (all indirect, jobs) | 40 / 40 (38 jobs, 2 layer) |
| Direct mesh draws from a first past 0 | 0 | 0 |
| Task+mesh draws, skipped and named | 17.88 (15.88 jobs, 2 layer) | 43.94 (41.94 + 2); `chisel_hf_market` 74 |
| Storage slots read unbound (draws a frame) | 0 mesh, 0 others | 0, 0 |
| Validation, sync on (`m4b_val_kokiri`, `m4b_val_hf`) | one type, M3b's `ShaderOutputNotConsumed-DynamicRendering` | same |
| Vulkan objects in steady play | 0 | 0 (9,918 since the start, constant) |

**Task+mesh programs a frame, HF** (codes: task / mesh / fragment; family as `m0-task-mesh.md` 1): `2c56` meshlet cull
(family A), direct draws: `6942222e`/`c2a9f81c` 22.4 (32 program objects), `ef1f1c50`/`c2a9f81c` 7 (34), `e67ab188` 6,
`4bf9f4d0` 2, ten more at 0.1-1; `563d` (Kokiri-style, no input) 2; indirect: `919b`/`54000927` 3, `1813` 2, `8fb3` 2,
`d91e` 2. Kokiri: `2c56` 9.9 direct (16 meshes), `919b` 3, `1813` 2, `8fb3` 2 indirect.

**What HF's terrain and foliage are drawn with.** The 6 culling lists of `f_80362ff0` and the other GPU lists feed
**mesh-only** programs only (every indirect mesh draw at those addresses names a mesh-only program); the three lists at
`0x117896a000`, `0x1178982000`, `0x117898e000` feed **task+mesh** families B/B'/C (`8fb3`, `919b`, `1813`, `d91e`:
instances/patches, foliage). The direct mesh draws are all `2c56` task+mesh. HF's depth 1536x864 against M3a is 0.03
(mean abs., 0-255) and against the frozen back end still 4.4, the ground missing: the mesh-only draws read zero counts
(no culling compute yet) and draw nothing, so the missing ground is `2c56` (task+mesh) or a count M4a's culling will
write. INFERRED from the counters: terrain = `2c56` task+mesh meshlets; foliage = mesh-only culled lists plus the B
families. The 1600x1600 shadow cascades of the two back ends cover different regions at that present (rt order), so
their 87-102 distance is not a mesh measure. PNGs `m4b_dd_hf/dump_1500/072_*`,
`055_*` beside `m3a_dd_hf/dump_1500/070_*`, `053_*` and `m2b_fd_hf_b/shots/dump_1500/605_*`, `510_*`.

## The patches and their counters (Kokiri = HF)

| Patch | Counter a frame |
|---|---|
| **mesh args**: splice after `f_80362ff0`'s last `stp xzr, xzr, [x8, #32]`; records 0-3 recomputed from `[x19+24]`/`[x19+32]` as the function does, w1 = w2 = 1 | 6 headers (24 records), address mismatches 0. Records read at draw time: `{0, 1, 1, 0}` |
| **mesh system** (part 1): `f_80101ec0` hooked to note its view object V = `[[info+0xe0]+8]`; splice after the job prologue's SetDepthRange (`0x800855ac`) calls `f_801060f0` (reads only `info+0xb0`, `+0xbc`) for slots 0-2 (`[V+0x538]`, `[V+0x540]`, `*[0x84ca0fa8]`) and `f_80100210` for slot 3 on the job's command buffer | 100.5 jobs bound; 1 view object (`0x113b6436f8`) all run; 0 misses |
| **mesh binds** (part 2): entry hooks on `f_801a43e0`, `f_801a46a0`, `f_801a6720` make `f_82c02bc0`'s two binds (names 4, 5) | 5 + 5 + 5 calls, all bound; **0 of them draw a program naming 4 or 5** |

**`f_801a6720` settled:** its programs' name-table words for names 4 and 5 are `ffffffff` (not read), in both scenes, and
so are those of `f_801a43e0` / `f_801a46a0`. The binds are no-ops there; the order-dependent value M3's survey saw
belongs to slots these programs never read. Part 2 stays (it costs two calls a drawer) as the guard for a program that
names them; it could be dropped.

**Layout verified.** The culling writers (`4247f1b4`, `f0fed2bb`, `cb2e06dd`, `3e6fe279`, and the other lists' `102f5b0a`,
`3de6d86e`, `6344c53d`, `7650519f`, `abccf81b`) touch a record only by `atomicAdd(+12)` and `atomicMax(+0)` (IR via
`tmtool`); `102f5b0a`/`6344c53d` also the next record's +0/+12. `vkCmdDrawMeshTasksIndirectEXT` reads +0/+4/+8 as
X/Y/Z, stride 16 allowed: `{x, 1, 1, appended}` is Vulkan's record.

**The five other indirect addresses** (meshlog sentinel: a word put in w2 of each unpatched record, read 120 presents
later, with no GPU work running): `0x117a4d6040` and `0x117c7cc000` are headers of GPU lists (sizes 0xf33080, 0x168040)
bound by `102f5b0abb0f1e63`, `abccf81b45c5eb50`, `7650519f88e59b19` (and `6977fe01e8c8b426` in HF), whose atomics write
them; **no CPU code resets them** (the sentinel stayed), so they are reset on the GPU: `f_80362ff0`'s patch does not
reach them, and the same holds for `0x117caa0000`, `0x117899a000`, `0x1178b7e000`/`0x117982a000` (writer `3de6d86e`). All
feed mesh-only draws. `0x117896a000`, `0x1178982000`, `0x117898e000` are bound by no dispatch and are rewritten by the CPU
(the sentinel was gone): task+mesh only (M4c).

## Deviations

- **The other GPU lists are not yet in Vulkan's layout.** Their reset is a GPU write (not a `ClearBuffer`: none on them in the
  logged frame), so the fix is a shader patch of the resetting dispatch (write `{0, 1, 1, 0}`), or a patch of the writers;
  it needs M4a's compute to be observed. Until then their draws read `y = z = 0` and draw nothing (safe).
- Part 1 calls the binder's slot functions with a 0x100-byte stand-in for its argument block (draw context and kind only),
  not `f_80101ec0` itself: its other binds need the pass's full block, which the job prologue does not have. One view
  object exists per run, so V is taken from the binder's last call.
- Mesh-only first-workgroup support is pushed (`kMeshFirstPush` = 56) but not read by the translator: 0 draws need it.
- `HOS_DBG=meshdry` and `meshlog` are debug aids (rule 1). `meshlog` writes a sentinel into guest records: diagnosis only.

## Open problems

- The GPU-reset lists above (layout patch; M4a must run first). The mesh-only picture is unverified until counts exist.
- `HOS_DBG=meshlog`'s dispatch line prints the grid as 0 0 0 (the payload read there is wrong or the op carries none).

## What M4c needs from the tree

The slot state 4 and its `TaskMesh` table (codes, draws by program: the M4c work list); the mesh stage and its banks;
`taskShader` stays off: the task stage becomes a compute step (M4a's dispatch path, `mesh_fallback`'s task half in the
fork) writing a `{slots, 1, 1}` header in this layout, drawn with `vkCmdDrawMeshTasksIndirectEXT` here. Families B/B'/C
read lists the CPU fills (`0x11789xxxxx`): their inputs are ready before the pass. `2c56` (38 direct draws a frame in HF,
the terrain) reads the grid at c3+3088/3092: clamp it (m0-task-mesh.md 4) before any native try.
