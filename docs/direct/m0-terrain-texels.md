# M0: terrain tiles and the other CPU texel paths (2026-11-12)

Analysis only (tree at 23d0aa6; no game run). Tools and scratch output: `work/proto/direct-m0/` (`tarb.py`, `survey.py` +
`analyze.py` over every TARB, `deswz.py`, `emu_sw.py` and `emu_conv.py` run game code in unicorn, Ghidra output in
`decomp*/`). **C** = CONFIRMED (address / file:line / file sample), **I** = INFERRED.

## 1. Terrain tiles: the plan's "flip the layout bytes, no hook" does not work

**File (C, all 51,233 TARB under `TerrainArc/`, 3 more are damaged non-TARB dumps):** `"TARB"`, u32 version 1, u16 records 1,
u16 header 0x14, u32 decompressed size, u32 zstd size, then one zstd frame (no dictionary). The record (decompressed):
`+3` layers, `+8` bit 7 = the CPU copy is filtered, `+0x1a/+0x1b` layout bytes, then three (s32 self-relative offset, u32
size) pairs: **A** `+0x1c/+0x20` the CPU copy (filtered), **C** `+0x24/+0x28` (empty in every file), **B** `+0x2c/+0x30`
the GPU texels, always last in the record. Every file has layout bytes `4f 7a`.

| kind (files) | texture (NVN, census `work/runs/bl_census/log.txt`) | B: block-linear bytes | linear bytes | A |
|---|---|---|---|---|
| `bake_ao.extm` (22,811 with dream/reverse/adult2 variants) | 0x4b BC5, 260x260, 1 level | 139,264 | 67,600 | none |
| `bake_gi.extm` (22,808; the 3 damaged files are of this kind) | 0x4f BC6H SF16, 2D array x6, 1 level | 835,584 | 405,600 | none |
| `custom.extm` (702) | 0x25 RGBA8, 1 level | 417,792 | 270,400 | 270,400 |
| `.hght` (2,655) | 0x0a R32F, 1 level | 417,792 | 270,400 | 270,400 |
| `.mate` (2,257) | 0x25 RGBA8, 2D array x2, 1 level | 835,584 | 540,800 | none |

B is the standard Tegra layout with NVN's automatic block height (host/nvn_layout.c: 16 GOBs for 260 rows, 8 for 65 BC
rows): sizes match exactly, and deswizzled RGBA8/R32F tiles are 10-100x smoother than raw order; BC5/BC6H decode to
coherent images only with that block height (C, `deswz.py`). The 9-level RG8 `MINIMAL_LAYOUT` tile texture is the normal
map (`norm`, no file): GPU-drawn, not loaded (C, census "the GPU drew").

**The device bytes are a constant (C):** `f_809e3350` at `0x809e36a0` stores `0x7a4f` at `[mgr+0x7a30]` right after making
the 0x7fd8-byte manager, so `[x24+0xa30]/+0xa31` = `4f 7a` on every run: the memcpy branch is always taken today.

**The branch (C, `decomp/f_80c17040.c`; flags = the layer descriptor's u32 at `+6`, format = its tera format byte at `+5`):**
- `+0x4e4..+0x544`: B is memcpy'd (`+0x544`, x0 = the tile texture's storage, x1 = B, x2 = `+0x30`) when the layer has a
  texture (flags bit 16) and the layout bytes match; it then sets state bit 1 ("complete").
- On a mismatch the game only sets a bit in `[mgr+0x7b24+..]` and falls through to the WriteTexels path (`+0x66c`), which:
  1. uploads **A, or the CPU copy decoded from A** (`f_80eb8460` / vtable `+0x60`), never B. For `bake_ao`, `bake_gi` and
     `mate` A is absent and the source pointer is **NULL** (`lVar27 = 0`).
  2. **refuses every compressed format**: tera formats 0x30..0x59 (table `0x83f4848c`: BC 0x42..0x50, ASTC) skip to
     `+0xb2c` with no upload unless flags bit 26 (a runtime-compressed layer, which uploads uncompressed and leaves the
     compression to the polish step) is set (C, `emu_sw.py`, every format byte run through `0x80c176ac..0x80c17768`).
  3. writes level 0 only, region 260x260 x layers, `f_83a6d8f0(..., w3 = 0)` (`+0xa48` `mov x3, xzr`): WriteTexels +
     FlushTexels, no temp pool. Then it sets state bit 4, which sends the tile down the GPU polish path
     (`f_80d7e090`, `0x4d4` bits 0x20/0x40/0x80), not the plain one.
- For `hght` / `custom` the decoded CPU copy is **not** the GPU texels: `f_80eb8460` run in unicorn on Kokiri and Hyrule
  Field tiles differs from deswizzled B in 0-13% of texels (heights up to 0.19 apart, alpha 255 vs 0) (C for these samples;
  I that the difference is intended, CPU copy vs. rendered data).

So marking records as mismatched would leave 90% of the tiles (BC) unwritten, fault on `mate`, and upload different
data for `hght`/`custom`.

**Instead: keep the memcpy branch and patch the one call (C: lifted line `c->x[30] = 0x80c17588ULL; f_8572f020(c);`,
`work/sa/gen/code_010.c:90281`, `f_80c17040+0x544`).** The converter deswizzles B to packed rows (level 0, layer after
layer: the image's `vkCmdCopyBufferToImage` layout, bufferRowLength 0), shrinks `+0x30` and the file header's sizes, keeps
`4f 7a`, recompresses. The patch calls a host function with x0/x1/x2 instead of memcpy: it finds the texture whose storage
starts at x0 (a tile slot made by tera::FileResourceMgr: one of the 5 descriptors above), and if x2 is that descriptor's
linear size it queues a buffer-to-image copy of x1 for the next submission; the guest storage is never written. The two
size sets never coincide, so an unconverted file (x2 = the block-linear size) is detectable: it draws the placeholder
and is counted, per the plan's rule. Nothing else reads B: the CPU copy and C go to other buffers (memcpys at `+0x494`/`+0x4d4` and `+0x5f4`).

**Converter work (C, `survey.py`):** 51,233 files, 6.06 GB compressed, 26.4 GB decompressed; B is 25.5 GB block linear and
**12.9 GB linear** (the padding goes), so records shrink to ~13.8 GB raw. The cost is zstd: decompress 26 GB (about a
minute on 16 cores) and recompress 13.8 GB (minutes at level 3-9, about 5-10 min at 19) (I). Deswizzling is
`hk_block_linear` from `nx2/texconv.cpp` (agent/assets1) as it is; block heights by `pick_tile`/`adj_tile`
(host/nvn_layout.c), not BRTI's. `prepare_textures.py` is BNTX-specific (packs and an index the renderer reads); the
TARB tool is better a sibling (`nx2/prepare_terrain.py`, ~200 lines) importing its zstd, worker and incremental-scan helpers
and the native deswizzle, writing whole romfs files. Delivery: the rewritten tree mounted with `HOS_FS_OVERLAY`
(host/nn_fs.c:347: `<overlay>/<path after the mount>` replaces the file at OpenFile, one stat per open), or the rewritten
romfs.

## 2. Other CPU texel paths: all linear at the boundary, but the font cache is not an API call

| Path | How texels arrive | Layout at the boundary |
|---|---|---|
| agl `f_809e9d60` (PrimitiveTexture `f_809e9300` and 3 siblings, ModelProcedural `f_80ddab00`, WaterVolumeDataDummy `f_80c5f1e0`) | WriteTexels + FlushTexels, one level, whole level region (C, `decomp/f_809e9d60.c`; 43 calls, block-linear.md site 4) | linear rows |
| TextureDataInitializerJPG `f_809ea090` | decodes on the CPU into a buffer, then `f_809e9d60(.., level 0)` (C, `decomp/f_809ea090.c:219`), RGBA8 | linear |
| packaged ui2d `f_807982f0` | SetPackagedTextureData with a pointer into the loaded BNTX (mip table at `+0x70`, count `+0x16`) (C) | **block linear in the file**: the BNTX path of prepare_textures (it walks SARCs), keyed by `pkg_data` |
| tera polish `f_80d7e090+0x438` | `f_83a6d8f0(w3 = 0)` WriteTexels from the tile's CPU buffer (`[tile+0x4c0]`) into DTA temp "tera::temp_for_compressed_in_polish" (C) | linear |
| `f_83a6d540` | InvalidateTexels + ReadTexels (w3 = 0), or CopyTextureToBuffer + map (C) | linear out |
| font glyph cache (eui, FreeType, `Font2/*.bfttf/.bfotf`) | **direct CPU stores into the texture's storage after a GPU clear**, no WriteTexels (C: none of the 43 WriteTexels calls; `gpu/vk/vk_texture.cpp:2459` `only_cleared`, b306c4d, issues 40/44) | the texture's own layout; I: pitch-linear, since the game has no GOB arithmetic (`scan_gob.py`: none) and the letters show correctly today |

The font cache is the one texture the back end cannot serve from API calls: its glyphs reach memory with no call to hook.
It needs either the plan's "no page tracking" relaxed for that one texture (upload its dirty pages each submit), or a
patch at the glyph store (find it from the texture: one run with `HOS_NVN_LOG_OBJECTS=1` and text on screen gives its
descriptor and storage, then a write watch on it gives the store's PC).

## 3. Texture storage used as plain memory

- **Site 13 (C, `decomp/f_80271be0.c`):** "ndsr::smaa_edge_list" is a DTA texture, 2048 x ceil(w*h/8*4 / 8192) RGBA8, made only
  for its memory: `+0x384` GetTextureAddress (the only call site of that proc, `0x80271f64`) and the byte size
  `w*h/8*4` (663,552 B at 1536x864) go into the SMAA pass's argument block (`f_802722b0`); its handle is never bound, and it is
  released after the pass. I: written and read only as a buffer. No layout question; it needs a GPU address that is a
  buffer.
- **Site 16:** the one buffer write that reached a 1536x864 D32F target's storage (bl_census). I: that is this buffer
  landing on memory a depth target uses at another point of the frame, i.e. aliasing of unlike users, undefined on the console
  too; nothing reads across it.
- **Simplest:** patch `f_80271be0+0x384` to return the address of a host-owned buffer sized for the largest resolution (and
  skip its DTA allocation), so no texture storage is ever a buffer. Otherwise the transient pool's memory must carry a
  `VkBuffer` beside its placed images.

## Decision for the plan

Replace the "Terrain tiles" bullet: the mismatch branch cannot carry the data (BC refused, NULL source for 3 of 5 kinds,
CPU copy not the GPU texels), so the converter keeps the layout bytes and the memcpy branch, and **one game patch** at
`f_80c17040+0x544` turns that memcpy into a host buffer-to-image copy of the converted, linear record; an unconverted
record is recognised by its size and drawn as the placeholder. The converter is a sibling of `prepare_textures.py`
(51,233 files, 25.5 -> 12.9 GB of texels), delivered by `HOS_FS_OVERLAY` or a rewritten romfs. "CPU-written textures"
stays true for WriteTexels/ReadTexels users (all linear), but the **font cache needs its own item**: per-submit
dirty-page upload for that one texture or a patch at its glyph store, to settle in M0 with one run. Site 13 becomes a
patch to a host buffer; site 16 needs nothing.
