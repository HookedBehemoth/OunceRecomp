# The patch sites, confirmed by runs

These sites were confirmed by runs of logging hooks that are no longer in the tree (Kokiri and Hyrule Field, the pause menu, map changes). Counts are over presents 450-2400 (about 1,900 frames) unless stated.

## 1. Pass keys

**CONFIRMED, with one correction: the key at `0x8028a3c4` is not the recorder's pointer.**
- The recorder's x3 is stable and the same in every boot and scene: Kokiri and HF have the same pointers (12 pipeline
  keys `0x113b646ff8..0x113b6486b8`, 4 probes `0x115bd74ec8 + n*0xc0`, cube faces `0x1137375608 + n*0xc0`). Steady play
  has 18-19 job keys a frame (12 + 4 + 1-2 cube faces); 23-25 distinct over a run (the other cube faces record once, at
  the load).
- **Map changes** (`m3_cyc`: Kokiri -> Death Mountain Crater child -> adult -> Dark Link's room -> Volvagia, 3,300 presents):
  the same pointers throughout; keys come and go with the scene (the cascades `0x113b646ff8..7198` stop in the crater, a
  new pipeline index `0x113b648518` and the second cube-face set `0x11373756c8 + n*0xc0` appear), 31 distinct over the
  run. A view record changed key 53 times, all at scene changes (records are handed out by arrival).
- **Formats per key never changed** (0 in all three runs). The only target changes are sizes (DRS): 1536x864 -> 683x384 ->
  back at frames 455-461 in Kokiri, 1536x864 <-> 1457x820 at frames 3246-3303 in `m3_cyc`, none in HF. Samples 1 (NVN 0).
- `f_806688c0` (`0x80668a9c`): `[sp+0x18]-0xc0` is the recorder's pointer; every job called there matched it.
- **`f_8028a2a0` (`0x8028a3c4`): x23 is a copy of the key** on the worker's own stack (`0x10ffbb8`, `0x1007712e28`, ...: one
  pointer per thread for the same probe); the game finds the record by CRC + memcmp. By pointer, 55,596 of 196,020 job
  calls missed (`m3_gold_k`: every probe and cube-face job). Mapped through the found view record (`[sp]` there; the
  recorder's x25 at BeginRecording -> its x3), **every job call matches its site's key in the same frame**: 193,160 of
  193,169 in `m3_gold_hf`, 141,458 of 141,546 in `m3_cyc`, the rest called from other sites, none different. So: table
  keyed by the recorder's pointer; site `0x8028a3c4` takes the key from the record (a record -> key map written at job
  entry), not from x23. 18 jobs in `m3_gold_k` were recorded and never called (load frames).

## 2. The inline path (`f_80681320`)

**CONFIRMED complete.** The recorder hook returns at entry (the same as its early return at `0x8008693c`: no record, no
slot) for the chosen keys; the layer lists then draw those passes through `0x80668d80` / `0x8028a588`.

| run | forced | job calls | passes drawn inline | pictures vs golden | serial inline time |
|---|---|---|---|---|---|
| `m3_inl_one_k` | `0x113b647338` (first colour pass: culling waits, a dispatch, GPU-culled mesh draws) | 183,450 others | 1 | PASS, worst distance 4 | 0.16 ms a frame |
| `m3_inl_all_k` | all | 0 | 45 keys | 12 of 13 equal; present 2400 distance 16 | 0.80 ms a frame |
| `m3_inl_all_hf` | all | 0 | 43 keys | 7 of 8 equal; present 2400 distance 14 | 2.2 ms a frame |

The two "bad" shots differ only in Link's idle pose and Navi's position (checked by eye). No pass failed, no new log
group. Frame rate unchanged (Kokiri 60/60, HF 50.8 vs 49.2 fps): the cost moves from the workers to the layer-list thread.
Dearest in HF: cascade `0x113b647198` 0.56 ms, colour passes `0x113b648038` 0.49 ms and `0x113b647338` 0.45 ms a frame.
The game already draws 20 `f_8028a2a0` keys inline every frame (local passes with no jobs, 7-15 us each).

## 3. Inherited task/mesh storage buffers

**CONFIRMED: three drawers read slots their job never bound, and the values depend on submit order in one of them.**
Logged at every in-job `DrawMeshTasks*`: the drawer's LR, whether the job bound stage 6 slots 0-3 / stage 7 slots 0-3 before
it, and, at the job-call site, what the calling list's slots held by then (its own binds plus the jobs called before).
Slots below: s6.0, s6.1 (task) and s7.1 (mesh); s6.2/s6.3/s7.2/s7.3 are inherited by every drawer, always the system values.

| drawer (DrawMesh LR) | function | Kokiri (`m3_gold_k`), draws a frame | HF (`m3_gold_hf`) |
|---|---|---|---|
| `0x801a45a8` | `f_801a43e0` | 2 own, 2 inherit s6.0/s6.1 = `f_82c02bc0`'s binds (`0x82c02cf4`/`0x82c02d3c`) and s7.1 = the pre-draw hook's bind, all from an earlier job of the pass | 4 own |
| `0x801a48f4` | `f_801a46a0` | 2 own, 3 inherit, as above | 5 own |
| `0x801a690c` | `f_801a6720` | 5: s6.0/s6.1 inherited, **2 a frame the system values from the layer list** (`f_80101ec0` <- `0x8027caa0`), **3 `f_82c02bc0`'s** from an earlier job | 5: 3 own, 1 system (list), 1 unbound in the list (state from an earlier command buffer) |
| `0x801a62dc`, `0x801a65f8` | `f_801a60f0`, `f_801a6410` | 3 own, 2 the system values (list) | 3 own, 1 system, 1 unbound in the list |
| `0x82c02dc0` | `f_82c02bc0` | s6.0/s6.1 own; s7.1: 2 own, 2 pre-draw hook (earlier job), 1 system (earlier job) | the same |
| `0x8008fb70`, `0x801a4ca8`, `0x82c02710`, `0x82c03758`, `0x82c03cb4`, `0x82c01d18`, `0x801a3f78` | | all own | all own |

- The inherited values are frame constants, so a patch can bind them itself: `f_82c02bc0`: s6.0 `0x117e8ce780`, s6.1
  `0x1178982000` / `0x1178988000` (alternate frames); pre-draw s7.1 `0x1178702000`; system: s6.0 `0x113cb2e000` /
  `0x113cb46040` (alternate), s6.1 `0x113b796000`, s6.2 `0x108c6d6540` / `..6580`, s6.3 and s7.3 `0x11371e7f00`.
- The system binder runs inside jobs 4-6 times a frame (`f_80101ec0` from `0x8027caa0`; keys `0x113b647338`, `..648038`,
  `..648448`, `..6486b8`; logged in a first `m3_font_k` run that was later overwritten). Its argument there is not the
  recorder's x1 but a block 0x620-0x820 bytes below it on the thread's stack, the one `f_8027c9e0` builds (its x5).
- `f_801a6720` reads either value set depending on whether an earlier job of the pass ran `f_82c02bc0` first (Kokiri: 40 %
  system, 60 % culled-instance values). Whether its program reads those slots at all is not settled here: the program's
  name table was not logged.

**Patch** (two parts, both in the game's lifted code):
1. Job prologue: a SPLICE in `f_80084de0` after `SetDepthRange` (`0x800855ac`) that calls `f_80101ec0` with an argument
   block built as `f_8027c9e0` builds it for the job's view, so slots s6.0-s6.3/s7.0-s7.3 start every job at the system
   values. This removes every "system (list)" and "unbound in the list" row.
2. In `f_801a43e0`, `f_801a46a0` and `f_801a6720`, before their `DrawMeshTasks*` (`0x801a45a4`, `0x801a48f0`,
   `0x801a6908`), the binds `f_82c02bc0` makes: `f_801a2d70(ctx, item, 4, o+0x1b0, 0)` and `f_801a6a40(ctx, item, 5,
   o+0xd70, 0)`, `o = [[param_1+0x18]+0x338]`, plus the s7.1 bind the pre-draw hook makes for culled items. The binders
   skip names the program does not list. For `f_801a6720` this changes the game's own order-dependent result in the 40 %
   of draws that today see the system values; one log of its programs' name tables (names 4/5 present or not) decides
   whether part 2 applies to it.

## 4. Waits, barriers and dispatches inside jobs

**CONFIRMED, every frame, all in one pass.** (`m3_gold_hf`, 1,912 frames; Kokiri the same.)

| op | site (LR) | via | event | pass key | a frame | signal recorded before the call |
|---|---|---|---|---|---|---|
| WaitEvent | `0x82c0301c` (`f_82c02f20`) | pre-draw hook at `0x8008fb6c` | one of `0x100d6c8000/8004/8008` (the slot is assigned lazily, so it rotates) | `0x113b647338` | 1 | 1,904 of 1,912 |
| Barrier | `0x82c030c4` | same | | `0x113b647338` | 1 | |
| WaitEvent | `0x82c0326c` (`f_82c03170`) | same | the same three | `0x113b647338` | 1 | 1,895 of 1,912 |
| Barrier | `0x82c03314` | same | | `0x113b647338` | 1 | |
| WaitEvent | `0x8027944c` (`f_80279400`, water) | item callback `0x800876f8` | the same three | `0x113b647338` | 1 | 1,912 of 1,912 |
| DispatchCompute | `0x8067c488` (`f_8067c310`) | item callback `0x800876f8` | | `0x113b647338` | 1 | |
| DispatchCompute | `0x8067c488` | `f_8008d640` (`0x8008da50`) | | `0x113b648038` | 1 | |

- Every one of these jobs was called at `0x80668c84` (`f_806688c0`) in the frame it was recorded; none at `0x8028a538`.
- "Signal recorded before the call" = a SignalEvent with value >= the wait's value was recorded on the host, in this
  present, before the layer list reached `0x80668c84`. The 8-17 misses a run are signals recorded later on another
  thread; recording order is not GPU order, and the wait is a GPU wait in the same queue position either way (the job's
  commands are inlined at that call), so moving it changes nothing for them. No signal was ever lower than the wait.

**Patch** (the hooks, then one splice in `f_806688c0`):
- In `f_82c02f20` / `f_82c03170` replace the `WaitEvent` + `Barrier` calls (`0x82c03018`, `0x82c030c0`; `0x82c03268`,
  `0x82c03310`) by a host call that stores (event, value) in a per-view-record slot (the job's x25); the CPU side (slot and
  value assignment under the mutex) stays. Same for `f_80279400`'s `blr` at `0x80279448`.
- A SPLICE in `f_806688c0` before the `CallCommands` at `0x80668c80`: for the record in x21, record the stored
  `WaitEvent`s and one `Barrier(0x28)` on the layer list's command buffer (`[[sp+0x10]+0xd8]`).
- `f_8067c110`'s tail call at `0x8067c1c8`: queue (object, flags) on the view record instead; after the `CallCommands`
  (`0x80668c84`) the splice calls `f_8067c310` for each queued entry with the layer list's command buffer. The wait position
  is reached before the jobs' draws, the dispatch after them, in the same frame as the records (1,910-1,912 of 1,911-1,912).

## 5. Deferred table ids

**CONFIRMED: only one table word gets a fresh id each frame; the other changes alternate between a few long-lived ids.**
Logged after each `f_80667d20(ctx, buffer_idx)` (m0diag's table address computation): its 30 words, against the same
(ctx, buffer_idx)'s previous write, with the registration (LR chain, frame) of the new id. `m3_gold_hf`, 1,904 frames:
96,490 of 745,290 words changed (`m3_inl_one_k`: 85,906 of 755,520), all in the main view context `0x113b6436f8`:

| word | changes in frames | new id is | registered by |
|---|---|---|---|
| 4 | 1,904 of 1,904, ~8 tables a frame | **fresh every frame** (registered in the same frame) | `f_80064920` (`0x80064b7c`) <- `f_800634b0` (`0x800638c0`) <- **`f_80667d20` itself (`0x806680dc`)**, caller `0x80662cd8` |
| 1, 10, 16, 26 | 1,737 | one of two ids registered at the load (age 619-660 frames) | `0x80065fa0` <- `0x80b65618` / `0x809e819c` (persistent textures) |
| 13, 18 | 1,213 / 1,255 | one of two DTA targets, registered 23-94 frames earlier (parked) | `0x80060c94` (DTA alloc) <- `0x8065cc54` / `0x8067aabc` |
| 17 | 242-461 | a DTA target registered 0-3 frames earlier (not parked: re-registered) | `0x80060c94` <- `0x80679464` |

- **Where the ids come from:** agl's descriptor manager (`*0x84ca2d60`), `f_80064920(mgr, texture, view, owner)`: a
  next-fit ring over `+0x9f0` slots of 0x28 bytes (cursor `+0xa64`, id = slot + `+0xa62`), which itself calls
  RegisterTexture. A freed id goes onto one of two deferred lists (`+0xa00`, picked by `+0xa60`) and returns to the ring
  only when `f_80557420` drains that list after the flip: the game's own protection for frames still in flight.
- **Word 4** is a system texture with a view (swizzle at entry `+0xe8..+0xeb`, `+0xf0` bit 1). `f_80667d20` finds its dirty
  bit (`+0xec` bit 0) set every frame and calls `f_800634b0`, which frees the old id and takes a new one even when the
  texture and the view are unchanged.
- Words 1/10/16/26 and 13/18 alternate between two textures (ping-pong): stable ids, the game's own double buffering;
  parking already keeps the DTA ones. Word 17 is a target parking misses (re-registered).

**Patch:** `ENTRY_HOOK(0x800634b0)` when the caller is `0x806680dc` (from `f_80667d20`): if the entry already has an id
(`+0x68 != -1`) and its texture object (storage address, format, size) and view bytes equal what that id was registered
with (kept by the hook), return without freeing or registering. Reusing an id is safe only for an identical descriptor:
an id whose descriptor changes while the previous frame's GPU work still samples it would show the new texture in that
work, so a changed descriptor keeps the game's path (new id, deferred free). With the patch only the ping-pong words
still change; they keep the submit snapshot (a few words) unless their parity is shown to match the table copy's. Word 17
needs its parking key fixed in `rt_slots.c` (caller `0x80679464`).

## 6. The font glyph cache

**CONFIRMED: the writer is `f_81539300`, the layout pitch-linear R8, one small glyph atlas per font face and size.**
Kokiri with the pause menu opened by script. While it shows text there is no texel upload call, and none of the 32
textures the GPU clears (`ClearTexture`: GPU-drawn ui2d layers) gets a CPU byte (`m3_font_k`). The upload log
(`RX_VK_LOG_UPLOADS`, `m3_font2_k`) named R8 textures taken from guest memory; a page watch on every non-compressible R8
texture from its initialization caught the stores (`m3_font3_k`).

- **Textures:** created by ui2d at `0x807989e8` (`f_80798920`), NVN format R8 (`0x1`), flags `0x44` (IMAGE |
  LINEAR_RENDER_TARGET, so pitch-linear, stride aligned to 128), 128 or 256 wide, 34-53 rows (`128x34`, `128x53`,
  `256x39`, ...), a few KB each; 15 made when the menu opened (frames 2755-2756). The CPU clears its own cells.
- **Content:** a glyph cache, not a text line: a dump of a 256x39 one shows each glyph of "Collection" once
  (`C o l e c t i n`), packed left to right; the 128x53 one a button glyph. Viewed as plain rows of `stride` bytes the
  images are exact, so the layout is pitch-linear (no GOB swizzle).
- **Writer:** `f_81539300(cache, glyph, ...)` (nn::font's plot, reached from `f_8156ca60` -> `f_8156fdc0` ->
  `f_815392a0` -> `f_81535eb0`). It asks the shelf packer `f_8152f540(cache, &x, &y, w, h)` for a cell (cursor at
  `cache+0x148/+0x14c`, row height `+0x150`), clears the cell with one `memset` per row (LRs `0x81539428..0x8153953c`,
  the first stores seen on every texture), then max-blends the FreeType bitmap (`glyph+0x18`) into
  `[cache+0xf0] + (y+row)*[cache+0x140] + x`. `[cache+0xf0]` is the texture's storage, `[cache+0x140]` its stride.

**Patch:** `ENTRY_HOOK(0x8152f540)` notes the cell it returns (x, y, w, h) for the thread; an entry hook on `f_81539300`
runs the body and then queues a region upload (WriteTexels-like) of rows y..y+h+3, columns x..x+w+4 (the memset extent)
of the texture whose storage starts at `[cache+0xf0]`, source pitch `[cache+0x140]`. The texture is found by its storage
address. All 30 (writer, texture) faults were these memsets, so this covers the cache; no page tracking needed.

## 7. The lists begun at `0x8007ba00`

**CONFIRMED: they hold nothing.** 5 lists a frame in both scenes (`m3_gold_k` 2,399 frames, `m3_gold_hf`), 0 records
between BeginRecording and EndRecording (counted at the host's record function). Built by `f_8007b520(sched, layer, n)`
called from the layer job runner `f_8007fe60` (LR `0x800812c4`), for 4 layers of vtable `0x84ba9c48` (`0x1134e89930`,
`0x104dd1d230`, `0x104dd1bf78`, `0x104dd1e4e8`, flags +0x88 = 0x84c3) and one of vtable `0x84c2b9f0` (`0x1136f127c0`; the
vtable sits 0x20 after the scene pipeline's `0x84c2b9d0`). Why: `f_8007b520` begins a list for every layer job before
it knows whether the layer has items; its per-item step `f_8006e160` returns at once when the item's draw list is empty (`[layer+0xb8] + i*0x820 + 0x818` bit 0
clear), so these layers record nothing, and the list is never called or submitted.

**Patch: none needed.** Skipping `f_8007b520` would also skip the layer's per-frame bookkeeping (ring slot, counters,
`f_8006e160`'s non-recording work), and there is nothing to gain: the lists cost a Begin/End and are never used. The back
end should simply not create a list object until the first record arrives (plain behaviour, no special case).

## 8. The long layer of calc stage C, started with the render jobs

**Why.** Stage C's task graph opens the layer-list phase only after every render-job loop has ended, and one layer (the
3D layer `0x1136f127c0` in the market: the scene pipeline and post effects, 3.6 ms) then records alone for about 2 ms of
the stage (boot-to-main-loop.md 4.3). The game's job-call sites do not wait for their jobs: the graph's order did.

**Sites** (`host/direct_layer_ahead.c`; on unless `HOS_LAYER_AHEAD=0`):
- `SPLICE_AFTER(0x8007fe60, ...)` after the scheduler's render-job-loop test (bit 10 of the node's kind): every thread
  entering a job loop passes it. The first of a frame (a lock; the others wait for it) puts the job list (singleton
  `[0x84ea2780]`, lists at +0x38; entries 0x28 bytes at +0x3c18, count +0x3c28, claim counter +0x3c84, key pointer at
  entry +0x10) in the order the layer run ahead called its pass keys the frame before, only while the claim counter is 0.
  Then the first thread that may take the layer phase's queue 1 (work object `W = [[sp + 0x48] + 0xd0]`: claim counter
  `W + 0x4dc`, count `+0x4e8`, items `+0x4f0`; the thread whose worker index, TLS slot `[0x84e9f7ac]` - 1, is `W + 0x4d8`
  skips queue 1) claims queue 1's item 0 (kind `item + 0x10` = 0, a layer) with the same counter, 0 to 1, and runs it
  as the scheduler does, `[[item] + 0x10](item)`, before its job loop.
- `ENTRY_HOOK(0x806688c0)` (key bytes at `x0 + 0x3830 + x2 * 0xd0`) and `ENTRY_HOOK(0x8028a2a0)` (key bytes at `x2`),
  on that thread only: wait until every list entry with the key's first 0x70 bytes went through the recorder this frame.
  The recorder's hook (`host/eng_jobs.c`) stamps each entry, recorded or taken by the early return; a condition variable
  (the virtual core given up) is woken by each, and the wait gives up after 50 ms.
- `ENTRY_HOOK(0x80706be0)` counts frames for the stamps.

**What it leaves to the game:** the rest of both queues, the phase's per-list work (bit 11) and every node of the graph.
Recording order changes nothing in the picture: a pass calls its jobs sorted by their item ranges (+0x68).

**Check that it applies:** the 600-present line "direct patch layer ahead" (`HOS_DBG=logpatch`): run ahead 1.00 a frame,
job lists ordered 1.00, claimed before the order 0, waits given up 0, jobs recorded from another caller 0 (the
scheduler's twin `f_80083960` claims none in the market, Hyrule Field and Kokiri); and "direct patch passes": inline passes of
recorder keys 0 at both sites (without the wait the depth-probe passes are drawn inline, 4 a frame in the market).

## Causes found for picture bugs

Each was found by comparing passes against the old renderer and is fixed by the code named.

- **Memory read only through stored pointers.** Shaders follow pointers the game stores in memory (the probe tables of a
  150 MB pool in Kokiri), so a pool the GPU can read is bound in full when it is created; memory reachable only through
  a pointer left unbound read as zeros, which gave the exposure x3.96, a green cast and a light shaft
  (`gpu/direct/direct_pools.cpp`; texture data pools are named at creation with `hsd_texture_data_pool`). VRAM-class pools
  (CPU_NO_ACCESS) can hold pointed-to data too.
- **The sky / ambient LUT.** `f_80846070` copies a block-linear blob (0x128000 bytes, three RGBA16F surfaces: 256x64 2D,
  256x32x16 3D, 64x64 2D) out of `Env/GameScene.Nin_Ounce_NVN2.genvb.zs` (`res/Master_Field.skybin`). The converter
  `nx2/prepare_skybin.py` deswizzles it into the prepared overlay and `host/direct_sky_lut.c` uploads each part with
  `hsd_texels` instead of the memcpy.
- **Two GPU culling lists read w1 = 0.** `f_80361a00` (called twice from `f_80361c80`, for the lists at `[x27+0x180]` and
  `[x27+0x1b8]`) zeroes w0/w1 with its own store every frame; the culling shader only does `atomicMax(+0)` / `atomicAdd(+12)`.
  The splice after that store is `host/direct_mesh_lists.c`.
- **CPU writers of texture storage.** Seven writers, the same in the title, Kokiri and Hyrule Field, 0 file reads into
  textures: the sky LUT blob (above), a 64x64 R16F LUT computed on the CPU in a buffer a texture also names (`f_809e6760`,
  copied out by `f_809e70a0`: a staging use, inferred, not a texture's content), `memset` 0 of 1x1 textures (nothing to do:
  the image starts as zero) and the glyph atlas (`f_81539300`, patched).
- **Link's hair, cap and face: the game's texture dedup.** A load with no data that is an alias of another load takes that
  texture's record (`HSD_ASSET_SAME`, `host/direct_tex_dedup.c`; `HOS_DBG=nodata` logs the loads with no data). Kokiri: 149
  such loads, 149 take a record.
- **Prepared textures need their channel map.** The prepared store keeps a texture's channels in the store's order, so every
  sampled view of a prepared image takes `image_swizzle(im, game)` (`direct_heap.cpp sampled_view`); without it the static
  meshes came out under-lit. The environment cube lights the pots, basket and bark walls.
- **Rendering into a compressed texture.** The game BC4-encodes its noise volumes (the 64^3 volume `531432c3` samples) on the
  GPU: program `53509044401d78d2` renders into the volume 64 times, one draw a slice, through a view of an uncompressed format of
  the block's size (`f_809eb720`, for the noise generators of `f_809eb4a0`). A compressed image that is not the prepared
  store's is therefore made `BLOCK_TEXEL_VIEW_COMPATIBLE | MUTABLE_FORMAT | EXTENDED_USAGE` with colour-attachment use, and
  such a view is an attachment of the block grid. Open: the remaining offset of the volume's B channel (mean 0.945 against
  0.853 in the old renderer's picture).
- **The frame is forward-rendered:** a depth prepass, then colour draws into the lit B10G11R11F 1536x864 target. The RGBA8
  1536x864 target is a screen-space mask with its own passes, not a normals target.
- **Render conditions need no predicate compute step or gating patch:** `HOS_DBG=condcheck` found the second counter word
  non-zero in none of ~270 / 840 conditions a frame and the first word past 32 bits in none, so conditional rendering on the
  report's own low word is exact. Reports are written by a compute step, not a copy (NVN's word is `{counter, timestamp}`
  and the timestamp needs the clock conversion).
- **The game's CPU occlusion culling** takes the GPU's 32x18 depth grid in `f_80365f00` if its stamp is one or two frames
  old, and `f_80822260` culls against it only when it was taken; the grid accepted 4,379 of 4,380 tested.
