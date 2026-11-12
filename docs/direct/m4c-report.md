# M4c report: the task stage as a compute step (2026-11-12)

Branch `agent/m4c` (from native-vk 555e26a). Runs `work/runs/m4c_*` (local), RX 6950 XT, RADV Mesa 26.2.4, `RUN=timing`, one
game process at a time, Kokiri `chisel_kokiri`, Hyrule Field (HF) `chisel_hf`. **No device was lost** in any run of this stage
(the kernel log of this boot has no ring timeout or GPU reset line; the native experiments below were not run, see section 6).
Tests: 292 pass / 5 fail (the baseline five).

## 1. What was built

Every task+mesh program now draws. Its task stage runs as a compute shader, its mesh stage natively, and the device's
`taskShader` stays off. No program gets a case of its own: any program with a task stage and a mesh stage is converted this
way. A program that cannot be converted is skipped, counted and named with its reason. Those cases are a task stage with no
mesh stage, one beside a vertex, geometry or tessellation stage, a header-only task or mesh stage, more than 256 task threads,
or a translation failure. None occurs in Kokiri or HF.

| Part | Where |
|---|---|
| `RuntimeInfo::task_buffer`. The task stage is compiled as compute (Eden's fallback task half: GLCompute, payload in workgroup memory, first task workgroup pushed at 56). Its epilogue `TaskBufferEpilogue` takes map entries for the mesh workgroups it starts. Thread 0 does an `atomicAdd` on the header's taken word and an `atomicMax` of the end on the group count; entries past the map's capacity are not placed. Each started thread writes `task << 8 \| index`, and the payload is copied once per task workgroup. The mesh stage is native (`NO_TASK_SHADER`, no task-payload variable). Its entry block reads its map entry and its task's payload address from the header, `IsbeRead(0)` is its index within its task, and the other `IsbeRead`s go to the payload by address. Header layout: `emit_spirv.h TASK_HEADER_*` | `gpu/direct/eden/.../emit_spirv_mesh.cpp`, `spirv_emit_context.{h,cpp}`, `runtime_info.h`, `emit_spirv.h` |
| A task+mesh slot is a family of its own (its key is salted with the task code). Its variant has three stages: mesh, fragment, and task (compute). The task stage's SPIR-V is emitted last, because the stages' bindings must be numbered in the order `finish()` maps them. The cache record carries each stage's threads and payload words | `direct_shader.cpp` |
| The task step. A direct draw's header, map (`tasks × threads` words) and payload area (`tasks × payload words`) are written by the CPU into the frame arena. An indirect draw's header is made on the GPU by a one-invocation prep step (`shaders/task_prep.comp`). The prep reads the game's record (`{x, 1, 1, n}` through the existing mesh-list patch; NVN's first is 0, as for every indirect mesh draw here), clamps it to `maxComputeWorkGroupCount[0]`, and takes the map and payload area from the recording thread's task pool for that frame slot. The pool starts at 8 MB, is reset with its slot, and doubles at the reset after a frame in which it refused a draw. The header's first 16 bytes are reset by `vkCmdUpdateBuffer` at every run, so a list that runs again counts its groups from 0. The mesh draw is `vkCmdDrawMeshTasksIndirectEXT(header)` | `direct_task.{h,cpp}`, `shaders/task_prep.comp` |
| Where the compute work goes. In a job (a rendering secondary), the work is recorded into two plain secondaries of the job's slot: `List::task_pre` (header resets, prep steps) and `task_cb` (task dispatches). At the job's call, the layer list executes them outside its rendering before it executes the job, with a barrier after each (`task_job_step`). In a list that owns its rendering (a layer list's inline draw), that rendering is ended before the step and begun again after it, loading, with its query and condition re-begun (`pass_outside` / `pass_resume`) | `direct_pass.{h,cpp}`, `direct_lists.h`, `direct_cmd.{h,cpp}` (`cmd_secondary_plain`, the slot's `TaskPool`) |
| Draw path: the task step before the mesh draw, the draw from the header. Counting per program (drawn, or skipped for any reason, by stage codes). New skip reasons: "task step not recorded" and "indirect of several records or with a count". Arena chunks gain `INDIRECT` and `TRANSFER_DST` usage and keep their `VkBuffer` | `direct_draw.{h,cpp}` |
| Debug aids (rule 1, `HOS_DBG`): `taskdry` (resolved, nothing recorded), `taskspv:<dir>` (each stage's SPIR-V and heap mappings, named before it is made), `tasklog:<present>` (that frame's headers once the GPU has run it), `tasknocull` (the game's own culling-off flag c3+2320 bit 4 taken as set, the frozen `RX_VK_MESH_NOCULL`; those variants are not cached) | |

**Ordering and cost.** A task step runs where the draw's commands would have reached the GPU: after every barrier,
dispatch and wait recorded before the job call or the draw. No task input is read earlier than the console would read it.
Two costs follow. (a) A begun rendering is ended and begun again: in Kokiri 8.5 job task steps a frame, 5.1 of them inside a
begun rendering, plus 2.0 inline steps; in HF 8.1, of which 4.0 inside a begun rendering, plus 2.0. (b) There are two
barriers per job step and two per inline step (compute and transfer to compute and indirect; compute to indirect and mesh
shader), and none between the dispatches of one job. Vulkan objects: no new objects in play. The 18 task pools are made at
the first indirect task draws of each thread and slot (load time) and never grew. Pool use peaked at 2.5 MB (Kokiri) and
4.4 MB (HF), and no draw was refused. The object counter is constant in steady Kokiri; HF shows +2 late in the run, as before
this stage (M4g).

## 2. Done-items

| Item | Kokiri | HF |
|---|---|---|
| Task+mesh draws recorded / drawn a frame (`m4c_t1_*`) | 15.88 / 15.88 | 41.12 / 41.12 (45.83 / 45.83 in `m4c_val_hf`) |
| of them direct / indirect (with a prep step) | 8.88 / 7.00 | 32.12 / 9.00 |
| task workgroups of the direct draws a frame | 82 | 151 |
| skipped, by reason | 0 (no program unconvertible) | 0 |
| Validation with synchronization validation (`m4c_val2_k`, `m4c_val_hf`, 150 s each) | **0 messages** | **0 messages** |
| Device losses | 0 | 0 |

Validation's first run (`m4c_val_k`) found one real hazard: the header reset, a `vkCmdUpdateBuffer`, runs in the clear
stage, which the first barrier's source (COPY) did not cover. It reported 10 messages of
`SYNC-HAZARD-READ-AFTER-WRITE` at the mesh draw's indirect read. Fixed: the source is now ALL_TRANSFER.

Translation check (`m4c_dry3_k`, `taskspv`): all 57 task+mesh stages of Kokiri pass `spirv-val` (Vulkan 1.3 with the scalar
and standard-layout flags the other stages need too). They were made by RADV with no error.

`tasklog:1500` (`m4c_tl_k`), after the GPU ran the frame. The `2c56` meshlet-cull draws placed 18 of 64 and 55 of 384 possible
mesh workgroups. The `563d` draws placed 512 of 512, which is right for that program: it keeps every lane. The B-family
indirect draws (`8fb3`, `1813`, `919b`) had 31 and 310 task workgroups from the GPU's record, and placed 0-12 and 1068 mesh
workgroups.

## 3. Pictures and per pass

Shot pairs, direct then frozen:
- Kokiri: `work/runs/m4c_t1_k/shots/vk_001800.png` against `work/runs/m4f_fd_k/shots/vk_001800.png`
- HF: `work/runs/m4c_t1_hf/shots/vk_001500.png` against `work/runs/m4c_fd_hf/shots/vk_001500.png`. The frozen HF run was made
  with this build on the same overlay, so both stand at the tunnel's mouth. The older `m2b_fd_hf_b` stands elsewhere.

**Kokiri** (`m4c_dd_k` against `m4f_fd_k`, 136 aligned, mean distance 1.00):
- **The floor grass draws** (the B families). The G-buffer RGBA8 1536x864 moves from 12.2 to **2.78**, and the half-resolution
  passes the `2c56` programs draw in the frozen run (depth 768x432 and the two RGBA16F 768x432 targets, frozen states 241-247
  named `974ea2c9` / `da7a7ab2`) are **0.00-0.02** from the frozen ones.
- The lit target's final states are 1.8 (M4g: 5.9-18.8), and the composite is 7.19 (M4g 7.9). The frame is still darker: the
  shot's mean is 17 / 14 / 5 against 32 / 22 / 8 frozen, and M4g's was 15 / 11 / 4.
- **The bark walls of the giant tree are still dark.** They are not missing geometry from task+mesh: every task+mesh draw is
  drawn, and the culling is not the cause. With `tasknocull` (`m4c_nc_k`) the picture does not change (mean 11.6 against
  12.0). Fixed occlusion results do not change it either (`HOS_DBG=qfixed`, `m4c_qf_k`: 17.5 / 13.9 / 5.0). Brightened 3x,
  the trunk and the moss wall are in place in the direct shot but under-lit, and so are the stone path in the foreground, the
  pots and the basket.
- The environment cube chain is still darker (128x128 level 0: 0.0088 against 0.0156; M4g 0.0076). So M4g's guess, that the
  dark cube, props and composite are caused by the missing task+mesh walls, is **not confirmed**: those draws now run, and the
  cube and the lit means moved only a little.
- Still beyond noise:
  - RGBA16F 512x512 #2 (8.8)
  - RGBA8 384x216 (6.1; M4g's 384x216 B offset)
  - the D16 1600x1600 cascades and two D32F 1536x864 states (a cleared state aligned against a drawn one, as in M4f)
  - B10G11R11F 1536x864 #107 (17.6: aligned against another state of the target)

**HF** (`m4c_dd_hf` against `m4c_fd_hf`, 130 aligned, mean 3.49): **the composite is 3.86**, and the lit target's final states are
1.6-2.1. Beyond those, only the cascades (a drawn state aligned against a cleared one, 66-238), one 1536x864 depth state
(7.2) and the lit state #094 (16.5, the same alignment artefact) differ. In the shots, the tunnel floor's grass and foliage
now draw as in the frozen shot (M4g's shot had none). The rock wall at the right is darker than the frozen one.

## 3b. The remaining darkness and the GI textures (checked on the coordinator's hint)

The hint was that the darkness (bark walls, pots, basket, the environment cube at about half) may come from the GI textures'
conversion. What was checked, and the result:

1. **Are the walls drawn, and by what?** The trace (`m4c_dl_k`, `drawlog:1800`) shows every task+mesh draw of the frame
   drawn. In the lit 1536x864 pass the `2c56` draws are depth-only occlusion proxies (`6993fab2`, `f55044a0`: colour write
   mask 0, depth write on) plus two coloured ones (`cfebb562`, ivy vine; `58c8fd0b`, roof fence). In the frozen frame the
   `2c56` programs draw into the 768x432 passes, and those passes match the direct ones at 0.00-0.02. Brightened, the trunk
   and the moss wall are in the direct shot, drawn but dark. So it is lighting input, not missing geometry. Two more checks
   say the same: `tasknocull` and `qfixed` change nothing.
2. **The terrain GI tile** (`bake_gi.extm`, BC6H x6, 260x260). Kokiri loads exactly one,
   `TerrainArc/KokiriForest_01/1000000000.child.bake_gi.extm` (overlay open #7). Its converted blob in `work/prepared/terrain`
   is **byte-identical** to an independent Python deswizzle of the romfs file's blob with the Switch 2 GOB formula, written
   for this check (not texconv): 405,600 bytes, block height 3, layer stride 139,264. The texture is NVN 0x4f
   (BPTC_SFLOAT), and both back ends map that to `VK_FORMAT_BC6H_SFLOAT_BLOCK` (`direct_formats.cpp` / `vk_formats.cpp`). The
   direct image is uploaded ("written R"). Same bytes, same format: no fault here.
3. **The scene lightmaps from the store** (`RBake_Scene_KokiriForest_*`: `_d0_t0` BC5, `_d1_shgi0_uf3` BC6H x3,
   `_d1_shgi0_un3` BC7 x3). These are `copy_bc` records: the blocks are kept and only deswizzled. The store was built at
   11:14-11:46, after the GOB fix (590c57f, 11:10). For `Child_G_3_3`, all three textures' level 0 / layer 0 in the store are
   **byte-identical** to the same independent deswizzle of the BNTX source. The formats follow the BNTX codes: 0x1f0a (BC6H
   *unsigned* float) to `BC6H_UFLOAT`, 0x2001 to BC7 UNORM, 0x1e01 to BC5. All 495 BC6H textures of the store are 0x1f0a, and
   there is no signed BC6H texture in the store. No ASTC HDR block exists in the romfs ("ASTC HDR blocks 0"), so no HDR texture
   was squeezed into an LDR BC target. Synchronization and core validation reported 0 messages in 150 s, image and view
   creation at load included; a view format that disagreed with the image would have been reported. The child bakes that have
   no data of their own take the adult load's record (M4g's dedup), as the frozen back end reads the same memory.
4. **The environment cube's inputs**: the same lit materials and the same GI textures as point 3. M4g found its probe compute
   inputs equal to the frozen ones. The cube is darker by the same factor as the lit frame (0.56x against 0.61x).

**Result:** no fault found in the GI conversion or the tile upload. What these checks do not cover is what the frozen
back end's GPU image of a lightmap holds, compared texel by texel with the direct one. The lightmaps are read through handles
in game buffers, so the trace does not list them per draw, and frozen `RX_VK_WATCH_TEX` dumps were not made. That comparison
is the next check. The cause of the darker lit frame stays open. It is not the task stage, not the terrain GI tile and not
the store's lightmap bytes or formats.

## 4. Frame time, pacing off (`HOS_NO_PACING=1 HOS_FPS_TARGET=1000 HOS_FPS_MAX=240`)

"Before" is the same build with `HOS_DBG=taskdry`: task+mesh draws resolved and counted, with no task step and no mesh draw
recorded, which is what native-vk did. Same caches, alternated pairs, last six 600-present windows:

| Scene | with the task step (new1, new2) | without (dry1, dry2) | hsd_emit a frame, new / dry |
|---|---|---|---|
| Kokiri | 12.14 / 12.17 ms mean (p99 13.69 / 13.99) | 12.20 / 12.15 (p99 14.22 / 13.78) | 2.55 / 2.56 ms |
| HF | 11.64 / 11.59 (p99 13.10 / 12.98) | 11.56 / 11.54 (p99 12.91 / 12.88) | 3.46 / 3.46 ms |

The task steps cost nothing measurable in Kokiri and about +0.06 ms mean in HF, which is within the run-to-run spread. The
frame is CPU-bound, so the GPU work of the task computes and their mesh draws is hidden. The CPU side, at most two
secondaries, one arena allocation and a few commands per draw, is not visible in `hsd_emit`. The GPU cost of the steps
themselves was not timed (no timestamp queries around them).

## 5. Device losses

None. All 24 M4c game runs (`m4c_*`: data, dry, bring-up, dump, trace, diagnosis, timing and validation runs, one of them the frozen back end) ran without
`VK_ERROR_DEVICE_LOST` or a kernel ring timeout. Before the first run that recorded a task step (`m4c_t1_k`, 15:41), everything
was committed (7093e85). The kernel log retained for this boot then held 0 reset lines; the 11 counted at the start had
rotated out of the retained journal before 15:26.

Two RADV crashes happened in the dry runs (`m4c_dry_k`, `m4c_dry2_k`, segfaults in `libvulkan_radeon.so`, not GPU hangs). Their
cause was ours: the stages' bindings were numbered task-first while their heap mappings were made mesh-first, so the mesh
stage's uniform buffers had no mapping. A reproducer (scratch, not committed: `vkCreateShadersEXT` on the dumped SPIR-V and
mappings) and a debug build of Mesa main's RADV, for its backtrace only (`radv_nir_lower_descriptors.c:210
load_ubo_descriptor`, a classic layout looked up for an unmapped binding), found it. It was fixed by the emission order.

## 6. The parked native experiments (not run)

- **Native task shaders with the bystander `ffb53ed102dd9908` skipped**: the frozen back end has the switch
  (`TASK_MESH_FALLBACK=0 RX_VK_SKIP_PROG=ffb53ed102dd9908`), one HF run, one possible loss. Not run: it is optional, and each
  run that loses the device costs a loss and a recovery wait. This session's runs went to the compute route and the GI check.
- **A drain after every native mesh draw**: it needs a barrier after each native mesh draw in the frozen back end's draw path
  (`gpu/vk`, not editable here). The direct back end has no native task path to put it in (`taskShader` off by design).
  Building one only for this experiment was not done.
- **Mesa main**: `../mesa` (27.0.0-devel) builds RADV alone in about 10 minutes (`meson setup <dir> -Dvulkan-drivers=amd
  -Dgallium-drivers= -Dplatforms= -Dllvm=disabled -Dbuildtype=debugoptimized ...; ninja src/amd/vulkan/libvulkan_radeon.so`,
  done here for the backtrace above). A game run on it needs:
  - an ICD file pointing at that library (`VK_DRIVER_FILES=<icd.json>`, as the reproducer used it);
  - the shader-object binary cache and the program caches made again, because the binary UUID differs: one cold start of about
    150 s;
  - `TASK_MESH_FALLBACK=0` on the frozen back end, or a native task path in the direct one;
  - root for the devcoredump if it hangs.

  The GL2-bypass fix for the gang semaphore (m0-hang-dump.md) is in main. Whether it changes the graphics-ring signature is
  the open question.

## Open problems

- The darker lit frame in Kokiri (the bark walls, the stone path, the props, the environment cube). Its cause is not task+mesh
  geometry (section 3) and is not found.
- The indirect task draws take NVN's first as 0, as every indirect mesh draw does here; a record with a first past 0 would
  draw from 0 (not seen: the patch's own records only).
- Multi-record or count-buffer indirect task draws are skipped (counted, none seen).
- Primitive order within a task+mesh draw follows the task workgroups' atomic order, not NVN's. This was not seen to matter:
  every task+mesh draw in these scenes is opaque or alpha-tested.
