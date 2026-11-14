# The shader contract

What a program translated by Eden (`gpu/direct/eden`, driven by `direct_shader.cpp`) expects at a draw, and what `direct_draw` provides. **C** = read in code at the line given; **I** = inferred. Paths are `gpu/legacy/vk/` (the old renderer, which the contract was first read from) unless named.

## 1. Program identity and variants

**Key** (`graphics_key`, `vk_shader.cpp:873-883`) **C**: `vs.key ^ fs.key*31`, then for each of the 16 attributes the
`attribute_type_of(vattr[i])` (Float / SignedInt / UnsignedInt / Signed- / UnsignedScaled, `:841-861`) only when the vertex
SPH reads it (`InputGeneric`), then the 8 colour-target types masked to the outputs the fragment SPH writes (`written_types`
`:869-871`). A stage's code key hashes its control section and code area (`code_key` `:142-144`). Mesh: `mesh_key`
(`:884-892`) = mesh, task, fragment code keys + the header-only stage's SPH + written colour types. Compute: the code key
alone (`:2183`). **Variant** within a key: the texture *answers* (`record_answers` `:747-771`): for every single-count texture
/ image descriptor, the bound handle's type and integer bit (`texture_answer` `:668-678`), canonicalised (`:704-737`: a type
fixed by the instruction is no answer except Rect/Buffer; depth non-integer drops the bit), hashed into `Program::variant`
(`:739-744`); checked per draw by `still_valid` (`:774-780`). Counted: one key and one variant per NVN program almost always
(e7-design.md 1.3).

**What translation reads beyond the key** **C**: all 16 attribute types into `vrt.generic_input_types` (`:1612`, not only
the read ones), `convert_depth_mode = host->depth_minus_w()` (`:1614`), fixed point size (`:1609`), all 8 colour types and the
NaN guard of float targets (`:1645-1652`), the device profile (`make_profile` `:505-555`: subgroup lanes, `ssbo_bda`,
`ssbo_bda_wide`), and the bound state through `Env`: `ReadCbufValue` gives c1 from the code, c0/c2 from `driver_bank_word`, c3+
from guest memory (`:402-418`); `IsCbufStorageBufferPointer` asks whether a bank word points into a pool now (`:468-479`);
`ReadTextureType` / `IsTexturePixelFormatInteger` ask the bound handle (`:419-431`). The last three are not in the key: the
first translation's state wins (e7-design.md 1.2).

**Info per stage** used at draw time (`vk_shader.h:20-78`, `shader_info.h`): `constant_buffer_descriptors` and
`constant_buffer_used_sizes`; `storage_buffers_descriptors` {cbuf_index, cbuf_offset, count, is_written}; `texture_descriptors`
{type, type_fixed, is_depth, is_integer, count, cbuf_index/offset, has_secondary + shifts, size_shift, handle_program};
`image_descriptors`; texel-buffer descriptors; `handle_programs`; `uses_render_area`, `uses_rescaling_uniform`,
`uses_global_memory`, `native_global_bounds`, `ssbo_bda`, `bda_why`; `loads` / `stores` (Generic, PointSize),
`stores_frag_color`, `frag_color_components`. Program-level: stages {nvn_stage, vk_stage, info, c1, module}, workgroup,
shared memory, `answers`, `codes[4]`, `mesh` / `fallback`, `uses_render_area` / `uses_push` (`vk_draw.cpp:704-713`).

**Caches** **C**: translations in `hookshot_programs.bin` + `.idx` by key (`vk_shader.cpp:1124-1159`), good for one
`disk_stamp` (`:1072-1122`: `HOOKSHOT_SHADER_STAMP`, depth mode, mesh flags, lanes, BDA bits, struct sizes, coopmat flags,
`RX_VK_*`/`EDEN_*` env prefixes). Shader-object binaries in `hookshot_shader_objects.bin` (`vk_shader_object.cpp:54-152`),
whole file dropped for another driver UUID (`:74-87,114`); a binary's key = SPIR-V hash, stage, next stage, the layout or
**heap mapping signature**, the 32-lane pin, NO_TASK (`:240,256-257`). The heap signature includes the heap's own layout
(`region_base`, `tail_at`, strides: `vk_desc_heap.cpp:405-418`), so *a mapping change invalidates only shader-object
binaries, never translations*. ProgNotes `{codes, key, variant}` (`vk_shader_object.cpp:408-422`) let the prefetch thread make
known variants at `nvnProgramSetShaders` (`:455-484,632-647`). Direct must name its own files.

**Known at load** (e7-design.md 2): code (SetShaders), vertex attribute types (E2 vertex packet), material handles. Known
later: the pass's colour types (float in every logged pass), the 30 system textures. A load-time path can therefore compute
the key and pick a *cached* variant; it cannot reproduce a translation's hidden `Env` reads without a state (**I**, e7 S6).

## 2. What a graphics program expects at a draw (heap + BDA=2)

**Push data**, one block from offset 0 (`emit_spirv.h:24-52`; `push_bytes` `vk_draw.cpp:4112-4145`; heap words
`vk_desc_heap.cpp:293-403`) **C** (offsets computed from the structs: RescalingLayout is 32 B with its alignas):

| Offset | Size | Field | Written by | Rate | Arithmetic only? |
|---|---|---|---|---|---|
| 0 | 16 | render area {w, h, 0, 0} if `uses_render_area` (over the rescaling words) | `push_bytes` `:4137-4144` | per pass | yes |
| 24 | 4 | down_factor = 1.0f | `:4113-4114` | constant | yes |
| 48 | 8 | guest page table address | `:4115-4118` (`memory_guest_pages`) | per run | only read by non-BDA pointer-following stages (`spirv_emit_context.cpp:1257-1307`) |
| 56 | 8 | task first-workgroup table address | `:4128-4131`, `eng_draw_mesh_first` `:5259-5270` | per mesh draw | yes |
| 64-87 | 24 | mesh fallback addresses | `:4132-4136` | drop (fallback not carried over) | |
| 88 | 8 | layer-0 window table address (`bda_emit.h:104-110`) | `:4119-4125` | constant | yes |
| 96-119 | 24 | virtual range lo, hi, delta (pools from 1 TB) | `bda_virtual` `:3169-3183` | per draw, rare | yes, given a window for the pool |
| 120 | 4 | ring block index (64-byte units) | `eng_draw_descriptors` `:5519` | per draw | yes |
| 128+ | 8 each | device address of each driver bank an Indirect texture reads | `:5503-5509` | per bank change | yes (arena copy) |
| then | 4 each | handle of each PushIndex texture | `:5520-5521` | per draw | **no**: CPU read of guest memory |

Push base = `(PUSH_CONSTANTS_END + 7) & ~7` = 120 (`vk_desc_heap.cpp:530-532`); total must be ≤ `maxPushDataSize`
(`:404`) or the program gets no heap layout.

**Banks** (constant buffers by Eden's index) **C**:
- **c0** (driver bank, graphics): the stage's storage-buffer table, 16 entries of {addr lo, addr hi, size} at
  0x110/0x510/0x410/0x210/0x310/–/0x670/0x770 for NVN stages 0..7 (`vk_bank.cpp:351-353,409-424`); vertex 0x610 = 2047.9375f
  (`:361`); task/mesh pointer table at 0x880 made by a CPU scan of small storage buffers (`:242-308,354-359`). The draw
  parameters at 0x640/0x644/0x648 are not read: the translator replaces them with BaseVertex / BaseInstance / DrawID builtins
  (`vk_shader.cpp:452-488`). With BDA the shader reads storage addresses from these words (`spirv_emit_context.cpp:707-719`).
- **c1**: the stage's own constants, the code area's tail (`vk_shader.cpp:295`); constant per program, yet uploaded per draw
  by the job path (`vk_draw.cpp:5413-5422`).
- **c2** (driver bank): low words of handles, 8 bytes a slot: textures 0x20 (32 slots) and 0x168 (128), images 0x120 (9),
  samplers 0x568 (128) (`vk_bank.cpp:319-324`). Reaches the shader twice: as a uniform buffer and as the memory an Indirect
  texture mapping reads its index from (`vk_desc_heap.cpp:354-371`).
- **c3..c18**: `s.ubo[stage][i-3]`, a layer-0 window range (`vk_draw.cpp:5423-5434`): alignment `max(minUniformBufferOffset
  Alignment, 16)`, range `min(used or size, size, maxUniformBufferRange)`; unaligned is refused (`:5431`). The replay instead
  copies it into its stream from `memory_read_as_submitted` (`:2363-2367`).
- Every bank is a uniform-buffer element of the per-draw **ring block**: 16 B buffer descriptors, then 64 B storage-image
  descriptors (`vk_desc_heap.cpp:308-309,322-337`), mapped `HEAP_WITH_PUSH_INDEX` at push offset 120. c0/c2 are filled from the
  state into the job stream per draw (`vk_draw.cpp:5380-5400,5409-5412`; `driver_bank_fill` `vk_bank.cpp:310-362`).

**Textures and samplers** **C** (`vk_desc_heap.h:4-15,64-72`): resource heap = ring rows, then one region of 65,536 image
descriptors per `VkImageViewType` (texture id = index), then a zero tail; sampler heap = plain and compare regions of 4,096.
Per texture descriptor, `make_program` picks (`vk_desc_heap.cpp:339-383`):
- **Indirect**: handle in c0/c2, one word, no handle program, 4-aligned → `HEAP_WITH_INDIRECT_INDEX` +
  `useCombinedImageSamplerIndex`: the GPU reads `id | sampler << 20` from the bank at the pushed address. No CPU work.
- **PushIndex**: anything else (user UBO word, two words, handle program) → the CPU reads the handle now
  (`eng_handle_of` `vk_draw.cpp:5237-5258`, `host->gpu_read`) and pushes it.
- **Null**: a word that is a storage pointer, not a handle (`handle_is_storage_pointer` `:2053`): the zero tail.
- Refused (no heap program): texel buffers, texture or image arrays (`vk_desc_heap.cpp:300-305`); the job path also refuses
  storage images (`vk_draw.cpp:5501`) and texel buffers (`:5475`).
- **Deferred binds**: their c2 words are patched at the submit (`vk_draw.cpp:5387-5397`); a PushIndex word from one is refused
  (`:5489-5494`). So deferred handles reach shaders only through c2 Indirect, which a submit-time copy into the bank serves.

**Storage buffers** **C**: with `ssbo_bda` the shader loads the address word from its bank, translates it through the window
table (`window[g>>31] + (g & 0x7fffffff)`, null window above 1 TB: `bda_emit.h:115-129`) and has no descriptor (`vk_draw.cpp:
5464-5473`). =2 also allows tables, render-area stages, CAS loops, native bounds (`bda_emit.h:58`;
`spirv_emit_context.cpp:568-576`). A stage still refused keeps descriptors, and the job path refuses its draw
(`EDW_SSBO_CLASSIC` `vk_draw.cpp:5440-5443`; why: `eng_bda_refusal` `:5273-5303`). Written buffers are refused only for the
replay's write tracking (`:5446-5454`). Count>1 table entries are not read by the shader under =2: the global-memory functions
translate their own addresses (`spirv_emit_context.cpp:711-716`).

## 3. Fixed-function and dynamic state (`eng_draw_state` → `dynamic_state`, `so_state`, `so_bind`)

`eng_draw_state` (`vk_draw.cpp:5203-5216`) builds a `Targets` from the job's formats, resets the thread's diff caches when
asked, sets `cur_program`, then `dynamic_state` (`:4666-4853`), `so_state` (`:4891-4952`), `so_bind` (`:4870-4887`). **C**:

| State | Conversion |
|---|---|
| viewport | only `viewport[0]`; 0x0 → whole target; **y flipped**: `{x, y+h, w, -h}` (NVN NDC +y up) (`:4696-4698`) |
| depth range | `depth_near/far` clamped to [0,1] (`:4698`); NVN's −1..1 clip handled in the vertex stage (`convert_depth_mode`, `vk_shader.cpp:1614`), so `DepthClipNegativeOneToOne = false` (`:4896`) |
| scissor | one rect, w/h ≤ 0 → whole target, clamped to the target (`:4699-4705`) |
| front face / cull | front face **not** inverted: the flipped viewport keeps the winding (`:4746-4748`); cull none/front/back/both (`:4742-4745`) |
| topology | `topology()` (`:550-572`): POLYGON → fan; QUADS → triangle list with indices built on the CPU, indexed ones from a CPU read of the guest index buffer (`:8085-8100`); the job path refuses quads (`eng_job.cpp:602`); PATCHES unused (no tessellation translated, `vk_shader.cpp:2178`) |
| primitive restart | enabled only for strip/fan topologies (`:4751-4752`); `restart_index` is never applied (Vulkan's all-ones; read only by a log, `:7905`) |
| polygon offset | `vkCmdSetDepthBias(units, clamp, factor)` from `poly_offset[1], [2], [0]`, enable from `poly.offset_en` (`:4709-4710`), no unit scaling; **plus** a back-end special case of +250000 units on square depth-only targets ≤768 and an env-driven cascade bias (`:4711-4741`) |
| depth | test/write only with a depth aspect; compare `f-1` (`compare_op` `:510-512`, `:4753-4757`) |
| stencil | enable only with a stencil aspect; ops and compare per face from `ds.*_f / *_b` (`:4769-4772`); ref / compare mask / write mask `& 0xFF`, set per face from `NOP_STENCIL` face bits 1=front 2=back (`eng_job.cpp:1048-1057`, `:4707-4708`) |
| raster | discard, polygon mode (point/line/fill), depth clamp, alpha-to-coverage (`ms.a2c`) (`:4773-4777`); line width 1, depth bounds off (`:4817`); alpha-to-one off, logic op off, provoking vertex last (`:4893-4896`) |
| blend | per target: enable `color.blend_en>>i & 1` and a format; equation from `blend[i]` (`blend_factor` / `blend_op` `:516-548`); blend constants (`:4778-4785`) |
| write masks | `chmask >> 4i & 0xF`, 0 for an output the FS never stores, and only the components it writes (`target_write_mask` `:924-934`) |
| multisample | samples from the targets, sample mask all ones (`:4898-4903`) |
| vertex input | for each attribute the VS loads: format `vertex_format(fmt)` (`:583-626`, 3-component fallback `:628-657`), offset `& 0x3FFF`, binding = stream; unset → binding 15, R32_SFLOAT, stride 0, zero buffer (reads 0,0,0,1); stride and `divisor` from `vstream` (instance rate when divisor ≠ 0) (`:4909-4940`) |
| buffers | 16 vertex streams `layer0_buffer(addr, size)` with dynamic strides (`eng_job.cpp:676-692`); index `layer0_buffer`, uint8 needs the extension (`:711-718`); `vkCmdDraw[Indexed](count, max(1,inst), first/0, base_vertex, base_instance)` (`:718-720`) |

**Minimal state for `direct_state`** (the ops that set it: the job walk `eng_job.cpp:956-1086`, payloads `rx_nvn.h:164-248`):
`prog` copy + `has_prog` (BIND_PROGRAM, from the guest object at emit); `ubo/ssbo[8][16]` (BIND_UBO/SSBO, BIND_BUFS);
`tex/samp[8][128]`, `img[8][32]` (BIND_TEX/SAMP/IMG) plus each slot's deferred source; `blend[8], color, chmask, ms, poly, ds`
(BIND_STATE kinds 0-5); `vattr[16], nvattr` (BIND_VATTR clears first); `vstream[16]`, `vbuf[16]`; `viewport[0]`, `scissor`,
`depth_near/far`; stencil masks/refs [2]; `poly_offset[3]`; `blend_color`; `restart`; `depth_clamp`, `raster_discard`,
`render_enabled`; `cs_push[0x400]`/`cs_push_n` (PUSH_CONST, `vk_replay.cpp:1883-1891`); `grid[3]`; targets from SET_RT
(direct_pass). Not needed: `rt_serial`, `bind_gen`, `tex_gen`, `ssbo_gen`, `samples_passed`, `restart_index`, `copy_stride`
(`vk_state.h:30-48`). The translator's `Env` takes a full `NvnState` (`vk_shader.cpp:386`), so for a miss's background
translation the direct state must convert to it (≈26 KB, copied per miss as `program_pick` does, `:2207-2219`).

## 4. Compute

Same push block (no render area). c0 is NVN2's packed compute bank (`vk_bank.cpp:49-67,327-349`) **C**: grid at 0x0-0xB, the
push constants at 0x10 (size `ctl[0xa9]*4` rounded to 16), then UBO table, SSBO table (16 B entries), texture handles,
image handles (8 B each), counts from control bytes 0xb1/0xb2/0xb3/0xb6; the pointer table at 0x880 when the layout ends
before it (CPU scan again). Compute textures are read from **c0**, not c2 (`TextureBoundBuffer` `vk_shader.cpp:436-439`),
so Indirect textures point at the c0 bank. **DispatchIndirect**: the replay guesses the grid on the CPU
(`memory_read_as_submitted`, `vk_draw.cpp:9319-9323`) and copies the 12 GPU words into the c0 upload before the dispatch
(`:2241-2247,9684-9695`); direct keeps only the GPU copy. Shared memory from control +0x64 (`vk_shader.cpp:298,443-448`),
32-lane pin (`vk_shader_object.cpp:264-282`); bound with `vkCmdBindShadersEXT(cs[0])` (`vk_draw.cpp:9621-9623`). The replay's
full barrier between dispatches (`:9599-9601`) is replay policy; direct uses the game's barriers. Storage images, common in
compute, have no registration-time entries in the old heap (ring block only).

## 5. Mesh (later stages)

Key `mesh_key` (§1); shader objects with `NO_TASK_SHADER` when no task stage (`vk_shader_object.cpp:275`); no vertex input
(`vk_draw.cpp:4904`), no topology or restart (`:4820-4823`). NVN draws tasks `[first, first+count)`, Vulkan from 0: the
translator adds a first word read at `push[56] + DrawIndex*16` (`emit_spirv_mesh.cpp:539-549`), but only when built with
`RX_VK_MESH_FIRST` (`spirv_emit_context.cpp:627-648`), otherwise from 0 (`emit_spirv_mesh.cpp:550`). The job path writes
`{0,0,0,first}` to a stream slot and pushes its address + 12 (`vk_draw.cpp:5259-5270`); indirect mesh arguments are
converted to `{count,1,1}` by copies (`eng_job.cpp:649-668`), which the plan replaces by a game patch (plan, Data a draw
reads). c0 storage tables at 0x670/0x770 and the 0x880 pointer table (CPU scan) apply to stages 6/7. The task stage becomes
a compute step (plan, Task and mesh shaders): it is then translated and bound as §4.

## 6. What goes, and the target interface

**Old-design only, dropped** **C**: lazy heap entries and their three put paths (`vk_desc_heap.h:116-165`, `heap_texture`
`vk_draw.cpp:2484`); residency touches and hazard scans (`draw_hazards` `:5150`, `draw_tex_check` `:10009`,
`eng_draw_sampled` `:5329`); bank and descriptor caches keyed by `bind_gen/tex_gen` (`:2240-2290,2568-2590`); the pick memo
(`program_for` `vk_shader.cpp:2101-2167`) and the `published` map under a shared lock (`eng_job.cpp:604-610`); the per-draw
answer check through `texture_of` under `reg_mu` (`vk_bank.cpp:220-226`); stream overlays (`ub_read`, `vk_shader.cpp:622`);
every `memory_read_as_submitted` (`vk_draw.cpp:2367,8091,9322`, `vk_bank.cpp:267`); on-demand binds and `layer0_ready`
checks (`vk_draw.cpp:3201,3233,5452-5471`); write tracking; the fallback; the shadow/cascade biases (rule 8: a game patch
if the pictures need them; UI passes do not).

**Step A: translator unchanged** (Eden linked from the frozen objects, same profile as BDA=2, own cache files). Only the
heap *mapping* changes, which costs a shader-object rebuild and nothing in the program cache (§1):
1. Programs: slot table by NVN serial (e7-design.md 3.1); key by `graphics_key` arithmetic at the draw; answers checked
   against a per-texture-id answer byte written at registration (no lock). Miss: skip, count, translate in the background.
2. Textures/samplers: entries at registration in every view-type region the texture's target allows, both sampler regions.
   Indirect as today. PushIndex reading a single c3+ word: map it `HEAP_WITH_INDIRECT_INDEX` with the pushed **UBO address**
   and `addressOffset = cbuf_offset` (same handle format as c2) **(verify** on RADV**)**; handle programs and two-word
   handles: counted and skipped in A.
3. Uniform buffers: map every UBO binding `PUSH_ADDRESS` (`vulkan_core.h:16740,16911`): c3+ = window address + offset;
   c1 = a buffer made at program load; c0/c2 = an arena copy of the stage's bank made when its binds change, its address
   shared with the Indirect textures' push slot. No ring block, no buffer descriptors. **(verify:** a dynamically indexed
   cbuf read past its range through an address; RADV push-data size for ~10 addresses + 120 B.**)**
4. Storage buffers: nothing (BDA). Stages not `ssbo_bda` (`bda_why` ≠ 0): counted and skipped.
5. Storage images: a storage region at pool ids written at registration, `HEAP_WITH_PUSH_INDEX` / constant offset per slot
   **(I)**; until then counted and skipped (UI passes: expected none, **I**).
6. Deferred c2 words: one copy into the bank's arena words at the submit (plan's snapshot).

Per draw: arithmetic on the list's state, a bank copy on bind changes, push data, dynamic-state diffs. UI first: these
programs are plain vertex/fragment with c2 textures and c3+ uniforms (**I**: count the refusals on the UI passes first).

**Step B (M3, one cache invalidation, forked `gpu/direct/eden`)**: handles read from memory by the shader (handle programs,
two-word and dynamically indexed handles → heap-indexed sampling), so no PushIndex remains; every stage by address (no
`bda_why`, no descriptors for storage, no page table); compute grid from `NumWorkgroups` instead of c0+0..0xB (drops the
indirect copy); the 0x880 pointer tables dropped (addresses translated); c2/c0 handle words read from per-list binding tables
in the arena; the task stage translated as the compute step. Translation without a bound state (`Env` answering from the
program alone) so load time can translate.

## 7. Files to lift for step A, and traps

| Piece | From | Depends on |
|---|---|---|
| code read/parse, `Env`, profile, translate, key, answers, graphics `translate_into`, caches | `vk_shader.cpp:113-302,384-589,605-678,686-780,841-892,1072-1159,1308-1489,1604-1671` | `NvnState` (prog, vattr, ubo, tex/samp/img, ssbo, cs_push, grid), `driver_bank_word`, `texture_of`, `gpu_pool_end`, `bda_on/bda_wide`, coopmat globals, `so_module_made` |
| banks | `vk_bank.cpp:310-362,370-432` minus `pointer_table` `:242-308`, `cs_layout`'s map `:68-99` | `NvnState`, control section |
| heap layout | `vk_desc_heap.cpp:287-420` (`make_program`), `:429-526` device/heaps, `:530-532`, `:611-642`, `:664-770` encode | `Shader::Info`, `texture_view_type`/`view_type` `vk_draw.cpp:2002-2012,3898`, `handle_is_storage_pointer` `:2053` |
| shader objects | `vk_shader_object.cpp:54-152,224-424,436-484,589-647` | `HeapProgram`, SPIR-V hash per stage |
| state → Vulkan | `vk_draw.cpp:510-657,924-934,4666-4853` (minus hacks), `4870-4952`, `4112-4145`, `5219-5225`, `5368-5527` (reference) | `Targets`, `Program`, `Info` |
| draw calls, state walk | `eng_job.cpp:676-724,956-1086` | `layer0_buffer` (not yet in `gpu/direct`) |

**Traps** **C**: `dynamic_state` writes the replay's `pass()` (`vk_draw.cpp:4673,4852`) and diffs against thread-locals
(`last_dyn`, `dyn_fast`, `so_dyn`, `last_so`) that `eng_draw_state` resets only when told (`:5211`): RADV inherits no
dynamic state, so reset at every command buffer begin. `stage_cbuf_word` reads the replay stream unless the thread-local
`cbuf_live` is set (`vk_shader.cpp:604-623`). `pointer_table` caches by `&s` and `bind_gen` in thread-locals and reads
"as submitted" (`vk_bank.cpp:248-267`). `cs_layout` is a global mutex and map by serial, cleared at 4096 (`:68-99`).
`desc_heap_program` takes `prog_mu` per call (`vk_desc_heap.cpp:534-549`), called per job draw (`eng_job.cpp:625`): keep the
`HeapProgram` in the program slot. The job ring wraps with no retirement (`vk_desc_heap.cpp:611-626`). `so_make` keys
binaries by the `VkShaderModule`'s recorded SPIR-V (`vk_shader_object.cpp:245-253`): modules exist only for that key
(`vk_shader.cpp:591-599`). `push_bytes` reads `static` page-table / table addresses and the thread-local `bda_virt` that only
`build_descriptors` sets, so job draws push stale or zero virtual ranges (`vk_draw.cpp:4116-4124`). `getenv` statics in hot
paths (`:5446,5483`; `vk_desc_heap.cpp:291`) and `_environ` in the stamp (`vk_shader.cpp:1118-1120`): direct's profile and
stamp must match BDA=2 explicitly or translations differ. `depth_minus_w` can flip at run time (`host/nvn_native.c:278`) and
is in the stamp. Indexed quads read guest indices on the CPU (`vk_draw.cpp:8085-8100`). The page table at push 48 is read by
non-BDA pointer-following stages (`spirv_emit_context.cpp:1257-1307`): direct has none, so such stages are skipped.

## 8. The caches

Beside the executable. A file of another stamp, format or driver is made again from its start, never read.

| File | Records keyed by | Made again when |
|---|---|---|
| `hookshot_direct_programs.bin` (graphics variants: SPIR-V, Info, answers) | code pair, then the variant's key | `HOOKSHOT_DIRECT_SHADER_STAMP` changes (the Eden translator tree; the `// <stamp graphics>` and `// <stamp shared>` regions of `direct_shader.cpp`; `direct_state.h`; `direct_spv_late.*`), `kDiskFormat`, the device's subgroup answers, the depth mode, the translation switches (`nolate`, `latevertex`) |
| `hookshot_direct_compute.bin` (compute variants) | compute code key | `HOOKSHOT_DIRECT_COMPUTE_STAMP` changes (the translator tree; the `// <stamp compute>` regions of `direct_compute.cpp`; the shared regions), `kDiskVersion`, the device's subgroup answers |
| `hookshot_direct_shader_objects.bin` (driver binaries, graphics and compute) | hash of the create info: stage, next stage, flags, subgroup size, SPIR-V, heap mappings | the driver's `shaderBinaryUUID` or `shaderBinaryVersion` changes; a binary the driver refuses is compiled again from SPIR-V |

Code that decides a translation's output belongs inside a stamp region (`nx2/build.py` hashes the regions; markers that do
not pair stop the build). The workers, stats, cache file I/O and the draw side are outside: editing them keeps the caches.
A change to a record's layout raises `kDiskFormat` / `kDiskVersion`.
