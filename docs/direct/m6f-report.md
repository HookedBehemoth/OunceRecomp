# M6f report: walking and changing scenes (2026-11-13)

Branch `agent/m6f` (from native-vk 82b9cd2 + agent/m6d 7c77749: the wait before every task+mesh draw). Runs in
`work/runs/m6f_*` (local; game pictures never leave the PC). RX 6950 XT, RADV, `RUN=timing`, one game at a time.
**No device was lost by these runs** (the one reset of the day, 00:32:18, was a `meshlab` process during the user's play
session; the play session itself ended normally). Tests: 295 pass / 5 fail (the baseline five).

The user's report, playing `work/play` (agent/m6d ad92e20) from the market overlay (Hyrule Field at the castle town's
drawbridge): foliage flashing bright, the floor gone halfway to Lon Lon Ranch with a "wall to bedrock", bright coloured
tiles, VRAM to 16 GB over scene changes. All of it reproduced with a scripted walk and a scripted scene route
(section 5), and each has a cause found by a run.

## Summary

| Symptom | Cause (verified) | Fix | Verdict on the walk check |
|---|---|---|---|
| Foliage white, rainbow bands, the floor gone, a wall to bedrock | Terrain tiles: tera initializes tile textures **again** over storage it does not copy into again (`HOS_DBG=tilelog`: the height and material tiles at present 305 copied, initialized again at 993 with no copy). The back end had destroyed their images at the first texture's finalize, so the new images were empty: height 0 (the floor below, the tile edge a wall), and the lit pass reading those tiles (`64b82cf07fe2387e`, t0/t1 the 260x260 R32F height tiles) wrote 65504 into the lit target (dump: pass 100 mean 148, max 6.5e4; pass 98 max 1.03) | The tiles' bytes are kept by storage address, as the storage keeps them, and taken by a texture initialized there later into an image nothing has written (`HSD_TEXELS_TERRAIN_KEPT`) | flashes: before 6-7 shots of 49 (`m6f_w1_d`, `m6f_w3_d`), after 0 (`m6f_w11_d`, `m6f_w15_d`) |
| "Loaded with no data ... another descriptor: a plain image" (19 in the play log; ASTC placeholders among them) | These loads are UnifyTex **aliases** (docs/direct/texture-streaming.md 2.3). M4g's slot map guessed the alias's target as "the last load with data at that slot" and kept the loading texture *object*; the game finalizes texture objects and initializes them again for other loads, so the object noted named another texture by then (logged: `Obj_LakeHrl_WarpPoint_Emblem_Light_Mtl`, 1x1 BC1, whose noted object had become `Plant_Kokiri_FernOsmund_A_01_Alb`, 256x256) | The alias's target is read from the alias itself, the head of its `td+0x188` chain; the slot map is deleted | unresolved aliases: before 1 (walk) / 19 (play log), after 0 of 104 (walk) and 0 of 372 (scene route) |
| VRAM growing while walking (15.5 GB whole device after 115 s; frozen 11.7) | DynamicTextureAllocator (transient targets): its free gives the block back but never finalizes the texture made over it; the object stays initialized, so its image stayed held. Walking: 89-168 images of 2432x2432 D32F local-light shadow maps (2-3.9 GB) held by freed entries | The DTA's free tells the back end which texture is over the freed block; its image lets go when a texture is initialized over that storage, is held again when the game's alloc gives the entry back or registers it, and is made again if the game uses the texture after that | VRAM after 115 s: 15.5 GB -> 11.7 GB (frozen 11.7) |
| VRAM over scene changes | see section 4 | | see section 4 |

## 1. Aliases (no-data loads)

**Cause (verified by run).** `m6f_w1_d` named the one walk failure with the record the noted object carried *then*
(the message was extended for this): the 1x1 alias and a 256x256 fern. The frozen run (`m6f_w1_f`, `HOS_DBG=nodata`)
showed every no-data slot nonzero (868 of 868 reports) and every no-data load landing on a slot whose earlier load had
the same size (103 of 103): the console reads the target's texels, as texture-streaming.md says.

**Fix** (`host/direct_tex_dedup.c`, rewritten): at `loadImage_` with src 0 the hook walks the alias's prev links
(`td+0x188`) to the head; the head is the target; its NVN texture (`[target+0xd8]+0x10`) goes to the back end as
`HSD_ASSET_SAME`, which takes that texture's link. The heap-slot table and its notes in the load and rebind hooks are
gone. A no-data load that is no alias (no chain) stays a plain image, named. A load whose target cannot be resolved
is now also a strict event (`a texture loaded with no data over a content not resolved (undefined content)`).

**Verified:** `assets: alias patch: 'Fire_Thick_Burst_00_Dst' (no data) is an alias of 'Fire_Burst_SingleShot_S_01_Dst'`
(texture-streaming.md's table has the same pair); walk 104 of 104, scene route 372 of 372 aliases take their target's
record with an equal descriptor ("the alias has its target's descriptor": holds in every case).

Not done: resolving aliases offline in the prepared store (UnifyTex is a romfs file). The chain read is exact at run
time and costs one walk per alias load, so the store was left as it is.

## 2. Terrain tiles in motion (the floor, the white foliage)

**Cause (verified by run).** The direct dump at present 1200 of a glitched walk (`m6f_w2_d`, `HOS_DBG=dump:1200`; a
stepped clock makes the glitch windows repeat at the same presents, 1000-1350 and 1900-2200) has the lit target go from
max 1.03 (pass 98) to 6.5e4 (pass 100, program `64b82cf07fe2387e`). That program's draws (`m6f_w3_d`,
`HOS_DBG=drawlog:1200`) sample 260x260 R32F height tiles, two of them "written never". `HOS_DBG=tilelog` (`m6f_w6_d`):
those addresses had their memcpy and first initialization at present 305 and were initialized again at present 993
with no memcpy (12 such re-initializations in the walk, every tile kind). The frozen back end (`m6f_w5_f`,
`RX_VK_WATCH_DIM=260x260`) uploads those re-initialized tiles from memory ("sampled, uploaded"), which still holds the
first copy. `HOS_DBG=texwatch` (`m6f_w4_d`) saw no CPU writer into tile storage, and no file read into one.

**Fix** (`host/direct_assets.c`): the tile memcpy (which copies nothing in direct mode) keeps its bytes by storage
address; a texture initialized at that address with their linear size gets them as `HSD_TEXELS_TERRAIN_KEPT`, which the
back end takes only into an image nothing has written yet (an image of that storage and shape that still lives holds the
content already, possibly GPU-written). A texture of another size there does not drop them (its initialization writes
no memory); the next memcpy there replaces them. About 223 addresses in Hyrule Field (~60 MB of host memory).

Terrain has no event better than its memcpy and the texture initialization (tera is not TextureDataMgr:
texture-streaming.md 5), so this rule is by storage address, as tera's own storage is.

**Shots** (main session: view these): before `work/runs/m6f_w1_d/shots/vk_001200.png` and `vk_002160.png`; after
`work/runs/m6f_w15_d/shots/vk_001200.png` and `vk_002160.png`; frozen `work/runs/m6f_w1_f/shots/vk_001200.png`.
(The walk's input is wall-clock timed, so positions differ between runs by a few metres.)

Not found: whether anything else in tera's polish path writes tile storage on the GPU after a re-initialization (no
case seen: "its image had content" is counted on the assets line).

## 3. agl's texture events (host/direct_texdata.c)

Hooked, one table keyed by the TextureData, written only by these hooks:

| Event | Hook | Verified by run |
|---|---|---|
| created / aliased | `f_80798690` (with the alias chain) | 5,247 created, 104 aliases on the walk |
| moved by compaction | `f_807981f0` (body run from the hook): the rebind's Finalize + Initialize of the td's own object reach the back end as nothing; the object keeps its serial, so its image and registration follow it | `revived` went from 1,900-4,200 per 600 presents to 0; 8,000+ moves per walk with no image made |
| released to the LRU cache / taken back | `f_8079aa20(&td->slot, 1/0)` (the doc's "slot" argument is `td+0x18`: td = x0 - 0x18) | 0 unknown tds in every run; 13,400 released, 12,500 taken back on the scene route |
| destroyed | `f_8079bbe0(mgr, td)` | 156-444 a run |

The texture heap is now the pool made inside `f_80f6c580` (InitializeHeap's body runs from the hook with the thread
noted), not every pool of `f_80f6c9d0`: the log names one pool (2432 MB) where it named three (2432, 201, 2 MB: tera's).

**What frees VRAM, by system:**
- TextureDataMgr textures: the game's destroy (`f_8079bbe0` -> `f_8079c220`, and an alias at its release) finalizes
  the NVN object; that releases its hold; an image (shared by content) goes with its views and heap entries once its
  last texture is gone and 8 presents plus the GPU's completion have passed (`image_sweep`). **Yes, a game free frees.**
  Released to the LRU cache is not a free (the image stays).
- Transient targets (DTA): **yes**, at the free plus the next texture over that storage (section 0's table).
- Terrain tiles: tera's finalize releases the image like any texture: **yes**; the kept bytes (host memory) stay per
  address until replaced.
- Other textures the game finalizes in play: **yes** (as before).

## 4. VRAM

Whole device (amdgpu sysfs; the desktop's ~1.5-2 GB included), and the back end's memory line (device-local heap:
driver usage / VMA blocks), MB:

| Route | Build | whole device, end | driver usage per 600 presents | images live (MB) |
|---|---|---|---|---|
| walk 115 s | before (`m6f_w1_d`) | 15,527 | 6,695 -> 9,861 | 4,539 -> 7,187 |
| walk | after (`m6f_w15_d`) | 11,698 | 6,318 -> 7,703 | 4,067 -> 5,283 |
| walk | frozen (`m6f_w1_f`) | 11,615 | | |
| scenes (~6 changes, 185 s) | before the DTA rule (`m6f_s2_d`) | 16,096 | 6,579 -> 12,965 | 4,357 -> 9,430 |
| scenes | DTA rule by storage (`m6f_s5_d`) | 11,913 | 6,446 -> 7,850, flat after the 2nd change | 4,172 -> 5,193 |
| scenes | final (`m6f_s7_d`) | FINAL_S7 | | |
| scenes | frozen (`m6f_s1_f`) | 13,105 | | |

The fresh load of the market scene (the first window of every run) is 6.2-6.7 GB driver usage; with the DTA rule the
scene route settles at 7.8 GB after the second change and stays there through the next four.

**agl's LRU cache:** images held only by cached textures (count 0, still registered): on the walk 0 until the area
changes, then ~2,100 textures, 1.1 GB; on the scene route it swings with each scene between 185 images / 378 MB, 871 /
559 MB and ~2,200 / 1.4-1.6 GB. It is not what grew (the store images stay at 4,132 MB across every change); it is a
fixed ~0.4-1.6 GB the game keeps by design. If it has to go, the game-side lever is
`TextureCacheRejectionThreshold` (120 MB) in `Default.game__gfx__ModuleInitializeParam.bgyml`: the cache is evicted only
while the 2,400 MB heap has less than that free, so a threshold near the heap size (or a smaller `TextureHeap` in
`Product.Nin_Ounce_NVN2.HeapDef.byml`) would evict the cache every frame. Not done (report only, as asked); the store
images are 1.4-2x the console's bytes (BC7/BC5 for ASTC), so the game's byte budget is not a VRAM budget here.

**Not done:** the Kokiri -> Hyrule Field -> market -> back route. The scene route here goes back and forth through the
castle town's drawbridge (Hyrule Field <-> the market entrance), 6 loads in 185 s; a scripted walk from Kokiri to the
field needs the Lost Woods bridge and was not attempted.

## 5. The walk check

`scripts/direct_walk_check.sh walk|scenes [name]` (`BACKEND=direct` default, `frozen` for the reference): the market
overlay, `HOS_INPUT_SCRIPT` (walk: the stick up 80 s from second 20; scenes: the stick down 5 s every 25 s, each scene
starts facing away from the door), `RUN=timing`, shots every 120 presents, VRAM sampled every second. Direct runs use
`HOS_DBG=strict,strictall` (strict's survey mode). The verdict (`scripts/direct_walk_check.py`) fails on: unresolved
aliases, ASTC placeholders, the undefined-content event, "a texture with no image sampled", "no render targets set in
the list", a black shot (walk only: on the scene route those are the loads) or a white flash over the lower two thirds
of a shot after present 900, a lost device, a short run. It prints the memory lines, the LRU cache's images, the texture
events and the strict reasons after arming.

| Run | Verdict |
|---|---|
| `m6f_w1_d` (before) | FAIL: 6 shots with a white flash |
| `m6f_w15_d` (after) | pass |
| `m6f_s6_d` / `m6f_s7_d` (scene route, after) | pass / FINAL_S7_VERDICT |

Strict in its real mode cannot pass a walk yet: reasons outside this stage remain after arming, on every route
(`executed in a pass of another size` 132-1,086, `program miss in play` up to 4,469 on a cold cache, the dump's
stand-in `CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm` as "the placeholder texture sampled" 120-4,400, and on the scene
route `a uniform buffer read at a constant offset past its bound range` 188-282). Streaming adds images in play: store
images are now a named cause (`an image of a texture the game loaded (its prepared content)`, on the known list); 160-630
generic `Vulkan object made in steady play` remain (compute and graphics shader objects of programs met in play, and
plain images): steady means here "no object per frame", and the objects made follow the game's loads.

## 6. What remains

- Transient targets: the image of a freed DTA target follows the next texture over its storage, not a game event of its
  own (the DTA has none: its free does not finalize). On the scene route images still wait to die in bursts at scene
  changes.
- The terrain rule is by storage address (tera has no better event).
- Strict's other reasons in motion (section 5).
- The play log's `copies skipped: packaged N, rebinds N`: every packaged copy is a compaction rebind (the counters are
  equal, texture-streaming.md 2.5); in direct mode those now reach the back end as nothing.

## Code

- `host/direct_tex_dedup.c`: the alias patch (the chain's head), replacing the slot table.
- `host/direct_texdata.c` (new): agl's texture events; `host/nvn_native.c`: a move keeps its serial and tells the back end
  nothing; the DTA free notice.
- `host/direct_assets.c`: tile bytes kept by address; the texture heap's pool from `f_80f6c580`; `HOS_DBG=tilelog`.
- `host/rt_slots.c`: the DTA free and alloc tell the back end which texture is over the block.
- `gpu/direct/direct_image.cpp`: freed transient targets (pending, let go at reuse, held again, made again on use); agl's
  cache counted on the image; the images' memory line (by kind, the largest shapes, the cache, the frees).
- `gpu/direct/direct_assets.cpp`: `HSD_ASSET_SAME` by the target's object, `HSD_TEXELS_TERRAIN_KEPT`, the strict event.
- `gpu/direct/direct_vma.cpp`: the memory line (VK_EXT_memory_budget usage and budget, VMA's blocks).
- `scripts/direct_walk_check.{sh,py}`.
