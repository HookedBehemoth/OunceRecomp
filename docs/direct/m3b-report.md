# M3b report: the translator's step B and the draw path's cost (2026-11-12)

Branch `agent/m3b`, rebased onto native-vk with M3a (8ec9439): jobs are drawn. Runs in `work/runs/m3b_*` (local), RX 6950 XT,
RADV, `RUN=timing`, one game process at a time, `chisel_kokiri` / `chisel_hf_market`, pacing off (`HOS_FPS_MAX=240`)
unless named. Profiles: Superluminal, 30 s of steady play.

## What was built

| Part | Files (lines) |
|---|---|
| The fork | `gpu/direct/eden` (243 files, 163 `.cpp`, 48,000 lines, 2.6 MB): Eden, sirit and the compat headers copied from `gpu/vk`; 16 files differ from the frozen copy (about +300 / -45). Its 32 `getenv` calls answer "unset" (`common/no_env.h`), as the frozen back end runs. Built into `dx_eden_*` for `hookshot_direct.so` only (`nx2/build.py` +26: the direct flags swap the frozen include folders for the fork's; the stamp hashes the fork). Full build of the fork: 28 s wall, 4.5 min CPU at 12 jobs. The frozen library's 217 objects are byte-identical (only `vk_menu.o`, which carries the tree's source stamp, differs, as for any edit) |
| Step B, translator | `texture_pass.cpp` (`HostTranslateInfo::heap_handles`): every texture and image instruction keeps its handle (a bound one's from its constant-buffer word, a bindless one as the program computes it); no handle programs. SPIR-V back end (`Profile::heap_handles`): each texture or image descriptor is a runtime array of its image type, plus two runtime sampler arrays (plain, depth compare), indexed by the handle's id and sampler bits, non-uniform where not constant. Every stage by address (an undeclared address bank is declared; a stage with global memory and no storage buffer goes by address too) |
| Programs | `direct_shader.cpp` (1,154 → 1,210): constant-offset heap mappings (no indirect indices, no pushed bank addresses: push data 128 B); `Env` answers storage-pointer questions and game-buffer words from the program; variants shared by every slot of the same code (`Family`); counters |
| Heap | `direct_heap.cpp` (515 → 565): 7 storage-image regions beside the 7 sampled ones (116 MB), written at registration for images made with STORAGE usage |
| Draw path | `direct_draw.cpp` (918 → 1,080), `direct_state.cpp` (+40): see the profile |
| Outside my files (small, additive) | `direct_pools.cpp` +8 (window lookup without the mutex once made), `direct_assets.cpp` +17 (block size), `direct_image.{h,cpp}` +17 (`storage` bit, `image_key_describe`) |

## Done-items

| Item | Runs | Kokiri | Hyrule Field |
|---|---|---|---|
| No "handle program", "not by address", "storage image" skips | `m3b_r2_k`, `m3b_r2_h` | 0 (job draws: 877 drawn, 50 mesh; M3a alone had 223 step-A draws) | 0 (M3a: 527 step A) |
| Translator self-check: step A (native-vk's build, frozen Eden) against step B, `HOS_DBG=noassets`, same present and pacing; thumbnail mean abs. difference 0-255 | `m3b_pt_{a,b}`, `m3b_pk_{a,b}`, `m3b_ph_{a,b}` (before the rebase, jobs not drawn) | title UI 0.03, composite 0.11 (15 + 1 draws both); Kokiri UI 0.00, composite 0.01, median of 136 targets 0.00 | UI 0.02, composite 0.04, median 0.00. Not compared: probe faces (A drew 2 faces, B 3) |
| Draw path a frame, jobs drawn (M3a's tree → after) | `m3a_fin_*` → `m3b_r2_*` | **4.16 → 1.68 ms** | **7.41 → 3.00 ms** |
| `hsd_emit`; packet expansion (`hsd_list_call` from jobs) | same | 2.79 → 1.50; 2.34 → 1.14 | 4.87 → 2.76; 4.14 → 1.86 |
| Frame time, mean (p99) | same | 11.89 (13.86) → 10.78 (12.61); M2b with no job drawn: 10.73 | 12.98 (14.87) → 11.71 (13.77); M2b: 11.57 |
| Draw path before the rebase (jobs only resolved; the brief's 1.10 / 1.85) | `m2b_ceil_*` → `m3b_c1_*_cold` | 1.10 → 0.488 | 1.85 → 0.855 |
| The three title textures | `m3b_b3_title` (draw log), `m3b_v1_title` (dump) | the 683x683 / 341x341 RGTC1 textures sample their prepared BC4 images: the logo's glow is drawn. Placeholders left: `DummyTextureForEvent` (no record, A-rt) and two ASTC loads with no data | |
| Validation (sync on) | `m3b_val3_k`, `m3b_val3_hf` | TBD | TBD |
| Cold and warm start | `m3b_c3_*` | TBD | TBD |

## The draw path's profile (Kokiri, jobs drawn, `r1` = after the rebase, before the last cut)

ms a frame, inclusive, under the profiler (2,380 frames). Before M3b the top entries were the inline-update copies of
whole buffers with a byte coverage vector and a guest read (`ubo_address` 0.61 of `draw_emit`'s 1.07, no jobs drawn), a
global mutex in every layer-0 lookup (0.15), global atomics bumped by every draw (0.06): removed (thread counters, a
lock-free window table, spans and uniform addresses cached per binding, a per-list shadow copied for the range the
program reads), and the state generations now move only when a binding changes (fewer bank copies, tables and picks).

| r1, after the rebase | ms | cut in r2 |
|---|---|---|
| `emit_one` (M3a's per-op path) → `pass_emit` 2.22 → `draw_emit` | 1.52 | |
| `bind_data` → `ubo_address` | 0.35 → 0.31 (0.21 own) | the past-range log's global counter was bumped on every call: read first |
| `set_vertex_input` | 0.18 | rebuilt only when attributes, streams or the program's inputs change |
| `set_dynamic` | 0.11 | rebuilt only when a state op, the topology, the program or the pass changed |
| `clock_gettime` (vDSO; the draw timer two calls a draw, M3a's emit timers) | 0.27 | the draw timer on the TSC |
| `shader_pick`, `layer0_span`, `bank_fill`, `span_of` | 0.08, 0.05, 0.03, 0.02 | |
| RADV, every call of the process | 0.41 | |

What remains: per draw, the bank copies and address tables of changed bindings (925 / 981 a frame, Kokiri), the Vulkan
calls, and a secondary's first draw, which must set the heap, the shaders and every dynamic state (RADV inherits none:
M3a's 10.4 µs against 3.0 for later draws); M3a's emit path (`pass_emit` 0.28 own, `hsd_emit` 0.21 own).

## Skip table, draws a frame (`m3b_r2_k` / `m3b_r2_h`)

| | Kokiri | HF |
|---|---|---|
| job: drawn (of them under a render condition, not evaluated: M5) | 877 (318) | 2,434 (886) |
| job: mesh (M4) | 50 | 111 |
| layer: drawn; mesh; no targets | 165.7; 2; 0.83 | 422.0; 2; 0.83 |
| step A reasons (handle program, two-word or array handle, not by address, storage image, texel buffer) | 0 | 0 |

## The lit colour pass and the composite (Kokiri, `m3b_dd2_k` against frozen `m2b_fd_kokiri_b`, present 1800)

- The lit target (B10G11R11F 1536x864) has mean 0.010 (frozen 0.075; M3a's trial merge 0.0067). Its light comes from one
  full-screen draw before any geometry: program `489438d734e2463a` (frozen item 223 alone brings it to 0.075). It is
  drawn in direct, into the same target (rt 29), and adds nothing. Its only texture is a 256x256 RGBA16F target (id 728,
  `0x113c2a5000`), whose alpha matches the frozen one (0.504) but whose colour is 0 (frozen 0.60 / 0.69 / 0.44). That
  target is drawn by `b069ca31d98f98f8` from one texture: a 256x32x16 RGBA16F array (`0x113c19b000`) that nothing has
  written in the direct back end (no draw, copy, clear or texel write) and that no draw of the frozen frame writes: a
  compute output (M4). **Missing for the ground and Link: that compute LUT (the ambient/sky chain), then the geometry's
  own inputs:** the lit key `0x113b648038` draws 318 job draws (14 mesh draws skipped, M4); its fragment stages read 1-4
  storage buffers and global memory (58 draws a frame with 4) and some computed-handle textures, which the culling and
  light compute would fill.
- The composite: after the lit chain (direct 1536x864 `0x10014e00000` mean 0.0089, frozen 0.021), the frozen frame runs
  only three dispatches (`26fe0c70`, `685adb41` 1x1x1, `6637ac1d` 96x54 writing a 1536x864 E5B9G9R9 storage image) and
  the next draw into the 2048x1152 target is the UI. **The scene reaches the composite only through compute (M4)**; in
  direct that target holds the UI (mean 0.0008) and the composite shows the hearts.

## Settled by evidence

- **The title's placeholder textures were not an ordering bug.** A log at registration showed them linked to a record
  yet keyed "an asset with no record": `assets_desc` failed because `block_of` gave the record's format field, which
  holds the BNTX code (`0x1d01`), to the NVN block-size lookup. Every copied record (natively BC or uncompressed) failed
  so. Fixed at the source with the converter's own table (`bntx_block`).
- **Second variants: none** (title, Kokiri, HF): 0 for other inputs, 0 for other answers. Kokiri sets 12,528 programs of
  5,945 codes and draws 333 variants with jobs (152 before); sharing variants by code turned 1,446 cached variants and
  2,904 shader objects of a warm start into one per variant.
- **Push data and the address table.** RADV allows 256 B. After step B the push data is 128 B (Eden's 120 and the table's
  address). A variant reads 8.1 uniform buffers on average (HF 8.5), at most 16; 17 pushed addresses would fit, so every
  variant seen could push its addresses (`PUSH_ADDRESS`), but at the edge: the table stays.
- **Uniform buffers past their bound range:** with jobs drawn, 786 address tables a frame in Kokiri (1,240 in HF) hold a buffer that a
  program may read past its bound size (one program before jobs: `f8d01f6626632433`, c6 read to 688 B, 128 bound). They
  read the guest memory after the buffer where Maxwell reads zeros. Exact handling needs the bytes at record time (a
  guest read) or a game patch binding the full size; not done, and it may matter for the job draws' pictures.
- **Translation and the bound state:** only the key (vertex attribute and colour types) and driver-bank texture answers
  are read; 0 game-buffer words asked. **All programs before play** needs (1) the attribute types ahead of the draw (the
  packets have the vertex state) and float colour types, (2) a choice of what to translate: 5,945 codes set, 333 drawn.
- **Validation** found one real bug: the sampler arrays were mapped with the combined-sampler fields, so every sample read
  sampler 0 (fixed, 8a01258).

## Deviations from the contract

- Heap-indexed sampling uses descriptor arrays placed by `HEAP_WITH_CONSTANT_OFFSET` mappings, not `SPV_EXT_descriptor_heap`
  untyped access (sirit lacks untyped pointers). Same effect: the shader indexes the heap with the handle it read.
- Textures whose handle the shader computes or reads from a game buffer are translated as their instruction says (2D unless
  fixed; float): no answer can be checked without a guest read. Storage pointers: only driver-bank storage words.
- Not done (M4: compute and mesh are skipped): grid from `NumWorkgroups`, the 0x880 tables, the task stage as compute.
  Texel and image buffers get no heap entries (none in a graphics program seen).
- Variants are kept per code, not per NVN serial. The fork leaves out suyu's texture decoders (the translator uses none).

## Open problems

- The draw path is 1.7 / 3.0 ms with jobs drawn: bank copies and address tables per binding change and the secondaries'
  first-draw state are the rest; a per-pass shared bank table and fewer dynamic states a secondary must set are next.
- Uniform buffers read past their bound range (above). Integer textures behind computed handles would be sampled as float
  (none known, not counted).
- The lit pass and the composite wait for compute (M4), above.
