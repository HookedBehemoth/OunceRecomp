# Native Vulkan: replacing the renderer from the bottom up

A local branch (`native-vk`), not for release: the NVN2 record/replay renderer replaced layer by layer with native
Vulkan on this PC (RX 6950 XT, RADV, Linux), the old path kept behind a switch until the new one covers it.

## Targets

| Target | Means |
|---|---|
| No stutter | every pipeline built before it is drawn (all programs known from the romfs), no shader translation or texture decode during play |
| Fast loading | assets prepared once into the GPU's own formats and read straight into their final memory; no zstd, ASTC or block-linear work at load time |
| No double backing | one copy of each byte: pools are Vulkan memory at the guest address, textures live only in their images |
| Native storage formats | BCn instead of ASTC (no ASTC on RDNA2), Vulkan's tiling instead of Tegra block linear |

`VK_EXT_memory_decompression` is not a requirement (RADV does not have it).

## What this device gives

From `vulkaninfo` (Mesa 26.2.4): `VK_EXT_map_memory_placed` (4 KB alignment), a 16 GB device-local host-visible heap
(Resizable BAR), `VK_EXT_descriptor_heap`, `VK_EXT_descriptor_buffer`, `VK_EXT_device_generated_commands`,
`VK_EXT_shader_object`, `VK_EXT_graphics_pipeline_library`, `VK_KHR_pipeline_binary`, `VK_EXT_host_image_copy`,
`VK_KHR_copy_memory_indirect`, `VK_EXT_mesh_shader`, `VK_KHR_present_wait2`. Not there: ASTC, cooperative matrix.

### Measured (work/tools/placed_test.c, 2026-11-10)

- Placed mapping works for device-local host-visible (BAR) and host-cached memory, at a guest-like address inside a
  `PROT_NONE` reservation; `VK_MEMORY_UNMAP_RESERVE_BIT_EXT` leaves the range reserved.
- Requested device addresses (`VkMemoryOpaqueCaptureAddressAllocateInfo`) are refused
  (`VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS`): RADV takes back only addresses it gave. Device address = guest
  address is not possible that way.
- `maxBufferSize` and `maxMemoryAllocationSize` are 4 GB; sparse binding and residency (buffers, 2D and 3D images,
  aliased) are there, `sparseAddressSpaceSize` 128 TB. So: one sparse buffer per 4 GB-aligned guest window that holds
  pools, each pool's memory bound at its offset; a shader's device address is
  `window_base[guest >> 32] + (guest & 0xffffffff)` (a table of at most 128 entries) instead of today's page table.
  NVN virtual pools become sparse bindings of physical pool pages in the same buffers.
- **CPU reads from BAR memory: 4 MB/s** (host-cached: 5.7 GB/s); CPU writes to BAR 2.8 GB/s with a plain loop. A read
  of VRAM on the CPU is ~1400x slower than of system memory: every read site the census finds must move its pool to
  host-cached memory or stop reading.

## The layers, bottom up

0. **Memory (pools).** Each NVN memory pool becomes a `VkDeviceMemory` mapped with `vkMapMemory2` +
   `VkMemoryMapPlacedInfoEXT` at `A64_PTR(storage)`, the guest address the game gave `nvnMemoryPoolInitialize`.
   Guest address = GPU address already (`host/nvn_native.c`), so the game's CPU code and the GPU see the same
   bytes, as on the console. CPU-no-access pools need no mapping; virtual pools become sparse bindings of physical
   ones. Gone: host-memory imports, VRAM mirrors, write-backs, the per-store page marks (`A64_MARK`, ~1 ms a frame).
   **The hazard:** the game relies on the shared memory. CPU reads from BAR memory are uncached, and CPU texel
   writes are in the console's block-linear layout. The **pool census** (below) lists every call site; each is
   resolved by pool placement (system memory for read-back pools) or by changing the code.
1. **Commands.** Command buffers translated at submit into Vulkan in submit order, with honest syncs
   (`nvnSync` = timeline semaphore value), so the GPU reads live memory exactly when the console's would. No
   submit-time captures.
2. **Shaders.** All 38,809 programs (42,856 unique stages, `Nin_Ounce_NVN2` and effects) are known from the romfs: translated offline, pipelines (or
   shader objects) built before play, kept in a pipeline binary cache. Replacements for hot or broken shaders as
   hand-written GLSL keyed by program hash.
3. **Textures and assets.** The loaders above NVN (agl / gsys texture resources, bntx) replaced: textures prepared
   once as BCn in Vulkan layout, streamed into images; the guest pool range for them stays unbacked.
4. **Engine.** sead / agl / gsys render paths replaced by host code where the profile says so.

## What the current back end does with memory (mapped 2026-11-10)

- Linux already shares one backing in system RAM: the guest window is a memfd (`host/hos_main.c`, `HOS_GUEST_MEMFD`),
  pools are imported as dma-bufs through `/dev/udmabuf` (`vk_memory.cpp` `import_dma_buf`), host-pointer import as
  the fallback. The GPU reads those pages over PCIe, so discrete cards get **VRAM mirrors** (`mirror_make`,
  `mirror_sync`, `RX_VK_MIRROR`): the second copy, kept in step from the per-store page marks.
- **Captures** (`memory_capture_submit`, every submit) copy written pages at submit time, because queues held by GPU
  event waits replay later while the game has moved on. **Write-backs** copy GPU writes from mirrors back to guest
  memory at the end of a submission, with a VRAM shadow per pool to diff against (`RX_VK_WB_DIFF`).
- Device addresses are not guest addresses: shaders that follow pointers go through a page table pushed as a
  constant (`memory_guest_pages`). Placed pools could make them equal if RADV honours a requested address
  (`VkMemoryOpaqueCaptureAddressAllocateInfo`): to test.
- Reports are synthesized: occlusion always 4096 (`RX_VK_OCC_FIXED`), timestamps from host ticks scaled by
  `RX_VK_TS_SCALE`. No Vulkan queries.
- The central lookup is `gpu_buffer_bind` (`vk_memory.cpp`, 30 callers); textures are keyed by address and layout
  in `vk_texture.cpp` and uploaded through GPU ASTC decode and GPU unswizzle.
- **Open question for layer 1:** do the game's syncs, honoured exactly, make captures unnecessary (as on the
  console)? Test: the current renderer with `RX_VK_CAPTURE=0`, and the census's write sites against sync points.

## Iterating quickly (start here)

Everything game-made lives in `work/` (ignored). Run Python as `work/venv/bin/python -I`.

**Setup once** (on a fresh clone; this PC has it): `python3 -m venv work/venv && work/venv/bin/pip install -r
requirements.txt`; `nx2/nup.py <.nup> --extract work/dump` then `nx2/dump.py --dump work/dump --set`; header-only
deps in `work/deps` (SPIRV-Headers `vulkan-sdk-1.4.341.0`, VulkanMemoryAllocator `v3.3.0`); `mkdir -p work/image`
before the first `nx2/lift.py --standalone --write` (it does not make it); `nup.stand_in_damaged('work/dump/romfs')`
for this dump's three zero-headed textures. The builder cache's `hot_funcs.txt`, `hot_order.txt`,
`hookshot_pieces.prof` were copied to `work/`.

**Build.** Always with the headers:
`HOOKSHOT_SPIRV_HEADERS=$PWD/work/deps/SPIRV-Headers HOOKSHOT_VMA_HEADERS=$PWD/work/deps/VulkanMemoryAllocator
work/venv/bin/python -I -u nx2/build.py --standalone --jobs 12` (output `work/sa/build/`).
- A full rebuild is ~14 min (any change to `third_party/a64-kit/rt/a64.h`, build flags, or `RX_POOL_CENSUS` on/off).
- **A host-only or hook change is ~40 s.** To replace a game function, write `ENTRY_HOOK(0x8xxxxxxx)` or
  `ENTRY_HOOK_SYM(name)` in `host/*.c` (return 1 = handled), run `work/venv/bin/python -I nx2/entry_hooks.py
  work/sa/gen` (the lift runs it too), then build: only the spliced code file and the runtime recompile. Read the
  lifted body first: `grep -l "^void f_<addr>(Ctx \*__restrict" work/sa/gen/code_*.c`.
- New host `.c` files appear in the next build only (the list is taken at the start).
- Keep builds side by side: copy `hookshot`, `hookshot_vk.so`, `hookshot.map` and the caches (`*.bin`, `*.idx` from a
  warm run) to `work/sa/<name>/` and run with `EXE=`. Present: `census` (census v2), `zstd` (libc + zstd;
  its caches predate the shader-object stamp change), `so` (shader objects; warm caches in `so_caches_warm`), `so2` (compute
  shader objects, 578f409; HF caches in `so2_caches_hf`), `so3` (its bisection build), `rb2`
  (byte-range readback waits), `tt` (transient targets, step 4), `rt` (stable render-target slots, step 4e: the
  current best). Older ones were removed 2026-11-11.

**Parallel work (2026-11-11).** `RUN=timing scripts/run-vk.sh ...` takes the GPU lock exclusively (fps, launch time,
anything that may hang the GPU: a reset kills every run beside it); `RUN=check` takes it shared, at most `RUN_SLOTS=2`
runs at once, each on a private reflink copy of the build folder (caches not raced; `frames_trace.csv` copied into the
run). Do not wrap either in flock (plain `flock work/run.lock scripts/run-vk.sh` still works, exclusive). Two check runs
fit (trial: 16.1 of 16 GB VRAM with 8 GB spilled to GTT, 4.2 GB RAM left, ~36 fps each), three do not.
`scripts/agent-worktree.sh new <name>` gives an agent its own worktree (work/wt/<name>, branch agent/<name>) with its
own `work/sa/gen` and `work/sa/build` (reflink copies) and the shared object cache: a host-only build in ~10 s;
`land` rebases and fast-forwards, `drop` removes it.

**Run.** `OVERLAY=work/overlay/chisel_kokiri EXE=work/sa/zstd/hookshot scripts/run-vk.sh <name> <seconds> -- [VAR=..]`:
straight into Kokiri Forest (no title, no intro), headless, muted, shots every `SHOT_EVERY` presents in
`work/runs/<name>/shots`, `timeline.txt` (second, presents, RSS MB), `log.txt`. It sets the RDNA2 workarounds
(`RX_VK_MESH_NOCULL=2c56dc1006942f74`, `RX_VK_TASK_MESH_FALLBACK=1`; `TASK_MESH_FALLBACK=0` for native task/mesh,
which hangs the GPU under faster timing). Other overlays: `work/tools/make_chisel_overlay.py <dir> --route newgame
--banc <map> --start <name>` or `--route load --slot N` (maps in the chisel section below). Scripted input:
`"HOS_INPUT_SCRIPT=30:LUP/3000,34:RRIGHT/2500" HOS_INPUT_EXCLUSIVE=1` (seconds from the first controller read).
- **Launch time:** `work/venv/bin/python -I scripts/launch_time.py work/runs/<name> ...` (gameplay and playable seconds).
- A first run of a build dir is cold (shader translation and pipelines: ~45 s); copy a warm dir's caches for A/B.
- GPU hangs reset the whole GPU (allowed on this PC); after `VK_ERROR_DEVICE_LOST` read `journalctl -k` (the guilty
  ring and thread) and rerun with `RX_VK_CHECKPOINTS=1` (AMD buffer markers: the last draws reached).
  `RADV_DEBUG=hang` crashes inside RADV on this workload.

**Measure.**
- CPU, all threads with waits: Superluminal (`~/source/Superluminal`, read its AGENTS.md):
  `sudo -n ~/source/Superluminal/SuperluminalCmd run linux --json --capture-path <scratch>/x.linux --max-duration 30
  --env-var K=V ... work/sa/<build>/hookshot work/sa/image/image.bin --romfs work/dump/romfs --user <run>/user`
  (the env of run-vk.sh passed by hand). Hand the analysis to a sub-agent: the `llm` output is large.
- Cheaper sampler: `HOS_PROFILE=5 HOS_SAMPLE_ALL=<f>` with `<f>.go` present, `RX_MAPS=work/sa/<build>
  scripts/crit.py <f>`.
- Pool census: build with `RX_POOL_CENSUS=1` (full rebuild), run with `HOS_POOL_CENSUS=<f>`, then
  `scripts/pool_census.py <f> [--since <f>.003] --exe work/sa/census/hookshot`.
- Tests: `work/venv/bin/python -I -B tests/run_all.py`; three fail without our changes (`test_lift_cleanup_error`,
  `test_recomplauncher_json` fail on main too; `test_make_game_cache` only in this checkout).

**RE.** Flat modules `work/re/{main,sdk,subsdk0}.bin` (main base 0x80020000, sdk 0x84fa0000), strings
`work/re/main_strings.txt`, exports `work/sa/image/image.json`, tools `work/tools/` (`disasm.py`, `xref.py`,
`callers.py`, `bntx.py`, `bfres.py`, `bfsha.py`, `ainb.py`, `byml.py`, `sarc.py`, `zs.py`), reports
`work/assets/*.md` (`formats.md`, `boot.md`, `zstd.md`).

## Tools

- **Pool census** (`RX_POOL_CENSUS=1` build, `HOS_POOL_CENSUS=<file>` at run time, `scripts/pool_census.py`): every
  lifted load and store of a pool page, counted by place (the lifted function), pool kind and direction.
- **Chiseled boot**: straight into a map with no menus, through patched sequence graphs read with
  `HOS_FS_OVERLAY=<folder>` (`host/nn_fs.c`). `work/tools/make_chisel_overlay.py <dir> --route newgame --banc <map>
  --start <name>` (or `--route load [--slot N]`) writes them: NormalTitle node #8 skips the button wait, the root's
  `FirstBancParam` (`Work/Banc/GameBancParam/KokiriForest_Child...`) names the map, Opening's intro events are cut
  (`--keep-intro` keeps them). Maps: `KokiriForest_Child`, `HyruleField_Child`/`_Adult`, `KakarikoVillage_Child`,
  `InsideTheDekuTree`. Run with a scratch `--user`: the new-game route saves. Details: `work/assets/boot.md`.
  The movie in it is the title screen's background (event `Title_Background`, `Dm_Title_0005.webm`): BeforeTitle waits
  for it to start, which is why `HOS_NO_MOVIES=1` (an MKW switch: no `.webm` found) never leaves the boot.
  `--skip-title` leaves the event out and `--skip-fades` the loading fades; both are opt-in because neither shortens
  launch (below).

## The engine (what is known)

`main` is stripped and built without RTTI: only heap, task and thread names survive (`sead::TaskMgr`,
`sead::FileDeviceMgr`, `agl::driver::GraphicsDriverMgr`, `agl::lyr::RenderDLBuffer`, `gsys::SystemTask`,
`gsys::ModelResourceMgr(Heap)`). Middleware: NintendoWare G3d2 22.2.8, Vfx3, Ui, SMAA, Havok 2025.2. Functions are
found by matching open-ead/sead and by string references. romfs layout as TOTK's: `.zs` with dictionaries in
`Pack/ZsDic.pack.zs`, textures as one `.bntx.zs` per texture in `Tex/`, models `.bfres.zs`, shaders in
`Shader/*.Nin_Ounce_NVN2.release.sarc.zs`.

## Assets (scanned 2026-11-10; parsers in work/tools, report work/assets/formats.md)

| | Found |
|---|---|
| Textures | BNTX 4.1.1 (`'Ounc'`), one texture a file, 20,664 readable; block linear (64x8-byte GOBs, block height in the header) verified by size on all. ASTC 10,886 (6.2 GiB: 5x4 to 12x12, mostly 5x5, 8x5, 8x6, 6x6), BCn 9,566 (8.45 GiB, BC4 the most), 212 others. 15.7 GiB decompressed. 146 textures with `bh_log2=1` to check on the GPU |
| Models | BFRES 10.4.0, 6,464 models, 21,515 materials; one interleaved vertex buffer each, triangles, u16 indices. Strings by 64-bit id from `Shader/ExternalBinaryString.bfres`. 896 shapes (flag 0x10) keep their geometry as meshlets in an encoding not decoded yet |
| Shaders | BFSHA 9.1.1 with BNSH 2.4.1: 25,565 programs, 13,628 unique stage binaries (10,108 fragment, 3,012 vertex, 491 mesh, 17 task); `Model/model` 23,346 programs. Key to options: `(key[word] & mask) >> shift`. Effects (`vsetb`): 22,403 unique stages (23,162 compute among all). agl / gsys / terrain `.sharcb` and NDSR `.pck`: ~3,000 more, container not parsed |

So every stage the game can run is known before it runs (~39k unique): translated offline, built as shader objects
(`VK_EXT_shader_object`), no pipeline is ever made during play.

## Measurements (2026-11-10, dev build without the profile, chiseled boot into Kokiri Forest)

| | Cold caches | Warm caches |
|---|---|---|
| Boot (first 120 presents) | ~7 s | 8 s |
| Black loading screen | ~8 s | 8 s |
| First gameplay frames | ~30 s at 4 fps: 22 s waiting for 599 pipelines, 3.6 s translation, <0.5 s textures | 10 s at 12 fps: 4.7 s pipeline waits, 0.5 s imports, 0.5 s translation |
| Steady state | 42 fps | 47 fps |

- The loading screen is not throughput-bound: in ~48% of the samples no game thread runs; the main thread sleeps in
  `nvnWindowAcquireTexture` (pacing) 78% of its time, yet `HOS_NO_PACING=1` loads no faster (the loading screen at
  500 fps, gameplay 2 s later). What the loaders wait for: a Superluminal capture (`~/source/Superluminal`).
- Lower-resolution textures would not shorten launch: texture work is under half a second of it.
- **RDNA2 hang:** walking in Kokiri Forest hung the GPU (gfx ring timeout, full GPU reset) in task shader
  `2c56dc1006942f74`'s depth-grid walk, the Steam Deck hang; `RX_VK_MESH_NOCULL=2c56dc1006942f74` (set by
  `scripts/run-vk.sh`) avoids it.

### Launch time (warm caches, chiseled boot into Kokiri, `scripts/launch_time.py`)

| Build | Playable | |
|---|---|---|
| baseline (6 virtual cores, task/mesh fallback) | 25 s | |
| + native libc: `ENTRY_HOOK`s spliced at last (`nx2/entry_hooks.py`), `__memcpy_aarch64` hooked | 21 s | the hooks in host/nn_libc.c had never been linked |
| + native zstd (host/nn_zstd.c: `f_8010b380` whole files, `f_80115c80` ParallelDecompThread streams) | **18 s** | 6,000 files, 3.3 GB in the first 50 s, no failures |

Where the 18 s go (2026-11-11, `HOS_LOG_TIME=1`, a Superluminal capture; `work/assets/native/boot-speed.md`):
0-0.6 s image, device, first submit; **0.6-4.4 s single-threaded guest init** on sead::MainThread (effect resources
`f_812eed00`/`f_812ef8c0` 1.3 s, `f_8017ed80` 0.6 s; loaders idle; the program and pipeline caches load in ~120 ms at
4.4 s, off the critical path); 4.4-6.1 s the title's setup (resident events, the title event, frame-paced); 6.1-8.6 s
the new-game transition, with the title movie playing (no file opened 5.5-8.0 s); 8.6-13.2 s the black loading screen
(3.3 of 6 vcores busy: threads queue for their masked cores, 27 s of thread time in `vc_acquire`); 13.2-17.5 s the
first scene frames at ~28 fps (renderer work). Cutting the movie (`--skip-title`) made it 1-2 s slower: the first
scene frame then waits for ~2200 more actor files. Cutting the fades started the load 1 s earlier but did not end it
earlier. `HOS_VCORE_LEND_US=2000` (experiment, off: a vcore waiter takes any free core after 2 ms) ended the load 0.85 s earlier (3 runs) but playable only once of three and steady fps looked lower; `HOS_FS_KEEP_CORE_KB` no change.

Tried and dropped: `HOS_VCORE_ANY` (any free core for any thread: 24 GB and 27 fps, the game's per-core data follows
its core masks), `HOS_VCORE_SLICE_US=10000` (22 s, no gain), `HOS_NO_MOVIES=1` (never leaves the load: the boot
needs its movie).

### The RDNA2 hang with faster timing (2026-11-10)

`RX_VK_CAPTURE=0`, `HOS_CORES=8` and `HOS_VCORES=0` each lost the device at the map load (~20 s), every time. The
kernel names a **compute ring** (`comp_1.x`) as guilty and the graphics context as innocent, while every submission
goes to the one graphics queue: RADV runs task shaders on an async-compute ring, gang-submitted with the graphics ring.
`RX_VK_MESH_NOCULL` on every task shader did not help; `RX_VK_TASK_MESH_FALLBACK=1` (task + mesh programs as compute
shaders, issue 996's Navi 22 workaround) did: all three run 60-100 s, captures off included, the picture right, 44-47
fps (native mesh: 47).

| With the fallback | Present 720 at | fps |
|---|---|---|
| 6 virtual cores | 24 s | 46.4 |
| `HOS_CORES=8` | 20 s | 47.6 |
| `HOS_VCORES=0` | 21 s | 47.3 |

So the game survives more cores once the task path is off the async ring, and the per-submit captures are not needed for
a correct picture in Kokiri Forest. Open: which task program hangs natively, and why (a RADV gang-submit issue, or a
task shader that loops on data it reads too early).

GPU hangs since, all with the fallback on: `rblog` and `rb_nowait` (readback front, `work/sa/zstd`, the old whole-chunk
captures; `rb_nowait` with `RX_VK_NO_READBACK_WAIT=1`): a **graphics-ring** timeout (`gfx_0.0.0`, guilty thread
sead::MainThread, mode-1 reset) at ~67 s, both while walking on the scripted route (`45:LUP/10000,56:RRIGHT/6000,...`).
Reproducible with the wait on and off; not seen on the short `40:LUP/12000` route (8 runs of exact capture ranges).

### Pool census, first pass (110 s of Kokiri Forest with scripted walking)

4,573 pools: 3.6 GB CPU-cached + GPU-cached, 0.87 GB GPU-only compressible (render targets), 0.5 GB physical
(behind virtual pools), 0.32 GB shader code, 0.18 GB CPU-uncached. Accesses: **reads 2.9 GB, writes 1.6 GB**;
`memcpy` (sdk `f_8575cdc0`) is ~90% of both (2.68 GB out of pools, 1.31 GB into them), the zstd decoder
(`f_801102e0`) 81 MB read and 100 MB written in one pool. So the CPU-cached pools cannot simply live in BAR memory:
the copies out of them have to be found (census v2 names memcpy's callers) and either rewritten or their pools kept in
host-cached memory.

### Pool census v2 (callers, phases; work/runs/census2.txt*)

| | Load (0-30 s) | Gameplay (30-120 s) |
|---|---|---|
| CPU reads | 1.05 GB, 35 MB/s | 181 MB, 2.0 MB/s (32.4 M reads, ~360 k/s) |
| CPU writes | 0.87 GB | 349 MB, 3.9 MB/s |
| Pools the CPU reads | 51, 2.6 GB | **35, 132 MB** |

- Gameplay's reads are small and in small pools (0.12 MB pool read by `f_82c1d750` at 0.8 MB/s, 0.03 MB pools by
  `f_806dc520`'s copies, `f_80365f00`, `f_812342c0`, `f_8009bb80`, ...). At ~1 us an uncached read that would be ~0.36 s
  of CPU a second from BAR: **those 35 pools stay host-cached, every other pool goes to VRAM** (~4.5 GB, no mirrors).
  Writes (3.9 MB/s) are cheap over BAR.
- The load is one pool: **2.4 GB at 0x109c190000, CPU-cached + GPU-cached, 870 MB read during the load**: the resource
  heap the game loads files into so their GPU parts are used in place. That is the double backing (textures are there
  and in their images) and the asset loader's to replace (layer 3); until then it stays in host memory.
- Next: name the gameplay read pools (what each holds: report buffers, a constant ring, GPU results the CPU reads),
  then layer 0 with the placement rule above, the census's list as the host-cached set.

### Shader objects (`RX_VK_SHADER_OBJECTS=1`, 2026-11-10; details `work/assets/native/shader-objects-impl.md`)

Chiseled Kokiri boot, 80 s, walking 12 s from 40 s (`HOS_INPUT_SCRIPT=40:LUP/12000`), one build (`work/sa/so`) for both paths,
caches restored before each run (`work/proto/shader-objects/ab.sh`, `summary.py`). "Driver cold": empty
`MESA_SHADER_CACHE_DIR` and no `mkw_vk_pipelines.bin`; the program cache (SPIR-V) is warm in every row. Waits and draws left out
are totals at the end of the first gameplay window (present 720).

| Run | Playable | Waits (time) | Draws left out | Compile (driver, workers) | fps standing / walking |
|---|---|---|---|---|---|
| pipelines, all warm | 18 s | 400 (10 ms) | 402 | 607 pipelines, 15 ms | 48.7 / 46.2 |
| **shader objects, binaries warm** | **18 s** | **0** | **0** | 0 compiled; 812 from binaries, 406 programs prefetched (0.26 s on the prefetch thread) | 48.6 / 46.3 |
| pipelines, driver cold | 41 s | 457 (18.5 s) | 903 | 634 pipelines, 39.5 s | 45.7 / 45.8 |
| shader objects, no binaries, driver cold | 36 s | 301 (13.3 s) | 839 | 664 stages, 16.3 s (+ 92 compute pipelines, 18 s) | 45.6 / 45.7 |
| shader objects, binaries warm, driver cold | 23 s | 0 | 217 (compute pipelines, not shader objects yet) | 0 stages; compute 17 s | 47.6 / 45.2 |

- No fps cost on RADV: unlinked stages with dynamic vertex input, blend, samples draw as fast as the pipelines (within run
  noise, +-1 fps). Screenshots match the pipeline path to the run-to-run noise (mean |diff| 1.8-2.2 standing, the same as two
  pipeline runs). No GPU hang in 9 runs (native mesh-only programs as shader objects included; task programs stay on the
  RDNA2 compute fallback).
- The binary cache (`hookshot_shader_objects.bin`, 5 MB for this route: 639 binaries, ~8 KB each, plus 379 program notes)
  makes a driver-cold start 41 -> 23 s; what remains cold there is compute: dispatches and the mesh fallback's dispatches
  are still compute pipelines (next: compute shader objects through the same cache).
- Cold, a stage compiles in ~25 ms on a worker (vs ~62 ms a pipeline): 13.3 s of waits instead of 18.5 s.

### Compute shader objects (2026-11-11; details `work/assets/native/compute-so-impl.md`)

Under `RX_VK_SHADER_OBJECTS=1`, dispatches and the mesh fallback's task/mesh dispatches bind compute VkShaderEXT from the same
binary cache (`RX_VK_SO_COMPUTE=0`, `RX_VK_SO_FALLBACK=0`: their pipelines); every compute variant noted is made from its
binaries when the prefetch thread starts (92 in 53 ms). Build `work/sa/so2`, `work/proto/compute-so/ab2.sh`, runs with
`HOS_GUEST_RT_ALLOC=1`. Kokiri `chisel_kokiri` 60 s; Hyrule Field `chisel_hf_market` 45 s.

| Run | Kokiri playable | HF playable | Compiled at play |
|---|---|---|---|
| pipelines, all warm | 18 s | 17 s | 0 |
| pipelines, driver cold | 41 s | 49 s | 634 / 788 pipelines |
| SO graphics only, binaries warm, driver cold (the 23 s row above, rerun) | 21 s | 24 s | 92 / 94 compute pipelines |
| **SO + compute, binaries warm, driver cold** | **17-18 s** | **20-21 s** | **0** |
| SO + compute, all warm | 16 s | 17 s | 0 |
| SO + compute, no binaries, driver cold | 35 s | 45 s | 822 / 1137 stages |

- The fallback's GPU hang with shader objects (10 of 10 runs, every mesh stage, one program alone too) was a RADV bug, not the
  optimizer: a compute shader object's bind does not mark the push constants dirty (a pipeline's does; 26.2.4 and main), so the
  mesh stage bound after the task stage's dispatch ran without them. `fallback_record` pushes them again after each bind: no hang
  in 22 runs since. Optimized fallback pipelines (`RX_VK_FALLBACK_OPT=1`) did not hang either on this GPU.
- fps: Kokiri within noise (~1 fps); **Hyrule Field ~3 fps lower than pipelines** (51.0 / 52.8 vs 48.3 / 49.1 standing / walking),
  nearly all from the graphics stages (graphics-only shader objects 48.8 / 50.5). Pictures match (standing mean |diff| 1.5-2.3).
- A route's binaries need more than one run: HF's variants drawn depend on the load's timing (69 stages missed by one warm-up).
- Not yet the default: next a hybrid (shader objects until each pipeline is built behind them), or linked VS+FS, then the
  default on RADV. Whole-game binaries offline: planned (needs a re-prepare after the stamp change, ~1.2 GB, an indexed cache).

### Exact capture ranges (2026-11-10)

The submit-time capture took whole 16 MB import chunks and the soft-watch collection merged GPU ranges < 16 MB apart (a
GetWriteWatch rule), so the game's heap between pools was captured and mirror-synced. Now a capture takes only the
pools' pages of a mirrored import (`cap_ranges_clipped`, vk_memory.cpp; an import with no listed pool stays whole), and
the soft watch's collection merges ranges only where they touch (`hos_gpu_guest_ranges`, nvdrv.c; the gap doubles while
more than 2048 are left; the GetWriteWatch build keeps 16 MB). Old behaviour: `RX_VK_CAPTURE_CHUNKS=1
HOS_GPU_RANGE_GAP_MB=16`. One build (`work/sa/ranges`), Kokiri, 80 s, `40:LUP/12000`, standing presents 840-1800, walking
1920-2400 (`work/proto/capture-ranges/ab.sh`, `report.py`), two runs each:

| | Captured a frame | Mirror syncs a frame | Capture thread CPU | fps standing / walking | `HOS_NO_PACING=1` gameplay |
|---|---|---|---|---|---|
| whole chunks | 487 pages/submit, **11.4 MB** | 10.6 MB | 2.1-2.3 ms | 48.6 / 45.7, 47.5 / 46.0 | 48 standing, 45.5 walking |
| **exact ranges** | 180 pages/submit, **4.2 MB** | **5.5 MB** | 0.85 ms | 49.8 / 47.8, 48.2 / 46.3 | 49 standing, 47.5 walking |

- Captured bytes -63%, mirror syncs -48%; ~+1 fps. The replay thread (Presentation Thread, ~8 ms a frame) is unchanged:
  the frame is still held by the readback wait, which no-pacing shows (48 -> 49 only).
- Picture: correct; screenshots differ between the modes only by timing (Link's pose, the walk's position at a present),
  same-mode pairs 1.4-2.4 standing. No GPU hang in 8 runs.

### The occlusion grid's readback without its wait (2026-11-10)

`readback_stamped` (host/nvn_native.c): none of the four invalidates of DepthBufferEx's two 0x3980 slots
(`f_80365f00`, tail call lr 0x80099d84) waits (`HOS_NVN_READBACK_WAIT_ALL=1`: all wait). rbshim, 3054 frames: the
invalidate 7.85 ms mean -> 0.00; the current slot holds frame-3 then and the game takes the other slot, stamped frame-2,
in 3050 of 3054 frames (4 frame-1); before, the wait brought frame-1 in 3130 of 3131. Picture the same (new vs old as two
runs of one mode); no culling pop found on a camera turn (`40:RRIGHT/4000,46:RLEFT/4000`, a shot every 6 presents).
**fps unchanged** (standing 50, walking 48, with and without `HOS_NO_PACING=1`): the same ~8 ms wait moves to the next
readback of the frame, `f_80362ff0` (lr **0x803633cc**), invalidating 0x9b040 bytes of one of a double-buffered pair
(0x117cf04000 / 0x117cf9f040, ~7.8 ms each frame for the newest submission; its sibling pair 0x117d9ec000 / 0x117da87040
does not wait). It read 0.1 KB a frame in the census. Next: what it reads and whether it tolerates a frame's age; the
exposure meter (0x11371e7f00) keeps waiting. `RX_VK_EXPOSURE_LOG=2` logged only 3-4 frames on this build (the final
pass's program not matched), so the exposure was checked by the screenshots only.

### Exact write-back ranges: the frame's last readback wait (2026-11-11; work/assets/native/readback2.md)

After the grid, the frame's ~8 ms wait was `f_80362ff0`'s (lr 0x803633cc): the CPU reads and zeroes the counter
headers of six double-buffered GPU culling lists (0x9b040 / 0x44b040 / 0x8b040 bytes a slot), the slot frame N-2
filled, which the game had waited for (`nvnSyncWait` lr 0x8042cde0). It waited anyway because the write-back table
kept one value per page and the second slot starts 0x40 into the first one's last page: the newest submission, writing
the other slot, stamped the shared page. `WbPages` (gpu/vk/vk_memory.cpp) now keeps two values a page with the bytes
each covers (`RX_VK_WB_PAGES=1`: whole pages). Every invalidate of the frame then waits ~0 (7.91 -> 0.07 ms a frame);
the grid still waits when its exemption is off (`HOS_NVN_READBACK_WAIT_ALL=1`: 7.8 ms), so real dependencies stay.
Kokiri, `work/sa/rb2`, A/B with the same binary: **standing 48.8 -> 56.3 fps, walking 46.7 -> 54.6** (`HOS_NO_PACING=1`:
48.1 -> 55.3, 44.4 -> 54.8; camera turn 46.4 -> 54.7, 44.2 -> 50.2). Exposure block float steady (no frame-to-frame
step > 1% in 2,300 frames, either mode), no flash or pop in ~1,000 shots, no GPU hang in 15 runs. Next limit: the
main thread's submits block 5.3 ms a frame (the replay / GPU is behind). The rule kept: an invalidate waits for the
write-back of exactly the bytes it reads; every readback but the grid is already covered by the game's own sync
waits. `RX_VK_EXPOSURE_LOG` logs only the title (it keys on a draw program; gameplay's final passes are compute).

### Transient targets (2026-11-11; work/assets/native/transient-targets-impl.md)

Four changes, measured separately with one binary (`work/sa/tt`; runs `tt_*`, scripts `work/proto/transient/`):
- **a, per-texture invalidation** (`RX_VK_TEX_SERIAL=0`: the global `destroy_gen` as before): the handle cache and the
  draws' descriptor reuse check the textures they resolved by id (`texture_alive`). A miss is kept only until a frame
  defers uploads (`texture_miss_generation`, where `texture_frame` counts them), so a deferred ASTC texture is looked up
  again by the next frame (the trap above).
- **b, take-over** (`RX_VK_TARGET_TAKEOVER=0`: off): a new render target made over the storage of one of the same create
  info that this frame and recording have not used takes its image at once (not in a parallel replay).
- **c, lazy creation clear** (`RX_VK_LAZY_CLEAR=1`, off): a new single-level target is cleared by its first pass's load
  op when the pass covers it, else before its first other use. 750-1,600 clears a run moved, 0-3 made late; no gain.
- **d, GPU unswizzle for BC and textures under 8 MB** (`RX_VK_CPU_UNSWIZZLE=0`, off): no faster on this PC. Kokiri's
  load: CPU 113-161 ms for 210-293 MB; GPU from staging 127-188 ms for 260 MB (copying into the BAR's VRAM at the PCIe
  rate costs what the CPU's unswizzle does), from the pools 199-467 ms (imports). Launch 16-18 s either way. Offline
  conversion (block-linear.md step 3) is the lever.

Turning (`40:RRIGHT/4000,46:RLEFT/4000,52:LUP/6000`, 80 s), defaults vs `RX_VK_TEX_SERIAL=0 RX_VK_TARGET_TAKEOVER=0`:
**47.4 -> 53.7 fps** (standing 52.1 -> 57.0), renderer 21.3 -> 18.8 ms a frame (draw 5.9 -> 4.8, descriptors 2.1 -> 1.7),
render-target images made new 0.34 -> 0.14 a frame (taken over 0.79), dead targets let go 1,880 (4.7 GB) -> 262 (1.2 GB),
textures retired 2,150 -> 538. Walking straight (`40:LUP/12000`): 55 fps in every mode (old 55.4, all 55.0; a, b, c, d
alone 52.8-54.4), new images 0.10 -> 0.05 a frame. The textures' VRAM peak is about the same (VMA keeps freed blocks).
Pictures unchanged (shot diffs at the noise of two old runs), no GPU hang in ~30 runs.

### Stable render-target slots (2026-11-11; work/assets/native/rt-slots-impl.md, design rt-alloc.md)

agl's DynamicTextureAllocator (`f_8005ed00` alloc, `f_802737a0` free) places ~66 transient targets a frame in 7
per-worker arenas over the same 512 MB, so they move every frame and lanes alias. `host/rt_slots.c` keeps the arenas'
bookkeeping but rewrites the offset each texture uses (block +0x38, spliced after its two stores with
`SPLICE_AFTER_EACH`, now implemented in nx2/entry_hooks.py) to a slot of its family (caller lr, name, format, type) in the
top 256 MB, which the arenas are shrunk away from at first use. On by default; `HOS_GUEST_RT_ALLOC=1` the game's
allocator, `RX_RT_SLOTS_MB` the range, `HOS_RT_SLOTS_LOG=1` slots by name; one `rt slots:` line every 600 presents.
- Checked in every run: each transient allocation reaches the splice and each block comes back through `f_802737a0`
  (0 unmatched); no slot busy at a present; 65-74 slots, 130-177 MB, no back-off, no eviction.
- Kokiri A/B (two runs each, turn / walk): distinct (shape, address) pairs **460 -> 147 / 188 -> 71**; textures let go
  1.37 -> 0.22 / 0.30 -> 0.05 a frame, take-overs 0.87 -> 0 / 0.22 -> 0; fps 52.7 -> 53.8 / 55.6 -> 55.2 (noise);
  renderer 19.0 -> 18.6 / 18.0 -> 18.1 ms. Hyrule Field (`chisel_hf_market`): texture VRAM 4.55 -> 3.65 GB, 48.4 /
  47.1 fps (one run each). Pictures at the noise of two runs.
- nvnTextureInitialize stays ~43 a frame: the game's registration compare would skip it, but the free clears the entry's
  registration. The variable shapes (local shadows sized by distance, mip chains) still make new images. No renderer
  change was needed (no `nvn_rt_slot` API).
- `chisel_kokiri_lw` with `16:RRIGHT/4000,22:LUP/8000` hangs the GPU 6 of 6 runs, with and without slots (3 each).

### Layer 0: window buffers (2026-11-11; work/assets/native/layer0-impl.md, PoC work/proto/layer0/)

Steps 7a and 7b are written, behind switches, **not yet run in the game** (test plan `work/proto/layer0/plan.sh`).
- **`RX_VK_LAYER0=1` (7a, host class):** guest memory reaches the GPU through 4 GB sparse aliased window buffers, one
  every 2 GB of guest space, so a binding of up to 2 GB is `(window[g >> 31], g & 0x7fffffff)`
  (gpu/vk/vk_pools.cpp, arithmetic in layer0_windows.h). Memory is bound in chunks (64 MB, `RX_VK_LAYER0_CHUNK_MB`) that
  are udmabufs of the memfd's own pages, imported once, never merged or moved. No imports by pool, mirrors, mirror syncs or
  write-backs; GPU writes land in guest memory and a readback waits for the submission that wrote it. The 4 KB page table
  of the pointer-following shaders gets the window addresses (no shader or cache change). Captures stay on (the replay's
  CPU reads as submitted, texture storage); `RX_VK_CAPTURE=0` drops them once step 6 lands. `RX_VK_LAYER0_EAGER=1` binds
  a pool's chunks when the game makes it; `RX_VK_LAYER0_SPAN_MB` (64) a first use binds a small pool whole.
- **`RX_VK_LAYER0=2` (7b, + VRAM class):** pools with `CPU_NO_ACCESS` (render-target and GPU-only pools) get their whole
  64 KB granules in device-local memory, one contiguous slot run in guest order; partial edges and granules a CPU-visible
  pool shares stay host. A finalized one is kept `RX_VK_LAYER0_KEEP_S` (10) seconds and handed back on an Initialize of
  the same base, size and flags (the map change's render-target pools). `RX_VK_LAYER0_HOST_KEYS` keeps creation keys
  host, `RX_VK_LAYER0_VRAM_MB` caps the class (2048). Needs **MKWVK_API_VERSION 14**: `nvn_pool_native` carries each pool's
  flags and creation key (a hash of the caller's return address and the guest frame-pointer chain, host/nvn_native.c).
  The replay's CPU reads of VRAM granules are logged (`CPU reads of VRAM`).
- PoC on this GPU (`l0_poc.out`): GPU reads of host chunks over PCIe **28.6 GB/s** streaming, **10.3 GB/s** scattered
  64-byte reads, against VRAM's 560 / 444 (BAR the same as VRAM; VRAM in a window with contiguous 64 KB pages 561 / 424).
  A chunk's bind into both windows 0.04 ms; **scattered 64 KB binds 127 us each** and permuted pages read 5x slower
  (hence slots in guest order). A 16 MB udmabuf import 0.52 ms on touched pages, 4.5 ms on fresh ones. Buffer objects cost
  **~0.7 us each per submit, whatever their size** (hence 64 MB chunks). GPU atomics on host memory work.
- Next: 7c (write-only pools in BAR by creation-key policy, needs step 6), 7d (virtual pools as sparse binds in runs).

## Fronts surveyed (2026-11-10; reports in work/assets/native/*.md, tools and data in work/proto/<front>/)

Kokiri, `work/sa/zstd` build, ~47 fps, 6 submits a frame. One line each; the reports have addresses and evidence.

- **Shader upload** (`shader-stream.md`): the game never copies shader code (246 pools over the decompressed
  archives); no program is created during play; every stage is in the romfs seed. With all SPIR-V prepared the first
  gameplay still waited **15.9 s** on 515 pipelines (~41 ms each, driver compile). RADV shader objects from a driver
  binary: **0.017 ms** a stage. Almost all state is dynamic on this device.
- **Asset loading** (`asset-loading.md`): the 2.4 GB pool at 0x109c190000 is `agl::TextureDataMgr`'s, textures only;
  zstd output (block-linear) is memcpy'd in by `f_80798690`, moved by a compaction thread (899 MB), then imported,
  mirrored, ASTC-decoded and CPU-unswizzled by us: texels copied 3-4x, stored 2-3x. Models and shaders are used in place.
- **Block linear** (`block-linear.md`): no game CPU code swizzles; gameplay writes 0.23 MB of texels in 80 s; Read/
  WriteTexels at load only. The layout can go: assets offline, texel calls as Vulkan copies, transient targets as
  aliased images. Today BC textures are unswizzled on the CPU (152 ms on one load frame).
- **Command lists** (`command-lists.md`): ~132 per-job lists a frame plus ~37k precompiled lists (775 CallCommands a
  frame). Recording costs ~0.5 ms; **our replay at submit 5.2 ms** on the critical path. Byte-identical repeats are only
  6% frame to frame (UBO ring addresses and texture handles are baked in); structure repeats 83-95%. So cache by
  structure only after bound data becomes indirection (layer 0); precompiled lists translate once per handle.
- **Shader maps** (`shader-maps.md`): not per-frame buffers (0 vkAllocateMemory/CreateBuffer a frame). It is ~42
  transient render targets re-initialized every frame in agl's 512 MB virtual pool (`f_8005ed00`, 45 callers) whose
  offsets shift with the passes: walking remakes 2.8 targets a frame (1.4 new VkImages), VRAM grows 3.0 -> 4.4 GB, and
  each destroy bumps the global `destroy_gen`. Dynamic resolution holds only thanks to the faked timestamps.
- **Memory churn** (`memory-churn.md`): readback waits ~8 ms a frame of blocked threads, captures 11.6 MB / 1.4 ms,
  mirror syncs 10.5 MB / 0.93 ms GPU, write-back diffs 66 MB / 0.43 ms, marks ~1.5 ms, ~1.8 GB duplicate memory.
  Layer 0 prototype (`layer0_proto.c`, 64 KB sparse pages, 2 GB-stepped 4 GB windows, 1024-entry address table,
  udmabuf host class + BAR dma-buf VRAM class) passes all checks.

### Order of work (revised after review, 2026-11-10)

The second survey round (`alloc-transfer.md`, `nvn-callpaths.md`, `cpu-to-gpu.md`) found the frame's limiter: one
readback, `f_800998a0` -> `f_80365f00` -> `nvnMemoryPoolInvalidateMappedRange` at lr 0x803665d4, waits 7.4-8.8 ms every
frame on a job worker on the critical path. The game reads the previous frame's slot of a double-buffered pair, but
`memory_writeback_value` (vk_memory.cpp) waits for the newest submission that wrote any 4 KB page of the range, i.e.
this frame's. `RX_VK_NO_READBACK_WAIT=1`: standing 47 -> 60 fps (vsync cap), walking 38-41 -> 46-55. What the data
is and feeds is being traced (`readback.md`); the exposure meter's read must stay strict (white-flash race).

1. *Done (73a798c, 0d50991: 48.8 -> 56.3 fps standing, 46.7 -> 54.6 walking).* **Readback wait per written range** (exposure meter strict). A/B with `HOS_NO_PACING=1`, walking, exposure checked.
2. *Done (9aac6fe, compute a41a58c..419da13; on by default since 2026-11-11, `RX_VK_SHADER_OBJECTS=0` for pipelines).* **Shader objects + binary cache** (`RX_VK_SHADER_OBJECTS=1`): removes the 15.9 s / 4.7 s pipeline waits; with compute, a driver-cold start is as fast as a warm one (Kokiri 17-18 s, HF 20-21 s). Left: the HF fps cost (hybrid with pipelines), whole-game binaries offline.
3. *Done for the ranges (68fde05: captures 11.4 -> 4.2 MB a frame); mirrors of invalidated pools remain.* **Exact capture ranges** (69% of captured pages are outside pools) and no mirror for pools the game invalidates.
4. *Done for a and b (on by default: turning 47.4 -> 53.7 fps); c and d behind switches, off (no gain measured).*
   **Transient targets**: per-texture invalidation instead of `destroy_gen`; a moved target takes over the image it
   drew over; skip creation clears the game overwrites. GPU unswizzle for every format and size. Trap:
   `texture_view_of` (vk_draw.cpp) caches misses too (a deferred ASTC upload, `upload_defer`, samples the dummy) and
   retries them only when the slot version or `destroy_gen` moves; today `destroy_gen` moves every frame, so with
   per-texture invalidation a deferred texture could stay the dummy. Do not cache misses, or retry after the upload.
   *e, done (on by default, `HOS_GUEST_RT_ALLOC=1` off):* **stable slots** for the game's transient targets
   (host/rt_slots.c): pairs a third, take-overs gone, same fps. Left: the ~43 initializations a frame (keep entry,
   descriptor slot and slot together) and the variable shapes.
5. **Second-map validation**: rerun the censuses (pool, alloc-transfer shim, readback) on Hyrule Field and across a
   map change; root-cause the walking gfx-ring hang (`work/runs/rblog`, ~67 s, workarounds on). Census v3: pool
   creation sites, and whether the 3,504 pools of <= 64 KB are the per-draw constant rings (if so, placement per
   64 KB granule rather than per pool, or they never reach VRAM).
6. *Done behind `RX_VK_TIMELINES=1` (off by default; work/assets/native/timelines-impl.md): fps the same, present drains and the gate's 1 ms wake gone; the game never waits on unrecorded work.* **Timelines and events** in the current replay. Hard prerequisite of layer 0 (captures can only go once syncs are
   honest) and the riskiest step. Caveat: wait-before-signal on ONE Vulkan queue deadlocks (a submission blocked on a
   timeline wait holds everything behind it, including the signal), and the five NVN queues map to one graphics
   queue. Keep a host-side hold for waits whose signal is not yet submitted; count them first.
7. *Written for a and b, not run yet (`RX_VK_LAYER0=1`, `=2`; section "Layer 0: window buffers").* **Layer 0**: windows
   with the host class only is a correctness milestone, expected slower than mirrors (GPU reads
   over PCIe) -> VRAM class for CPU-no-access pools -> write-only pools -> virtual pools as sparse binds. Render-target
   pool as one device-local heap with images cached by (offset, create info).
8. **Layer 1**: lists by reference with per-thread arenas (3.3 ms of copies under one lock today); record-time packet
   translation with state deduplication (5,700 of 15,100 ops repeat bound state); descriptor buffer indexed by NVN
   handle; precise barriers; real queries + conditional rendering; then structure-keyed command caching.
9. **Layer 3**: buys memory (-2.35 GB RAM + mirror) and first-frame upload stutter (590 MB, a 155 ms frame), not launch
   time: the load's bottleneck is not diagnosed (idle 48% of samples, textures < 0.5 s). Profile what the loaders
   wait on before committing. Needs an offline ASTC -> BC7 encoder.

Expectations: the game is ~33 of 41 ms of CPU a frame standing, the GPU ~7 ms; layers 0+1 remove ~8 ms of renderer
work, not all on the critical path. After the readback, the larger CPU lever is the lifted floating-point helpers
(`a64_fpx_*`, 5.5-10 ms a frame): a lift option emitting host FP for hot functions (full rebuild), then host-native
hot kernels (`f_812342c0`, `f_808226e0`, Vfx3 `f_8125f0e0`, Havok cloth). More cores do not help until the readback
and the Presentation-Thread replay are fixed (`HOS_CORES=8` was slower).

## Status

- 2026-11-10: dump extracted, lifted (295k functions), first Linux build. Pool census written.
- 2026-11-11: shader objects on by default (the user: pipelines bring back the stutter at each new pipeline; the ~3 fps they cost in Hyrule Field is GPU work, for later). `RX_VK_SHADER_OBJECTS=0`: pipelines.
- 2026-11-10: shader objects (`RX_VK_SHADER_OBJECTS=1`, off by default then; gpu/vk/vk_shader_object.cpp): graphics and native
  mesh programs as VkShaderEXT with all state dynamic, driver binaries kept in `hookshot_shader_objects.bin`, a prefetch thread
  fed by `nvnProgramSetShaders` (MKWVK_API_VERSION 13). Warm: 0 pipeline waits and 0 draws left out (pipelines: 400 waits,
  402 left out), same fps; driver-cold start 41 -> 23 s with binaries kept. Left: compute programs as shader objects, the
  binaries made offline from the seed (step 3 of shader-stream.md 2.4).
- 2026-11-11: compute programs and the mesh fallback's dispatches as shader objects too (a41a58c, 578f409): driver-cold start
  with binaries kept 23 -> 17-18 s (Kokiri), 24 -> 20-21 s (HF), 0 compiles. The fallback's hang was a RADV push-constant bug
  (worked around). HF fps ~3 lower with shader objects (graphics stages), so still off by default.
- 2026-11-10: exact capture ranges (step 3, first half): captures take the pools' pages of a mirrored import, the soft
  watch's collection no longer merges 16 MB gaps. Kokiri: 11.4 -> 4.2 MB captured and 10.6 -> 5.5 MB mirror-synced a frame,
  ~+1 fps (`RX_VK_CAPTURE_CHUNKS=1 HOS_GPU_RANGE_GAP_MB=16`: as before).
- 2026-11-10: the occlusion grid's four invalidates no longer wait (step 1, part): its 8 ms is gone, but the next readback
  (`f_80362ff0`, lr 0x803633cc, 0x9b040 bytes) now waits the same, so no fps yet.
- 2026-11-11: write-back values by byte range (two a page) instead of by page: the false dependency of `f_80362ff0`'s
  back-to-back slots is gone, no invalidate of the frame waits (7.91 -> 0.07 ms). Kokiri 48.8 -> 56.3 fps standing,
  46.7 -> 54.6 walking (`RX_VK_WB_PAGES=1`: as before). Native replacements of the readback systems (two-phase GPU
  occlusion, real queries, exposure on the GPU, indirect counts): work/assets/native/readback2.md.
- 2026-11-11: transient targets (step 4): per-texture invalidation and render targets taking over the image they are made
  over, on by default; turning the camera in Kokiri 47.4 -> 53.7 fps, new target images 0.34 -> 0.14 a frame, dead targets
  4.7 -> 1.2 GB. The lazy creation clear (`RX_VK_LAZY_CLEAR=1`) and the GPU unswizzle of BC and small textures
  (`RX_VK_CPU_UNSWIZZLE=0`) are kept behind switches: no gain on this PC (work/assets/native/transient-targets-impl.md).
- 2026-11-11: render-target parking and shadow rounding (c22846a, host/rt_slots.c, on by default): the game's free
  (`f_802737a0`) only returns the arena block, so the free hook keeps the whole entry (block, registration, descriptor
  slot, texture object) and the next alloc with the same caller and 0x38-byte descriptor gets it back without the alloc
  running; entries unused 6 presents go through the game's own free (DRS, map changes). `local_shadow_depth` (384-2432)
  is rounded up to a multiple of 128 (the pass takes viewport/scissor from the texture's size). Slot range 320 MB.
  Allocator inits 41-45 -> 2-5 a frame; new target images turning 0.17 -> 0.02 a frame; retired 500 -> 140; fps and
  renderer time unchanged; pictures match. One deadlock in ~40 runs (MainThread holding a DTA mutex while a parked entry
  was freed from the alloc hook), fixed in c22846a (real frees only on the game's free path, never under the DTA mutex;
  blocks with an on-free callback not parked), 18 clean runs since. `HOS_RT_PARK=0`, `HOS_RT_QUANT=0` turn them off.
- 2026-11-11: stable render-target slots (step 4e, host/rt_slots.c with SPLICE_AFTER in nx2/entry_hooks.py): the game's
  transient targets keep their address frame to frame; Kokiri's distinct (shape, address) pairs 460 -> 147 turning,
  textures let go 1.37 -> 0.22 a frame, Hyrule Field texture VRAM 4.55 -> 3.65 GB; fps the same
  (work/assets/native/rt-slots-impl.md). The Lost Woods exit start with a turn and a walk hangs the GPU every run, with
  or without slots.
- 2026-11-11: NVN syncs as timelines (step 6, `RX_VK_TIMELINES=1`, off by default): a timeline semaphore per NVN queue,
  nvnSyncWait on it, nvnQueueWaitSync as a host-held `NOP_WAIT_FENCE`, never a GPU-side wait (one queue deadlocks on a wait
  before its signal: measured, work/proto/timelines/onequeue.c). Census (`RX_VK_SYNC_CENSUS=1`): per frame 25 event waits,
  7 held on the host, 0 forced; the one QueueWaitSync is on the window's signalled sync; no CPU wait on unrecorded work.
  Warm A/B: Kokiri 53.6/53.2 vs 53.6-53.8 fps, HF 46.1/46.4 vs 45.6/46.4; present drains 43-71 a run -> 0 in steady state;
  pictures the same; the Lost Woods hang reproducer did not hang (0/4, either mode, on the new base).
- 2026-11-11: layer 0 steps 7a and 7b written behind `RX_VK_LAYER0=1` / `=2` (gpu/vk/vk_pools.cpp, MKWVK_API_VERSION 14 for
  the pools' flags and creation keys): window buffers over udmabuf chunks instead of imports, mirrors and write-backs, and
  CPU_NO_ACCESS pools in VRAM with a hand-back across map changes. Measured standalone only (work/proto/layer0/l0_poc.out);
  the game runs are in work/proto/layer0/plan.sh.
