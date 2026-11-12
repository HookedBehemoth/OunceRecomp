# M4d report: CPU-written textures, the magenta, the GPU lists, colour, cold start (2026-11-12)

Branch `agent/m4d` (from native-vk 7ee871d). Runs `work/runs/m4d_*` (local), RX 6950 XT, RADV, `RUN=timing`, one game
process at a time. **No device was lost.** The texture store and terrain tiles were rebuilt into `work/prepared_m4d`
(`HOS_PREPARED=` that folder for every picture run below). Tests: 289 pass / 7 fail: the baseline's 5, plus
`test_prepare_textures` and `test_prepare_terrain`, whose reference swizzle is the wrong GOB (below; `tests/` is not mine
to edit: their `in_gob` line needs the Switch 2 formula).

## 0. Shots of the presented frame (asked mid-task)

`hsd_shots(dir, every)` (new export; the host passes `RX_VK_SHOTS` / `RX_VK_SHOT_EVERY` from `rx_start`, no getenv in
gpu/direct): every Nth present, the swapchain or offscreen image after the cropped blit is scaled into a 1280x720 RGBA8
image, copied to that frame slot's buffer, and written as `shots/vk_<present>.png` when the slot's fence has passed (a
thread of its own). Made once at the first shot; nothing recorded when off. Checked: Kokiri and title headless
(`m4d_dd_k`, `m4d_dd_title`) and in a window (`m4d_win_k`, 1280x720 mailbox swapchain): full frame, 16:9, no bars.

## 1. CPU writes into texture storage

`HOS_DBG=texwatch` (`host/direct_texwatch.c`): every texture's storage pages read-only from its initialization; a write
faults, is noted by (texture, writer), the page is released and re-armed every 2 s; file reads into storage noted from
`nn_fs.c`. Also `memwatch:<addr>` and `memfind:<value>@<present>` for the GPU lists (task 3). Kokiri, HF, title
(`m4d_tw_k`, `m4d_tw_hf`, `m4d_tw_title`): **the same 7 writers in all three**, 0 file reads into textures:

| Texture (guest, size, NVN format, layout) | Writer (guest lr) | What | Done |
|---|---|---|---|
| 256x64 RGBA16F, 256x32x16 RGBA16F array (`0x113c19b000`), 64x64 RGBA16F; two sets (`0x113c17a000..`, `0x113c339000..`) | `f_80846070` (called at `0x80845ec8` / `0x80845ef8`) | `memcpy` of one blob (`[res+0xf0]`, `[res+0xf8]` bytes = the three sizes summed) into the three storages, **block-linear** (flags 0) | **stopped, as told**: the back end has no block-linear code |
| 64x64 R16F (`0x113cb27000`, `0x115ccbf000`) | `f_809e6760` (stores at `0x809e6a70`) | a LUT computed on the CPU into a buffer whose memory a texture also names; then `f_809e70a0` copies it out and hands it on (`f_809c9d60`) | not patched: INFERRED a staging use, not the texture's content |
| 1x1 RGBA32F x2, 1x1 RGBA8 (pitch) | `0x80c558c0`, `0x8093ea40` | `memset` 0 of a buffer over them | nothing to do (the image starts as zero) |
| glyph atlas R8 (pitch) | `f_81539300` | glyphs | already patched (A-rt) |

The sky LUT's fix needs the blob's bytes linear before `f_80846070` copies them: find its file (the resource at
`[x8+0xf0]` in `f_80845e..`), deswizzle it at build time (rule 4, as the terrain tiles), and replace the memcpy by
`hsd_texels`. Not done.

## 2. The magenta was the converter, not the placeholder

The placeholder is black (zeros). The magenta texels are ASTC error blocks of the store: **`nx2/texconv.cpp` deswizzled with
the Switch 1 GOB** (`(y%8/2)*64 + (x%32/16)*32`), the Switch 2's is `(y%8/4)*128 + (x%32/16)*64 + (y%4/2)*32` (the one the
frozen back end uses). It permuted the 16-byte pieces of every GOB: every converted texture and terrain tile was
block-scrambled (the A-rt dumps show it), and a partial last GOB read zeros, which the ASTC decoder made magenta.
CONFIRMED on `Cloth_Kokiri_Stitch_Tile_A_Nrm` (the last GOB holds data at pieces 0-3, 8-11 exactly as the Switch 2 rule
says). Fixed in texconv (commit 2); store rebuilt (1,983 s, 22,576 textures, 0 errors; terrain 51,233 files).

Textures sampled as the placeholder (`HOS_DBG=phlog`: first sampling draw per heap id): **Kokiri 10, HF 7, title 0**, all
ASTC loads with no data (formats 0x7b/0x7d/0x8c/0x94, 1x1 to 512x512, `written never`): 7 by layer lists `lr 0x8007b0c8` at
the load only (present 442-498), 3 by jobs (`lr 0x8008533c`, programs `2857a241`, `eaa82471`, `fde16ed1`) sampling
`0x10c0def400` (1x1 ASTC) at presents 456-472, then never again. No texwatch write, no GPU write reaches them: on the
console they hold no data either at that time. Left as they are (the zero image), not converted.

## 3. Mesh-only draws (`host/direct_mesh_lists.c`)

The premise "words 1-2 never written" is half right: a page watch on `0x117a4d6000` (`m4d_mw_k`) shows **`f_80361c80`
zeroes the first record of each GPU list every frame on the CPU** (`str xzr, [x19, layout+4]`: w0 and w1; `str wzr` the
appended count). Patch: `SPLICE_AFTER_EACH(0x80361c80)` puts w1 = w2 = 1 after each such store (12 a frame); the
GPU-reset records (w1/w2 never written) get w1 = w2 = 1 once where an indirect mesh draw names them (`draw_mesh`,
direct mode). Counters (`m4d_ml_k`): 12 resets a frame, 41 records drawn a frame; **records with work after the GPU: 24 of
41 (m4a: 6 of 34)**. The rest: `0x117982a000` (10 groups) and `0x1178b7e000` keep w1 = 0, **written on the GPU** (M4b:
writer `3de6d86e`): needs a replaced shader; 12 records stay at 0 groups (`0x117cf04000`, `0x117d03c000`, `0x117d8d4000`,
`0x117e3bc0x0`, `..d9ec010`, `..db24010`, `..a4d6040`: the culling found nothing for them); 5 are task+mesh (M4c).
Picture: the ivy on the house's arch and around the hollow now draws; the ground's grass does not.

## 4. Per pass against the frozen back end (Kokiri, last states, `m4d_dd_k` vs `m2b_fd_kokiri_b`)

Frozen-vs-frozen noise ≤ 3.2 except two rows (RGBA16F 512x512 #1 8.1, RGBA8 1536x864 37). **First pass in frame order
beyond noise: the sky/ambient LUT, RGBA16F 256x256 at `0x113c2a5000` and `0x113c464000` (37.6, black)**, a layer-list pass
whose input is the CPU-written block-linear LUT array of task 1. After it: env cube mips 64x64..1x1 3.0-7.3 (frozen ~0),
RGBA8 384x216 `0x10012df0000` (prog `531432c3`, 13.2: blue 0.70 against 0.85, flat; unchanged since M4a, not yet explained),
the lit colour 2.2, the composite 7.1 (M4a 11.1). Depth, shadow and the depth chain 0.00-0.65. HF: the same LUT first, then
the terrain's depth chain (task+mesh, expected).

## 5. Start times

Cold (no direct caches): **171.9 s** from present 1 to the first frame in play (`m4d_cold2_k`): compute 11 variants
translated in 155.6 s, almost all five networks' shader objects (3.0, 4.0, 28.0, 42.7, 77.6 s), made one after another
because each dispatch waits for its own. Fix (direct_compute.cpp): a compute program with no texture or image handle has
one variant: it is translated when set at the load, on four workers, with a dispatch's request ahead of the queue:
**105.7 s** (`m4d_cold3_k`; 527 programs translated ahead, the longest network 85.9 s now bounds it). Warm: **9.9 s**
(`m4d_dd_k`, 15.2 s after the device; 3,220 store uploads 5.8 s on the loaders). Next: the graphics shader objects are not
cached as binaries (748 made, 13.9 s of worker time each warm start); adding it changes the translator stamp (one more cold
start).

## Validation

`m4d_val_k` (Kokiri, `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`): one message type,
`Undefined-Value-ShaderOutputNotConsumed-DynamicRendering` (200, the limit; known since M2b). No hazard.

## Open problems

The sky LUT (block-linear CPU blob: build-time deswizzle + upload patch); the GPU writer of w1 in two lists; grass; the
RGBA8 384x216 blue level; the converter tests' reference GOB.

## Pictures (direct, frozen), under `work/runs/`

- Kokiri: `m4d_dd_k/shots/vk_001800.png`, `m2b_fd_kokiri_b/shots/vk_001800.png`
- Hyrule Field: `m4d_dd_hf/shots/vk_001500.png`, `m2b_fd_hf_b/shots/vk_001500.png`
- title: `m4d_dd_title/shots/vk_001800.png`, `m2b_fd_title_b/shots/vk_001800.png`

Kokiri: textures are clean now (curtain, pots, Link, hearts; no magenta), the ivy draws; still green-blue and darker than
the frozen warm light (the LUT), the grass is missing. HF: the shot is at the tunnel's mouth (the frozen one already on the
bridge: another position at that present); blue fog, the field's ground missing (task+mesh). Title: logo, menu and movie
correct and sharp (M4a's logo was block-scrambled); another movie frame than the frozen one.
