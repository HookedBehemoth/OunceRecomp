# M4g report: Link's textures, the dark props, the vertical lines, the 384x216 noise (2026-11-12)

Branch `agent/m4g` (from native-vk d7a6717). Runs `work/runs/m4g_*` (local), RX 6950 XT, RADV, `RUN=timing`, one game
process at a time, Kokiri `chisel_kokiri`, HF `chisel_hf`, title (no overlay). **No device was lost; no task stage was
enabled.** Tests: 292 pass / 5 fail (the baseline five). Reference: the frozen run `m4f_fd_k` (`RX_VK_DUMP=1800`, same host as far as the frozen back end
goes: every host change here is direct mode only or behind `HOS_DBG`).

## 1. Link's hair, cap and face: the game's texture dedup (fixed)

**Cause (verified).** `HOS_DBG=nodata` (new, `host/direct_diag_nodata.c`, either back end) logs each texture that
`agl::TextureData::loadImage_` (`f_80798690`) makes with no data (src 0, no copy into its slot), its slot in the texture
heap (`[td+0x18]+0x10`, where a load with data copies its bytes) and every load before it at that slot. Kokiri: 144 such
loads of 141 names, Link's `Player_Child_Hairs_Alb/Nrm/Rgh/AO/Thc`, `Mt_Cloth_Parts_Player_Child_Rgh/AO`, the child bakes
`RBake_Scene_KokiriForest_Child_*`, `Rock_Pot_A_Rgh`, `KokiriBasketCloth00_AO` among them; none of the names is in any BNTX
of the romfs (the store has every BNTX; e.g. `Tex/` has `Player_Child_Hairs_Gn0/Gn3` but no `_Alb`). In the frozen run
(`m4g_nd_f`) **every one of the 141 that has a load before it at its slot lands on the slot of the load just before it,
with data and of the same size**: `Player_Child_Hairs_Alb` (789504 bytes) on `Mt_Hairs_Player_Adult_Alb`'s, the child
bakes on the adult ones, `Mt_Cloth_Parts_Player_Child_Rgh` on `Mt_AnginParts_Npc_Kokiri_WC_Saria_Rgh`'s, `Rock_Pot_A_Rgh` on
`Rock_Pot_A_02_Rgh`'s, the 1x1 defaults (`Mt_Item_DekuStick_Trs`, `CmnTex_BakeLightDefault`, ...) on one 512-byte slot
in a chain. The game shares identical texels that way. Those slots hold texels in the frozen run at present 300
(`Player_Child_Hairs_Alb`: 42226 of its first 65536 bytes not zero) and a page watch on the hair albedo's slot
(`hos_watch_guest_writes`, all threads) saw no write into it after the load before it (its one hit, 0x10b7882600, is the
next slot's load memcpy past its end). So on the console, as in the frozen back end, the texture reads what the earlier
load left in memory. The direct back end never reads texture storage and made a plain image: undefined content (dark or
speckled hair).

**Fix (rule 8: the game does the awkward thing, so the patch is the game's).** `host/direct_tex_dedup.c` (direct mode
only, called from the loader hooks in `direct_assets.c`): each load with data notes its slot's content (the NVN texture it
loaded), and so does compaction's move (`f_807982f0`, the new base). A load with no data over a noted slot of the same size
is handed to the back end as the new `HSD_ASSET_SAME` key naming that texture; the back end (`same_load`,
`direct_assets.cpp`) takes that texture's link (its record, or its plain image) when the descriptors agree, else a plain
image as before, counted and named. A log line for the first four and every 100th (`assets: texture dedup patch: ...`),
counters in the assets line (`loads with no data over another texture's content N: its record N, ...`).

**Result.** Kokiri (`m4g_dd_k`): 149 such loads, 149 take a record, 0 plain images left (M4f: 146). HF (`m4g_fin_hf`):
125 / 125, title (`m4g_fin_title`): 7 / 7, 0 plain images in both. Link's hair is blond, the face and cap have their
textures (`m4g_bv_k/shots/vk_001800.png`). One name had no load before it at its slot in the frozen run
(`CmnTex_Map_HrlFieldRock_A_DetailMap_Alb`, at a scene change, present 471); it did not occur in the direct runs.

## 2. The pots and the basket: under-lit through the environment cube (cause found, fix follows M4c)

**The basket is drawn** (`HOS_DBG=drawlog:1800`, `m4g_dl_k`): its depth draw (program `ffb53ed102dd9908`, depth write) and
its colour draws (`78b3892467b71cad`, `5fa56485a6b07619`, depth test, no write) in the lit 1536x864 pass, with prepared
textures. Brightened 4x, the direct shot shows it in place with the right weave: it is dark, not missing. Linear means
against the frozen shot: pot right 0.25x, pot left 0.40x, basket 0.26x; the lantern 0.95x, the curtain 0.84x, Link's body
2.7x (exposure adapts to the darker frame).

**What the dark objects share** (verified): every lit draw that samples the 128x128 environment cube array
`0x113c806000` (target 8) is a prop: the pots (`Rock_Kokiri_Terracotta_A/B`), the basket, carpets, ropes, lamp plants, the
signboard, the scarecrow, foliage (179 of 750 lit draws). None of Link's or Navi's materials samples it, and Link is lit
correctly. That cube is the capture of the scene from a probe (the 128x128 pass, 46 draws a frame of which **3 are skipped
task+mesh draws**, the walls), and it is darker in direct: level 0 means 0.0076 / 0.0041 (R, two cubes) against 0.0156 /
0.0099 frozen, 0.49x and 0.42x, down to the 1x1 level (`m4g_bv_k` vs `m4f_fd_k`). The probe compute chain the props also
read (storage `0x115bc2d000`, written every frame by 8 programs, `HOS_DBG=writer`, `m4g_dw_k`) gives the frozen values
(e.g. `0x1141214000` means 0.0071 / 0.0566 / 0.0566 direct, 0.0071 / 0.0573 / 0.0577 frozen).
**Suspected, not provable without M4c:** the cube is darker only because the capture lacks the lit bark walls (the
brightest surfaces of the frozen frame, M4f 2). The textures the props sample are now all prepared (the no-data
`Rock_Pot_A_Rgh` and `KokiriBasketCloth00_AO` included, 1), which did not change their brightness.

## 3. The vertical line pattern: the moss wall behind the missing bark walls (not a pass artefact)

The streaks are not regular: in the 1280x720 shot there is no periodic structure in the dark areas that the frozen shot
lacks (row spectra, 2D spectra and column profiles of the same regions give the same values for `m4g_dd_k`, `m4f_fin_k`
and `m4f_fd_k`; e.g. top centre, column share of the high-pass energy 0.021 / 0.018 / 0.030). They show at reduced
display size, in the dark top centre and left, where the frozen frame has the bark walls (task+mesh, skipped). The
320-wide dumps cannot hold them, so two dump aids were added: `HOS_DBG=dumpw:<w>` (the dump's width) and `dumpraw` (each
colour item's floats beside its PNG, `.f32`). In the 768-wide float dump (`m4g_raw_k`, since deleted) the streaks are in the
lit target from its first lit state on (dump 095): a moss-covered surface with vertical drip streaks, lit and textured,
in every later state and the composite. It is the geometry behind the walls, drawn correctly; it goes when the walls draw
(M4c).

The RGBA8 384x216 pass (`531432c3`) the caller suspected is unrelated to the streaks, but its flat B channel had a cause,
found and fixed:

**The BC4 noise volumes: rendering into a compressed texture (fixed).** `531432c3` samples the 64^3 BC4 volume
`0x1140bcb000`, `KokiriForest_Child` (R16 1360x1000x8, a height map: its stored layers decode to coherent maps) and the
768x432 depth. The volume is in a pool of its own (`0x1140bcb000+0x20000`), the only texture there (`HOS_NVN_LOG_OBJECTS`),
never written by a dispatch, storage buffer or storage image (`HOS_DBG=writer` now checks images too, `m4g_vol_k`) and by
no copy (frozen `RX_VK_LOG_COPY`, `m4g_cp_f`). The frozen back end's watch (`RX_VK_WATCH_TEX`, `m4g_gw_f`) names its writer:
at the load, program `53509044401d78d2` (outputs unsigned integer) renders into it **64 times, one draw a slice**: the
game BC4-encodes its noise volumes on the GPU by rendering 8-byte blocks through a view of an uncompressed format of the
block's size (made by `f_809eb720` for the noise generators of `f_809eb4a0`). The direct back end gave a compressed image no
attachment view (`image_attachment_view`: "targets with no image" 0.2 a frame in the load window), so those passes drew
nothing and the volume stayed undefined. Rendering into a compressed texture's blocks is a general NVN operation, so this
is the back end's: a compressed image that is not the store's (it holds only what the GPU writes) is made
`BLOCK_TEXEL_VIEW_COMPATIBLE | MUTABLE_FORMAT | EXTENDED_USAGE` with colour-attachment use; a view of an uncompressed format
of its block size is an attachment of its block grid (the pass size in blocks). Store images are unchanged.

Result (`m4g_bv_k`): "targets with no image" 0.0 in every window; the 384x216 B channel is noisy like the frozen one
(the same structure, the canopy's shape at the top left), mean 0.945 (0.76-1.0) against frozen 0.853 (0.70-0.91), was a flat
0.697; per-pass distance 11.5 -> 8.0. **Open:** the remaining offset of B. Not found: whether the frozen back end's own path
for that render (it renders into its texture cache's image of the volume) matches the console's block layout, or the
direct encode's; a decode of both volumes would tell.

## 4. Per pass, HF and title

Kokiri, `m4g_bv_k` against `m4f_fd_k` (`scripts/direct_dump_compare.py --pictures`), beyond the frozen noise: lit
B10G11R11F 1536x864 5.9-18.8 and the composite 7.9 (walls and grass, task+mesh, as M4f); RGBA8 1536x864 12.2 (grass); the
environment cube chain 128x128 .. 1x1 0.3-1.3 (2); RGBA8 384x216 8.0 (3); the D16 cascades (another state aligned, M4f).
Everything else within the noise, the LUT, sky, depth and probe passes included.
HF (`m4g_fin_hf/shots/vk_001800.png` against M4f's direct `m4f_full_hf`, same start): no visible change but Link's
textures; title (`m4g_fin_title`): logo, menu and movie as `m4e_fin_title` (another movie frame). No regression from M4f's
bind-everything pools seen in either; no device loss, no Vulkan error in their logs.

## Validation, timing

- Validation with synchronization validation, Kokiri 90 s (`m4g_val_k`, `VK_LOADER_LAYERS_ENABLE=*validation`, the loader
  log shows the layer inserted): **0 messages.** The known `ShaderOutputNotConsumed` warning of M5 did not show either, so
  this setting reported errors only; warnings were not looked at.
- Frame time, Kokiri, pacing off (`HOS_NO_PACING=1 HOS_FPS_MAX=240 HOS_FPS_TARGET=1000`), alternated pairs, native-vk
  d7a6717 (`m4g_t_base*`) against this branch (`m4g_t_new*`), the last six 600-present windows of steady play: **base 12.09 / 11.97 ms mean (p99 13.9 / 13.8), this branch 12.00 / 11.96 ms (p99 14.0 / 13.7)**: no change. Vulkan objects made in play: a few dozen a minute late in the run on both builds (base 10409 -> 10441, branch 10428 -> 10556), not from this branch; to look at. The frame path changed by one branch per render
  target (the block grid's size); the host patch runs at loads only.

## Open problems

Task+mesh (M4c) owns the walls, the grass, the dark props (through the cube) and the visible moss wall; the 384x216 B
offset (3); `CmnTex_Map_HrlFieldRock_A_DetailMap_Alb` loaded with no data and nothing before it at its slot (frozen run, a
scene change), not met in the direct runs.

## Shot pair

(The dump pictures of these runs were deleted after the comparisons for disk space; their logs keep the statistics lines.)

`work/runs/m4g_bv_k/shots/vk_001800.png` (direct), `work/runs/m4f_fd_k/shots/vk_001800.png` (frozen).
