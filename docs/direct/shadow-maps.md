# The game's shadow maps: how they live and die, and what pins them

Analysis for the coordinator's choice of a fix. Nothing here is a fix: the code changed for it is a probe that was not
committed (section 3). Static analysis of this build (Ghidra decompiles made with `work/ghidra/decomp.sh`, kept under
`work/proto/shadow/d1..d6/`, and the lifted code in `work/sa/gen`), the romfs parameter files, and runs of the direct
back end on this PC (RX 6950 XT, RADV, `RUN=timing`, one game at a time). Runs: `work/runs/m6s_*` (local only).
Addresses are main's VA (the lifter's `f_<addr>`); offsets are this build's (HOS_GAME_VERSION 100).

**VERIFIED** means read in the decompile or the lifted code at the given address, read from a romfs file, or seen in a
run (named). **INFERRED** means concluded from the code's shape, from names or from numbers, and not confirmed.

## 0. Short

- **The memory that grows is local-light shadow maps held by the allocator's entry cache, not by lights or by the
  frame.** agl::utl::DynamicTextureAllocator (DTA) has 768 entries (VERIFIED, probe: `DTA 768 entries`). Its free
  (`f_802737a0`) gives the block back but leaves the entry's texture object initialized and its descriptor id
  registered, with a count of 1 that the entry itself holds (VERIFIED, probe: every pinned image is named by a free
  entry's own id, count 1). About 650 of the 768 entries are free at any time. Each one keeps an image alive in the
  back end, because M6f's rule keeps a freed target's image while any descriptor id names it.
- **On the console this costs nothing.** All DTA targets alias one 512 MB pool, so a free entry's registration names
  memory that some other target is using now. Here every (storage address, shape) pair is an image of its own. rt_slots
  gives out new addresses whenever it evicts and remakes slots or falls back to the game's offsets (1,128 slots made,
  1,001 evicted and 364 game-offset placements on the scene route: `m6s_s3`). So the number of pinned images grows with
  every new address a big map lands on.
- **A local shadow map lives for one frame and is only sampled in that frame** (VERIFIED, `m6s_k3`, `m6s_k4`: 3,601 of
  3,601 presents have one draw and one free per light). It is allocated in `f_802640a0` when the local-shadow pass
  records, sampled by the deferred lighting that same frame through a handle the light writes, and freed at the view's
  last pipeline stage (`f_802740c0`). The light keeps two counts on the map's id until its next shadow draw. Nothing
  samples a map after the present it was drawn in. All 44 draws that bound a "let go" shadow map in `m6s_k4` bound the
  map drawn in that same present. Those were the back end's own mistake (section 5.3), not stale use by the game.
- **Measured:** standing in Kokiri or Hyrule Field, 1-2 lights, 6 slots (35 MB and 115 MB), and nothing pinned grows.
  On the scene route the pinned images reach 584 images and 3.85 GB (`m6s_s3`, 2432x2432 D32F: 127 images, 2.9 GB). M6f
  measured 4.8 GB (208 of 2432x2432). This is the same mechanism, and run-to-run spread changes how many addresses get
  used. On the scene cycle the pinned images grow from 0.7 GB at the first stop to 3.9 GB on the second loop. There
  they hold VRAM under the eviction margin after agl's cache has been emptied (section 4.4).
- **Recommended** (section 6): bind the transient targets into one fixed device allocation at rt_slots' slot offsets,
  so that they alias as on the console. VRAM for transient targets then becomes a constant 512 MB that the VRAM limit can
  count. With it, make the host tell the back end about targets through the allocator's hooks (entry identity, not
  address) and add a small hysteresis to the local-shadow size classes. **Second choice:** a game patch that releases
  the entry's descriptor id at the DTA's free, plus a host hook at the game's id drain that tells the back end which ids
  the game has freed. Images then go when the game is done with them, but they are made and destroyed in play.

## 1. The shadow system, from the code

### 1.1 The kinds

| Kind | Who | Target (DTA name, format, size) | Life | Evidence |
|---|---|---|---|---|
| Sun (directional) cascades | `f_80668fc0` x3 -> `f_8028f770` -> `f_8028f980`; alloc in `f_80669460` | `"shadow_map_depth"`, array of cascades, format 0x5a or 0x5c (by `+0xf0`), width `+0x70`, height `+0x90`, layers = the cascade count (1600x1600x3 in Kokiri and HF) | transient when `+0x110` is 0 (alloc flag `+0x110 ^ 1`), else persistent; the same descriptor every frame | VERIFIED decompile `f_80669460`; engine-frame.md row 14 |
| Local light (point/spot), dynamic | `f_80255be0` "LocalDepthShadow::drawDepth" -> `f_802640a0` | `"local_shadow_depth"` (string 0x83e15fea), format = shadow `+0x4ec`, square, side = shadow `+0x4e8` (= light `+0x5e8`), 1 level, desc flags 4 | **one frame** (2.1) | VERIFIED decompile, runs `m6s_k3`/`k4` |
| Local light, static | `f_80f76a20` "LocalDepthShadow::drawStaticDepth" -> `f_802640a0(obj, ctx, 1)` | when shadow `+0x4f0` bit 0 is clear, `f_802640a0` uses the TextureData kept in the shadow object (`+0x328`) and allocates nothing | the light's | VERIFIED decompile (the `+0x4f0` branch); never seen in the runs (every logged draw was "dynamic") |
| Local light, updatable static | `f_80663f00` "LocalDepthShadow::drawUpdatableStaticDepth": two maps, `+8` and `+0x500` | as above (`f_802640a0(.., 0)`) | the light's | VERIFIED decompile; not seen in runs |
| Static (sun) depth shadow | `f_809ec950` | `"static_shadow_depth"`, square `+0x80c`, format `+0x800`: a kept texture (`+0x578`) in mode 2, else a transient DTA target | by mode | VERIFIED decompile |
| Screen-space / prepass | `f_8065ddf0`, `f_8065e180` (`"shadow_prepass_light_buffer"`, `_temp`, `_temp_half`), `f_80664830` (`"ScreenSpaceShadow"`), `f_83568010` (`"shadow_map_reduce_depth"`), `f_83626d20` (`"shadow_mask_temp"`) | screen-sized transients | one frame | VERIFIED names (rt-alloc.md `sites.txt`); not studied further |

The big persistent images in every run's "largest shapes" (12288x12288 D16 297 MB, and 4096x4096 D32F, R32F and D16)
are most likely the static shadow and the sun's far depth (engine-frame.md: "4096x4096 far depth"). They are
persistent, made once, and never pinned (INFERRED from shape and from the fact that they never appear in the pin
census).

### 1.2 Where it sits in the frame

All of it is recorded in calc stage C (boot-to-main-loop.md 4.3), on the worker that records the 3D pipeline's layer
list (`f_80660320`). In GPU order it is engine-frame.md's row 14 (sun cascades, after the opaque pass and the
quarter-res chain, then signal `ef0`) and row 15 (local light shadows: ClearDS, barriers, 36 precompiled calls and 2
draws per light). Then the deferred lighting (row 18, `f_8066bec0`) and the async-compute list #244 (screen-space
shadows and froxel fog read the cascades). The local maps are sampled by the lighting's per-light draws in the long
layer list (`lr 0x8007b0c8`, programs `1e26755d8cb22dc8`, `3427a1dbefd5e02c` and others, the texture at `c2+0x20` of
the fragment stage: VERIFIED, `m6s_k4` and `m6f_s8_d`'s log). They are not in the 30-slot system texture table.

### 1.3 Parameters (romfs, VERIFIED by reading the files)

- `System/GameGraphics/LocalDepthShadowParam/Default.game__gfx__LocalDepthShadowParam.bgyml`: `LocalDepthShadowSetting`
  [3] (`BlurWidth` 2.0 / 3.5 / 3.5, `PcfType` 1 / 3 / 3, `ResolustionMaginify` (sic) - / 0.3 / 0.3),
  `DemoLocalDepthShadowSetting` [4] (`ResolusionScale` (sic) - / 3 / 5 / 15), `NPCFadeStart` 25, `NPCFadeEnd` 30,
  `ZDirEfficiencyInDungeon` 0.075.
- `System/GameGraphics/LocationalLightMgrInitializeParam/Main...bgyml`: `Light_PointNum` 400, `Light_SpotNum` 600,
  `Light_LineNum` 64, `Light_AmplifierNum` 112, `Rig_ObjNum` 512: the light pools, not a limit on lights that cast shadows.
- `gsys/ModelSceneShadowParam/*`: the sun's depth shadow (`dir_main`), the static one (`dir_static_shadow`),
  projection shadows, SSAO. No resolution table.
- Strings in main name more of the rule (`LocalDepthShadowQuality`, `LDShadowFadeStartDist`/`EndDist`,
  `IsEnableStaticLocalDepthShadow`, `game__gfx__LocalDepthShadowSetting`). **The distance-to-size code was not
  found.** Observed sizes (light `+0x5e8` before rounding, `m6s_k3`/`k4`): 1142-1158 for Navi in Kokiri, 2432 and 896
  in Hyrule Field at chisel_hf, and 384 to 2432 on the scene route (sizes 385, 398, 411 ... 508 in steps of 12-13 while
  a light approaches). INFERRED: the size is set in calc from the light's screen size or distance, scaled by the
  quality entry's `ResolustionMaginify` and clamped to about 2400 (2432 is the largest size ever seen, which is 2400
  rounded up by `HOS_RT_QUANT`).

## 2. The life of a local-light shadow map

Per frame and per light that casts a dynamic shadow (VERIFIED by decompile; the order and counts by `HOS_DBG=shadowlog`
in `m6s_k3` and `m6s_k4`: 3,601 presents each, one light, one draw and one free in every present):

1. **Request.** `f_80255be0` walks its light list (`+0xab0`, count `+0xaa8`). A light whose `+0x5f8` bit 2 is set gets
   its map drawn, and at the end the pass sets bit 3 (0x80255be0 decompile, lines 708 and 3792 of `d1/f_80255be0.c`).
2. **Alloc.** `f_802640a0(light + 0x100, ctx, 1)` calls `f_8005ed00(dta, ctx, desc, 1)` (lr `0x80264130`) with
   `{"local_shadow_depth", format +0x4ec, type 1, w = h = +0x4e8, depth 1, levels 1, flags 4}` and stores the entry at
   shadow `+0x320` (light `+0x420`). rt_slots rounds w and h up to multiples of 128 (`HOS_RT_QUANT`). The pass's
   viewport and scissor come from the entry's own width (`+0x40 >> mip`), so the rounding only raises texel density.
3. **Ids.** The entry's TextureData is copied twice into the shadow object: `+0x220` (the render target) and `+0x3e8`
   (for sampling). For each copy the previous id (`+0x288`, `+0x450`) loses a count (inline `f_834bf9b0`; at 0 it goes
   onto the deferred list) and the new one gains a count. When the new id's count was 0, a fresh id is made with a
   texture object of the manager's own (`f_834bf750`). In the runs the entry's id is reused directly: after the draw
   the id has count 3 (the entry 1, the light 2). VERIFIED, `shadowlog` lines `ids +0x450 N (was M, count 3)`.
4. **Draw.** The viewport and scissor come from the target, then the clear and the casters (`f_80265760`, `f_80263b60`
   x8). After the pass, `f_80255be0` writes the sampling handle into the light's data:
   `nvnDeviceGetTextureHandle(dev, +0x550 id, +0x5da sampler)` into `*(light + 0x5e0)` (0x80255be0 decompile line
   1365). The lighting reads it from there.
5. **Sample.** The deferred lighting's per-light draws in the same frame (1.2).
6. **Free.** `f_802740c0`, the pipeline stage callback labelled `pass_vrs_bilateral`, at its last stage for the view,
   walks the same light list. For each light with bit 3 set and a dynamic map (`+0x5f0` bit 0) it calls
   `f_802737a0(dta, light + 0x420)` and clears bit 3 (lr `0x802747b4`; a second copy of the loop at `0x80274b14`).
   VERIFIED: every free in the runs came from `0x802747b4`, in the same present as its draw.
7. **After the free.** The DTA's free puts the block back and the entry on its free LRU (`+0x29d0`). It does not touch
   the texture object, the TextureData or the descriptor id (rt-slots-impl.md 7, VERIFIED there by decompile). The
   light's two counts go at its next shadow draw (step 3). The entry's own count stays until the entry is taken again
   for another target. Then the alloc re-initializes the texture and re-registers it (`f_800634b0` frees the old id
   and takes a new one). The registration compare at alloc never hits in play (rt-slots-impl.md 3).
8. **Id reuse.** A count that reaches 0 puts the record on one of two deferred lists (`+0xa00`, picked by `+0xa60`).
   `f_80557420` drains them after the flip: the texture object goes back, it is finalized when its own count reaches 0,
   and the record becomes -1 (free). agl's ring has 32,768 records, about 25,000 of them free. Its cursor
   (`+0xa64`, monotone) moves about 10,000-27,000 a 600 presents, so a free id is registered again within seconds
   (VERIFIED, probe: `the ring 32768 records (25141 free), cursor ..., moved N in 600 presents`). The back end's heap
   entry for an id changes only when the game registers that id again. There is no "unregister" in NVN.

With rt_slots' parking (`HOS_RT_PARK`), the game's free of a target that is used again is intercepted. The entry is
kept whole and handed back at the next alloc with the same key, oldest first. In Kokiri one light rotates through 4-6
parked entries (`shadowlog`: entries `0x100dbdd780`, `0x100dbdfd00`, `0x100dbdd600`, `0x100dbdce80` in turn). An entry
not handed back for 6 presents is freed for real (the frees with lr 0 in the log, rt_slots' sweep). 79-94% of local
shadow allocations are hand-backs (probe line `local shadows since the last line: N allocs (M handed back)`).

**Why old ids keep naming freed maps:** at the DTA's free, the entry keeps its texture object registered. It holds a
count on that id until the entry is reused for another target, and with 650 free entries that can take thousands of
presents (probe: pinned images let go 3,600-18,000 presents earlier, `m6s_s3`). The lights let go of theirs one
present later. **When the game stops using them:** at the end of the present the map was drawn in. **Is a freed map
sampled for real?** No case was found in which the game samples a map after its present. The only binds of images the
back end had let go were maps drawn in the same present (44 of 44, `m6s_k4`), so their content matters and they were
not freed. Whether some light with bit 2 clear (no shadow drawn this frame) still has its lighting bind the handle left
at `*(light + 0x5e0)` was not seen in these runs: every light drew every present. It would be content-irrelevant on
the console (the memory is reused at once), so a shader would have to ignore it (INFERRED).

## 3. The probe and the runs

`HOS_DBG=pinlog` (back end), `HOS_DBG=shadowlog:<from>:<to>` and more lines under `HOS_RT_SLOTS_LOG=1` (host/rt_slots.c).
The patch is `work/proto/shadow/probe.patch`.
- **pin census**, every 600 presents: the images held only by descriptor ids (let go, refs 0, an id names them). It
  gives their ids against agl's books read from guest memory (the manager `*(*0x84ca2d60)`: record count `+0x0`, -1
  free; the DTA `*(*0x84ca9c68)`: entries `+0x11c8`, `+0x11c0` of them, the entry's id at `+0x68`, in use when
  `+0xc0` is set), how long ago they were let go, how far behind the ring's cursor their ids are, and their shapes.
- **draws binding a let-go image**: a driver-bank texture handle whose image is let go (`pinlogall`: every one logged).
- **shadowlog**: every `f_802640a0` (light, entry, its id, size wanted and given, storage, the ids and counts after)
  and every free of those entries (any caller).
- **local shadows since the last line**: allocations, hand-backs, sizes, the family's slots, distinct slot offsets.

| Run | What | Presents |
|---|---|---|
| `m6s_k1`..`k4` | Kokiri (`chisel_kokiri`) standing | 5,400-6,000 |
| `m6s_hf1` | Hyrule Field at chisel_hf standing (not walked: the known hang) | 6,000 |
| `m6s_s3` | M6f's scene route (`scripts/direct_walk_check.sh scenes`, `HOS_DBG=strict,strictall,...`): verdict pass | 10,200 |
| `m6s_c1` | scene cycle, 6 of `m6f_c3_d`'s stops x 2 (`work/proto/shadow/maps6.txt`), `HOS_DBG=strict,strictall,pinlog` | 12 stops |

No device was lost in any of them.

## 4. Measured

### 4.1 Standing

| | Kokiri (`m6s_k4`) | Hyrule Field (`m6s_hf1`) |
|---|---|---|
| lights casting, size a present | 1 (Navi), 1152 (1280 when the wanted size passes 1152) | 2: 2432 and 896 |
| local-shadow slots, MB | 6, 35-37 | 6, 115 |
| allocations / hand-backs a 600 presents | 600 / 472-486 | 1,200 / 1,111-1,123 |
| distinct slot offsets (all time) | 6 | 6 |
| images made a 600 presents after the load | 0-3 | 0-2 |
| pinned (held only by ids) | 10-15 images, 55-75 MB (2560x1440 colour, 1280² and 1152² maps, small depths) | 3, 45 MB (2560x1440 colour) |

Standing, the local maps are bounded: the slots are reused and nothing new is pinned.

### 4.2 The scene route (`m6s_s3`, M6f's route: market overlay, the drawbridge back and forth, 6 loads)

Every 600 presents (pin census, VERIFIED):

| block | pinned images | MB | 2432x2432 D32F | local-shadow sizes requested in the block | slots made / evicted / game's offsets (all time) |
|---|---|---|---|---|---|
| 1 | 27 | 121 | 1 | 2432, 896 | 184 / 40 / 23 |
| 3 | 270 | 1,907 | 55 (1,271 MB) | 2432, 1920, 1664, 1536, 1408, 1280, 1152, 640, 384 ... 508 | 522 / 392 / 198 |
| 6 | 364 | 2,377 | 69 (1,595 MB) | 2432, 768 | 671 / 513 / 238 |
| 10 | 543 | 3,149 | 95 (2,196 MB) | 2432, 896, 768, 1024, 384 | 881 / 746 / 307 |
| 13 | 584 | 3,854 | 127 (2,936 MB) | 2432, 896, 768, 1024, 384 | 1,063 / 924 / 352 |
| 17 | 571 | 3,499 | 115 (2,659 MB) | 2432, 896 | 1,128 / 1,001 / 364 |

- At the peak (block 13): 584 images, 3,854 MB. Square D32F maps account for about 3.3 GB of it: 2432² 2,936 MB, 1920²
  128 MB, 1536² 83 MB, 1024² 65 MB, 1664² 54 MB. The rest is screen-sized targets: 1536x864 R11G11B10 152 MB, 1536x864
  D32F 134 MB, 683x384 and 1366x768 targets of the other DRS levels, 2560x1440 colour.
- **Every pinned image is named by a free DTA entry's own id with count 1** (e.g. block 17: 575 ids, all counted, 575
  of them free entries' own). Ids the game had freed (count -1) appear only for a block after a load (888 in block 15)
  and are gone in the next: the ring's cursor reaches them.
- Pinned images were let go 600-18,000 presents earlier. Local-shadow slot offsets placed at (all time): 6 -> 94.
  "Renderable images made in play" (strict, after arming): 1,160 in about 150 s.
- Device-local driver usage 6,684 -> 11,745 MB. The pinned images are 3.5 GB of the 5 GB growth, and agl's LRU cache
  (0.4-0.8 GB here) is most of the rest.

**M6f's 4.8 GB reproduced and broken down:** the same route gave 3.5-3.9 GB here, against M6f's 4.8 GB (`m6f_s9_d`: 208
images of 2432x2432). The mechanism is the same: the 2432 map is one or two lights drawn every present, and each new
storage address it lands on (a slot remade after an eviction, or a placement at the game's offset when the 320 MB of
slots are full) is a new image, which the entry left on the free list then pins. How many addresses get used varies
from run to run.

### 4.3 Why the addresses keep changing

The route's targets need more than the 320 MB slot range: the slots are evicted LRU and remade at another offset
(`place()`), and placements that find no room take the game's offset below the range (`n.game_offset`). Each is a new
(address, shape) pair for the back end. A family's slot also holds several local-shadow sizes (rounded by 128). Near a
class boundary a light flips between two classes (Navi's 1142-1158: 1152 and 1280), so it uses two shapes, and so two
images, at one address.

### 4.4 The scene cycle (`m6s_c1`)

One boot into Kokiri, then `m6f_c3_d`'s first six stops twice: Death Mountain Crater, Goron City, Kokiri, Zora's
Fountain, Dark Link's room, the Forest Temple. 12 stops noted, no device lost. Pinned images (the census closest to
each stop), against `stops.txt`:

| stop | first visit: pinned images / MB (2432² MB) | second visit | free VRAM first / second (MB) |
|---|---|---|---|
| Death Mountain Crater | 285 / 700 | 565 / 2,423 | 5,060 / 2,490 |
| Goron City | 306 / 711 | 563 / 2,409 (1,063) | 5,054 / 2,489 |
| Kokiri | 517 / 1,407 | 693 / 2,963 | 4,118 / 2,227 |
| Zora's Fountain | 550 / 2,016 (370) | 688 / 3,948 (2,150) | 3,202 / 1,073 |
| Dark Link's room | 538 / 1,961 (462) | 590 / 3,348 (1,873) | 3,173 / 1,056 |
| Forest Temple | 612 / 2,375 (786) | 667 / 3,732 (2,220) | 2,876 / 1,051 |

On the second loop free VRAM fell under the 1.5 GB margin at Zora's Fountain. The eviction patch then emptied agl's
cache (`cache 0`, 5,443 textures evicted by stop 11), while 3.3-3.9 GB of pinned transient targets stayed. The patch
cannot reach them. This is why the targets have to be bounded before a VRAM-limit setting can mean anything. Strict
(survey) counted 2,626 renderable images made in play. Its 416 "texture with no image sampled" are another target
(800x760x16 array, format 0x6, a job list at lr `0x8008533c`), not a shadow map.

## 5. What the back end does today, and two faults found

### 5.1 The rule (M6f)

`host/rt_slots.c` tells the back end at the DTA's real free which texture is over the entry's block. It finds it by
storage address: `entry_tex` -> `nvn_tex_live_at(pool + block +0x38)`, the texture last initialized at that address.
The back end marks it pending. A texture initialized over overlapping storage makes the pending one let go of its
image, and the image then waits until no heap slot names it (`direct_image.cpp`: `wait_heap`, `heap_refs`). The game's
alloc that gives the entry back marks it held again, through the same address lookup.

### 5.2 The id hold is what pins

A heap slot is overwritten only when the game registers that id again, and a free entry's id is never released while
the entry is free. So every free entry whose texture was let go pins its image (4.2). Clearing slots when the game frees
ids would not help: the game has not freed these ids.

### 5.3 The address lookup loses targets that are in use (VERIFIED, `m6s_k4`)

44 draws bound an image the back end had let go. All 44 bound the map drawn by the light in that same present, at the
entry the shadowlog names (for example id 23023 at 0x10018410000, presents 2546-2576). Those textures were live and
drawn, but the back end held no reference on their images. INFERRED cause: when an entry comes back from the DTA's free
list, `alloc_gave` holds again the texture found at its storage address, which can be another object initialized there
in between (the entry's own object keeps its serial and stays "freed"). Without M6f's id hold these images would have
been destroyed while in use. That matches `m6f_s8_d`'s 10,079 "texture with no image sampled": 896x896 and 2432x2432
D32F and the 1536x864 depth, sampled by the lighting at `c2+0x20` in the long layer list. **So the id hold covers a
fault in tracking by address, and what it pins is the DTA's free entries.** On the scene route, 2,049+ binds of let-go
1536x864 and 683x384 targets (other passes, around loads) are most likely the same fault (INFERRED, not traced).

## 6. Options

Each option says what it changes, what it saves on the scene route (the 3.5-4.8 GB pinned), how it bounds VRAM for
the VRAM-limit setting (a menu value that sets the eviction margin: the transient targets must have a known, bounded
cost to be counted against it), and how it affects 60 fps (image creation in play costs frame time).

### a. One device allocation for the transient pool, images bound at their offsets (aliasing as on the console)

**What:** the back end allocates one `VkDeviceMemory` for the DTA's pool range and binds every image of a texture whose
storage is in it at (storage - pool base), instead of one allocation per image. With rt_slots this can be limited to
the slot range, 320 MB, if the game-offset fallback is removed (below). Pinned images then cost only their VkImage and
view objects.

**What Vulkan requires** (spec, "Resource Memory Aliasing"; RADV behaviour INFERRED from its known design):
- Images may be bound to overlapping ranges of one allocation. Unless they are created with
  `VK_IMAGE_CREATE_ALIAS_BIT` with identical parameters, which these are not (different sizes and formats), writing one
  makes the others' contents undefined. That is fine here: a transient's content is never read after another target
  used its memory (section 2, and rt-alloc.md: "nothing transient carries contents across frames").
- Before an alias is used after another one wrote the memory, it must be transitioned from
  `VK_IMAGE_LAYOUT_UNDEFINED` (this also initializes RADV's metadata: HTILE for depth, DCC/CMASK for colour), with a
  memory dependency on the other alias's last access. The back end keeps images in GENERAL, so it must add a "discard"
  barrier (UNDEFINED -> GENERAL, src ALL_COMMANDS) at the first use of an image whose range was last owned by another
  image. The back end has to track this, per slot: the image that owns the range now. The ownership changes at a
  texture's initialization over the range, which the back end sees (`image_texture_created`), so the barrier can be
  queued there. The game's clears are the first use of these targets (ClearDS on every shadow map, row 15). In-order
  execution on the one Vulkan queue orders the previous frame's use against it.
- **Sizes and alignment:** `vkGetImageMemoryRequirements` on RADV includes the metadata and a 64 KB (sometimes larger)
  alignment, so an image can be larger than NVN's storage size: for D32F 2432² about 1.5% (HTILE about 4 bytes per 8x8
  tile, INFERRED). Binding at the NVN offset can then overrun into the next target. rt_slots already owns the offsets,
  so each slot's size (`take()`'s `cap`) must be at least the Vulkan requirement of the largest shape it will hold. That
  takes a query to the back end, or a fixed margin. The 64 KB slot alignment already suits RADV.
- **The game's own reuse barriers do not help.** The DTA's per-range history (`+0xb0` of an arena) emits
  `nvnCommandBufferBarrier(.., 0x101)` or `f_810c7530` when a range is reused with another kind, but against its
  bookkeeping offsets, which rt_slots moved away from the memory (rt-slots-impl.md 6). They are NVN cache barriers, not
  layout transitions. The back end must make its own discard barriers.
- Lane aliasing within a frame (the console's 7 arenas over one range) does not happen with rt_slots: live targets
  never share a slot. Placements at the game's offsets (when the slots are full) would alias within a frame across
  workers' lists, which needs ordering between lists. So remove that fallback (a larger slot range, or a slot policy
  that never fails) rather than bind it.

**Saves:** all of the pinned bytes (3.5-4.8 GB on the route). The transient targets become a constant 320 MB (or
512 MB with the arenas' range). **VRAM limit:** a constant, counted once, which is the console's own budget for these
targets. **60 fps:** no memory allocation in play for targets. A new (address, shape) still makes a VkImage and its
views (cheap: no memory, about 20-50 µs, INFERRED). Size classes (c) keep that rare. **Risks:** a missed discard
leaves stale metadata (wrong depth, a flicker), which validation's synchronization checks can find. A slot sized below
RADV's requirement corrupts its neighbour. The M6f free/pending/hold machinery stays only for targets outside the range.
**Effort:** M: direct_vma (one allocation), direct_image (bind at offset, the owner per slot and its discard),
rt_slots (slot sizing, the range told to the back end).

### b. A game patch that releases the descriptor id at the DTA's free

**What:** at the DTA's free of an entry (the real free, not parking), release the entry's id. Call
`f_834bf9b0(*(*0x84ca2d60), entry + 0x68)`, which is the game's own count release onto the deferred list, then set
`+0x68 = -1` and clear the registration fields (`+0x70..+0xb4`, as `f_800634b0` does on its no-pool path). The alloc
already re-registers every entry it hands out (the registration compare never hits: rt-slots-impl.md 3). With
`+0x68 = -1` its compare misses for the same reason, and `f_800634b0` takes a new id (INFERRED from its decompile: the
`-1` path returns early on no pool and the register path takes a new slot). The back end must then learn when the
game's books free an id: a host hook after `f_80557420` (the drain) passes the ids that became -1 to a new
`hsd_texture_ids_freed`, and the heap stops counting them for the image (`slot_image[id] = nullptr`, `heap_refs--`).

**Saves:** the pinned images go 1-3 presents after the light's next draw (the light's two counts, then the drain).
That is all 3.5-4.8 GB. **VRAM limit:** bounded by the live targets plus a few frames, but not constant: it varies with
the scene and how often the game reuses addresses. **60 fps:** images are destroyed and made in play whenever an entry
comes back at an address whose image went. The back end keeps a dead image 8 presents (`image_sweep`) and a texture
over the same (address, shape) revives it. With parking most allocations are hand-backs, so the churn is the real
frees (about 0.1-0.2 a present a light, `m6s_k4`). **Risks:** a light that stops casting keeps its two counts, so one
map stays per such light (bounded by the lights). The fault of 5.3 remains (in-use maps let go), so (e) is needed with
it. **Effort:** S for the patch and the drain hook, plus (e).

### c. One shape per light, or a few size classes

**What:** in rt_slots' alloc hook (it already rewrites the descriptor: `HOS_RT_QUANT`), round local shadows to a few
classes, for example 512 / 1024 / 1536 / 2048 / 2432, with hysteresis per light (keep the light's class while the
wanted size is within the class and 80% of it), or one fixed size. A romfs change cannot do this because the
distance-to-size code was not found (1.3).

**Saves:** fewer shapes per address. Navi's 1152/1280 flip goes away, and the scene route's 16+ local sizes become
about 4. That divides the number of images per slot by the number of shapes a slot sees (about 2-4 on the route). It
does not stop the address churn (4.3), so the pinned bytes still grow, only more slowly. **VRAM limit:** not bounded on
its own. **60 fps:** fewer image creations. **Risks:** shadow detail: a light's map can be up to 25% coarser or finer
than the game asked for (the projection maps the frustum to the whole map; rt-slots-impl.md 8). **Effort:** XS. It is
worth doing with any other option.

### d. Host-owned pools by size class

**What:** rt_slots' slot becomes the identity. The back end keeps one image per (slot, class) for the slot's life, and
rt_slots tells it when a slot is made or evicted. A free entry's id then names an image the slot still owns, or nothing
once the slot is gone.

**Saves:** bounded by the slots' images: about 320 MB times the shapes a slot holds (1-2 with c). **VRAM limit:**
bounded, but by a product rather than a constant. **60 fps:** images are made only when slots are made, which is rare
standing still and frequent on loads. **Risks:** a free entry whose slot was evicted names a destroyed image. The game
does not sample it (2), but strict would count it as "no image sampled" if any draw bound it. **Effort:** M. It is a
middle way between a and b without memory aliasing.

### e. Targets told by the allocator's hooks, not recognised by address

**What:** rt_slots records each entry's own texture object when the DTA initializes it (`nvnTextureInitialize` with
lr in `f_8005ed00`, where `rt_slots_texture_init` already gets the object, and `alloc_sp` + 40 holds the entry from
0x80060308 on). It tells the back end by object at the real free (`hsd_texture_freed(obj)`), at a hand-back or an alloc
that returns the entry (`hsd_texture_held(obj)`), and at the entry's re-initialization. `entry_tex()` and
`nvn_tex_live_at()` go from this path.

**Saves:** nothing on its own, since the free entries' ids still pin. It removes the fault of 5.3 (in-use maps let
go), which is needed before the id hold can be dropped (with b or d). It is the design the plan already names ("render
targets told to the back end by the allocator's hooks instead of recognised by address", Status and next steps).
**Effort:** S.

### Table

| | Changes | Saves (route) | VRAM bounded / constant | Images made in play | Effort | Main risk |
|---|---|---|---|---|---|---|
| a. aliasing in one allocation | back end + rt_slots sizing | 3.5-4.8 GB | yes / yes (320 or 512 MB) | VkImage + views only, no memory | M | discard barriers, RADV metadata sizes |
| b. release id at DTA free | game patch + drain hook + (e) | 3.5-4.8 GB | yes / no | made and destroyed with real frees | S (+e) | churn, lights keeping old maps |
| c. size classes | rt_slots (hook) | some (shapes / slot) | no | fewer | XS | shadow detail |
| d. pools per slot and class | rt_slots + back end | ~3.5 GB | yes / no (slots x classes) | at slot creation | M | evicted slots under free entries |
| e. identity by hooks | rt_slots + host | 0 alone | - | - | S | none (fixes 5.3) |

## 7. Recommendation

**First: a + e + c.** Bind rt_slots' slot range into one fixed device allocation, with a per-slot owner and a discard
barrier on ownership change. Size the slots by Vulkan's requirement. Remove the game-offset fallback. Tell the back end
about targets by entry identity (e). Use size classes with hysteresis for local shadows (c). It matches the console
model, removes all the pinned memory, needs no policy, and gives the VRAM-limit setting one constant to count. It also
removes image memory allocation from play (60 fps). M6f's free/pending/hold machinery can then go for targets in the
range.

Patch sites:
- `host/rt_slots.c`: `shrink_arenas()` (tell the back end the slot range `[dta_pool_gpu + lo, + hi)`). `take()` (cap
  = max(NVN need, the Vulkan requirement of the shape, from a new back-end query or a fixed margin), keeping the
  family's +25%). `place()` / the splice: no `game_offset` placement (evict or grow until there is room, or count it as
  a strict event). `alloc_pre()` (classes and hysteresis in place of `HOS_RT_QUANT` at lr `0x80264130`). The
  `rt_slots_texture_init()` / `alloc_gave()` / `PATCH_HOOK(0x802737a0)` trio (entry -> object, e).
- `gpu/direct/direct_api.h`: `hsd_transient_range(lo, hi)` and an image-size query, `hsd_texture_freed/held` by
  object (exists; the host passes the right object).
- `gpu/direct/direct_vma.cpp` (the one allocation, counted in `hsd_vram_free`'s figure), `gpu/direct/direct_image.cpp`
  (`mem_image` for keys in the range: create and bind at the offset; the owner per slot; the discard barrier queued
  at `image_texture_created` and emitted on the upload command buffer before the next submit, as `init_image` does).

**Second: b + e + c.** The game patch at the DTA's real free releases the entry's id
(`PATCH_HOOK(0x802737a0)` in `host/rt_slots.c`, after the lifted body when the free is not parked: `f_834bf9b0(mgr,
entry + 0x68)`, `+0x68 = -1`, clear `+0x70..+0xb4`). A host hook on `f_80557420` (a PATCH_HOOK that runs the body,
as rt_slots' alloc hook does, then reads which records of the drained list are -1) calls a new `hsd_texture_ids_freed`,
and `direct_heap.cpp` drops those slots' `slot_image`. Then (e) and (c) as above. This is smaller and keeps the back
end plain (rule 8), but VRAM for targets is a moving figure and images are made and destroyed in play.

Either way, check with the scene route and the cycle under strict: pinned bytes (the probe's census) about 0, "texture
with no image sampled" 0, and "renderable images made in play" counted per minute.

## 8. Not known

- The distance-to-size rule of local shadows (the writer of light `+0x5e8`), and the class `ResolustionMaginify`
  applies to.
- Whether the lighting binds the handle `*(light + 0x5e0)` for a light that did not draw its map this frame (every
  light in these runs drew every present). If it does, the content is the console's garbage, so it must be ignored by
  the shader; not checked by a picture experiment.
- The exact cause of 5.3 (which object `nvn_tex_live_at` returns at the hand-back): inferred, not traced per object.
- The 2,049+ binds of let-go 1536x864 / 683x384 targets on the scene route (other passes), and whether they are the
  same fault.
- RADV's exact image sizes against NVN's for these shapes (a's slot sizing): one `vkGetImageMemoryRequirements` table
  would settle it.
- Static and updatable-static local shadows (kept textures in the shadow object) were never met in these scenes.
