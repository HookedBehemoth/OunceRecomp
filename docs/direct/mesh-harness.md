# The game's mesh stages outside the game

## Summary

Verified (each by a check that could have failed):

- **A capture of one frame's mesh draws** (`HOS_DBG=meshcap:<present>`, a debug aid) and **a standalone harness**
  (`tools/meshlab`, built apart from the game) that draws them with the stages the back end made, at the game's own guest
  addresses (sparse windows as layer 0), and compares two mesh stages by the geometry they emit, bit for bit. The
  translator's stage against itself: 110 of 110 market draws equal, no fault.
- **Decompiled sources that emit exactly the translator's geometry** (`gpu/direct/shaders/mesh/`, committed, not used by the
  game): the depth-only meshlet stage (game stages `26fa8419`, `6942222e`, `ef1f1c50`) and the lit meshlet stage
  (`e67ab188`: clip position, view-space normal and tangent, world position, handedness, texture coordinates). **69 of 69
  captured draws equal** (market 43, Hyrule Field 24, Kokiri 2), every output of every vertex a triangle uses.
- **Isolated, the market's lost job does not hang.** The job command buffer the breadcrumbs named (22 task+mesh draws of 11
  programs, `a437aea2`, `bee3d34e`, `cfebb562` among them) back to back in one rendering with **no barrier**, with the
  translator's stages, survived every configuration tried, adding in turn the real fragment stages, a secondary command
  buffer, the render condition, an occlusion query, the task steps run again before each submission, vertex draws in the same
  rendering, 2,200 draws a submission, and the whole frame's 110 mesh draws: between 2.1 and 7.7 million draws in 30-60 s a
  run, none lost (the game loses the device in 22-111 s). So **two overlapping native mesh draws of these stages are not
  enough for the loss**; something else of the game's command stream around them is needed (section 4).
- **The inventory** (section 1): 51 distinct mesh stages in the three scenes (market 47, Hyrule Field 46, Kokiri 34; 22 of
  task+mesh programs, 29 mesh-only), 47 still distinct with every literal masked (a stage per material), about ten families
  by what they do. **None has a loop**; only two have private memory of the game's own (native local memory: `55e5c685`
  208 bytes, `8d46218c` 16 bytes, neither in a job that hung); six write storage (statistics atomics).
- **Scratch** (the coordinator's lead): RADV's compile of every market stage was read (`RADV_DEBUG=shaders`). `a437aea2`, the
  stage the breadcrumbs named most, uses **no scratch** (153 VGPRs, 30,826 bytes of LDS). `bee3d34e` and `30dcbaf5` keep
  their outputs in **RADV's mesh scratch ring** (past the 30 KB of NGG LDS), as do four mesh-only stages; `cfebb562` also
  spills VGPRs. The private memory comes from the compiler (spills, the ring), not from the game's code (no native local memory
  in these stages) nor from M4c's conversion (its header and payload reads are loads; M6d: no private or function variable
  in the implicated stages' SPIR-V); and a mesh stage that needs scratch runs correctly in isolation (the scratch probe, 20,000 workgroups). So
  "scratch → hang" does not hold here. What does push the stages over the LDS limit is a translation artefact: the
  translator's stage buffer in workgroup memory on top of the outputs (section 5, 13); the rewritten stages have none.

Suspected, not proven:
- The loss needs a part of the frame the harness does not replay: the job's own vertex draws (real ones), the frame's other
  command buffers (the culling compute that writes the job's indirect records, the depth prepass), the render condition's
  predicate written by the GPU, real targets and textures, or the game's submission shape (section 4 lists the next steps).

## 1. Inventory

Source: the market capture `m6e_cap2` (present 1200, `HOS_DBG=meshdry,meshcap:1200`: 110 mesh draws, 70 of them task+mesh,
54 programs), each mesh stage through `meshtool` (the translator fork offline, as the game translates it) and RADV's compile
of the captured SPIR-V (`meshlab stats` under `RADV_DEBUG=shaders`). Hyrule Field (`m6e_cap_hf`, present 1500: 82 mesh draws) and Kokiri (`m6e_cap_k`, present 1500: 47) were captured the same way; four of their mesh stages are not in the market (`4de96741` and `5215d3b1` of Kokiri's and Hyrule Field's `563d` task stage, `98a50f93` and, in Hyrule Field only, `ad491a45`, both `2c56` meshlet stages), and 31 are in all three.

**How many distinct mesh stages:** 47 mesh stage codes in the market (18 of task+mesh programs, 29 of mesh-only ones); 51 in the three scenes. With
every literal masked (buffer offsets, attribute counts, constants), 43 are still distinct: the game's compiler made a
stage per material, and they differ in which streams and outputs they have, not only in constants. By control shape (the
nesting of branches) 36. By what they do, about ten families:

| Family | Stages (market) | Task stage (M0's families) | What it draws | Inputs | Outputs |
|---|---|---|---|---|---|
| **Meshlet, depth-only** | `26fa8419`, `6942222e`, `ef1f1c50` (one source: they differ in the model buffer's slot) | `2c56` (A: meshlet culling) | the occlusion proxies of the main view and the shadow casters: position only | task payload word 4 + its index = the meshlet id; the model buffer (slot 0 or 2): header, meshlet record, packed positions and indices; c3 view and projection, c4 the object's matrix and offset | position; up to 64 vertices, 126 triangles |
| **Meshlet, lit** | `e67ab188` (one uv set), `7d40fcb1` (slot 2, 12 lines: Generic1, Generic2, Generic5.xy; drawn into a 128x128 target), `7161ddc4` (+ a lightmap uv through an atlas table in slot 0), `93f8f2d5` (`a437aea2`'s: + a second uv, a colour, a 4-bit mask of optional streams scanned bit by bit), `4bf9f4d0`, `201388b3` (also samples a texture) | `2c56` | the main view's opaque geometry: buildings, props | as above, plus c5 (the material's uv transform), more record words (one uv origin and exponent pair a uv set) | position, view-space normal and tangent, world position, handedness, 1-3 uv sets (12-40 output lines) |
| **Meshlet, skinned / through pointers** | `40d0c076` (`cfebb562`, `f69141e9`), `6eaef39e` (`bee3d34e`), `072e452f` (`d551405e`) | `2c56` | main-view meshes whose streams are elsewhere (skinned or deformed, by the look of it: not established) | the model buffer's first quadwords are guest pointers to further streams (about 140 global loads), c6 (912 bytes, read by index: a table, perhaps a matrix palette) | as the lit ones (28-33 lines) |
| **Foliage / instances** | `0c9d1cac`, `54000927` | `1813` / `919b` (B': instance and patch lists, 800-word payload) | grass and foliage (M4c: the floor grass) of the main view and the shadow map | the payload's instance and kept item arrays; the instance buffers | 7-27 lines, 96 vertices |
| **8fb3 instances** | `6ba0b411` (an atomic primitive counter), `8d46218c` (5 atomics, 16 bytes of local memory, a texture) | `8fb3` (B) | instanced detail | as B | 7-21 lines |
| **d91e instances** | `9d026703`, `b86297c2` (`30dcbaf5`'s; an atomic counter) | `d91e` (C) | instanced detail; `30dcbaf5` draws 1,019 mesh workgroups from 982 task workgroups | as C, through pointers | 7-25 lines, 128 vertices |
| **Mesh-only, small** | `644dba7a`, `ed01e3e8` (32 vertices, 101 instructions) | none | the 2432x2432 shadow map | a list buffer, c3, c4 | position |
| **Mesh-only, depth** | `214df803`, `64cd30b2`, `d697c8d4`, `aecefbb3`, `b1ae3a82`, `c74d8a29`, `5c93e3f1` | none | the 1600x1600 shadow map | the workgroup id, one or two buffers | position (+ a few) |
| **Mesh-only, lit** | `124d0223`, `fa2e135e`, `c9797657`, `9ff985a4`, `41f04d67`, `32423793`, `2c90c8cb`, `a8370ece`, `ac8b839a`, `04de48c2` | none | main-view geometry of 96-160 vertices a workgroup | one to three buffers | 6-11 lines |
| **Mesh-only, big** | `e3fc8fcf`, `463f4263`, `66c494fe`, `3d33bb73`, `c9ef41ba`, `73f4d0a5`, `d38b0d52`, `55e5c685`, `354dde83`, `b509ae33` | none | main-view geometry through pointers (72-144 global loads), some with storage writes | four or five buffers, c5/c6 tables | 21-26 lines |

Per stage (market). Columns: task stage; captured draws; native instructions; max vertices / primitives; output lines;
payload words; the driver bank's storage slots it reads (0x770 + 16 a slot; `w` written); global loads (through computed
addresses); storage writes and atomics; native local memory (bytes); RADV's LDS (bytes) / VGPRs of the compiled stage; RADV's
mesh scratch ring (outputs past 30 KB of LDS) and VGPR spills; the passes it draws in (`gated`: under a render condition):

| Mesh stage | Task | Draws | Native | V/P | Lines | Payload | Slots | Global | Writes | Local | LDS/VGPR | Ring | Passes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `e3fc8fcf540f7a48` | - | 1 | 2768 | 160/160 | 24 | 32 | 0x770 0x780 0x790 0x880 | 144 | - | - | 30592/256 | ring | 1536x864 (1 gated) |
| `463f42630fce83a0` | - | 1 | 2358 | 96/96 | 26 | 32 | 0x770 0x780 0x790 0x7a0 0x880 | 144 | - | - | 28352/256 | - | 1536x864 (1 gated) |
| `66c494fe4cfa930b` | - | 1 | 2245 | 96/96 | 26 | 32 | 0x770 0x780 0x790 0x7a0 0x880 | 144 | - | - | 28352/256 | - | 1536x864 (1 gated) |
| `3d33bb73d3deee34` | - | 1 | 2239 | 96/96 | 26 | 32 | 0x770 0x780 0x790 0x7a0 0x880 | 144 | - | - | 28352/256 | - | 1536x864 (1 gated) |
| `c9ef41badee35012` | - | 1 | 1510 | 128/128 | 25 | 32 | 0x770 0x780 0x790 0x880 | 72 | - | - | 29088/256 | ring | 1536x864 (1 gated) |
| `73f4d0a5847272ca` | - | 1 | 1509 | 128/128 | 25 | 32 | 0x770 0x780 0x790 0x880 | 72 | - | - | 29088/256 | ring | 1536x864 (1 gated) |
| `d38b0d52a94ab5a8` | - | 1 | 1481 | 128/128 | 23 | 32 | 0x770 0x780 0x790 0x880 | 72 | - | - | 30112/256 | ring | 1536x864 (1 gated) |
| `55e5c68554e506d6` | - | 1 | 1471 | 96/96 | 25 | 32 | 0x770 0x780 0x790 0x880 | 72 | - | 208 | 26432/256 | - | 1536x864 (1 gated) |
| `354dde838352c262` | - | 1 | 1380 | 96/96 | 21 | 32 | 0x770 0x780 0x790 0x7a0w 0x880 | 72 | 1 atomics, 10 stores | - | 21824/256 | - | 1536x864 (1 gated) |
| `b509ae330829ce02` | - | 1 | 1290 | 96/96 | 21 | 32 | 0x770 0x780 0x790 0x7a0w 0x880 | 72 | 1 atomics, 10 stores | - | 21824/256 | - | 1536x864 (1 gated) |
| `04de48c28fb15a55` | - | 1 | 760 | 160/160 | 11 | 32 | 0x770 | 0 | - | - | 19712/128 | - | 1536x864 |
| `b1ae3a82b168426b` | - | 2 | 736 | 160/160 | 8 | 32 | 0x770 | 0 | - | - | 15232/96 | - | 1600x1600 depth-only |
| `d697c8d47344648d` | - | 3 | 475 | 96/96 | 8 | 32 | 0x770 0x780 0x880 | 9 | - | - | 9152/80 | - | 1600x1600 depth-only |
| `214df803d2345913` | - | 3 | 473 | 96/96 | 8 | 32 | 0x770 0x780 0x880 | 1 | - | - | 9152/80 | - | 1600x1600 depth-only |
| `64cd30b2d42e4d36` | - | 3 | 473 | 96/96 | 8 | 32 | 0x770 0x780 0x880 | 1 | - | - | 9152/80 | - | 1600x1600 depth-only |
| `aecefbb33958a585` | - | 2 | 452 | 96/96 | 7 | 32 | 0x770 0x880 | 1 | - | - | 7232/80 | - | 1600x1600 depth-only |
| `124d0223201dd752` | - | 1 | 436 | 96/96 | 8 | 32 | 0x770 0x780 | 0 | - | - | 9152/80 | - | 1536x864 |
| `c97976573bc0588d` | - | 1 | 436 | 96/96 | 8 | 32 | 0x770 0x780 0x880 | 3 | - | - | 9152/80 | - | 1536x864 |
| `fa2e135eda00d7c1` | - | 1 | 436 | 96/96 | 8 | 32 | 0x770 0x780 | 0 | - | - | 9152/80 | - | 1536x864 |
| `9ff985a4d0dbf464` | - | 1 | 416 | 96/96 | 7 | 32 | 0x770 | 0 | - | - | 7232/80 | - | 1536x864 |
| `c74d8a29d9aaa03a` | - | 2 | 372 | 128/128 | 7 | 32 | 0x770 0x880 | 5 | - | - | 9632/80 | - | 1600x1600 depth-only |
| `41f04d67fd5aa4d5` | - | 1 | 364 | 128/128 | 10 | 32 | 0x770 0x880 | 3 | - | - | 13216/109 | - | 1536x864 |
| `5c93e3f11d52471d` | - | 3 | 344 | 128/128 | 7 | 32 | 0x770 | 0 | - | - | 9632/69 | - | 1600x1600 depth-only |
| `32423793502c71e7` | - | 1 | 317 | 128/128 | 10 | 32 | 0x770 | 0 | - | - | 13216/81 | - | 1536x864 |
| `2c90c8cbedf50657` | - | 1 | 308 | 128/128 | 7 | 32 | 0x770 0x880 | 1 | - | - | 9632/68 | - | 1536x864 |
| `a8370ece30f3eb42` | - | 1 | 300 | 96/96 | 6 | 32 | 0x770 | 0 | - | - | 6848/64 | - | 1536x864 |
| `ac8b839aadfe397f` | - | 1 | 275 | 96/96 | 6 | 32 | 0x770 | 0 | - | - | 6848/64 | - | 1536x864 |
| `644dba7ae7b8bced` | - | 1 | 101 | 32/32 | 5 | 32 | 0x770 | 0 | - | - | 800/18 | - | 2432x2432 depth-only |
| `ed01e3e8ff1c102b` | - | 1 | 101 | 32/32 | 5 | 32 | 0x770 | 0 | - | - | 800/18 | - | 2432x2432 depth-only |
| `0c9d1cac283fc059` | 1813 | 1 | 1576 | 96/96 | 27 | 800 | 0x770 0x780 0x790 0x880 | 73 | - | - | 27200/256 | - | 1536x864 (1 gated) |
| `54000927a831e80d` | 1813 | 4 | 488 | 96/96 | 4 | 800 | 0x770 0x880 | 1 | - | - | 4544/64 | - | 1536x864, 1600x1600 depth-only |
| `072e452fb9b68991` | 2c56 | 1 | 2450 | 126/126 | 33 | 64 | 0x770 0x780 0x790 0x880 | 141 | - | - | 30922/256 | ring | 1536x864 (1 gated) |
| `6eaef39e69dd118b` | 2c56 | 1 | 2422 | 126/126 | 31 | 64 | 0x770 0x780 0x790 0x880 | 143 | - | - | 29898/256 | ring | 1536x864 (1 gated) |
| `40d0c0768e874600` | 2c56 | 5 | 2131 | 126/126 | 28 | 64 | 0x770 0x780 0x790 0x880 | 139 | - | - | 30378/256 | ring spills | 1536x864 (5 gated) |
| `201388b3ce37de7f` | 2c56 | 4 | 1637 | 126/126 | 40 | 64 | 0x770 0x780 0x790 0x880 | 4 | - | - | 30474/256 | ring | 1536x864 (4 gated) |
| `93f8f2d5b744b527` | 2c56 | 2 | 1172 | 126/126 | 21 | 64 | 0x770 0x780 | 0 | - | - | 30826/154 | - | 1536x864 (2 gated) |
| `4bf9f4d01e8d7d8a` | 2c56 | 2 | 1156 | 126/126 | 24 | 64 | 0x770 0x780 0x880 | 4 | - | - | 30346/137 | ring | 1536x864 (2 gated) |
| `7161ddc486ae4071` | 2c56 | 2 | 849 | 126/126 | 18 | 64 | 0x770 0x780 | 0 | - | - | 25258/256 | - | 1536x864 (2 gated) |
| `e67ab18860ce163e` | 2c56 | 5 | 737 | 126/126 | 16 | 64 | 0x770 | 0 | - | - | 22218/239 | - | 1536x864 (5 gated) |
| `7d40fcb15243fb89` | 2c56 | 1 | 596 | 126/126 | 12 | 64 | 0x790 | 0 | - | - | 16138/128 | - | 128x128 |
| `ef1f1c50d829bf4e` | 2c56 | 19 | 331 | 126/126 | 4 | 64 | 0x770 | 0 | - | - | 5994/64 | - | 1536x864, 1600x1600 depth-only |
| `26fa8419a38c6a41` | 2c56 | 8 | 328 | 126/126 | 4 | 64 | 0x790 | 0 | - | - | 5994/64 | - | 1536x864, 1600x1600 depth-only |
| `6942222ec7d54fe9` | 2c56 | 11 | 328 | 126/126 | 4 | 64 | 0x770 | 0 | - | - | 5994/64 | - | 1536x864, 1600x1600 depth-only |
| `8d46218ca9ecd4f9` | 8fb3 | 1 | 2840 | 96/96 | 21 | 800 | 0x770w 0x780w 0x790w 0x7a0w 0x880 | 73 | 5 atomics, 0 stores | 16 | 21824/256 | - | 1536x864 (1 gated) |
| `6ba0b4112e97de49` | 8fb3 | 1 | 1856 | 96/96 | 7 | 800 | 0x770 0x780w 0x880 | 1 | 1 atomics, 0 stores | - | 7232/78 | - | 1536x864 |
| `b86297c24c8d8c7a` | d91e | 1 | 1368 | 128/128 | 25 | 288 | 0x770 0x780 0x790 0x7a0w 0x880 | 77 | 1 atomics, 0 stores | - | 29088/256 | ring | 1536x864 (1 gated) |
| `9d026703ad5da78c` | d91e | 1 | 408 | 128/128 | 7 | 288 | 0x770 0x780w 0x880 | 5 | 1 atomics, 0 stores | - | 9632/64 | - | 1536x864 |

Loops: none in any of them (IR). Private memory of the game's own: two (above). Not checked per stage: the fragment stages
(M6d: one loop each in the implicated programs, bounded by a uniform word).


## 2. The decompiled sources

In `gpu/direct/shaders/mesh/` (committed, by the user's decision of 2026-11-13; not used by the game):

| File | What |
|---|---|
| `common.glsl` | The back end's interface for a mesh stage, as the translator's stages use it: the push data (task header at 64, window table at 88, the virtual range at 96-119), a guest address to a device address (layer 0's windows, the translator's alignment kept), the task header's map entry and payload (`task_input`, `payload_word`), NVN's clip depth to Vulkan's (`vulkan_clip`), a bit-stream field reader |
| `meshlet.glsl` | The game's meshlet format (the model buffer's header, the meshlet record, the vertex stream's position, octahedral normal and tangent fields, the texture coordinates) and its decoding, structured by field |
| `meshlet_depth.mesh` | The depth-only meshlet stage: position only (stages `26fa8419a38c6a41` slot 2, `6942222ec7d54fe9` and `ef1f1c50d829bf4e` slot 0) |
| `meshlet_lit.mesh` | The lit meshlet stage with one texture coordinate set: clip position, view-space normal and tangent, world position, handedness, texture coordinates (stage `e67ab18860ce163e`) |
| `codes.txt` | Which game stage each source replaces, with its defines; `tools/meshlab/build_shaders.sh` builds them (glslang, Vulkan 1.3, `spirv-val` clean) |

Comparison results (market capture `m6e_cap2`, present 1200; `meshlab compare`, exact, every triangle's corners' attributes
and every primitive):

| Replacement | Game stage | Captured draws | Result |
|---|---|---|---|
| `meshlet_depth.mesh` (MODEL_SLOT 2) | `26fa8419a38c6a41` | 8 | 8 equal |
| `meshlet_depth.mesh` (MODEL_SLOT 0) | `6942222ec7d54fe9` | 11 | 11 equal |
| `meshlet_depth.mesh` (MODEL_SLOT 0) | `ef1f1c50d829bf4e` | 19 | 19 equal |
| `meshlet_lit.mesh` (MODEL_SLOT 0) | `e67ab18860ce163e` | 5 | 5 equal (all 16 output lines) |

RADV's compile of the replacements (`meshlab stats --repl` under `RADV_DEBUG=shaders`): the lit stage needs **12,490 bytes
of LDS** (the translator's `e67ab188`: 22,218), the depth-only one 2,410 (5,994); neither uses the mesh scratch ring or
scratch. The difference is the translator's stage buffer in workgroup memory (section 5, 13).

What is a transcription of the game's logic (for the user's decision what to keep): the meshlet layout (the model buffer's
header words, the record's fields and their bit positions, the order of the vertex record's fields, the index stream), the
dequantisation and decoding arithmetic (the octahedral decode with its 32/31 factor, the texture coordinates' exponents),
the transforms' rows and the order of their multiplies and adds (kept so that the results are bit-exact), and the limits
the game's code has (2 vertices and 4 triangles a thread). What is ours: the structure, the names, the comments, and the
back end's interface (`common.glsl`). The translation's artefacts are not carried over (section 5).

Not decompiled (the inventory says what they are): the lit stages with more streams (`7161ddc4`, `93f8f2d5` = `a437aea2`'s,
`4bf9f4d0`, `201388b3`, `7d40fcb1`), the skinned ones that read through pointers (`40d0c076`, `6eaef39e` = `bee3d34e`'s,
`072e452f`), the foliage and instance families of the other task stages (`1813`/`919b`: `0c9d1cac`, `54000927`; `8fb3`:
`6ba0b411`, `8d46218c`; `d91e`: `9d026703`, `b86297c2` = `30dcbaf5`'s) and the 29 mesh-only stages. What was learned of
their formats is in section 1; the method (meshtool's flat print, `simplify.py`, a draft, `meshlab compare` until equal)
took about an hour a stage of `e67a`'s size.


## 3. The tools

| What | Where | How to run |
|---|---|---|
| The capture: one frame's mesh draws (every task+mesh and mesh-only draw: program, push data, uniform buffers' bytes, the task and mesh stages' storage buffers, the 4 MB after each pointer in their first 4 KB, the task header, map and payload as the GPU left them, pass formats, the dynamic state; the frame's other draws and task steps listed in order) and every mesh program's stages (SPIR-V, heap mappings, c1) | `gpu/direct/direct_meshcap.{h,cpp}`, `direct_meshcap_format.h`; hooks in `direct_draw.cpp`, `direct_shader.cpp` (`finish`), `direct_task.cpp`, `direct_present.cpp` | `HOS_DBG=meshdry,meshcap:1200` (meshdry: no mesh draw drawn, so the capture run cannot lose the device); writes `meshcap/frame.bin` in the run's folder (market: 244 MB) |
| The harness | `tools/meshlab/meshlab.cpp`, `capture.h`, `spv_dump.{h,cpp}`; built by `tools/meshlab/build.sh` into `work/meshlab/` (system Vulkan, nothing of the game linked) | `meshlab <frame.bin> list`; `extract <dir>`; `compare <seq...> --repl <dir>`; `dump <seq>`; `run --job <seq> [--barrier none/local/full/split] [--fs simple/real] [--secondary] [--cond once/draw] [--query] [--tasks] [--vs N] [--frame] [--seconds T] [--watchdog S]`; `stats`; `probe <spv>` |
| A GPU run under the lock with the hang rules (3 minutes after the last reset before it starts, `vulkaninfo` checked, the lock held through a recovery, the kernel log saved) | `tools/meshlab/gpu_run.sh` | `tools/meshlab/gpu_run.sh <out.txt> <meshlab args>` |
| The offline translator: a mesh stage (with its task stage) from the seed through the translator fork as `direct_shader.cpp` does it: IR, a structured print, a flat print, SPIR-V, a summary | `tools/meshlab/meshtool.cpp`, `ir_pretty.h` (built by `build.sh` from `work/sa/build/obj/dx_eden_*.o`) | `meshtool <7_code.bin> <prefix> [6_task.bin]` (stage files from M0's `seed.py`) |
| The inventory and the reading aid | `tools/meshlab/inventory.py` (every mesh stage of a frame through meshtool, families by masked IR and by control shape), `simplify.py` (a flat print with the lowering's noise folded: XMAD multiplies, the uniform-register loads, 64-bit funnel shifts, condition codes) | `inventory.py <frame.bin>... --out <dir>`; `simplify.py <prefix>.flat.c` |
| The replacements' build | `tools/meshlab/build_shaders.sh` (glslang, Vulkan 1.3, `spirv-val`), the list `gpu/direct/shaders/mesh/codes.txt` | `tools/meshlab/build_shaders.sh <out dir>` |
| Probes | `tools/meshlab/probes/scratch.mesh` (a mesh stage that needs private scratch), `outputs.mesh` (the game's shape; at its default size RADV keeps its outputs in LDS, 29,290 bytes: a larger SHARED_WORDS is needed to reach the ring) | `meshlab probe <spv> [--groups N]`; `run ... --mesh <spv>` |

How the harness reproduces a draw: the program's mesh (and fragment) stage is made as a shader object from the captured
SPIR-V with the captured heap mappings, the descriptor heaps of the back end's layout all zero (null descriptors), the
uniform buffers copied into buffers of their own (64 KB at least, zeros past the captured bytes) through an address table
pushed at 120 as the back end does, the guest's memory **at its own guest addresses**: window k a sparse buffer of 4 GB -
64 KB over guest [k*2 GB, k*2 GB + 4 GB) with every captured page bound (in both windows that hold it) and nothing else
(reads of the rest are zeros, as in the game's null window), the window table pushed at 88. The task header is rebuilt in
a buffer of its own with the captured map and payload (and room for the whole map, so that `--tasks` can run the task stage
again). How it compares: the mesh stage is rewritten (`spv_dump.cpp`) so that every store to an output and the two counts
of `SetMeshOutputsEXT` are also stored to a buffer, a record a workgroup (a mesh stage may not read its outputs); the two
records are compared triangle by triangle, each corner's attributes by value and each triangle's own, exactly (bit for bit).


## 4. The hang in isolation

What was replayed: the job command buffer the breadcrumbs named (M6c: list kind 2, the main view's 1536x864 lit pass; in
the capture the command buffer with `a437aea2`'s two draws: 22 task+mesh draws of 11 programs, `a437aea2` x2, `cfebb562` x3,
`8b41aa44`, `bee3d34e`, `d551405e`, `d7933378` x2, `26811d11` x4, `691ce175`, `f69141e9` x2, `9d2bef11`, `79ddff6b` x4; every
one under a render condition in the game), each draw with its captured task header, map and payload, back to back in one
rendering of the pass's formats and size, again and again (two submissions in flight), with the watchdog (2 s, 4 s for
long submissions) and top- and bottom-of-pipe markers in host memory. Every run took the GPU lock (`gpu_run.sh`).

| Run | Configuration | Result |
|---|---|---|
| r1 | the job, the framebuffer-local barrier between draws; the first harness (storage relocated, uncaptured memory unmapped) | **device lost** (00:32:16, reset 20): a page fault (`TCP`, permission, address 0x800157455000) in draw 3 (`cfebb562`, which reads through pointers the capture had not followed), then `ring gfx_0.0.0 timeout`. A harness fault, not the game's: fixed (pointers followed in the capture, guest memory at its own addresses in sparse windows, unbound pages non-resident) |
| p1, p2 | the scratch probe (a mesh stage with a 256-word private array: scratch), 1 and 20,000 workgroups | correct (0 of 32 words wrong), no fault |
| c0 | every market draw once, the translator's stage against itself, with the output dump | 110 of 110 equal, no fault |
| r2 | the job, the framebuffer-local barrier (M6d's rule) | survived: 958,000 draws in 20 s |
| r3 | the job, **no barrier**, inline, a fragment stage of ours | survived: 5,734,916 draws in 30 s |
| r4 | + the real fragment stages (heap all null), in a secondary (as a job) | survived: 5,954,146 draws in 30 s |
| r5 | + under conditional rendering (a passing predicate), inside an occlusion query | survived: 5,683,986 draws in 30 s |
| r6 | + each submission runs the draws' task stages first (headers reset, the task compute, a barrier: the job's task step) + 2 vertex draws before each mesh draw in the same secondary | survived: 2,793,340 draws in 30 s |
| r7 | the whole frame: all 110 mesh draws, each command buffer's in a rendering of its own targets, in a secondary, task steps, real fragment stages | survived: 2,131,250 draws in 30 s |
| r8 | the job as r5 + vertex draws, 100 times a submission (2,200 mesh draws a submission), and an inline vertex draw in the primary before the secondary, the rendering begun with inline contents allowed (as the back end's layer lists) | survived: 7,689,000 draws in 60 s |
| r9 | our probe of the game's shape (`probes/outputs.mesh`: 126 vertices of 9 vec4 outputs, 2,688 words of workgroup memory) for every draw of the job, 20 times a submission. RADV compiled it to 29,290 bytes of LDS: just under the ring's threshold, so it did **not** exercise the mesh scratch ring as meant (not rerun) | survived: 16,746,840 draws in 20 s |

**The hang does not reproduce in isolation.** Two (and 22) task+mesh draws of these mesh stages overlapping in one rendering,
with every game detail the harness replays, ran 30-60 s each without a loss, against 22-111 s to a loss in the game (12 of
12 market runs, M6c). So the translator's mesh stages and their overlap are not enough; what the harness does not replay is
the rest of the game's command stream around the job:
- the job's own vertex draws (the 28 indexed draws before its mesh draws: real vertex shaders, vertex buffers, the game's
  pipelines' state) and the layer list's inline draws (the harness draws its own),
- the other command buffers of the frame (copies, compute, the culling dispatches that write the indirect records, the
  depth prepass whose depth the job tests against: the harness clears depth to 1),
- the render condition's predicate written by the GPU in the same frame (the harness's is a constant),
- the real targets' contents and the real textures (the harness's heap is null descriptors),
- the game's submission shape (one big submission a frame of about 25 chunks) and its timing.

The minimal pair cannot be named from the harness: no pair, and no single job, hangs in it. The next step is a capture of
the job's whole command stream (every command of the command buffer, not only the mesh draws: `meshcap` lists them but
records only the mesh draws) replayed in the harness, then reduced; or, in the game, the opposite reduction (M6d's
domain): leave out classes of commands around the job (`HOS_DBG=qnocond` for the render condition, the job's vertex draws)
until the loss goes, with one loss allowed per configuration.

Device losses of this stage: **one** (r1, 00:32:16, reset 20; a harness fault, kernel log `work/meshlab/runs/r1_job_local_kernel.txt`;
recovery verified by `vulkaninfo` and 3 minutes before the next GPU run). No configuration lost the device twice. The other
resets of this boot (19 and before) were other agents' game runs.


## 5. Oddities

Each says whether it is the game's code (G: in the native code, read from the IR the translator makes of it; the IR follows
the native instructions one for one, so a pattern there that the translator does not add is the game's) or the
translation's (T), and what it does on this GPU.

| # | What | Whose | Seen in | Effect |
|---|---|---|---|---|
| 1 | Threads past the meshlet's vertex or triangle count do not branch away: each takes `min(its index, count - 1)` and writes that vertex or triangle again, the same values | G | every `2c56` meshlet stage | several invocations store the same output; harmless (same values); the rewrite writes each once |
| 2 | The meshlet stages write at most 64 vertices (2 a thread) and 128 triangles (4 a thread), while they declare 126 and the record's counts are 8 bits (up to 255): vertices 64..125 are never written, triangles past 126 are cut by the output count | G | `26fa`, `6942`, `ef1f`, `e67a` (and the other 2c56 stages, by their shape) | a meshlet of more than 64 vertices would draw its triangles past vertex 63 to zero vertices (both the translator and the rewrite give zeros there); whether a captured meshlet has more than 64 was not checked |
| 3 | A triangle's vertex numbers are `findMSB(vertices - 1) + 1` bits wide: 0 bits for a 1-vertex meshlet, 32 for an empty one (`findMSB(0xffffffff)`), and only the low 8 bits are kept | G | the 2c56 stages | no out-of-range index: the translator clamps indices to 125 too (T, epilogue) |
| 4 | The normal's field is `(2 * (w5 & 15) + 12) & 30` bits: widths over 15 bits a component would lose bits | G | `e67a` (lit stages) | none in the captures |
| 5 | No mesh stage has a loop (every per-thread repetition unrolled by the game's compiler); data-dependent branches only in the lit stages with optional streams (a 4-bit stream mask scanned bit by bit, `93f8`, `4bf9`, ...) | G | all 47 | no runaway: unlike the task stages (M0's B family), nothing in a mesh stage loops on data |
| 6 | Private memory: only `55e5c685` (mesh-only, 208 bytes) and `8d46218c` (8fb3 family, 16 bytes) have native local memory | G | 2 of 47 | neither is in a job that hung |
| 7 | Storage writes in mesh stages: `6ba0b411`, `9d026703`, `b86297c2` (1 atomic each into the c0+0x780/0x7a0 slot: a statistics counter), `8d46218c` (5 atomics on four slots), mesh-only `354dde83`, `b509ae33` (10 stores and an atomic into slot 0x7a0) | G | 6 of 47 | `b86297c2` is `30dcbaf5`'s (one of the stages the breadcrumbs named): an atomic per workgroup into a buffer no later draw of the job reads; the others are not in hanging jobs |
| 8 | Reads through 64-bit guest pointers stored in the first words of the model's buffer (vertex streams elsewhere) | G | `40d0c076` (`cfebb562`), `6eaef39e` (`bee3d34e`), `072e452f`, `0c9d1cac`, `b86297c2`, the mesh-only `3d33bb73`, `463f4263`, `66c494fe`, `e3fc8fcf`, ... (the "global loads" column: about 70-145 a stage) | the harness must capture those ranges too (its first loss came from a draw of `40d0` reading guest memory it had not captured: a page fault, then a ring timeout; fixed by capturing the pointers' targets and mapping unbound memory as non-resident) |
| 9 | A storage buffer "at c0+0x880" in the stage's descriptor list, past the 16 slots of stage 7's table: never read (no GetCbuf at 0x880 in the IR), a leftover of the global-memory pass's candidates; the direct bank holds zeros there | T | 16 stages | none |
| 10 | Uniform loads as "the first active lane's value": a ballot, `FindUMsb(ballot & -ballot)` and a shuffle for every word the native code loads into a uniform register | T (Ampere's uniform datapath lowered) | 39 of 45 task+mesh mesh stages, 5 of 29 mesh-only (M6d's count) | a subgroup shuffle per uniform value; the rewrite loads the value once (it is the same for every thread) |
| 11 | IMAD lowered to XMAD: every 32-bit multiply as three 16-bit products and a shift | T | all | exact; noise only |
| 12 | Condition-code emulation: `SelectU1` over `Void()` predicates and empty `if` blocks | T | all | no effect on the result (the compare is exact) |
| 13 | The stage buffer (ISBE) in workgroup memory: up to 3,584 words of attributes and 382 of index bytes, zeroed at the start, copied to the outputs at the end behind a barrier; the vertex count set to the maximum (126) always | T | every mesh stage | this memory is on top of the outputs' own LDS: it is what pushes the big stages to 29-31 KB of NGG LDS and, past 30 KB, RADV moves their outputs to its mesh scratch ring (the next row). The rewrite has no stage buffer |
| 14 | RADV's compile of the translated stages: 11 of 54 market programs keep outputs in the **mesh scratch ring** (7 task+mesh: `bee3d34e`, `30dcbaf5`, `d551405e`, `d7933378`, `79ddff6b`, `cfebb562`, `f69141e9`; 4 mesh-only: `d4abbcb4`, `d8e59341`, `c36a4f3f`, `89ca750f`); `cfebb562` / `f69141e9` also spill VGPRs to scratch (256 VGPRs); `a437aea2` neither (30,826 bytes of LDS, 153 VGPRs) | driver | `RADV_DEBUG=shaders` on the harness's shader objects (same SPIR-V and mappings as the game's) | not a cause by itself: `a437aea2` is the stage named most often by the breadcrumbs and uses neither; mesh-only stages with the ring never hung |


## 6. What to swap (design; not built)

The user's decision (2026-11-13): the replacements are not swapped in for now; this is the design for later.

- **Key**: the game mesh stage's code key (`Code::key`, the hash of its control section and code area; the same key the
  inventory, `meshtool` and `codes.txt` use) together with the program's shape (task+mesh, reading the task header, or
  mesh-only). A replacement is made for one shape.
- **Prepared offline** (rule 4): `build_shaders.sh` makes `<code>.spv` for each `codes.txt` line; a packing step writes them
  with each one's interface into one file the back end reads at start (`hookshot_direct_mesh.bin` beside the executable, or
  in the prepared store): per record the code key, the shape, the stage's uniform buffers it declares in order (the
  translator's `constant_buffer_descriptors`: buffer index and used size), the storage-buffer slots, the per-vertex and
  per-primitive output lines (`MeshInfo::vertex_lines` / `primitive_lines`, which the fragment stage's inputs and the
  per-primitive generics are translated against), max vertices and primitives, the SPIR-V, and a hash of it.
- **Where it hooks**: `direct_shader.cpp`, `translate_task_mesh_variant` and `translate_mesh_variant`, at the mesh stage's
  `EmitSPIRV`: the translator still runs for the stage (its `Shader::Info` drives `finish()`'s heap mappings, the push
  layout and the fragment stage's runtime info), and `mst.spirv` is the replacement's when one exists for `mc.key` and its
  interface matches. The translator is bypassed for the stage's code only; the task stage (compute) and the fragment stage
  stay the translator's. The program cache's stamp (`disk_stamp`) takes the replacement file's hash, so cached variants of
  replaced stages are made again; the shader-object binary key already hashes the SPIR-V.
- **Mismatch detection** (rule 2: no fallback, a refusal is counted and named): at translation, the replacement's recorded
  interface against the translator's `Info` for this stage (the buffers in order and sizes, the slots, the output lines,
  the counts, the payload words); a difference refuses the replacement for that code (logged with the first difference,
  counted, a strict event) and the variant is drawn as translated. Before packing, `meshlab compare` over the local captures
  (market, Hyrule Field, Kokiri) must report every captured draw of the code equal; a replacement without an equal compare
  is not packed. In play, `HOS_DBG=meshcap` captures a frame of the swapped build too, and `meshlab compare` of the two
  stages on it is the check after a game update.


## 7. Code and tests

Committed on `agent/m6e` (nothing of the game: no SPIR-V translated from it, no captured buffer; the decompiled sources are
our own text, a transcription of the game's logic as section 2 says):

| What | Where |
|---|---|
| `HOS_DBG=meshcap:<present>` (a debug aid; unset: one load and a branch per mesh program made and per draw) | `gpu/direct/direct_meshcap.{h,cpp}`, `direct_meshcap_format.h`; calls in `direct_draw.cpp`, `direct_shader.cpp`, `direct_task.cpp`, `direct_present.cpp`; `DrawProg::task_code` |
| The harness and its tools | `tools/meshlab/` (`meshlab.cpp`, `capture.h`, `spv_dump.{h,cpp}`, `meshtool.cpp`, `ir_pretty.h`, `inventory.py`, `simplify.py`, `build.sh`, `build_shaders.sh`, `gpu_run.sh`, `probes/`) |
| The decompiled mesh stages | `gpu/direct/shaders/mesh/` (`common.glsl`, `meshlet.glsl`, `meshlet_depth.mesh`, `meshlet_lit.mesh`, `codes.txt`) |

With `HOS_DBG` unset nothing changes. Tests:
`HOOKSHOT_VULKAN_HEADERS=/usr/include work/venv/bin/python -I -B tests/run_all.py`: 295 passed, 5 failed (the baseline five),
19 skipped. Local, not committed: the captures (`work/runs/m6e_cap2`, `m6e_cap_k`, `m6e_cap_hf`, `m6e_cap1` the first),
the harness's builds and outputs (`work/meshlab/`: `runs/` the GPU runs' outputs and the loss's kernel log, `inv_mk/`,
`inv_all/` the IR and prints of every mesh stage, `trans_mk2/` the captured stages' SPIR-V, `repl/` the replacements'
SPIR-V), a local SPIRV-Cross build (`work/deps/SPIRV-Cross`, used only to read stages while working).
