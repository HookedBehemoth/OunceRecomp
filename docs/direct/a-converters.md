# A: the asset converters (2026-11-12)

Both are build-time tools: they read the romfs in place, write only under `work/prepared/` (game-derived, never committed,
never leaves the PC), are incremental and niced. The new renderer has no block-linear code, no ASTC decoder and no run-time
conversion: **every texture and terrain tile is prepared**, or it is drawn as a placeholder and counted.

## 1. Textures: `nx2/prepare_textures.py` -> `work/prepared/tex` (format: the script's docstring, FORMAT_VERSION 1)

Index + packs of zstd frames, keyed by BNTX file path (`outer::inner` inside a SARC) and texture index; unchanged by this work
except **one new texture-record field** (bytes 116 and 120, formerly zero, so old readers still work):

| field | meaning |
|---|---|
| `kind` 7 `raw` (new) | uncompressed and exact; `vk_format` 9 R8, 15 R8_SRGB, 16 RG8, 37 RGBA8 or 43 RGBA8_SRGB; the channel map says where each source channel is (RG8: first channel read -> R, second -> G) |
| byte 116 `rule` | 0 the target by the swizzle (BC4/BC5/BC7/BC1 or kept), 1 BC7 of the channels read, 2 raw |
| f32 at 120 `psnr_first` | PSNR of the first target tried (rule != 0: the one that fell below `--min-psnr` 35 dB) |

**No refusals.** A BC4/BC5/BC1 below the floor is retried as BC7; still below (or BC7 itself below) -> raw, PSNR 99 (the
floor never refuses an exact image). `status` is `ok` for all **22,576** textures (before: 20 refused); `unsupported` only
for a format with no Vulkan equivalent (none in this romfs; `why` says). The run after the change took 17 s (the 20 files;
the store was updated in place, 14.01 -> 14.07 GB packed); `--verify 1519` (1,500 random + all 20 fallback textures) all
match; `--verify` always includes every `rule != 0` texture. Tests: `tests/test_prepare_textures.py` (rule table, a run
that ends exact, record round trip, verifier, an old refused record is made again).

The 20 (all ASTC 5x5 UNORM normal maps, swizzle R,A,0,1 -> two channels read, 11 mips for 1536, 10 for 768). BC7 was the
second try and was **worse** than BC5 on all 20 (it spends bits on the colour channels it does not need), so all 20 are RG8
(rule 2). Sizes in bytes, for the whole mip chain: ASTC source / BC5 it would have been / RG8 raw / RG8 zstd-19 (packed):

| texture | px | BC5 dB | BC7 dB | ASTC | BC5 | RG8 raw | RG8 packed |
|---|---|---|---|---|---|---|---|
| CmnTex_Lava_Heat_A_Nrm, _B_Nrm | 1536 | 31.61 | 26.97 | 2,078,720 | 3,145,776 | 6,291,454 | 4.16 M |
| ..GerudoValley_Tent_A_Carpet_C_Nrm (CmnTex_Map_CityPropsSet_Land_) | 1536 | 33.11 | 27.99 | 2,078,720 | 3,145,776 | 6,291,454 | 4.59 M |
| Land_GerudoValley_Tent_A_Carpet_A_Nrm | 1536 | 33.31 | 28.63 | 2,078,720 | 3,145,776 | 6,291,454 | 5.15 M |
| CmnTex_Map_HrlField_RuinDestroy_A_Nrm | 768 | 33.42 | 28.58 | 541,184 | 786,480 | 1,572,862 | 1.29 M |
| CmnTex_Wood_Kakariko_Burned_Wall_A_Nrm | 768 | 33.71 | 29.04 | 541,184 | 786,480 | 1,572,862 | 1.19 M |
| Stone_SacredForest_Prop_A_20_Nrm | 1536 | 34.11 | 29.32 | 2,078,720 | 3,145,776 | 6,291,454 | 4.68 M |
| Blend_CrumblingGanonCastle_CmnB_A_Nrm, _CmnD_A_Nrm | 1536 | 34.33, 34.48 | 29.47, 29.70 | 2,078,720 | 3,145,776 | 6,291,454 | 5.12 M, 5.26 M |
| Land_Gerudo_FortressModuleCeiling_A_Nrm | 1536 | 34.48 | 30.54 | 2,078,720 | 3,145,776 | 6,291,454 | 5.33 M |
| CmnTex_Map_KakaEntranceSoil_A_Nrm | 768 | 34.55 | 30.24 | 541,184 | 786,480 | 1,572,862 | 1.23 M |
| HrlCity_ShopAnything_Inside_Wall_Nrm | 768 | 34.57 | 29.68 | 541,184 | 786,480 | 1,572,862 | 1.19 M |
| CmnTex_Map_HrlField_RuinMoss_A_Nrm | 768 | 34.62 | 29.58 | 541,184 | 786,480 | 1,572,862 | 1.26 M |
| Brick_InsideGanonCastle_CmnH_A_Nrm | 768 | 34.65 | 29.81 | 541,184 | 786,480 | 1,572,862 | 1.20 M |
| CmnTex_Map_LakeHrlBuilding_RattanBucket_A_01_Nrm | 512 | 34.68 | 30.76 | 241,664 | 349,552 | 699,050 | 0.46 M |
| CmnTex_Map_HidingHole_GroundSoil_A_Nrm, Land_HrlField_FallenTree_A_01_Nrm | 1024 | 34.70, 34.96 | 29.92, 30.35 | 933,888 | 1,398,128 | 2,796,202 | 2.35 M, 2.05 M |
| Body_Land_HrlField_PeahatGround_Nrm | 768 | 34.79 | 30.03 | 541,184 | 786,480 | 1,572,862 | 1.21 M |
| Mt_Shoulder_Armor_Npc_Hylia_MS_001_Nrm | 256 | 34.83 | 30.38 | 68,608 | 87,408 | 174,762 | 0.13 M |
| CmnTex_Metal_Well_WireMesh_C_Nrm | 768 | 34.93 | 30.49 | 541,184 | 786,480 | 1,572,862 | 1.22 M |

Totals for the 20: ASTC 23.1 MB, BC5 34.7 MB, RG8 raw 69.4 MB, RG8 packed 53.2 MB (+18 MB packed over BC5 would have been
~+30 MB of VRAM per copy resident: the cost of exactness, 0.2% of the store). Whole store: 22,576 textures, no placeholder.

## 2. Terrain tiles: `nx2/prepare_terrain.py` -> `work/prepared/terrain`

**Output = a rewritten-file overlay**: the same romfs-relative paths (`TerrainArc/<map>/<id>.<kind>`), each a valid TARB whose
GPU blob B is linear. Serve it with `HOS_FS_OVERLAY=work/prepared/terrain` (host/nn_fs.c: a file found there replaces the
game's at OpenFile; files not converted fall through to the romfs). **Chosen over an index + packs store because the
run-time side needs no code beyond the patch**: no key, no lookup, no second decompressor, no mapping from the memcpy site back
to a file or record: the game loads and zstd-decodes the file itself, so at `f_80c17040+0x544` x1 already points at the linear
bytes. (An index store needs a key at the memcpy site; the site has only pointers and a size: a content hash of a 0.8 MB
source blob per tile at run time is the cost we avoid; a path key needs the file name carried through FileResourceMgr.)

What changes in a file: B's bytes (block-linear -> packed rows, layer after layer, block rows for BC, format unchanged,
level 0 only), the u32 at record `+0x30` (B's size), the header's decompressed and zstd sizes, and the zstd frame (level 19,
content size in the frame, no checksum, no dictionary, as the game's). Everything else is as it was: B's offset field, A (the
CPU copy), C, the layout bytes `4f 7a` (the memcpy branch is still taken), `+3` layers. B stays last.

**What the run-time patch must know** (tile = 260 x 260 texels, the image the memcpy fills):

| kind (files) | layers | block-linear B (old x2) | **linear B (new x2)** | texel |
|---|---|---|---|---|
| `bake_ao.extm` (22,811) | 1 | 139,264 | **67,600** (65 x 65 blocks x 16) | BC5 |
| `bake_gi.extm` (22,808 less 3 damaged) | 6 | 835,584 | **405,600** (6 x 67,600) | BC6H |
| `.hght` (2,655), `custom.extm` (702) | 1 | 417,792 | **270,400** (260 x 260 x 4) | R32F / RGBA8 |
| `.mate` (2,257) | 2 | 835,584 | **540,800** (2 x 270,400) | RGBA8 |

x2 equals the linear size for a converted record and the block-linear size for an unconverted one (the two sets never meet),
so the patch copies x1 linearly as a `vkCmdCopyBufferToImage` region (`bufferRowLength` 0, `imageExtent` the 260 x 260 /
65 x 65 blocks, `layerCount` = layers) when `x2` is a linear size, else counts and draws the placeholder. The copy is
tightly packed layer after layer (layer stride = rows x row bytes, no 4 KB alignment). Padding bytes of the old blob are gone.
The A/C CPU copies are untouched (they go to other buffers).

**Numbers (full romfs, 51,261 files under TerrainArc, 8 workers niced):** 182 s wall; 51,233 TARB converted, 0 errors; 28
skipped and listed in `index.json`/`report.txt`: 25 `*.tscb` index files (not TARB) and the 3 damaged `bake_gi.extm` dumps
(HyruleField_02 x2, ZoraFountain_Adult), which the game serves from the romfs unchanged. In 6,057.5 MB (zstd) -> out 5,988.7 MB
(zstd 19), 12,921 MB of linear texels (B was 25.5 GB block-linear); 5.8 GB on disk, 51,233 files. Kokiri alone
(`--map KokiriForest_01`, 1,521 files): 7 s, 210.5 -> 207.9 MB.

**Verify:** `--verify N` decompresses output and source, checks the header and sizes, every byte of the record but B (and its
size field) equal, then re-swizzles the output's linear B into two buffers of different filler (0 and 0xFF) so texel bytes
are told from padding, and compares **every texel byte with the source's, byte-exact**. Full run: **all 51,233 of 51,233
match** (107 s). The sources' padding is not zero (123 M non-zero padding bytes over the set): the converter drops it, it
is not data. Tests (`tests/test_prepare_terrain.py`, synthetic TARB written by the test with garbage padding): the four layouts
vs an independent GOB reading and the BNTX tool's layout (RGBA8, R32F, BC5, BC6H x6, RGBA8 x2, RG8 with a 9-level chain), a
file round trip with every field checked, rejects (not TARB, bad header, damaged frame, unknown layout, B not last), a run
over a made-up romfs (overlay tree, incremental, `--map`/`--only`, missing output redone, a corrupted output caught).

Incremental: an `index.json` entry (size, mtime, BLAKE2b-64) skips a file; a touched file is kept by hash; another zstd level
or `CONV_VERSION` redoes all. Mip chains are not in TARB files (the 9-level RG8 tile normal map is GPU-drawn, no file).
Run: `python -I nx2/prepare_terrain.py [--map KokiriForest_01] [--jobs 8]`, then `--verify N`, `--report`.
