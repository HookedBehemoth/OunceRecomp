# A-rt report: prepared assets at run time (2026-11-12)

Branch `agent/m2art`. Runs in `work/runs/m2art_*` (local), RX 6950 XT, RADV, one game process at a time, `RUN=timing`,
chisel overlays, `RX_BACKEND=direct`. Baseline = the same build with `HOS_DBG=noassets` (every host patch off: M2a's
behaviour).

## What was built

| Part | Files (lines) |
|---|---|
| Store reader, asset images, checks | `gpu/direct/direct_assets.{h,cpp}` (61, 957; ~350 of it the `HOS_DBG` checks and dumps): index read once, frames read with `pread` and decompressed by the vendored zstd (`third_party/zstd`, the renderer's `vk_zstd_*` objects now also linked into `hookshot_direct.so`, `nx2/build.py` +3) straight into the upload's staging, whole mips per upload (16 MB groups); links texture object -> record (armed by a load or rebind), record kept per texture serial |
| Images | `direct_image.{h,cpp}` (+19, +210): an asset image is made at `nvnTextureInitialize` in the record's VkFormat, keyed by the record's content hash and descriptor (so compaction moves and the same texture in other files share one image), filled on the initializing thread before the game can register it (another thread taking the same content waits); `image_swizzle` composes the record's channel map for M2b's views; `image_upload_region` (WriteTexels, terrain, glyphs) with a leading execution barrier; a texture loaded with no data gets a plain image that follows its object |
| Uploads / frames | `direct_cmd` (+10): `upload_end` returns its timeline value, newest kept; `direct_sync` (+8): a submission waits (semaphore) on the newest upload when a new one was sent since the last such wait |
| API | `direct_api.h` (+25): `hsd_assets_open`, `hsd_asset_load` (`hsd_asset_key`), `hsd_asset_rebind`, `hsd_asset_pool`, `hsd_texels`; `host/direct.h`, `rx_backend.c` (+25) |
| Host patches | `host/direct_assets.c` (493): hooks `f_80798690` (load: record by name, descriptor and the record's data offset back to its BNTX header and table entry; copy skipped, x2 = 0), `f_80798b60` / `f_80792bd0` + `f_80798920` (in-place files), `f_807982f0` (rebind), `f_807da480` (compaction: `[worker+0x1f21]` cleared, the movers copy nothing), `f_83537d90` (Replace: counted), SPLICE `f_80c17040` (terrain memcpy), `f_8152f540` + `f_81539300` (glyph cells); `HOS_PREPARED` (default `<exe>/../../prepared`, else `<romfs>/../../prepared`); a 10 s memory line (memfd blocks, mincore of the heap). `nvn_native.c` (+13): packaged data not copied in direct mode, `nvn_tex_live_containing`, tiles held for their texture. `nn_fs.c` (+15): a second overlay root after `HOS_FS_OVERLAY` (direct mode only) |
| Terrain size table | `nx2/prepare_terrain.py` (+61): writes the game's `ResourceSizeTable` into the overlay with every grown file's entry raised (7,460 of 51,233 files grew); `--size-table` alone |

## Done items

| Item | Runs | Kokiri | Hyrule Field |
|---|---|---|---|
| Asset textures filled from the store before first use | `m2art_k10`, `m2art_hf1` | 4,574 loads + 995 in place: 5,420 prepared; 2,744 fills (2,369 MB from 1,939 MB of frames) into 2,729 images, 0 unfilled, 0 failed | 3,698 + 995: 4,566 prepared; 2,200 fills (2,046 MB) into 2,185 images |
| Placeholders (each named in the log) | same | 2 loads with data and no record: `DummyTextureForEvent` (4x4, made in memory) and `CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm` (not in this romfs's `Tex/`); 121-134 ASTC textures loaded with no data (below) | same 2; 122 ASTC |
| Self-check `HOS_DBG=assetcheck:N` (readback vs store / bytes given / atlas memory) | `m2art_k10`, `m2art_dump_k`, `m2art_hf1` | 60 of 60 prepared images equal to the store, 17 of 17 terrain uploads, 7 of 7 glyph atlas checks | 40/40, 18/18, 7/7 |
| No uploads on the Presentation Thread | all | 0 for every source; per thread e.g. sead::MainThread 48 uploads 429 MB 862 ms, BaseProcCreate* and ModuleSystemWorker* the rest | 0 |
| Resource pool (pool 127, 2,432 MB) resident | `m2art_t_*` | 1-8 MB (baseline 1,757 MB) | 19 MB (baseline 1,693 MB) |
| Guest memory (memfd blocks) | `m2art_t_*` | 5,043-5,196 MB (baseline 6,848-6,989); peak RSS 5.0 GB vs 6.8 | 4,559-4,737 MB (baseline 6,295-6,474) |
| VRAM in images | `m2art_k8`, `m2art_hf1` | 3,773 MB, of it assets 2,767 MB | 3,432 MB, assets 2,389 MB |
| Load time (present 1 to the first frame with 50+ job lists) | `m2art_t_{kokiri,hf}_{art,base}{1,2}` | 9.01, 9.08 s (baseline 9.19, 9.32) | 9.08, 8.88 s (baseline 9.28, 8.92) |
| Validation with synchronisation | `m2art_val_k`, `m2art_val_hf` | the 3 known hazards only (vkCmdCopyImage RAW/WAW, vkCmdEndRendering WAR) | the same 3 |
| Frozen back end unchanged | tests, `m2art_frozen` | 291 pass / 5 fail (baseline); 45 s frozen run 60 fps, no asset line | |
| Dumps (PNG, `HOS_DBG=assetdump`) | `m2art_dump_k/assets`, `m2art_hf1/assets` | font atlas ("saving"), 3 terrain tiles (R32F, RGBA8, BC5), 2 UI, 2 BC7 colour, 2 BC5 normal maps | 7 (no glyph atlas or UI picked) |

Terrain: Kokiri 108 tiles (34 MB), Hyrule Field 159 (46 MB), 0 not converted, all uploaded. Glyphs: 7 cells into 1 atlas.

## Content sources (counters)

Uploads a frame in steady play (600-present lines after the load): store 0, terrain 0, glyph 0, WriteTexels 0. At the
load (to the first frame in play, Kokiri `m2art_k10`): store 1,421 uploads 1,424 MB 3,498 ms; terrain 103 (32.7 MB,
13 ms); glyph 0; WriteTexels 42 (0.2 MB). GPU copies 2 and texture clears 2 a frame, 101.8 renderings a frame (pass
counters). Live images by first source (Kokiri): 169 rendered, 4 copied or cleared, 2,729 store, 106 terrain, 1 glyph, 43
WriteTexels, 327 never written (89 of them the plain images of no-data loads, the rest targets and textures nothing wrote
yet). No other path writes an image.

## Deviations

- **Compaction**: the per-site SPLICEs of asset-path.md 3.4 are replaced by clearing the movers' gate byte
  `[worker+0x1f21]`: nothing in direct mode reads pool 127, so all its copies can go; no per-record table.
- **Every** load's copy into the heap is skipped (also with no record); packaged texture data is never copied (direct mode).
- **Loads are matched without the 512-byte BNTX search**: each record of the name and descriptor names its data offset;
  the one whose `BNTX` header and table entry (index, name) sit that far back from src is taken (0 ambiguous).
- **Loads with no data** (src 0, 145-148 in Kokiri, and in no BNTX of the romfs: `RBake_*` bake textures, streaming
  stubs, model-compose textures) are not asset misses: plain images (non-ASTC, 75-89, never written in these runs) or the
  ASTC placeholder (121-134). The plan's "placeholders: 0 or each named" holds for loads with data (2, named).
- **Terrain tiles arrive before their texture is initialized**: the splice keeps a host copy of B (at most 1 at a time)
  and uploads it at that texture's `nvnTextureInitialize`; the storage is never written.
- **Converted terrain files can be larger than the game's** (288 of Kokiri's 1,521): the game drops a file larger than its
  size-table entry and reopens it forever (1.4 M opens in 120 s). Fixed by a size table in the overlay (romfs data, rule 4);
  it would shadow a size table served by the prompts feature (not active in these runs).
- The upload wait is a semaphore wait on the frame's submission only when an upload was sent since the last one.

## Candidate game patches

1. The no-data loads: find what fills them on the console (none was written by any path here); likely to settle with M2b's
   compute and copies. The ASTC ones cannot be written by the GPU at all.
2. `f_809ee930`'s `DummyTextureForEvent` (4x4 made in memory): a record or a WriteTexels.

## Open problems

- Terrain from the overlay vs a pack: the game read and decompressed ~110 (Kokiri) / 161 (HF) tile files at the load;
  offline the Kokiri set decodes at 736 MB/s single-threaded (268 KB a file), so ~40-60 MB and ~50-80 ms of loader time a
  scene, plus the held copy (34-46 MB memcpy). Small against a 9 s load: a pack is not worth it yet.
- Fill cost: 5.6 s of loader-thread time in Kokiri (zstd 3.9 s, ~600 MB/s: the vendored zstd is built with
  `ZSTD_DISABLE_ASM`; the x86-64 Huffman assembly would help), load time unchanged within run-to-run noise.
- Hyrule Field's heap shows 19 MB resident (Kokiri 1-8 MB): one 16 MB layer-0 chunk most likely; not traced.
- The BC7 `MaterialAlb` (1365x1365) dump shows dotted magenta texels on its last column and row: equal to the store
  (check passed), so a converter question for non-multiple-of-4 edges.
- Replace loads (`f_83537d90`) were not seen; their copy is counted, not skipped.
