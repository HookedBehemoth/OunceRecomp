# The transient pool: one device allocation for the game's transient render targets

**MEASURED** means read from a run's log or a tool's output (named). **INFERRED** means concluded and not confirmed.

**Device losses.** None of this branch's runs lost the device (22 runs: Kokiri standing, the scene route, the scene
cycle, two picture checks). The reset in the kernel log at 14:03:35 fell between two of these runs and belongs to the
other agent's work.

## 0. Short

- **The transient targets now cost a constant 512 MB of device memory**, allocated once when the game's allocator is
  first used (during the boot, before the first target exists), and nothing else. No device memory is allocated or
  freed for them in play (MEASURED: the allocation is one `vkAllocateMemory`, and nothing else binds a transient
  target; the pool's line says `512 MB of device memory (constant)` in every block of every run).
- **VRAM no longer grows across scenes** (MEASURED). The scene route's device-local usage is flat after the load
  (7,758 -> 7,806 MB over 6,000 presents, `sp_s7`; before: 6,684 -> 11,745 MB, `m6s_s3`). The scene cycle ends its
  second loop at 8,624 MB against 8,451 MB after the first (`sp_c1`; before: 10,647 and 12,473 MB, `m6s_c1`), and free
  VRAM stays at 4.8 GB, so the eviction patch never fires.
- **No image is missing and none is used after another took its memory** (MEASURED on the route and the cycle: 0
  "a texture with no image sampled", 0 "a transient target sampled after another took its memory", 0 "taken while it
  may be in use", 0 "does not fit its slot", 0 "placed outside the slots").
- **M6f's free / pending / hold machinery is deleted.** The descriptor-id hold that pinned 3.5-4.8 GB is gone with it.
- Shadows look as on head (section 6).

## 1. What is built

### 1.1 The pool (option a)

`gpu/direct/direct_vma.cpp` (`mem_pool_make`, `mem_pool_bind`), `gpu/direct/direct_image.cpp`.

The host names one range of storage addresses, once (`hsd_transient_pool`). The back end allocates that many bytes with
its own `vkAllocateMemory` (a device-local type that is not host-visible), outside VMA's blocks. A texture initialized
at a storage address inside the range gets a `VkImage` created without memory and bound in that allocation at
(storage address - range start). Two images whose ranges overlap share memory, as the targets do on the console.

**Ownership.** Each pooled image knows whether it owns its range. An image **claims** its range when its texture is
initialized over it, when it is made, or when the game's allocator gives its target out (1.3). A claim by the owner
does nothing. A claim by another image ends the ownership of every image whose range overlaps, then runs `init_image`
on the claiming one: a barrier from `VK_IMAGE_LAYOUT_UNDEFINED` to `GENERAL` with source stage `ALL_COMMANDS`, and a
clear to zero. That is the discard Vulkan asks for before an alias is used after another one wrote the memory, and it
resets RADV's metadata (HTILE, which lives inside the image's own range). The clear makes the content what a new image
had on head (zero), so a pass that reads a target before writing it sees what it saw before.

**Where the discard goes.** It is sent at once, on the claiming thread's upload buffer, which is after every list
already sent and before every list sent later. That is right only if no list that still uses the old owner is unsent.
The host guarantees it (1.2, `REUSE_AFTER`), and the back end checks it: a claim that takes memory from an image claimed
within the last 2 presents is the strict event `a transient target's memory taken while it may be in use` (0 in every
run).

**Image objects are kept.** A pooled image holds no memory, so it is not destroyed 8 presents after its last texture
goes, as other images are. It stays for the next texture of that storage and shape. Past 1,024 objects
(`kPoolImages`), the ones no texture is initialized over go, longest unclaimed first, after the usual 8 presents and
the GPU's completion. The bound was not reached in any run (926 objects at most, `sp_s1`), so that path is not
exercised by a run (the `prune` path: INFERRED correct from the code).

**Fit is checked, loudly.** Before a texture is initialized in a slot the host tells the back end the slot's bytes
(`hsd_transient_slot`). An image that needs more, a misaligned offset or a memory type the pool cannot take is not
bound: the strict event `a transient target does not fit its slot`, a log line, and the placeholder. There is no
fallback to an image with memory of its own. MEASURED: it fired while the slots were still sized by the game's figure
(`sp_k1`: 1920x1080 R8 needs 2,621,440 bytes, its slot had 2,228,224) and once for the cascade array when the target
type was guessed wrong (`sp_k3`), and 0 times since.

### 1.2 The slots (host/rt_slots.c)

- **The slots lie after the pool's end** (direct mode). The game's pool is 512 MB of GPU addresses; `host/nvn_native.c`
  now leaves 2 GB of addresses unused after every virtual pool, and the slot range is the 512 MB of offsets past the
  pool's size. The arenas are no longer shrunk: they keep the whole pool for their bookkeeping (the fullest arena
  reached 130 MB on the route, more than the 192 MB range left them much room for). Nothing is mapped at the slots'
  addresses; the direct back end never reads a texture's storage. With another back end the slots stay in the top
  320 MB of the pool, as before.
- **Slots are sized by the device's need.** `device_need()` asks the back end what an image of the target needs
  (`hsd_image_need`: a probe image's `vkGetImageMemoryRequirements`, cached by shape). The target's NVN format comes
  from the game's own table at `0x83f4848c` (u32 per agl format, 0x5f entries, the one `f_800634b0` searches), and its
  NVN target is agl's type number. Both are checked against each texture's own description when it is initialized in a
  slot (`rt_slots_texture_init`): 0 mismatches on the route and the cycle. The game computes a target's size itself and
  does not ask `nvnTextureBuilderGetStorageSize` on this path (MEASURED, `sp_k2`: the only return addresses in the allocator's code
  that ask it are `0x80060c34` and `0x80063c74`, and the slots sized from its answers did not hold the images).
- **`REUSE_AFTER` = 3 presents.** A slot's memory goes to another image (another shape of the slot's family, a slot
  made over an evicted one, a parked entry's slot taken) only 3 presents after its last use. Measured basis
  (`sp_s3`, the back end's line): of 71,921 lists, 71,914 reached the queue in the present they were begun and 7 one
  present later; of 308,751 chunks, 308,739 were sent to the device in the present of their submit and 12 one present
  later; none later than that. So a list can reach the device 2 presents after it was begun, and 3 is the first safe
  number.
- **When the range is full:** idle slots go, least recently used first, at 8 idle presents as before; with none of
  those, at 3. With none of those either, the slot of a parked entry not asked for in 3 presents is taken (this was the
  `HOS_NATIVE_TEXALLOC` experiment's `steal`, now always on). Large slots (8 MB and more) are placed from the top of
  the range, small ones from the bottom.
- **A target that still finds no slot** keeps the game's offset, gets an image with memory of its own, and raises the
  strict event `a transient target placed outside the slots`. This is the one place where the pool can be bypassed. It
  is 0 at 512 MB on the route and the cycle. At 320 MB it was not: section 3.

### 1.3 Identity from the allocator (option e)

`alloc_gave()` in `host/rt_slots.c`. Every entry the allocator returns (new, from its free lists, or a parked one
handed back) is told to the back end by **its own texture object**: the entry's descriptor id (`+0x68`) names a record
of agl's descriptor manager (`*(*0x84ca2d60)`: records of 0x28 at `+0x9f8`), whose `+0x18` is the holder of the texture
object registered under that id. This is read from `f_800634b0`'s decompile: the texture object is the manager's, not
the entry's. The object's storage is checked against the entry's block before it is told. The call is
`hsd_target_given`, and the back end claims the range for that object's image.

MEASURED (`sp_s7`): 375,000 targets given by their object, 0 entries with no registered object, 1 with an object over
other storage (at the boot). In a block of the route 5-15 of about 39,000 given targets were not their range's owner
and were claimed there: these are the entries that come back without a new initialization, the case the address
lookup got wrong (`shadow-maps.md` 5.3).

The address lookup (`entry_tex`, `nvn_tex_live_at` from this path) is gone. The game's free tells the back end
nothing: the memory is the pool's either way.

### 1.4 Size classes with hysteresis (option c)

`shadow_size()` in `host/rt_slots.c`, in place of `HOS_RT_QUANT`'s rounding to 128. A local-light shadow map gets the
next class up of 256, 384, 512, 640, 768, 1024, 1280, 1536, 2048, 2432 (above that, the next multiple of 128). A map is
never smaller than the game asked for. Each light (its shadow object, noted by a new entry hook on `f_802640a0`)
keeps its class until the size it wants passes the class, or falls to 90% of the class below. `HOS_RT_QUANT=0` turns
the classes off.

MEASURED: Kokiri standing, Navi's light wants 1142-1158 and now always gets 1280 (it flipped between 1152 and 1280
before); 0 class changes in 4,200 presents (`sp_k4`). On the route: 17,950 sizes changed to a class, 8 changes up, 2
down, 3,980 kept by the hysteresis where the plain class would have changed (`sp_s3`).

Cost: a map can have up to 1.5 times the side the game asked for at the smallest step (256 -> 384) and 1.33 at most
at the others, so more depth texels are drawn. The shadow filter's width is in texels (`BlurWidth`), so a finer map is
filtered over a slightly smaller area. INFERRED harmless; the pictures show no difference I can see (section 6).

## 2. What was deleted

- `gpu/direct/direct_image.cpp`: `Pending`, the `pending` and `freed` tables, `storage_reused`, `hold_again`, `let_go`,
  `image_texture_freed`, `image_texture_held`, the five `st_freed*` counters, and the freed-texture branches of
  `image_texture_released`, `image_texture_registered` and `image_find`.
- `gpu/direct/direct_image.h`: `Image::heap_refs` and `Image::wait_heap` (the descriptor-id hold), and with them the
  counting in `direct_heap.cpp`.
- `gpu/direct/direct_api.h`, `direct_main.cpp`, `host/direct.h`, `host/rx_backend.c`: `hsd_texture_freed` and
  `hsd_texture_held`.
- `host/nvn_native.c`: `nvn_texture_storage_freed`, `nvn_texture_taken_again`.
- `host/rt_slots.c`: `entry_tex`, the telling at the DTA's free, `rt_slots_freed_line`, `HOS_DBG=nodtafree`, and
  `HOS_RT_QUANT`'s rounding.

Every other texture keeps one rule: its image lives while a texture is initialized over it, and 8 presents more. For
agl's asset textures that is the game's destroy, as M6f had it.

## 3. The constant, and why 512 MB

The constant is the slot range. I first built it at 320 MB (the range rt_slots had), and it was not enough.

| Range | Run | Targets placed outside the slots | Device-local usage over the route (MB) |
|---|---|---|---|
| 320 MB in the pool | `sp_s1` | 48 | 6,462 -> 8,492, flat from block 4 |
| 320 MB, large slots from the top | `sp_s2`, `sp_s3`, `sp_s5` | 35, 26, 118 | 6,483 -> 8,115; -> 8,208; -> 9,524 |
| 1,024 MB after the pool (to measure) | `sp_s6` | 0 | flat at 8,26x-8,29x |
| **512 MB after the pool** | `sp_s7`, `sp_c1` | **0, 0** | 7,758 -> 7,806; the cycle below |

MEASURED, why 320 MB fails (`sp_s5`, the dump of the slots at a failed placement, `HOS_RT_SLOTS_LOG=1`): at a scene
load the dynamic resolution steps through several levels in a few presents (1536x864, 1457x820, 1366x768 ...). Each
level is a full set of about 100 MB of targets, and each set stays parked for 6 presents and may not be reused for 3.
At the failed placements every slot was in use or parked 0-2 presents ago. Each target placed outside is an image with
memory of its own, held afterwards by a free entry: the growth of 26 x about 20 MB in `sp_s3` is 512 MB of VMA blocks.
That is the old leak in small, so the range has to hold the peak.

MEASURED, what the peak is: the bytes of slots that are in use, parked, or used within 3 presents, taken at every
present (`rt slots: transient pool: N MB of 512 held`). The most since the start was 465 MB on the route (`sp_s7`, at
a load) and 406 MB on the cycle (`sp_c1`); in steady blocks it is 257-392 MB.

So **512 MB holds the measured peak with 47 MB to spare**, and it is the console's own budget for these targets (its
pool is 512 MB). It is not proven for every scene: the full cycle of 26 stops and a soak were not run (section 8). A
load that needs more raises the strict event and the count in the log, and `RX_RT_SLOTS_MB` changes the constant (up
to 2,048).

The game's pool grows with the output resolution (host/nn_applet.c hos_rt_pool_size: 687 MB at 3840x2160), and the range
did not: at 3840x2160, 512 MB placed 6,471 targets outside the slots in 120 s of the HF market walk (p99 frame time 43 ms
against 27-32 ms), and 12 at 2560x1440, where the peak held reached 468 MB but no gap was left for a new size. The range is
now twice the game's pool, at least 512 MB: 0 outside at both sizes, peak held 534-565 MB and 1,004 MB.

One thing tried and rejected: handing back the parked entry of a key that was parked last, instead of the oldest, so
that spare entries of a key age out. It made the route worse (163 targets outside, `sp_s4`) and was reverted. I did not
find out why.

## 4. Numbers

### 4.1 The scene route (`scripts/direct_walk_check.sh scenes`, 6 loads)

| | Before (`m6s_s3`, head with the probe) | After (`sp_s7`) |
|---|---|---|
| Verdict | pass | pass |
| Device-local driver usage, first -> last block (MB) | 6,684 -> 11,745 | 6,311 -> 7,806 (7,758 at block 2) |
| Images pinned by descriptor ids | 571-584 images, 3,499-3,854 MB | 0 MB (343 ids name an image that is not its range's owner: objects only) |
| Device memory for transient targets | grows with every new address | 512 MB, constant |
| Renderable images made in play (strict, after arming) | 1,160 | 415 (objects bound in the pool, no memory) |
| Targets at the game's offsets | 364 | 0 |
| "a texture with no image sampled" | 0 (with the id hold) | 0 |
| Draws binding a let-go / non-owner image | 2,049+ (`shadow-maps.md` 5.3) | 0 |
| Pool image objects kept | - | 605 (bound 1,024) |

`sp_s7` made 7,200 presents in its 185 s where the other runs made 10,200: the other agent's work was running. The
runs at 320 MB (`sp_s1`..`sp_s3`) made 10,200 and show the same flat line after the load; I did not repeat the 512 MB
run on a quiet machine.

### 4.2 The scene cycle (`MAPS=work/proto/shadow/maps6.txt LOOPS=2 scripts/direct_scene_cycle.sh`)

Device-local driver usage (MB) and free VRAM (MB) at each stop:

| Stop | Before, first / second visit (`m6s_c1`) | After, first / second visit (`sp_c1`) | Free VRAM after, first / second |
|---|---|---|---|
| Death Mountain Crater | 8,479 / 11,033 | 7,971 / 8,454 | 5,362 / 4,996 |
| Goron City | 8,485 / 11,034 | 7,977 / 8,468 | 5,492 / 4,982 |
| Kokiri | 9,420 / 11,296 | 8,259 / 8,614 | 4,692 / 4,836 |
| Zora's Fountain | 10,331 / 12,451 | 8,269 / 8,616 | 5,037 / 4,833 |
| Dark Link's room | 10,355 / 12,466 | 8,302 / 8,618 | 5,159 / 4,831 |
| Forest Temple | 10,647 / 12,473 | 8,451 / 8,624 | 4,998 / 4,825 |

- After the second loop usage is 173 MB above the end of the first (8,624 against 8,451). Of that, the live images
  account for 46 MB (5,263 -> 5,309 MB, of which the store's 4,514 -> 4,544), and VMA's blocks went up by one 128 MB
  block (5,527 -> 5,655). The transient pool's share is 512 MB at every stop. I read the rest as store textures and
  block granularity, not growth of targets (INFERRED from those three figures).
- Before, the second loop ran into the 1.5 GB margin and the patch evicted 5,443 cached textures. After, it evicted 0.
- 12 stops noted, every stop ready, no device lost. Strict (survey) after arming: 0 "no image sampled" (416 before),
  869 renderable images made in play (2,626 before).

### 4.3 Standing in Kokiri (`sp_k4`, 4,200 presents)

197 pool image objects, 187 own their range, 0 made after the load, 0 claims after the load, 10 ids naming a non-owner
image. Strict after arming: no event of any reason.

## 5. The diagnosis that stays

Every 600 presents (the back end's block; a per-present line would be 60 lines a second):

- `direct: transient pool: N MB of device memory (constant), N image objects kept (N own their range, N with a texture
  initialized over them; bound 1024), ... N made, N destroyed over the bound, N claims with a discard and clear, N
  targets given out by the allocator (N of them not their range's owner: claimed); since the start: N taken while in
  use, N that did not fit their slot, N draws binding an image that is not its range's owner`
- `direct: transient pool: descriptor ids naming a transient target that is not its range's owner: N (N square depth
  maps)`. This is what the pin census counted. They hold an object each and no memory.
- `direct: transient pool: lists handed to the queue 0, 1, 2, 3+ presents after they were begun: ...; chunks sent
  ...`: the basis of `REUSE_AFTER`. A count under "2" for lists or chunks would mean 3 is no longer safe.
- `rt slots: transient pool: N MB of 512 held at a present at most ...; N slots evicted before 8 idle presents, N
  targets placed outside the slots; shadow classes ...; targets given ...; slots told ...; slots sized ...`

Strict events (all 0 on the route and the cycle): `a transient target sampled after another took its memory` (the
"let-go image bound" of the probe; checked for every draw's driver-bank handles when strict is on), `a transient
target's memory taken while it may be in use`, `a transient target does not fit its slot`, `a transient target placed
outside the slots`, `the transient pool was not made`. "A texture with no image sampled" is the existing event.

`HOS_RT_SLOTS_LOG=1` adds the slots' state at the first 6 failed placements. `RX_NVN_LOG_TEX=<w>x<h>` logs each
initialization and finalize of textures of one size (used for section 7).

## 6. Shadows

- Kokiri at the spawn, Navi's local light and Link's shadow from it: `work/runs/sp_k4/shots/vk_003600.png` against
  head's `work/runs/m6s_k4/shots/vk_003600.png`. Same picture apart from Navi's place in her flight.
- The cycle's stops against head's (`work/runs/sp_c1/stops/` and `work/runs/m6s_c1/stops/`): the Forest Temple's
  courtyard (both visits), Goron City, Kokiri, Dark Link's room. I see no missing or wrong shadow, no flicker in the
  stills, no garbage. Link's health differs between the runs (he had taken damage in mine); that is play, not picture.
- The picture checks (`scripts/direct_check.sh`): section 6.1.

A still cannot show a one-frame flash. The counters that would show its cause (a target taken while in use, a
non-owner sampled) are 0.

### 6.1 The picture checks

`scripts/direct_check.sh` against the old renderer's golden pictures, 420 s each, `work/runs/sp_chk*.verdict.txt`. Every
run's verdict is **FAIL**; none is a pass, so the checks do not show the pool equal to head. Counts, pass states lined up
or not (FAIL / UNSTABLE or NO-NOISE / MERGED / PASS):

| Scene | Head, no pool (`sp_chkhead_*`) | Pool build (`sp_chk_*`) | Pool build, second run (`sp_chkq_*`) |
|---|---|---|---|
| Kokiri | 331: 52 / 9 / 228 / 42 | 338: 25 / 9 / 232 / 68 (1 missing, 3 extra) | 408: 24 / 9 / 305 / 70 |
| Market | 339: 23 / 2 / 243 / 71 | 338: 24 / 2 / 241 / 69 (1 missing, 1 extra) | 339: 38 / 2 / 243 / 56 |

The head runs are the build without the pool (their log has 192 MB arenas and no pool lines); the other two have the
pool. The lit target 2048x1152 B10G11R11F fails in all of them (Kokiri, worst tile 0.36 on head, 0.13 on the pool
build). In Kokiri the pool build fails fewer states than head (24-25 against 52); in the market 24 and 38 against 23.
The verdict files do not say what the second pool run (`sp_chkq_*`) was set to, and I have not looked into the failing
states one by one. So the checks ran, and the pool is not shown equal to head pass by pass.

## 7. The 416 "texture with no image sampled" of `m6s_c1` (the 800x760x16 array)

It is not a render target. MEASURED (`sp_c1`, `RX_NVN_LOG_TEX=800x760`): the texture is one of agl's TextureDataMgr
textures. It is initialized in pool 127 (`0x109c190000`) by the load path (`lr 0x807989e8`) on SceneLowPriority while
Dark Link's room loads, moved twice by compaction (`lr 0x8079856c`), and finalized by the game's destroy
(`lr 0x8079c4a8`) 3,000 presents later, after the room is left. On the second visit the same happens again.

In `m6s_c1` all 416 events fell in one present (9277, 170.7 s), 19 s after Dark Link's room was ready the second time,
while the VRAM eviction patch was emptying agl's cache (evicted 3,896 at 166 s, 5,443 at 176 s, cache 0). INFERRED: the
game released the room's texture to its cache at the scene change, the patch had the game destroy it at once (free
VRAM was under the margin), and job draws recorded for that present still named it. On the console a destroyed
texture's memory still holds its texels for that frame. In `sp_c1` free VRAM never came near the margin, the patch
evicted nothing, and the event count is 0.

So it is a consequence of evicting at the margin, not of the targets. It would come back whenever the patch fires. A
fix belongs to the patch (for example: not destroying a texture released in the same present), not to this work.

## 8. What remains

- **The constant is measured on two routes only.** The full 26-stop cycle (with the age switch), the walk, and a soak
  were not run. If a load needs more than 512 MB, the strict event says so.
- **The route at 512 MB was not repeated on a quiet machine** (7,200 presents in `sp_s7`).
- **The bound on kept image objects was never reached**, so its path is untested by a run.
- **Hyrule Field** was not run by this branch at all (the walk's known hang; the route starts at the market's drawbridge).
- **Persistent targets of the same allocator** (33 allocations in a run) are outside the slots and keep images with
  memory of their own, freed when their texture object is initialized again. They are not transient and were not
  changed.
- **The VRAM-limit setting.** The transient targets are now a constant the setting can subtract: 512 MB
  (`mem_pool_size()`; it is in the driver's usage from the boot on, so `hsd_vram_free` already counts it). The 1.5 GB
  margin of the eviction patch was sized for a scene change that allocates 0.6-1.4 GB of store images *and new
  targets*; the targets' part is now 0, so the margin can be measured again and lowered. Not done here.
- **The eviction at the margin destroys textures still drawn** (section 7).
- `docs/direct-backend-plan.md`'s status table is the coordinator's
  to update.

## Code

- `gpu/direct/direct_vma.cpp`, `direct_internal.h`: the pool's allocation and binding.
- `gpu/direct/direct_image.cpp`, `direct_image.h`: `shape()` (an image's description, apart from its memory),
  `pool_image`, `claim`, `image_pool_range`, `image_pool_slot`, `image_need`, `image_target_given`, the kept objects in
  `image_sweep`, `image_pool_stats`; M6f's machinery out.
- `gpu/direct/direct_heap.cpp`: `heap_stale`, `heap_stale_census`; `heap_refs` out. `direct_draw.cpp`: the strict event
  for a non-owner image sampled. `direct_sync.cpp`: `sync_age_stats`. `direct_check.cpp`: the known list's note.
- `gpu/direct/direct_api.h`, `direct_main.cpp`, `host/direct.h`, `host/rx_backend.c`: `hsd_transient_pool`,
  `hsd_image_need`, `hsd_transient_slot`, `hsd_target_given`, `hsd_host_event`. `gpu/legacy/vk/vk_api.h` is unchanged, so
  `MKWVK_API_VERSION` is too.
- `host/rt_slots.c`: section 1.2-1.4. `host/nvn_native.c`: the unused addresses after a virtual pool, the slot's bytes
  told before a texture's initialization, `RX_NVN_LOG_TEX`.
- `tests/test_rt_slots_native.py`: the reuse rule, the young eviction, the aged parked slot, large slots from the top.
- `games/oot/labels.txt`: notes on `f_8005ed00`, `f_802737a0`, `f_800634b0`, `f_802640a0`.
