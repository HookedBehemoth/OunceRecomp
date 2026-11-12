# M4h report: the under-lit static meshes (2026-11-12)

Branch `agent/m4h` (from native-vk e289175). Runs `work/runs/m4h_*` (local), RX 6950 XT, RADV, `RUN=timing`, one game process
at a time, Kokiri `chisel_kokiri`, Hyrule Field (HF) `chisel_hf`. **No device was lost; no task stage was enabled.** Tests:
292 pass / 5 fail (the baseline five).

## 1. The cause: prepared textures were sampled without their channel map (fixed)

**What was wrong (verified by reading the code and by the fix's result).** The prepared texture store keeps a texture's
channels where its converter put them. A two-channel ASTC normal map that the game reads as R,A is stored as BC5 (or RG8)
with R in R and A in G. A single-channel texture is stored as BC4 with its channel in R. Each record carries a channel map
for this (`nx2/prepare_textures.py`, record byte 28: "per source channel R,G,B,A, the VkComponentSwizzle that reads it from the
prepared image ... The renderer composes a view's swizzle with it: final[c] = map[guest[c] - R]"). The direct back end copies
that map into the image (`Image::chan_map`) and has the function that composes it (`image_swizzle`, `direct_image.cpp`), but
**nothing ever called it** (it is unused since A-rt, 04dd62c). The heap's sampled view (`sampled_view`, `direct_heap.cpp`) used the
game's swizzle alone. So a normal map whose game swizzle reads A for its second component read the BC5 image's A, which is
1.0. Every pixel's tangent-space normal got Y = 1. A single-channel texture read from G or A gave 1.0 instead of its value.

In Kokiri's trace frame (`m4h_k1`, `HOS_DBG=drawlog:1800`), the remapped textures that are sampled include:
- normal maps, stored with map 3,3,3,4: source R read from R, source A read from G
  - `Plant_Kokiri_MossGround_C_Nrm` (53 draws)
  - `CmnTex_Map_DetailMap_Wood_A_Nrm` (21)
  - `Tree_KokiriHouse_A_Nrm`: the bark, ASTC 0x7b, stored as BC5 (vk 141), 768x768, 10 levels
  - `Rock_Kokiri_Cliff_A_Nrm`
  - `Moss_Kokiri_Cover_A_Nrm`
  - `Kokiri_KokiriFuton_A_01_Nrm`
  - the lamp plants' and ferns'
- single-channel roughness and AO maps, stored with map 3,3,3,3:
  - `Cloth_Kokiri_Stitch_Tile_A_Rgh`
  - `Wood_Kokiri_CcounterBoard_A_Rgh`
  - `Land_Kokiri_BalconyRope_A_AO`

These are exactly the surfaces that were dark: the walls, the rock, the moss, the props. Characters and terrain were right for
two reasons. Terrain draws from the converted terrain tiles, which have no channel map. Most character textures are copied BC
blocks with an identity map.

**Fix** (`direct_heap.cpp sampled_view`, commit 215f3cb): every sampled view of a prepared image takes `image_swizzle(im, game)`,
which composes the game's swizzle (or its texture view's swizzle) with the record's map. The fix is general, not per texture:
- **A second NVN view with its own swizzle**: the view's swizzle names source channels just as the texture's does, so it is
  composed in the same way.
- **Textures taken through `HSD_ASSET_SAME`**: they share the record's image and so its map.
- **The X formats' alpha-as-one rule**: still applied before the composition.
- **Storage views**: unchanged. They carry no swizzle, and store images are not storage targets.
- Counted in the heap line: "views read through a prepared image's channel map": 6548 entries (Kokiri), 5074 (HF), since the
  start.

`HOS_DBG=nochanmap` restores the old mapping, as a diagnosis only. SNORM against UNORM was checked: ASTC has no SNORM, and the
converter's BC5/BC4/RG8 targets are UNORM, as the sources are.

## 2. Result

Shot pairs: before (M4c), after (this stage), frozen.

| Kokiri, present 1800 | frozen `m4f_fd_k` | before `m4c_t1_k` / `m4c_dd_k` | after `m4h_k1` |
|---|---|---|---|
| shot mean (sRGB, R G B) | 31.6 22.4 8.1 | 17.3 13.8 4.9 | **30.0 20.8 7.2** |
| lit B10G11R11F 1536x864, final state | 0.0228 0.0140 0.0051 | 0.0135 (M4c) / 0.0115 0.0096 0.0033 | **0.0220 0.0133 0.0045** |
| environment cube 0x113c806000, 128x128, the frame's two states | 0.0156 / 0.0099 | 0.0088 / 0.0028 | **0.0207 / 0.0083** |
| cube chain 64x64, first two states | 0.0218 / 0.0087 | 0.0086 / 0.0028 | **0.0204 / 0.0081** |
| composite RGBA8 2560x1440 | 0.0792 0.0561 0.0204 | 0.0437 0.0343 0.0125 | **0.0751 0.0521 0.0184** |
| per-pass picture distance against frozen (136 aligned) | | mean 1.00 (M4c) | **median 0.02, mean 0.74**; lit 0.37-0.57 (M4c 1.8), composite 2.28 (M4c 7.19) |

| HF tunnel, present 1500 | frozen `m4c_fd_hf` | before `m4c_t1_hf` / `m4c_dd_hf` | after `m4h_hf1` |
|---|---|---|---|
| shot mean | 24.0 34.8 37.4 | 21.7 33.0 34.8 | **24.9 36.6 40.2** |
| the right third of the shot (the rock wall) | 15.9 24.1 24.8 | 10.3 16.4 16.7 | **15.8 24.9 26.3** |
| lit B10G11R11F 1536x864, final state | 0.0211 0.0226 0.0162 | 0.0192 0.0214 0.0150 | **0.0220 0.0237 0.0173** |
| composite RGBA8 2560x1440 | 0.0602 0.0874 0.0938 | 0.0540 0.0824 0.0867 | **0.0624 0.0918 0.1010** |

In the shots:
- **Kokiri:** the bark walls, the moss wall, the stone path, the pots and the basket are lit as in the frozen shot.
- **HF:** the rock wall at the right shows its wet highlights and moss.
- What is left is within a few percent: Kokiri is 3-4 % darker than frozen, HF is 4-7 % brighter.
- The HF picture distances cannot be computed, because that frozen run's dump pictures were deleted. Only its means remain.

Not comparable at one frame: the environment cube is redrawn state by state across frames. Kokiri's two states are 0.0207 /
0.0083 against 0.0156 / 0.0099, against 0.0088 / 0.0028 before. HF's cube states differ by 20x between runs on both back ends
at the dumped frame, so no HF cube number is given. The cube's brightness now follows the scene, as M4g predicted.

New shot pairs (direct, frozen):
- Kokiri: `work/runs/m4h_k1/shots/vk_001800.png`, `work/runs/m4f_fd_k/shots/vk_001800.png`
- HF: `work/runs/m4h_hf1/shots/vk_001500.png`, `work/runs/m4c_fd_hf/shots/vk_001500.png`

## 3. The coordinator's questions

- **Forward or deferred.** Forward. A depth prepass runs, then colour draws go into the lit B10G11R11F 1536x864 target. Each
  lit draw samples its own material textures, the lightmaps (BC5 3048x1856, BC6H and BC7 1524x928x3), the environment cube
  and the probe data.
- **The RGBA8 1536x864 target is not a normals target.** It is a screen-space mask with its own passes (frozen dump 217-228):
  1. cleared to white;
  2. the characters' shadow projection (`1e26755d`);
  3. `d8180bd8c6ecc77b` (A ends at about 0.05).

  The lit draws sample it (`#851`, t12/t15 in the frozen dump). After the fix its states are 0.00 / 0.14 / 2.34 from frozen
  (2.34: G 0.911 against 0.915). Nothing overwrites it later.
- So the normals came from the normal-map textures themselves, as section 1 says. No target held them.

## 4. Trace additions (`HOS_DBG=drawlog:<present>`)

The trace now lists every texture descriptor of a draw:
- the stage and type;
- the handle word, whether from a driver bank or a game buffer (a game buffer's word is read when logged, as a diagnosis);
- a second word and its shifts;
- the texture's depth or layers, its levels, and its view's levels, layers, format and target;
- the image's layers, levels and the store record's channel map;
- the sampler the handle names (filters, wrap, compare, reduction, LOD range, bias, anisotropy, border).

Computed handles are named as such. The program's `TexWord` gained the fields for this (logs only).

The cheap leads of the brief, as far as they were looked at:
- a. Placeholders: `DummyTextureForEvent` and `CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm`. Neither is a Kokiri or HF lighting input.
- b and d were not needed: the fix accounts for the darkness.
- c. Lightmap views are 2D array views with all layers.
- e. Not looked at.

## Validation, frame time

- Validation with synchronization validation, Kokiri 150 s (`m4h_val_k`, `VK_LOADER_LAYERS_ENABLE=*validation
  VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`): **0 messages**. The run reached 4800 presents against about 8400 without the
  layer, which fits the layer running, but its loader log was not kept.
- Frame path: untouched. The change is in the heap entries written at texture registration, a composition of four swizzles.
  The drawlog additions run only in the trace frame. Frame time was not measured.

## Open problems

- The 3-4 % (Kokiri) and 4-7 % (HF) residual difference of the lit means. These were single runs; the run-to-run noise was not
  measured here.
- The environment cube cannot be compared at a single frame, because it is redrawn state by state.
- The rest of M4c's list stands: the RGBA16F 512x512 #2 (8.7), the RGBA8 384x216 B offset, and the cascades (an alignment
  artefact).
