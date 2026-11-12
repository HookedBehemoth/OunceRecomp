# M6b report: checks that can fail (2026-11-12)

Branch `agent/m6b` (from native-vk 5beed38). Runs `work/runs/m6b_*`, `golden_*`, RX 6950 XT, RADV, `RUN=timing`, one game
process at a time; Kokiri `chisel_kokiri`, Hyrule Field (HF) `chisel_hf`, title (no overlay), market `chisel_hf_market`.
**No device was lost** (the kernel log of this boot has no amdgpu reset or ring timeout line). Tests: 295 pass / 5 fail (the
baseline five; the three new test files pass).

The independent review was right: the earlier verdicts could not fail. With the checks below, the direct back end
**fails** the picture check in Kokiri (15 pass states) and HF (24), validation is clean in all three scenes with the layer
proven loaded, and strict mode passes 150 s of Kokiri and HF and the title with two known entries.

## Summary: verified and suspected

Verified (a check that can fail, run, and its evidence named):
- Validation (core, synchronization, thread safety, object lifetime): **0 errors, 0 warnings** in Kokiri, HF and the title
  (movie included), 150 s each, after one fix. The layer's library is mapped in the process and its status message
  (`CURRENT-VALIDATION-ENABLED`, listing "Synchronization") was heard by our messenger in every run. The same messenger
  reported `Undefined-Value-ShaderOutputNotConsumed-DynamicRendering` before the fix, so it does hear warnings.
- Strict (rule 2): Kokiri and HF 150 s, title 65 s: no stop. After arming, only the two known entries (renderable textures
  the game makes in play, and their attachment views).
- Two real faults found by strict and fixed: draws over **no render target** were skipped (0.83 a frame; one of them is a
  vertex stage that writes a storage buffer from the 1360x1000x8 KokiriForest store texture), and **command buffers were
  made in steady play** (80 secondaries in 150 s of Kokiri).
- The picture check, against three frozen runs per scene with the game stepped 1/60 s a frame: Kokiri 15 failing pass states,
  HF 24 (section 3). It finds M4f's RGBA8 384x216 blue-channel fault again (71 % of its tiles over threshold, while its picture
  mean differs by 0.03) and differences in the composite, shadow cascades, the screen mask and the bloom input.
- Determinism: the game's own clock is not repeatable at a fixed present by default (it steps by wall time); with
  `HOS_GAME_STEP_PER_FRAME=1` it is exactly present + 2 in every run. What still moves between runs: Link's idle pose, the
  fairy and her light, the grass, and which env-cube faces are drawn that frame.

Suspected (evidence, not proof):
- The draw-count gap (direct 1098 a frame against frozen 685 in Kokiri) is not the frozen statistics missing draws, not
  render conditions and not occlusion results (section 3d: each was tested and accounts for at most 20 draws). Where the
  other ~390 come from is not settled; the per-program table points at the main view's depth-prepass and lit job passes
  (about twice as many draws of the same programs in direct).

## 1. Validation that can fail

**What exists** (`gpu/direct/direct_check.{h,cpp}`, called from `direct_main.cpp` and the 600-present block):
- Validation is switched on for a run the existing way: `VK_LOADER_LAYERS_ENABLE=*validation` (with
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`, and `VK_KHRONOS_VALIDATION_ENABLE_MESSAGE_LIMIT=false` so the layer does not stop
  at 10 messages of an id). `HOS_DBG=validation` enables the layer and its synchronization validation from the back end
  itself (the only new switch; no `getenv` in `gpu/direct` but `HOS_DBG`).
- `VK_EXT_debug_utils` is enabled when the loader has it; a messenger is chained into `vkCreateInstance` (to hear the
  instance's own messages) and one is made for the instance's life when the layer is in the process.
- **Proof the layer is loaded**, logged at start: the library is found mapped in the process (`dl_iterate_phdr`), the
  messenger's `VkResult`, and whether the layer's status message was heard:
  `validation: the layer is loaded: /usr/lib/libVkLayer_khronos_validation.so mapped in the process (enabled outside the back
  end (VK_LOADER_LAYERS_ENABLE); installed: yes); messenger made (VkResult 0), status message heard`, and before it
  `validation: the layer's status message heard (info, CURRENT-VALIDATION-ENABLED): ... Core Checks - Synchronization - ...`.
  Without the layer: `validation: off (no validation layer in the process ...)`.
- Messages are counted by severity and by id; each distinct (severity, id) is logged once with its first text; a summary
  line (`validation: layer ..., messages: N errors, N warnings, N info; distinct ids ...`) is in every 600-present block and
  at a clean exit (the run script ends a run with SIGTERM, so the last block is the one to read).
- `HOS_DBG=strict`: any warning or error ends the run, exit code 87, unless its id is on the known list (none is).

**Run:** `OVERLAY=work/overlay/chisel_kokiri RUN=timing scripts/run-vk.sh <name> 150 -- RX_BACKEND=direct
VK_LOADER_LAYERS_ENABLE='*validation' VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true VK_KHRONOS_VALIDATION_ENABLE_MESSAGE_LIMIT=false`.

**Every message id seen:**

| Id | Severity | Runs | Cause | Status |
|---|---|---|---|---|
| `Undefined-Value-ShaderOutputNotConsumed-DynamicRendering` | warning | `m6b_val_k` (10, the layer's cap then) | a fragment stage writes colour 0 in a pass with no colour attachment 0 (depth-only passes): Eden emits every output the Maxwell program stores | **fixed** (below); 0 in `m6b_val2_k`, `m6b_val_hf`, `m6b_val_t` |
| `Loader Message` "Layer VK_LAYER_KHRONOS_validation forced enabled due to env var" | warning | every validation run (2) | the loader reporting how the layer got there | classified as proof, logged as `validation: the loader: ...`, not counted (the only text-matched case) |
| `Loader Message` (instance and device creation, 143) | info | every run | the loader's own account | counted as info; info never fails |
| `CURRENT-VALIDATION-ENABLED` | info | every run | the layer's status | the proof line above |

**The ShaderOutputNotConsumed fix** (translator fork's general output handling, not a silence): the draw's colour types
(`direct_draw.cpp`) now mark a location with no attachment as `AttributeType::Disabled`; the fork's fragment stage
(`spirv_emit_context.cpp`) defines such an output as a *private* variable, and `EmitSetFragColor` stores into it
(`emit_spirv_context_get_set.cpp`). The program's stores stay (the alpha test still reads colour 0), nothing reaches an
interface no attachment reads, and the driver removes the dead stores. The colour types are part of the variant key, so a
program drawn both into colour and depth-only passes now has two variants (translated once; the stamp change invalidated
the direct program cache once).

**Results:** Kokiri (`m6b_val2_k`), HF (`m6b_val_hf`), title with its attract movie (`m6b_val_t`, 4800 presents): **0 errors,
0 warnings**, 150 s each.

**What would make this check miss a bug:** validation checks API use, not results: a wrong shader, wrong data or wrong
order the API allows passes. GPU-assisted validation was not enabled (descriptor-heap reads, buffer-device-address reads
out of range and the shaders' own memory accesses are not checked). The layer's coverage of the newest extensions this
back end leans on (`VK_EXT_descriptor_heap`, shader objects with heaps) is the layer's to give; an extension it does not
know is passed through unchecked. Synchronization validation does not see the host's waits on guest memory or GPU work
ordered through external memory the CPU writes. A message from a thread after its log line was cut by SIGTERM is lost.

## 2. Strict mode for rule 2

**What exists** (`direct_check.{h,cpp}`; the hooks at each place that counts a skip):
- `HOS_DBG=strict[:<present>]`. **Armed** at the first present that is at least `<present>` (default 1200) and at least
  600 presents (`kStrictQuiet`, 10 s at 60 fps) after the last program miss (a draw or dispatch skipped because its program
  was not read or its variant was being translated). Once armed, the first event of a reason not on the known list is
  logged with what and where and ends the run with **exit code 86** (`strict: STOP at present P (armed at A): <reason>:
  <where>`). A program miss after arming is itself an event. Before arming, events are counted and the first two of each
  reason logged.
- Events: every draw not drawn, by its reason, with the program and the list and pass (`count()` in `direct_draw.cpp`: a
  draw cannot be skipped without passing there, which `tests/test_direct_strict.py` checks); every dispatch not recorded
  (but an empty grid); clears and copies skipped (masked, no target, not rendered, texture clears, buffer copies and clears
  not in layer 0); ops of lists that record nothing and ops a job's rendering cannot hold; jobs not recorded, not executed,
  called with no secondary or no pass, executed in a pass of another size, and passes drawn inline; reports not written,
  summed in part, or not NVN's value, and conditions not evaluated; the **placeholder texture sampled** (a handle in a driver
  bank naming a heap entry with the placeholder or no image); a storage-buffer slot read that nothing bound (**the null
  window**); a uniform buffer read at a **constant offset past its bound range**; task draws refused by a full pool or held
  to the dispatch limit; and **every Vulkan object made after arming** (one counter, `object_made`, named by what it is and,
  for images, what for).
- The **known list** is one table, `kStrictKnown` in `direct_check.cpp`, logged at the start of a strict run
  (`strict: known: <reason>: <why>`). Its events are logged (first three) and counted, and do not stop.
- `HOS_DBG=strict,strictall`: a survey; every reason's first events after arming are logged and nothing stops.
- The 600-present block has `strict: armed (at present A); known list N; events by reason (before arming / after): ...`.

**Run:** `RUN=timing scripts/run-vk.sh <name> 150 -- RX_BACKEND=direct HOS_DBG=strict HOS_GAME_STEP_PER_FRAME=1`; the run
script prints `exited with 86` when strict stopped it.

**Every event strict raised, with cause and status** (surveys `m6b_sv_k`, `m6b_sv2_k`, `m6b_sv_hf`, `m6b_sv_t`; the final
strict runs `m6b_st_k`, `m6b_st_hf`, `m6b_st_t`):

| Event (reason) | Where, how many | Cause | Status |
|---|---|---|---|
| `no render targets set in the list` + `with no target` | Kokiri layer list `0x8007b0c8`, SetRenderTargets #1 and #2, 0.83 draws a frame (programs `0b4780da69a698b5`, serial 14567 / 14570) | NVN draws with no target at all over the viewport's area; the drawn one is a 1-vertex draw over a 1x1 viewport whose vertex stage reads the `KokiriForest_Child` 1360x1000x8 store texture and writes a storage buffer. The back end did not begin a rendering without attachments and skipped it | **fixed**: a pass with no target begins a rendering with no attachments over the viewport's extent, as NVN and the frozen back end's `bare_targets` do (`direct_pass.cpp`, `bare_area`). Whether this is the source of M4f's 384x216 dither was not checked |
| `Vulkan object made in steady play: a secondary command buffer` | Kokiri, 80 in 150 s (`m6b_sv_k`) | a thread's command-buffer slot grew when the job system handed it a larger share of the frame's jobs than before; slots grew one thread at a time over minutes | **fixed by a rule**: a thread can be handed at most a whole frame's lists, so a slot that records is grown at its reset to the most command buffers any frame took (counted per frame, `cmd_frame_presented`); a slot then grows only when a frame needs more than any frame before it (`direct_cmd.cpp`). 0 in `m6b_sv2_k`, `m6b_st_*` |
| `Vulkan object made in steady play: an image for a texture the game made (renderable)` + `... an attachment view of a game's texture` | Kokiri 18 + 14, HF 15 + 11 in 150 s; title 29 + 18 | the game makes textures in play. In Kokiri and HF: local-light shadow maps (D32F 1152x1152, 1280x1280 ...) sized by the light's distance, which `host/rt_slots.c` rounds to multiples of 128 (`HOS_RT_QUANT`), each new size a new texture at a new address; on the title the menu's text textures (256x46 R8) made at presents 1213 and 1305 (its menu appearing: arming at 1200 is early for the title) | **known** (one entry each). Fix: a game patch that gives a light one shape; not done here |
| `program miss in play` + `a shader object` | HF, 6 + 4 (`m6b_sv_hf`) | the first HF run after the translator change: variants first needed at presents 2687 and 2765 were translated then | gone with the cache warm (`m6b_st_hf`: 0). A first run of a new build still stops strict: correct, it is a skip in play |
| `Vulkan object made in steady play: a command pool`, `an image` | title, present 3923 (`m6b_sv_t`) | the attract movie starts (MovieOperateThread records for the first time; its frame textures) | a load, not steady play; the title's strict run ends before it |
| `a uniform buffer read at a constant offset past its bound range` | Kokiri 69-70 a run, **before arming only** (present 474) | program `55c0748147310296` vertex stage reads c6 to 688 bytes, 192 bound, in a 12288x12288 D16 pass at the load | event by design (see below); never in steady play |
| `a storage buffer slot read that nothing bound (the null window)` | HF 1-2 **before arming** (present 413, program `116a3b54c41fd4f0` stage 6 slot 2) | a mesh draw at the scene's first frame | never in steady play |
| `recorder asked: key not known`, `recorded with no formats known`, `called with no secondary`, `resolved in a job list`, `executed in a pass of another size` | Kokiri, HF: before arming only (the scene's first frames) | a pass key's first frame (the plan's Jobs: expected about 23 keys a scene) | by design; never in steady play |

**Decisions the brief asked for:**
- **Uniform buffers read past their bound range (about 880 a frame).** Of these, a program whose read size is Eden's 64 KB
  "used size" of a dynamically indexed array is not a read past the range: the bound is the translator not knowing the
  index, and the game binds the range its index stays in. That is by design and not an event (still counted). A read at a
  **constant offset** past the bound range is one: Maxwell reads zeros there, this back end reads the memory after it. That
  is a strict event. It happens only at the load (above); in steady play Kokiri and HF have none.
- **Placeholders.** `CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm` (BNTX 0x2f01, ASTC 5x5) has no store record because its file
  in this dump is a **stand-in**: it is one of the three zero-headed files of the 23.3 GB dump (`nx2/nup.py
  damaged_game_files`: "the Great Fairy Fountain's floor"), replaced by a copy of another normal map. The BNTX inside it
  names its texture `CmnTex_Map_Sand_FairyFountain_Sand_A_Nrm`, so the store has a record under that name and none under the
  name the game asks for. `DummyTextureForEvent` (4x4 RGBA8) is in no `.bntx.zs` file of the romfs and the store has no record
  of it (where the game loads it from was not found). Neither was sampled through a driver-bank handle after arming in any
  run. "ASTC not sampled: placeholder" is a start-up line; its counter (ASTC placeholders) is 0.
- **Task pool.** No refusal and no growth in steady play in any strict run (both are events; the pools are made at the
  load). The design is still drop-then-grow with the CPU reading GPU-written counters at the slot's reset. A rule that would
  remove it: the records' task count is bounded by their writers (the culling shaders' `atomicMax(ceil(min(total, 0x1400)
  / 16))`, at most 320 task workgroups: `m0-task-mesh.md` section 3; observed 31 and 310), so each indirect task draw could
  reserve 320 x (threads + payload words) x 4 bytes (1.06 MB for the programs here) at record time, with no GPU pool, no CPU
  read and no growth. **Not built**: the bound is proven only for the writers M0 read (the five other record addresses'
  writer is inferred), and per-draw reservations of a megabyte move the high-water problem into the frame arena. Open.
- **Quads, 8-bit indices, multi-record task+mesh indirect draws:** never raised, before or after arming, in any of the seven
  strict runs (each is a skip reason through `count()`, so strict would have stopped).

**The known list** (`kStrictKnown`, logged at each strict start):
1. `Vulkan object made in steady play: an image for a texture the game made (renderable)`: the game makes render targets in
   play (local-light shadow maps sized by distance, 128-rounded; the title's menu text); each new size is a new texture.
2. `Vulkan object made in steady play: an attachment view of a game's texture`: the attachment view of such an image.

**What would make strict miss a bug:** a skip that is not counted at all (strict is as complete as the counters: the test
checks that every early return of `draw_emit` goes through `count()`, and the counters' kinds listed above; a new skip path
outside them would be silent); a texture sampled through a handle the shader reads from a game buffer (bindless: most
material textures) is not checked for the placeholder; events in the first 600 presents after the last program miss; a
known reason hides every event of that reason, not only the cause it was added for (both known entries name images the
game makes, so a back-end image made in play for another cause would be hidden if it goes through the same path: it does not,
the back end's own images are made under no `ObjectsFor` scope); a GPU-side drop (the task step's clamp) is seen only through
the CPU read at the slot's reset.

## 3. Picture checks that can fail

### a. Full-resolution dumps

- **Direct:** `HOS_DBG=dump:<present>,dumpw:4096,dumpraw,dumpdir:<dir>`: every pass target at full resolution, float targets
  (colour and now depth) as raw RGBA32F beside the PNG. The dump line names the pass by **the program of its rendering's last
  draw** (a job's executed in it included), as the frozen back end names its passes (`PassRec::last_prog`).
- **Frozen:** `RX_VK_DUMP=<present> RX_VK_DUMP_W=4096 RX_VK_DUMP_RAW=1 RX_VK_DUMP_PROGS=pass` (passes only: the sampled
  textures at full size made the dump take minutes).
- **Packed** by `scripts/direct_picture_check.py pack` into a lossless store: 8-bit UNORM/sRGB as uint8, 16-bit float and
  B10G11R11F as float16 (exact), D16 as uint16, D32F / R32F / RGBA32F as float32, integer targets as the dump's PNG, each
  zstd-compressed, with an index. A frame's 3.1 GB of raw dump packs to 225 MB (Kokiri) in about 10 s.
- **Stable pass identity:** a target is its slot, format and size and **its rank among the targets of that signature by first
  appearance in the frame**. Guest addresses are not used: transient targets' addresses move by a slot between runs (120 of
  281 Kokiri states of two frozen runs had another address). Consecutive renderings over the same targets are merged into
  their last (frozen ends a rendering at each job, direct around task steps), then a target's states are lined up by their
  last program in order, so a cleared state is never compared with a drawn one; a target whose last states are named
  differently on the two sides is compared by its final state. This solves M4c's "aligned against another state": the
  cascades and the depth prepass line up (one false failure on the depth prepass was found and removed while building this:
  a state named by the same program at another point of the pass).

### b. The comparison

`scripts/direct_picture_check.py compare <golden run> <packed run> --noise <noise.json>`:
- Values normalised (UNORM as is, depth as is, HDR floats through x / (1 + x), integers as the stretched PNG).
- 16 x 16 tiles; per tile and **per channel** the mean absolute difference. **A tile's threshold is twice its own noise**
  (the worst error of that tile between any two of three frozen runs), at least 2/255. A pass fails if **any** tile of any
  channel is over its threshold, or if more texels are NaN/Inf on one side only than the noise had. The picture's mean is
  printed and decides nothing.
- Tiles whose noise is above 0.05 are left out and counted ("something there moves between runs"); a pass is **UNSTABLE**
  ("not comparable at one frame") when under half its tiles are comparable or it is not in every frozen run; MISSING (a
  golden state the checked run lacks) fails; EXTRA (only in the checked run) and MERGED are listed.
- `noise <out> <run>...` builds the table (summary JSON and per-tile maps, `noise.json.npz`).
- Exit code 1 on any FAIL or MISSING; a one-line verdict with the counts.
- The synthetic self-test (`tests/test_direct_picture_check.py`): the same picture passes; **a difference that cancels in
  the mean** (+0.1 on one half, -0.1 on the other, mean shift 0.000000) fails; one flat channel fails; one wrong tile fails;
  a missing state fails; a NaN fails; a cleared state is not lined up with a drawn one; a tile noisy between frozen runs is
  left out while a difference beside it still fails.

### c. Golden references and the one-command check

- `work/golden/<scene>/` (in the m6b worktree's `work/`, not committed): `run1..3` (packed frozen dumps), `noise.json` and
  `noise.json.npz`, `manifest.json` (commit, scene, present, overlay, back end, how to make it again). Kokiri 672 MB, HF
  811 MB, TITLE_MARKET_SIZES.
- `scripts/direct_check.sh <title|kokiri|hf|market> [name]`: runs the direct back end (420 s, the dump at the scene's present),
  packs, compares, prints the failing passes and the verdict. `GOLDEN=3 scripts/direct_check.sh <scene>` makes the reference
  again from three frozen runs. Presents: Kokiri 1800, HF 1500, title 2400, market 1500.
- **Determinism.** By default the game steps by wall time (the host's real-time step), so at present 1800 its clock differs by
  about 20 frames between runs (337.1 / 716.4 / 1031.9 ... against 338.7 / 707.6 / 1012.1 ... at presents 300, 600, 900), and
  two frozen Kokiri runs differed in 77 of 281 states (lit target: 22-30 % of tiles beyond 2/255, worst tile 0.47).
  **`HOS_GAME_STEP_PER_FRAME=1`** (new, host only, `host/rx_backend.c`: option 0 answered 0, so every frame steps 1/60 s) makes
  the clock exactly present + 2 in every run (302.0, 602.0, ..., 2702.0 in two runs). The check and the reference both use it.
  **What still differs** between frozen runs at the same present: Link's idle pose, the fairy and her light, the grass
  (diff image of two runs' lit targets), so the shadow cascades (up to 6.9 % of their tiles noisy), the screen mask (22 %) and
  the composite (3 %) have tiles left out; and the env-cube chain (amortised faces, drawn in some runs' frame and not in
  others), which is UNSTABLE. Of Kokiri's 110 pass identities after merging, 10 are not comparable at one frame and 18 are
  comparable in part; HF 103, 16 and 15. The median pass's worst-tile noise is 0.009.

### Results

**Kokiri** (`m6b_chk_k`, verdict `work/runs/m6b_chk_k/verdict.txt`): **FAIL**, 343 states: 15 FAIL, 3 EXTRA, 9 UNSTABLE, 238
MERGED, 78 PASS. Failing passes, ranked by the share of tiles over threshold:

| Pass (target, state, last program) | Tiles over threshold (R G B A) | Worst excess | Picture mean diff | Reading |
|---|---|---|---|---|
| RGBA8 384x216 #1 `531432c3` | 0 / 0 / **71.4 %** / 0 | +0.028 | B +0.033 | M4f's flat-blue fault: the B channel alone, over most of the picture |
| composite RGBA8 2560x1440 | 3.9 / 6.8 / 13.6 % | +0.06 | -0.001 | the frame differs in places; B most |
| RGBA16F 512x512 `0ddde357` | 0 / 3.2 / 3.3 % | +0.033 | +0.001 | M4c's "512x512 #2" (G and B only) |
| depth D16 1600x1600 cascades, final | 2.1 % | +0.36 | +0.002 | shadow casters differ (M4c called it an alignment artefact; aligned now, it is real) |
| B10G11R11F 2048x1152 `fc10fab7` | 1.6 / 0.7 / 0.4 % | +0.05 | 0 | bloom/exposure input |
| RGBA8 1536x864 mask #2 `d8180bd8` | 1.2 / 0.3 / 0 / 0 % (22 % left out) | +0.12 | G -0.0075 | the characters' shadow mask |
| RGBA8 1536x864 mask #1 `1e26755d` | G 0.1 % | +0.096 | 0 | as above |
| depth D32F 1152x1152 final (local shadow) | 0.3 % | +0.03 | 0 | |
| lit B10G11R11F 1536x864 states #3, #5, #6, t1 #0/#1, t2 #0, 960x540 | 0.0-0.1 % (a few tiles) | +0.003 to +0.012 | 0 | small local differences |

**HF** (`m6b_chk_hf`, `work/runs/m6b_chk_hf/verdict.txt`): **FAIL**, 286 states: 24 FAIL, 12 EXTRA, 5 UNSTABLE, 191 MERGED, 54 PASS:

| Pass | Tiles over | Worst excess | Mean diff | Reading |
|---|---|---|---|---|
| depth D32F 384x216 final `bcc77ff4` | **89.6 %** | +0.095 | -0.029 | a whole quarter-size depth differs; the extra direct-only 384x216 depth states (EXTRA, t2-t5) suggest the passes around it are not the frozen ones |
| composite RGBA8 2560x1440 | 10.6 / 5.4 / 6.0 % | +0.13 | -0.0013 | |
| RGBA8 1536x864 mask #3 `59095243` | 8.3 / 0.7 / 2.5 % | +0.98 | | |
| RGBA8 1536x864 masks #1, #2 | G 2.7-2.9 %, B 2.5 % | +0.17 | | |
| RGBA32F 384x216 `25b435fc` | A 3.9 % | +0.45 | | |
| R16F / R32F 384x216 `6ffa3124` | 3.0 % | +0.75 | -0.013 | |
| B10G11R11F 2048x1152 `fc10fab7` | 2.1 / 1.4 / 1.2 % | +0.08 | | |
| depth D16 1600x1600 final | 2.0 % | +0.58 | | |
| depth D32F 896x896 final (local shadow) | 0.5 % | +0.07 | | |
| RGBA16F 768x432 c1 `c8ca29e4` | A 0.3 % | +0.018 | | |
| B10G11R11F 960x540 | 0.1 % | +0.006 | | |
| depth D32F 1536x864 (11 states) | one tile each | +0.0009 | 0 | marginal: one tile just over twice its noise |

TITLE_MARKET_RESULTS

### d. Draw-level comparison, and the 1078 against 685

`scripts/direct_trace_compare.py <frozen draw_stats_N.txt> <direct log> [--gated drawn|skip]`: per program, frozen draws
against direct draws in one frame, "same / frozen more / direct more", the dispatch programs on one side only, and a verdict
(exit 1 on any difference). The direct trace now marks a gated draw (`drawn gated`): it is recorded whatever its condition and
the GPU decides.

Kokiri, present about 1830, conditions as the game has them (`m6b_ds_f`), forced on (`m6b_ds_fa`, `RX_VK_COND_ALWAYS=1`), and
direct (`m6b_ds_d`, `drawlog:1800`), all with `HOS_GAME_STEP_PER_FRAME=1`: **FAIL**, 177 of 321 programs differ.

What the evidence settles:
- **The frozen count is not a statistics artefact.** Its own replay counter says 684-687 draws a frame in every 120-present
  window; the draw-stats file (pipeline-statistics queries on every draw of one frame) has 596. M4f's remark that the
  statistics miss draws at the frame boundary does not explain 400 a frame.
- **Not render conditions.** With every condition forced on, the frozen back end draws 687 a frame (684 without); in direct,
  330 of the frame's draws are gated.
- **Not occlusion results.** The frozen back end writes every samples-passed report as 4096 (everything visible,
  `vk_replay.cpp`, `RX_VK_OCC_FIXED`); the direct one writes the GPU's counts. With `HOS_DBG=qfixed` (4096, as frozen) the
  direct back end still draws 1077 a frame (1097 without).
- **Where:** the differences are concentrated in the main view's job passes (1536x864, list kind 2): the depth-prepass
  program `ffb53ed102dd9908` 95 frozen / 181 direct, `46c14f8e` 18 / 52, `58ad8db3` 5 / 34, programs drawn only in direct
  (`695ed228`, `4a8885a0` ...), and in the env cube (128x128), whose faces are amortised.
- Direct's own records: 448 draw records a frame in the lists recorded that frame (DRAW 24.5, DRAW_IDX 423.6) and about 650
  more from the 1131 load-time list (packet) calls a frame that expand into them.

**Not settled:** whether the direct back end draws a load-time list's draws more often than the frozen one replays them
(packet expansion), or the game submits more because something it reads differs (the frozen back end's timestamps are 0.1
of the real clock: `RX_VK_TS_SCALE`). The next check is a per-call count of the packets' draws by return address on both
sides.

## 4. Tests without the game

New files (the existing ones are unchanged except `tests/run_all.py`, which lists the picture test's modules, numpy and
zstandard, as it does for the texture tests):
- `tests/test_direct_picture_check.py`: runs the tool's self-test (above).
- `tests/test_direct_strict.py`: the known list's shape (each entry a reason the back end raises, or an `ObjectsFor` cause,
  with its why; each once; the sentinel), strict stops (86) and the messenger stops (87) and flushes the log first, a known
  reason never stops, the layer is proven with `dl_iterate_phdr` and the known list is logged, the arming uses
  `kStrictQuiet`, no `getenv` in `gpu/direct` but `HOS_DBG` in `direct_main.cpp`, every early return of `draw_emit` goes
  through `count()`, the pass, job, query and task counters raise their events, and Vulkan objects are counted only through
  `object_made`.
- `tests/test_direct_layer0_windows.py` with `tests/direct_layer0_windows_test.cpp`: the original layer-0 checks run against
  `gpu/direct/layer0_windows.h`, with what the copy changes (windows a granule short of 4 GB, at most RADV's 4 GB - 4; the
  longest whole range a granule short of 2 GB; the last granule of each stretch in its own window only; every piece of a
  random range inside its window; the aliased chunk place of a stretch's top chunk ends past the window, which is why
  binds go through `range_places`); that the copy differs from `gpu/vk/layer0_windows.h` only in its documented lines; and in
  `direct_pools.cpp`: binds through the pieces, imports outside the lock, bits only after the binds were waited for, no
  environment read.

Run: `HOOKSHOT_VULKAN_HEADERS=/usr/include work/venv/bin/python -I -B tests/run_all.py`: 295 pass, 5 fail (the baseline:
test_texture_init_barrier, test_lift_cleanup_error, test_make_game_cache, test_parallel_init_order, test_recomplauncher_json).

## Frame time

The frame path changed: one strict test per skipped draw or counted op (strict off: a load and a branch), `last_prog` stored
per draw, three relaxed atomic adds per command buffer handed out, one call per present (`cmd_frame_presented`), the draws
over no target (0.83 a frame more) and the private colour outputs. Kokiri, pacing off (`HOS_NO_PACING=1 HOS_FPS_TARGET=1000
HOS_FPS_MAX=240`), the baseline a build of 5beed38 (`work/wt/m6b_base`, warmed first), three alternated pairs of 100 s runs,
the last six 600-present windows each:

| Pair | 5beed38 mean / p99 | this stage mean / p99 |
|---|---|---|
| 1 (`m6b_t_base1`, `m6b_t_new1`) | 12.64 / 16.28 ms | 12.77 / 15.61 ms |
| 2 | 12.25 / 14.24 | 12.87 / 16.53 |
| 3 | 12.76 / 16.55 | 12.53 / 15.10 |
| mean (range) | 12.55 (12.25-12.76) | 12.72 (12.53-12.87) |

The difference of the means, +0.17 ms, is inside the run-to-run spread of either build (0.5 ms); a cost below about 0.3 ms
cannot be told from no cost with three pairs. HF was not timed.

## Files

- `gpu/direct/direct_check.{h,cpp}` (new): the messenger, strict, the known list, `object_made`.
- `gpu/direct/direct_draw.cpp`, `direct_compute.cpp`, `direct_pass.cpp`, `direct_query.cpp`, `direct_task.cpp`: the events; the
  colour types' Disabled; passes with no target; `pass_where`; `last_prog`.
- `gpu/direct/direct_cmd.{h,cpp}`: slots grown to the most a frame took.
- `gpu/direct/eden/.../spirv_emit_context.{h,cpp}`, `emit_spirv_context_get_set.cpp`: unattached outputs as private variables.
- `gpu/direct/direct_dump.cpp`: pass names by last program, raw depth.
- `host/rx_backend.c`: `HOS_GAME_STEP_PER_FRAME`.
- `scripts/direct_picture_check.py` (new), `scripts/direct_check.sh` (new), `scripts/direct_trace_compare.py` (rewritten).
- Tests above.
