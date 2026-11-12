# M4e report: the sky LUT, two mesh lists, start-up binaries, per-pass status (2026-11-12)

Branch `agent/m4e`, rebased onto native-vk with M5 (9b2b6b9). Runs `work/runs/m4e_*` (local), RX 6950 XT, RADV, `RUN=timing`,
one game process at a time, store `work/prepared`. **No device was lost.** Tests: 292 pass / 5 fail (the baseline 5; +1 new test).

## 1. The sky / ambient LUT (`nx2/prepare_skybin.py`, `host/direct_sky_lut.c`)

**Where the blob comes from** (diagnosis hook on `f_80846070`, `HOS_FS_TRACE`, run `m4e_cold_k`): its source pointer sits
0x128480 into the decompressed `Env/GameScene.Nin_Ounce_NVN2.genvb.zs`, a zstd SARC (game dictionary) holding
`gamescene.bgenv` and `res/Master_Field.skybin` (twice, as `master_field.skybin` too): 0x128000 bytes, no header, the
console's block-linear storage of three RGBA16F surfaces: 256x64 (2D), **256x32x16 3D** (NVN target 2 is 3D, not an array
as m4d said; 1 GOB high, 16 deep, `host/nvn_layout.c`'s rule, confirmed: the smoothest of all 15 (tileH, tileD) choices),
64x64 (2D). A file, so rule 4: the converter writes the archive with both members deswizzled (same sizes: all surfaces
are whole blocks), recompressed with the same dictionary, into the prepared overlay root (`work/prepared/terrain`, served
by nn_fs in direct mode); `--verify` re-swizzles byte-exact. Test `tests/test_prepare_skybin.py` (synthetic: layout, 2D and
3D against an independent GOB reading, a SARC with a trained dictionary, refusals). Run side: a splice before each of the
three memcpys uploads the part (`hsd_texels`, new source `HSD_TEXELS_FILE`) and copies nothing; converted is known from the
overlay having served the file, else the placeholder, counted. Counter: 12 uploads (4.6 MB) at boot and scene load, 0
placeholders, 0 odd sites.

**Verified** (`m4e_fin_k` vs `m2b_fd_kokiri_b`): the 256x256 LUT pass is no longer black and has the frozen one's picture
exactly up to one scale: means, minima and maxima all **x3.96**. Cause found: the pass's uniform `0x113b621700` words 11-14
(a CPU-written light intensity: frozen 3.571 1.518 2.857 3.571, direct 14.142 6.010 11.314 14.142) with the sun direction
words identical; the exposure block's colour floats (0x11371e7f30..) carry the same factor. Unchanged with `HOS_DBG=qfixed`
(M5), so not the occlusion reports. Its CPU source is **not found** (open). The sky drawn from the LUT (B10G11R11F
1536x864, prog `489438d7`) shows the same x3.96; the lit colour and the composite are within 2-8 of the frozen ones.

## 2. The two lists that read w1 = 0 (`host/direct_mesh_lists.c`)

Not a GPU write: a page watch (`m4e_mw_k`) caught **`f_80361a00`** (called twice from `f_80361c80` at 0x80361e08/e10 for the
lists at `[x27+0x180]`, `[x27+0x1b8]` = 0x117982a000, 0x1178b7e000) zeroing w0/w1 with its own `str xzr` every frame; the
culling shader `3de6d86e` only does atomicMax(+0) / atomicAdd(+12) (IR via tmtool). Fix: the same splice after that store.
**No shader needed replacing, so no replaced-shader mechanism was built** (none exists; it waits for a case that needs it).
Counters (`m4e_ml_k`): resets 12 -> 14 a frame; **28 of 41 records draw** (m4d 24); 0x117982a000 = {10, 1, 1, 160};
0x1178b7e000 has 0 groups (culling found nothing). The grass is still missing: the ground's grass is drawn from
0x117896a000 / 0x1178982000, which feed task+mesh programs (M4c).

## 3. The flat blue in RGBA8 384x216 (`0x10012df0000`, prog `531432c3`)

Narrowed, not fixed. The draw (`HOS_DBG=drawlog`, `m4e_dl_k`) writes B and A only and samples a **64x64x64 BC4 3D volume
`0x1140bcb000`** ("written never"), a 1360x1000x8 store texture and the depth. The direct pass is perfectly flat; the
frozen one shows the volume's faint noise. That volume (and its pair `0x1140c2d000`) is made at boot by `f_809eb5e4`
(lr 0x809eb834) beside two 64^3 R8 volumes; no packaged data, no file, no CPU write (texwatch + memwatch, `m4e_tw3_k`,
`m4e_3d_k`), and its guest memory is all zero in direct (`HOS_DBG=peek`, `m4e_dd_k`). So its texels reach the frozen image
some other way, most likely a GPU copy or encode from the R8 volumes at boot. Next: log the boot frame's copies and
dispatches that name those four textures.

## 4. Passes that still differ (Kokiri `m4e_fin_k`, title `m4e_fin_title`, both `qfixed`, against `m2b_fd_*_b`)

Kokiri: 126 aligned, median 0.09; beyond the frozen noise (<= 3.2; 8.1 RGBA16F 512x512 #1, 37 RGBA8 1536x864):

| Pass | Distance | Cause |
|---|---|---|
| RGBA16F 256x256 LUT x2 | 36.9 | the x3.96 light intensity uniform (1) |
| B10G11R11F 1536x864, sky from the LUT | 65.4 | the same factor |
| RGBA8 384x216 `0x10012df0000` | 13.2 | the zero BC4 noise volume (3) |
| RGBA8 1536x864 `0x100126a0000` | 12.8 | grass (task+mesh, M4c); the frozen noise of this row is 37 |
| B10G11R11F 1536x864 states 094-100 | 6-18 | not the same state: the frozen pictures show another pass on that target (Link and fog only); an alignment artefact |
| D16 1600x1600 cascades, D32F 1536x864 #034 | 6.9-16.5 | cascades cover other regions at that present (m4b); the depth one is a cleared state against a drawn one |
| composite RGBA8 2560x1440 | 7.8 | all of the above |

Title: 3 targets; the movie is at another frame (both runs show a correct movie frame, logo and menu); the first
aligned row is a cleared target against a drawn one (one extra direct pass). No other difference.

## 5. Start-up: graphics shader objects from binaries (`direct_shader.cpp`)

`hookshot_direct_shader_objects.bin`: each object made from SPIR-V saves `vkGetShaderBinaryDataEXT`'s bytes under a hash
of its whole create info (stage, next stage, flags, subgroup size, SPIR-V, heap mappings); the header holds
`shaderBinaryUUID` / `shaderBinaryVersion` (another driver: made again); a refused binary compiles from SPIR-V, counted;
locked like the program cache. 750 objects = 575 distinct binaries, 4.7 MB.

| Run | first frame in play after present 1 | after the device | graphics objects (worker time) |
|---|---|---|---|
| cold before (`m4e_cold_k`, FS trace on) | 82.4 s | 87.9 s | 750 from SPIR-V, 14.2 s |
| cold after (`m4e_cold2_k`) | 83.9 s | 89.0 s | 167 from binaries already (duplicates), 12.8 s |
| warm before (`m4e_sky_k`) | 10.25 s | 15.65 s | 750 from SPIR-V, 13.9 s |
| warm after (`m4e_warm_k`) | 10.14 s | 15.18 s | **753 from binaries, 0.1 s** |

The 13.8 s of worker time is gone, but the wall clock moved by 0.1-0.5 s: those objects were made in parallel and off the
critical path. Cold starts stay bound by the compute networks (599 variants, 145.7 s of object time; the stamp changed
with this file, so this cold start rebuilt them all).

**What stands between today and "every program built before play"** (plan, Programs):
- Programs are translated when a draw or dispatch first names them (or, for compute without handles, when set); nothing
  walks the romfs's programs offline. Needed: an offline pass over every program of the romfs (the seed) to the caches.
- Graphics variants depend on run-time answers (texture types in banks, vertex/colour types): the set of variants a
  program needs is only known from a run; an offline pass needs a list of variants per program (from runs) or answers
  that do not split variants.
- The caches are invalidated by the translator stamp (any edit of `direct_shader.cpp` or the fork: one long cold start);
  binaries by the driver; a shipped cache would need the SPIR-V cache kept separate from the stamp of unrelated edits.
- The compute networks' objects (26-86 s each from SPIR-V) dominate a cold start: they need their binaries before the
  first run (an offline pass on this machine) or a cheaper program (matrix multiplies in software).
- Task+mesh programs are not translated at all (M4c).
- The load waits for a missing program (rule 2); there is no count yet of programs first seen in play, which "built
  before play" requires to be 0 on the whole scene cycle.

## Open problems

The light-intensity factor's CPU source (1); the BC4 noise volume's texels (3); grass and the ground's foliage (M4c); the
replaced-shader mechanism unbuilt (no case needs it yet).

## Shot pairs (direct, frozen), under `work/runs/`

- Kokiri: `m4e_fin_k/shots/vk_001800.png`, `m2b_fd_kokiri_b/shots/vk_001800.png`. Right: the warm lantern light, the
  curtain, Navi, a light shaft through the canopy, overall brightness close. Still wrong: greener cast; Link's hair dark;
  the yellow ivy leaves on the house and the ground's grass missing; a pot at the lower left missing.
- Title: `m4e_fin_title/shots/vk_001800.png`, `m2b_fd_title_b/shots/vk_001800.png`: logo, menu and movie correct and sharp,
  another movie frame.
