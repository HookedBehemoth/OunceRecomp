# M3b report: the translator's step B and the draw path's cost (2026-11-12)

Branch `agent/m3b` (from native-vk 148ba5a; M3a not landed when measured). Runs in `work/runs/m3b_*` (local), RX 6950 XT,
RADV, `RUN=timing`, one game process at a time, chisel overlays (`chisel_kokiri`, `chisel_hf_market`), pacing off
(`HOS_FPS_MAX=240`) unless named. Profiles: Superluminal (8 kHz, a 30 s window of steady play), kept in the scratchpad.

## What was built

| Part | Files (lines) |
|---|---|
| The fork | `gpu/direct/eden` (243 files, 163 `.cpp`, 48,000 lines, 2.6 MB): Eden's recompiler, sirit and the compat headers copied from `gpu/vk`; 15 files differ from the frozen copy (+267 / -45 lines). Every `getenv` (32) answers "unset" (`common/no_env.h`), as the frozen back end runs. Built into `dx_eden_*` objects for `hookshot_direct.so` only (`nx2/build.py` +26: the direct flags swap the frozen include folders for the fork's; its own stamp hashes the fork). A full build of the fork: 28 s wall, 4.5 min CPU at 12 jobs. The frozen back end's objects are byte-identical (all 217; `vk_menu.o` carries the tree's source stamp, as for any edit) |
| Step B in the translator | `texture_pass.cpp` (`HostTranslateInfo::heap_handles`): every texture and image instruction keeps its handle (a bound one's from its constant buffer word, a bindless one as the program computes it); no handle programs, no "failed to track" errors. SPIR-V back end (`Profile::heap_handles`): each texture descriptor a runtime array of its image type, two runtime sampler arrays (plain, depth compare), indexed by the handle's id and sampler bits, non-uniform where not constant; storage images the same; every stage by address (an undeclared address bank declared; a stage with global memory and no storage buffer by address too) |
| Programs | `direct_shader.cpp` (1,154 → 1,206): heap mappings are constant-offset arrays (no indirect indices, no pushed bank addresses: push data 128 B); `Env` answers storage-pointer questions and game-buffer words from the program alone; variants shared by every program slot of the same code (`Family`); counters for second variants, uniform buffers a variant reads, shader-object and cache-read time, the last miss |
| Heap | `direct_heap.cpp` (515 → 560): 7 storage-image regions beside the 7 sampled ones (116 MB resource heap), written at registration for images made with STORAGE usage (one level, no swizzle, a storable format) |
| Draw path | `direct_draw.cpp` (918 → 1,050): thread counters; layer-0 spans and uniform addresses cached per list binding; inline updates as a per-list shadow, a draw's copy only the range its program reads (zeros past the bound range when the size is static) |
| Outside my files (small, additive) | `direct_pools.cpp` +8 (window lookup without the mutex once made), `direct_assets.cpp` +20 (the block-size fix), `direct_image.{h,cpp}` +17 (`storage` bit, `image_key_describe` for logs) |

## Done-items

| Item | Runs | Kokiri | Hyrule Field |
|---|---|---|---|
| No "handle program", "not by address", "storage image" skips | `m3b_c1_k_cold`, `m3b_c1_h_cold` | 0 of each (was 31.88 handle-program job draws) | 0 (was 17.00) |
| Translator self-check: step A (native-vk's build, frozen translator) against step B, same pacing, `HOS_DBG=noassets`, dumped at the same present; thumbnail mean abs. difference 0-255 | `m3b_pt_{a,b}` (title, present 2700), `m3b_pk_{a,b}` (2400), `m3b_ph_{a,b}` (2400) | title: UI pass 0.03, composite 0.11 (15 + 1 draws both). Kokiri: UI 0.00, composite 0.01, median of 136 targets 0.00, max 0.24 | UI 0.02, composite 0.04, median 0.00. Intended or unrelated difference: the probe faces (128x128 and their mips, max 25.6): the two frames rendered different faces (A two, B three), so they are not compared |
| Draw path, ms a frame (pacing off) | before `m2b_ceil_*`, after `m3b_c1_*_cold` | **1.10 → 0.488** | **1.85 → 0.855** |
| `hsd_emit`, all recording threads | same | 1.88 → 1.28 | 2.92 → 1.90 |
| Frame time, mean (p99) | same | 10.73 (12.71) → 10.35 (12.32) | 11.57 (13.72) → 10.90 (13.00) |
| The three title textures drawn | `m3b_b3_title` (drawlog), TBD dump | ids 1544/1545/1558/1559 (683x683 and 341x341 RGTC1) sample their prepared BC4 images; the placeholder only for `DummyTextureForEvent` (no record, A-rt) | |
| Validation | TBD | | |
| Cold and warm start | TBD | | |

## Skip table (draws a frame; job lists only resolve until M3a)

| Reason | Kokiri before (`m3b_base_k_warm`) | after (`m3b_c1_k_cold`) | HF before (`m2b_ceil_hf`) | after (`m3b_c1_h_cold`) |
|---|---|---|---|---|
| job: step A cannot draw (texture handle from a handle program) | 31.88 | **0** (row gone) | 17.00 | **0** |
| job: resolved, would draw (no command buffer: M3a) | 171.88 | 203.75 | 645.23 | 647.20 |
| job: mesh (M4); under a render condition (M5) | 41; 114 | 41; 114 | 47; 316 | 47; 316 |
| layer: drawn; mesh; no targets | 135.3; 2; 0.83 | 132.7; 2; 0.83 | 392.9; 2; 0.84 | 393.9; 2; 0.83 |
| gate: drawn | 1 | 1 | 1 | 1 |
| "not by address", "storage image", texel / image buffer, uniform array, push data over the limit | 0 | 0 | 0 | 0 |

No variant of any program seen (title, Kokiri, HF) has a step-A reason left.

## The draw path's profile

The draw path is `draw_emit` (the counter M2b reported). Before: Superluminal on the fork-only build (`fb304af`, behaviour
as M2b), Kokiri, 30 s of play (2,320 frames; 1.21 ms a frame by the counter under the profiler). Inclusive ms a frame:

| Before (Kokiri) | ms | What it was |
|---|---|---|
| `draw_emit` | 1.068 | |
| `bind_data` → `ubo_address` | 0.624 → 0.609 (0.164 own) | inline updates: each new update made a copy of the whole buffer (up to 64 KB), a byte-by-byte coverage vector, and a guest read of the rest (`nvn_gpu_read` 0.23, `memcpy` / `memset` 0.46 of it all) |
| `layer0_span` + `layer0_address` | 0.081 + 0.072 | a global mutex per vertex, index and uniform buffer of every draw |
| `count` | 0.061 | global atomics bumped by every draw on every recording thread |
| `bank_fill`, `shader_pick`, `set_dynamic`, `arena_alloc`, `set_vertex_input` | 0.052, 0.040, 0.031, 0.024, 0.017 | arithmetic |
| RADV (every call of the process) | 0.085 | |

TBD after

## Settled by evidence

- **The title's placeholder textures were not an ordering bug.** A log at registration (`direct: heap: id N registered with
  the placeholder ...`, `image_key_describe`) showed them linked to their record (record 1, 2, ...) yet keyed "an asset with
  no record": `assets_desc` failed because `block_of` handed the record's format field, which holds the BNTX code
  (`0x1d01`), to the NVN block-size lookup, which knows no such code. Every *copied* record (natively BC or uncompressed
  textures: kinds `copy_bc` and `copy`) failed so; the converted ASTC ones did not. Fixed at the source: `bntx_block`, the
  converter's own table (`nx2/prepare_textures.py` FMT). The m2b report's guess (registration before the link) was wrong:
  the entries were written with the right key, the key was wrong. In the title run 3 placeholder registrations remain
  (`DummyTextureForEvent`, no record; 2 ASTC loads with no data), against hundreds before.
- **Second variants: none.** Title, Kokiri and HF: 0 variants past a program's first, for other inputs (vertex attribute
  or colour types) or other answers. Kokiri sets 12,528 programs of 5,945 distinct codes and draws 152 variants (304
  shader objects); HF 13,247 / 6,117 / 302. Before the variants were shared by code, the warm Kokiri start made 1,446
  variants and 2,904 shader objects from the cache for those 149.
- **Translation and the bound state.** What still reads it: the vertex attribute types and the targets' colour types (the
  key, known at the draw), and the answers of textures whose handle is a driver-bank word (from the list's state, no guest
  read). Game buffer words asked by the translator: 0 (`game buffer words asked at translation`).

## Deviations from the contract

- **Heap-indexed sampling** is done with descriptor arrays the heap mapping places (`HEAP_WITH_CONSTANT_OFFSET`, stride 64 B,
  one array per texture descriptor at its view type's region; samplers two arrays), not with `SPV_EXT_descriptor_heap`'s
  untyped heap access: sirit has no untyped-pointer instructions, and the array form needs none. Same effect: the shader
  indexes the heap with the handle it read, the CPU reads no handle word.
- **Texture answers.** A handle the shader computes (handle program, dynamic index, two words) or reads from a game buffer
  has no answer the draw could check without reading guest memory: its texture is translated as its instruction says (2D
  unless the instruction fixes the type; float). Only driver-bank words (c0/c2) give answers, checked at the draw against
  the per-id answer byte (arithmetic, no lock). No case of a second variant was seen (below).
- **Storage pointers** are answered from the program: a word of the driver bank's storage table is a storage buffer, any
  other is not and its accesses stay global memory, read by address all the same (both through the window table).
- **Not done (M4, compute and mesh are skipped in M3):** the compute grid from `NumWorkgroups`, the 0x880 pointer tables,
  the task stage as a compute step. Texel and image buffers get no heap entries (none in any graphics program seen; still
  a counted skip reason).
- **The address table stays** (`INDIRECT_ADDRESS`, one pushed address); see "Settled".
- **Variants are shared by program code**, not kept per NVN serial (a `Family` per pair of stage code keys).
- **The fork** does not copy suyu's texture decoders: nothing in the translator uses them, and the direct back end has no
  decoder by plan.

## Open problems

- **Job draws are still only resolved** (M3a records them). The draw path numbers are for layer and gate draws recorded
  plus job draws resolved; with jobs recorded each job draw also binds its data and sets its state, so the path grows by
  roughly the layer draws' per-draw cost (about 1.6 µs) times the job draws (204 Kokiri, 647 HF). The changes here (no
  locks, no shared counters, copies of the used range) are what that path runs on.
- **The pictures of the newly drawable job draws** (handle-program textures) cannot be checked until jobs execute (M3a).
  The translation is exercised (152 / 302 variants, 0 failed) and the UI passes match.
- **Texture answers of computed handles** are the instruction's (2D, float): an integer texture behind a handle program
  would be sampled as float. None is known; nothing counts it (the CPU cannot see the handle).
- **Uniform buffers past their bound range**: a program that may read past it reads the guest memory after the buffer
  (Maxwell reads zeros). Counted (below); the pictures cannot tell yet.
- **All programs before play**: see "Settled".
