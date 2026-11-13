# M6f report: walking and changing scenes (2026-11-13)

Branch `agent/m6f` (rebased on native-vk a2cab64; M6d's wait before every task+mesh draw is in). Runs in
`work/runs/m6f_*` (local; game pictures never leave the PC). RX 6950 XT, RADV, `RUN=timing`, one game at a time; other
agents ran beside it, so timing figures here are not results (pictures, counts and memory are).

**Device losses.** None of this stage's runs lost the device: 17 walks and 9 scene routes from the market overlay
(Hyrule Field at the castle town's drawbridge, the field's terrain programs drawn from the first frame, `HOS_GAME_STEP_PER_FRAME`
on most, headless) and two scene cycles (`m6f_c2_d`, `m6f_c3_d`). The resets of the day were not these runs: 00:32 a
`meshlab` process; 03:36, 03:42, 04:20 and 04:35 the user's windowed play sessions of this branch in Hyrule Field
(`play.sh hf`; logs in `work/runs/play_hang_0420`, `play_hang_0436`, and `work/runs/m6f/play-log-0420.txt` with its kernel
lines). At the 04:20 loss the back end's memory line said 6.7 GB device-local in use and 7.9 GB free: **not VRAM
exhaustion**. That loss is hang work (the coordinator's decision), not investigated here.
Tests: 296 pass / 5 fail (the baseline five; native-vk added one test).

## Summary

| Symptom (the user's report) | Cause (verified by run) | Fix |
|---|---|---|
| Foliage white, rainbow bands, the floor gone halfway to Lon Lon, a wall to bedrock | Terrain tiles initialized **again** over storage tera does not copy into again; their images were empty: height 0 and the lit pass reading them writing 65504 (section 2) | Tile bytes kept by storage address for every texture initialized there later (`HSD_TEXELS_TERRAIN_KEPT`) |
| "Loaded with no data ... another descriptor: a plain image" (ASTC placeholders among them) | These are UnifyTex **aliases**; M4g's slot map named a texture *object* the game had reused for another load (section 1) | The alias's target read from its own chain (`td+0x188`); the slot map deleted |
| VRAM to 16 GB | Two kinds of images the game never finalizes: DynamicTextureAllocator targets (shadow maps, 2432x2432 D32F, 89-208 of them, 2-4.8 GB), and agl's LRU cache (count 0, 0.4-2.5 GB) (section 4) | The DTA free and storage reuse let a target's image go (with a hold while a descriptor id names it); agl's eviction driven by free VRAM (a game patch) |

## 1. Aliases (no-data loads)

**Cause.** Walk `m6f_w1_d` named the one failure with the record the noted object carried then: the 1x1 alias
`Obj_LakeHrl_WarpPoint_Emblem_Light_Mtl` on an object that had become `Plant_Kokiri_FernOsmund_A_01_Alb` (256x256). In the
frozen back end (`m6f_w1_f`, `HOS_DBG=nodata`) every no-data slot held texels (868 of 868 reports) and every no-data load
landed on a slot whose earlier load had its size (103 of 103).

**Fix** (`host/direct_tex_dedup.c`, rewritten): at `loadImage_` with src 0 the hook walks the alias's prev links to the
head; the head is the target; its NVN texture goes to the back end as `HSD_ASSET_SAME`. The heap-slot table and its notes
are gone. A target that cannot be resolved is a strict event (`a texture loaded with no data over a content not
resolved (undefined content)`).

**Verified by run:** `alias patch: 'Fire_Thick_Burst_00_Dst' (no data) is an alias of 'Fire_Burst_SingleShot_S_01_Dst'`
(texture-streaming.md's table has the same pair); 104 of 104 aliases on the walk, 371-375 of 371-375 on the scene
route take their target's record with an equal descriptor (the doc's "an alias has its target's descriptor").

## 2. Terrain tiles in motion (the floor, the white foliage)

**Cause.** A stepped clock makes the glitch windows repeat (presents 1000-1350 and 1900-2200 of the walk). The dump at
present 1200 (`m6f_w2_d`) has the lit target go from max 1.03 (pass 98) to 6.5e4 (pass 100, program `64b82cf07fe2387e`),
whose draws (`m6f_w3_d`, `HOS_DBG=drawlog:1200`) sample 260x260 R32F height tiles that were "written never".
`HOS_DBG=tilelog` (`m6f_w6_d`): those tiles were copied and initialized at present 305 and initialized again at 993 with
no copy (12 such re-initializations in the walk, every tile kind). The frozen back end (`m6f_w5_f`,
`RX_VK_WATCH_DIM=260x260`) uploads them from memory, which still holds the first copy. `HOS_DBG=texwatch` (`m6f_w4_d`)
saw no CPU writer of tile storage and no file read into it.

**Fix** (`host/direct_assets.c`): the tile memcpy (a no-op in direct mode) keeps its bytes by storage address; a texture
initialized at that address with their linear size gets them as `HSD_TEXELS_TERRAIN_KEPT`, taken only into an image nothing
has written (an image of that storage and shape that still lives holds the content already). The next memcpy there
replaces them; a texture of another size there does not drop them. ~223 addresses in Hyrule Field (~60 MB host memory).
Terrain is tera's, not TextureDataMgr's, and has no better event than its memcpy and the texture initialization: this
rule is by storage address, as tera's storage is.

**Shots:** before `work/runs/m6f_w1_d/shots/vk_001200.png`, `vk_002160.png`; after `work/runs/m6f_w15_d/shots/vk_001200.png`,
`vk_002160.png`; frozen `work/runs/m6f_w1_f/shots/vk_001200.png`. White-flash shots of 49: before 6-7, after 0.

## 3. agl's texture events (host/direct_texdata.c)

One table keyed by the TextureData, written only by hooks on the game's own functions:

| Event | Hook | Verified by run |
|---|---|---|
| created / aliased | `f_80798690` (the alias chain read there) | 5,247 created, 104 aliases (walk) |
| moved by compaction | `f_807981f0` (its body run from the hook): the rebind's Finalize + Initialize of the td's object reach the back end as nothing; the object keeps its serial, so its image and registration follow it | "revived" per 600 presents from 1,900-4,200 to 0; 8,000-64,000 moves a run with no image made |
| released to the LRU cache / taken back | `f_8079aa20(&td->slot, 1/0)`: x0 is `td+0x18` | 0 unknown tds in every run; 13,400 released, 12,500 taken back on the scene route |
| destroyed | `f_8079bbe0(mgr, td)` | 156-444 a run |

The texture heap is the pool made inside `f_80f6c580` (InitializeHeap's body runs from the hook with the thread noted):
the log names one pool (2432 MB) where it named three (2432, 201, 2 MB; the last two are tera's).

**What frees VRAM, by system (verified by run where said):**
- TextureDataMgr textures: the game's destroy finalizes the NVN object, which releases its hold; an image shared by
  content goes, with its views and heap entries, when its last texture is gone and 8 presents plus the GPU's completion
  have passed (`image_sweep`). Released to the cache is not a free: the image stays. **A game destroy frees** (destroyed
  156-444 a run, images destroyed per 600 presents follow).
- DTA transient targets: section 4.
- Terrain tiles: tera's finalize releases the image like any texture's; the kept bytes (host memory) stay per address.
- Textures finalized in play: freed as above.

## 4. VRAM

### What held it
- **DTA targets** (verified): agl::utl::DynamicTextureAllocator's free (`f_802737a0`) gives the entry's block back but never
  finalizes the texture made over it. Walking: 89 images of 2432x2432 D32F (2 GB); scene route: 168-208 (3.9-4.8 GB).
- **agl's LRU cache** (verified): textures at count 0 stay registered until the 2,400 MB heap has < 120 MB free. Images
  held only by cached textures: walk 0 -> 1.1 GB after the area changes; scene route 0.4-1.6 GB; scene cycle 1.3-2.5 GB.

### What was done
- **DTA:** `host/rt_slots.c` tells the back end, at the game's free, which texture is over the freed block (the live
  texture at the block's storage); its image lets go when a texture is initialized over that storage, is held again when
  the game's alloc gives the entry back or registers it, and is made again if the game uses the texture after that. A let-go
  image is destroyed only once no descriptor id names it: without that hold the game sampled freed targets (the scene's
  depth, through ids registered before the free) after their images were gone: 10,079 "texture with no image sampled"
  and the missing models of `m6f_s8_d/shots/vk_006360.png` and `vk_007560.png` (the user saw them). With the hold
  (`m6f_s9_d`) no image is missing, but the ids keep many images pinned: 208 shadow maps, 4.8 GB, on the scene route.
- **VRAM-driven eviction** (a game patch, `host/direct_texdata.c` on `f_80797240`): for the game's per-frame update the
  rejection threshold is raised by what free VRAM lacks of a 1.5 GB margin, so the game evicts its LRU-cached textures in its
  own order, through its own destroy, until its heap has that many more bytes free or nothing is cached; the value is put
  back after the call, so its own heap-pressure path (and the allocation-failure eviction) is unchanged. Free VRAM is the
  back end's figure (`hsd_vram_free`: the device-local heap's budget minus usage, VK_EXT_memory_budget, so other processes
  count, plus images already waiting to be destroyed and not pinned by an id), sampled once a present. The margin: a scene
  change allocates 0.6-1.4 GB (store images and the new scene's targets) before the old scene's textures are released
  (scene route), so 1.5 GB keeps a load that starts at the margin from exhausting VRAM. **Verified on the scene cycle**
  (`m6f_c3_d`): free VRAM fell to 1.46 GB at the age switch (stop 6); the patch then evicted 2,581 cached textures at that
  stop and 11,271 over the run, the cache went to 0 where it had to, and free VRAM stayed 1.41-2.7 GB from there on (the
  budget 13.8-14.1 GB, other processes included): VRAM held at the margin across 20 more stops with no device loss. On the
  walk and the scene route it never fired (free VRAM 5.4-7.9 GB). Log every 600 presents:
  `assets: VRAM eviction patch: free VRAM N MB (margin 1536 MB), textures in agl's cache N, evicted by the patch N`.

### Numbers (MB)

| Route | Build | device-local driver usage per 600 presents | images live (store) | whole device at the end |
|---|---|---|---|---|
| walk 115 s | before (`m6f_w1_d`) | 6,695 -> 9,861 | 7,187 (3,878) | 15,527 |
| walk | after (`m6f_w15_d`) | 6,318 -> 7,703 | 5,283 (3,863) | 11,698 |
| walk | frozen (`m6f_w1_f`) | | | 11,615 |
| scene route, 6 loads | before (`m6f_s2_d`) | 6,579 -> 12,965 | 9,430 (4,132) | 16,096 |
| scene route | DTA let-go without the id hold (`m6f_s8_d`: missing images) | 6,300 -> 7,953, flat | 5,337 (4,132) | 9,813 |
| scene route | final (`m6f_s9_d`) | 6,188 -> 14,091 | 10,432 (4,132) | 15,992 |
| scene route | frozen (`m6f_s1_f`) | | | 13,105 |
| scene cycle, 7 stops (`m6f_c2_d`) | final but the pinned-bytes fix | 9,507 -> 12,196 | 9,191 (4,297) | 14,541 |
| scene cycle 12 stops x 2 (`m6f_c3_d`) | final | 8,263 (stop 0) -> 12,482 (stop 6: free VRAM 1.46 GB, the patch starts) -> 12,505 max -> 11,152 (stop 25) | 5,833 -> 9,191 max (4,319-4,547 store) | 10,565 -> 14,954 max -> 13,638 |

The fresh load of the market scene is 6.2-6.7 GB driver usage. **The final build does not return to it**: on the scene
route and the cycle the device-local usage grows with each scene, by the DTA targets pinned by descriptor ids (4.8 GB of
the 14.1 GB in `m6f_s9_d`) and agl's cache (0.4-2.5 GB, which the eviction patch would take once free VRAM is under the
margin).

**What would fix the DTA part (not built):** the console's transient targets alias one 512 MB pool; ours are one image per
(storage, shape) each. Placing the DTA pool's images in one device allocation over that pool's range (layer 0's
VRAM class already backs such pools), each bound at its storage offset, makes the pinned images cost nothing and makes
reuse of storage the console's aliasing; the free and the id hold are then not needed.

**The game-side lever for the cache** (report only, as asked): `TextureCacheRejectionThreshold` (120 MB) in
`Default.game__gfx__ModuleInitializeParam.bgyml` and `TextureHeap` (2,400 MB) in `Product.Nin_Ounce_NVN2.HeapDef.byml`. The
eviction patch is the run-time form of it; the parameters would bound the cache by heap bytes, which are not VRAM bytes
here (store images are 1-2x the console's).

## 5. The checks

**The walk check** (`scripts/direct_walk_check.sh walk|scenes [name]`, `BACKEND=frozen` for the reference): the market
overlay, a scripted walk toward Lon Lon (the stick up 80 s) or a scene route (the stick down 5 s every 25 s through the
drawbridge), shots every 120 presents, VRAM sampled every second, strict's survey mode. The verdict
(`scripts/direct_walk_check.py`) fails on unresolved aliases, ASTC placeholders, the undefined-content event, "a texture
with no image sampled", "no render targets set in the list", a black shot (walk only) or a white flash over the lower two
thirds after present 900, a lost device, a short run; it prints the memory lines, the cache, the texture events, the strict
reasons after arming.

| Run | Verdict |
|---|---|
| `m6f_w1_d` (before) | FAIL: 6 white flashes |
| `m6f_w15_d` (after) | pass |
| `m6f_s8_d` (DTA let-go without the id hold) | FAIL: 10,079 textures with no image sampled (the missing models) |
| `m6f_s9_d` (final) | pass (pictures and counts; VRAM grows: section 4) |

**The scene cycle** (`scripts/direct_scene_cycle.sh <name>`, promoted from `work/proto/scenecycle/cycle.sh`; the stops list
from `scripts/scene_cycle_maps.py`, game-derived, under `work/scenecycle/`): boots Kokiri, warps through the maps
(`!warp` on `HOS_INPUT_LIVE`), pans and walks at each stop, notes each stop's memory (driver usage and budget, VMA blocks,
live images, the store's share, the cache, free VRAM and evictions, the device's VRAM) in `stops.txt` and a shot in
`stops/`. It runs on the direct back end as it is (`RX_BACKEND=direct HOS_DBG=strict,strictall`, `RUN=timing`).
`m6f_c2_d` (8 stops): 7 noted, `HyruleField_Child DoorOfTime_MeetImpa_EndPos` not ready (a cutscene start position),
no device loss; shots `work/runs/m6f_c2_d/stops/*.png`. `m6f_c3_d` (12 stops x 2: Death Mountain Crater, Goron City,
Kokiri, Zora's Fountain, Dark Link's room, the Forest Temple, the age switch, the Ice Cavern, Morpha, Volvagia, the Desert
Colossus, Kakariko, the Sacred Forest Meadow): 26 stops noted, every stop ready (5 needed the warp sent twice), **no device
loss**, no "texture with no image sampled" or missing target; shots `work/runs/m6f_c3_d/stops/*.png` (looked at: no
missing floor, flashing or wrongly coloured surface; Kakariko's stop faces a wall). Strict after arming on the cycle:
program misses on maps met first (24,988), `a uniform buffer read at a constant offset past its bound range` (2,068),
`executed in a pass of another size` (1,776), the stand-in placeholder (372), generic objects made (4,749); known: store
images 24,000, renderable images 7,249 and their views 7,310. The second loop of each map sits at the margin (e.g.
Death Mountain Crater 8.3 GB the first time, 11.9 GB the second): VRAM does not return to a fresh load's level, it is
held under the budget by the eviction patch; the rest is the pinned DTA targets.

Strict in its real mode cannot pass a route yet: reasons outside this stage remain after arming (`executed in a pass of
another size`, `program miss in play` on maps met first, the dump's stand-in `CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm` as
"the placeholder texture sampled", `a uniform buffer read at a constant offset past its bound range` on the scene route).
Store images made in play are a named, known cause (`an image of a texture the game loaded (its prepared content)`).

## 6. The title movie (the user's item B: unconfirmed, the user asked to hold it)

The title on native-vk head (`m6f_t3_base`) and on this branch (`m6f_t4_d`), 100 s each, shots every 30 presents: the
movie frames and the title composite are the same at the same presents (means 131/136/113/... in both, the same scenes).
Two earlier runs of this branch (`m6f_t1_d`, `m6f_t2_d`, the second with `HOS_DBG=nodtafree`: the DTA free not told) stalled
during the load at ~120-210 presents with the movie's decoder waiting for a sync; not seen again in later boots, cause
not found. No title-movie check was added to the routes.

## 7. Doc claims verified by run (texture-streaming.md)
- Aliases: the target is the head of the alias's chain and has its descriptor (section 1).
- Compaction applies moves through `f_807981f0` for each texture and alias (8,000+ a walk) under the same descriptor.
- `f_8079aa20`'s first argument is `td+0x18`; release to the cache and take back are its 1/0 (0 unknown tds).
- Release at count 0 does not finalize: the cache holds 185-3,900 textures and 0.4-2.5 GB of images (sections 3, 4).
- The texture heap is the one pool `f_80f6c580` makes; `f_80f6c9d0`'s other pools at the scene load are tera's.
- Not verified: Replace (never reached), the descriptor id's reuse delay.

## 8. What remains
- VRAM on the scene route and the cycle: the DTA targets pinned by ids (the aliasing design above) and agl's cache
  under the margin's control.
- The terrain rule is by storage address (tera has no better event).
- Strict's other reasons in motion (section 5); the cycle's unreached HF stop.
- The user's windowed Hyrule Field losses (hang work).

## Code
- `host/direct_tex_dedup.c`: the alias patch (the chain's head), replacing the slot table.
- `host/direct_texdata.c` (new): agl's texture events; the VRAM-driven eviction patch.
- `host/nvn_native.c`: a move keeps its serial and tells the back end nothing; the DTA free / take-back notices.
- `host/direct_assets.c`: tile bytes kept by address; the texture heap's pool from `f_80f6c580`; `HOS_DBG=tilelog`.
- `host/rt_slots.c`: the DTA free and alloc tell the back end which texture is over the block; `HOS_DBG=nodtafree`.
- `gpu/direct/direct_image.cpp`: freed transient targets (pending, let go at reuse, held while an id names them, made again
  on use); the cache counted on the image; the images' memory line.
- `gpu/direct/direct_assets.cpp`: `HSD_ASSET_SAME` by the target's object, `HSD_TEXELS_TERRAIN_KEPT`, the strict event.
- `gpu/direct/direct_vma.cpp`: the memory line and the free-VRAM figure; `direct_main.cpp`: `hsd_vram_free`,
  `hsd_texture_freed/held/cached`.
- `scripts/direct_walk_check.{sh,py}`, `scripts/direct_scene_cycle.sh`, `scripts/scene_cycle_maps.py`.
- `nx2/oot_labels.txt`: notes on `f_8079aa20` and `f_80797240` (the functions met here were labelled already).
