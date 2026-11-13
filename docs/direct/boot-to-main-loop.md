# From the process entry to the main loop (M6j, 2026-11-13)

The path from the host's `main` to the steady frame loop, written down once so that profiling, patches and frame-time
work do not re-derive it. Addresses are guest addresses of HOS_GAME_VERSION 100 (build id e0f3ccfa..., image at
`0x80000000`, `main` module at `0x80020000`); `f_<address>` is the lifted function, and the labels of
`nx2/oot_labels.txt` are given where one exists.

How each claim is known:

- **VERIFIED (decompile)**: read in the Ghidra output (`work/ghidra/decomp.sh`, outputs in `work/proto/boot/d1`..`d5`)
  or in the lifted C (`work/sa/gen`, call lists by `work/proto/boot/tools/calls.py`).
- **VERIFIED (run)**: seen in a run. Two short gdb runs on the chiseled Kokiri boot (`chisel_kokiri`, default
  settings, `RUN=check`, a copy of the `work/sa/build` build of 2026-11-11, the old Vulkan renderer: the guest code is the same lift): `m6j_tree1` dumps sead's method trees
  at presents 60, 300 and 1200 and every sead thread's start (`work/proto/boot/gdb/tree.py`, output
  `work/proto/boot/tree1.txt`); `m6j_order1` logs the entry of 36 frame functions on every thread for presents
  1500-1503 (`gdb/order.py`, `order1.txt`; `tools/order.py` groups it by the main thread's phases). gdb slows the
  game, so their times give the order, not durations.
- **VERIFIED (log)**: a line of an existing run's log (`work/runs/boot_iold3`, `HOS_LOG_TIME=1`; `work/runs/boot_t1`,
  `HOS_FS_TRACE=1`).
- **VERIFIED (trace)**: M6a's `HOS_DBG=crit` trace `work/runs/m6a_crit2_k` (Kokiri, direct back end, pacing off),
  re-read here with `scripts/crit_path.py` and `work/proto/boot/tools/blocks.py` (each thread's blocking sites).
- **INFERRED**: a reading of the code or of names that was not checked further.

Everything under `work/proto/boot` is local (decompiler output, game-derived data); nothing of it is in the commit.

## Summary

- **Host**: `main` (Linux) keeps the SDL window on the process's thread and starts the game on a 64 MB thread; that
  thread maps the 512 GB guest window at `0x100000000000`, reads `image.bin` to `0x80000000`, makes itself the guest
  main thread (1 MB stack, priority 44, core 0) and calls, in nn::init's order, `_init_libc0`, `nnosInitialize`,
  `_init_libc1`, `nninitStartup`, `_init_libc2`, the three modules' `DT_INIT` and `nnMain`. Services are the host's
  HLE; nninit's SDK service clients are never opened.
- **nnMain** (`f_81222cd0`) builds the sead world: the heap manager on the arena `nninitStartup` left (6.9 GB here),
  sead::MainThread (the nn main thread, priority 16), the file devices (romfs mounted at 0.23 s), the HeapDef table,
  the region, rom-config and address tables; then the framework object **sead::GameFrameworkOunce** (vtable
  `0x84c83f80`), its graphics initialisation (NVN device, queue, the 3-texture window, the **Presentation Thread**),
  and calls the framework's `run` (vtable +0x20, `f_80f48c30`), which never returns.
- **run** makes the MethodTreeMgr and the TaskMgr with the root task **ModuleSystem**, then a startup loop
  (`f_80966800`) polls the TaskMgr every 10 ms. The ModuleSystem's prepare (`f_83b797e0`) runs inside that loop on
  sead::MainThread: it creates the engine modules, their threads (the worker pools last, at ~4.4 s), and loads
  `Pack/Bootup`; this is boot-speed.md's single-threaded 0.6-4.4 s. The first frame starts at 4.42 s.
- **The title**: the sequence is data: `Sequence/SequenceParam/FormalScene` names `Sequence/FormalSequence.root.ainb`,
  loaded at 4.51 s in the first frames; its modules (BeforeTitle.110, NormalTitle, CloseTitle, Opening, LoadBanc, ...)
  run as frames go by (boot-speed.md has the timeline).
- **The main loop** is `f_809667b0`: `for (;;) framework->vtable[0xe0]()`. A frame on sead::MainThread is, in order:
  1. `f_83b6ea90`: display refresh rate and VRR, the frame-rate controller, the game step (60 / interval), nn::oe
     messages; then `f_8348c680`, the frame body:
  2. `f_802eaf10`: TaskMgr's pre-pass (task destruction);
  3. **calc** (`f_802eabd0`): TaskMgr creation, then the calc method tree: ModuleSystem → gsys PreCalc →
     **ModuleSystem::moduleCalc_ (`f_80706be0`)** → gsys PostCalc. moduleCalc_ runs three stages of the module job
     graph on the main thread with the five ModuleSystemWorkers and joins them: stage A the game logic (BaseProcExecutor
     0-5, NavMesh workers), stage B (short, content not identified), stage C the calc jobs (`f_800998a0`), the 102
     render jobs (`f_80084de0`), a second BaseProc step (`f_83acfb90`) and the agl::lyr layer lists (`f_8006cd60`,
     `f_8007ad40`, the 3D pipeline `f_80660320`);
  4. `f_8348d430`: reset requests (none in play);
  5. `f_81207a50`: wait for the Presentation Thread to finish the previous present, `nvnWindowAcquireTexture`;
  6. **draw** (`f_802eb020`): the gate primary's begin (`f_802eb390`), the draw tree (gsys DrawMainScreen "Draw(TV)":
     queue heads, `CallCommands` of the 9 layer lists, the final composite), its end, and message 1 to the
     Presentation Thread;
  7. `f_8042cd20`: the framework's frame callback starts the frame-end stage (the ModuleSystem graph again, via
     `f_80070d30`), then the wait for the previous frame's fence and for the window (or vsync), then the CPU gate
     `nvnEventSignal(value 1)` as a tail call; the frame-end stage's workers run on into the next frame.
- The Presentation Thread (`f_805902c0`) waits for the window on the GPU, submits the compute lists, the heads and the
  gate primary, fences, polls the gate event in 1 ms sleeps until the main thread's signal, presents and signals the
  framework's present-done event.
- Steady play has ~84 guest threads; the frame's work is on sead::MainThread, ModuleSystemWorker1-5, BaseProcExecutor0-5,
  the two NavMesh workers and the Presentation Thread; all others sleep in their message queues most of the frame.

## 1. The host: from `main` to the guest's first instruction

| Step | Where | What |
|---|---|---|
| process main (Linux) | `host/hos_main.c:1625` | reads `hookshot_env.txt` (`load_hookshot_env`), opens the SDL window on the process's thread (`hos_display_init`, `host/posix/display_sdl.c:184`), starts the Discord status, then `CreateThread(64 MB, game_thread)` named "Game"; the process thread runs the window's events (`hos_display_loop`) |
| `game_main` | `host/hos_main.c:1368` | the occlusion switch, the log file, arguments (`--romfs`, `--user`, `--trace`, `--prepare-shaders`), the default paths beside the executable |
| the guest window | `reserve_guest`, `host/hos_main.c:121` | `HOS_GUEST_SPACE` = 512 GB (`host/hos.h:46`) at `A64_HOST_BASE` = `0x100000000000` (`third_party/a64-kit/rt/a64.h:61`): on Linux a memfd mapped shared (`MAP_NORESERVE`), so the Vulkan back end imports guest pages as dma-bufs; anonymous memory without udmabuf; no transparent huge pages |
| the image | `host/hos_main.c:1492-1511` | `image.bin` read whole to `0x80000000` (96 MB); its size must equal `hos_image_end - hos_image_base` of the lift (`work/sa/gen/hos_image.c`) |
| memory | `hos_mem_init`, `host/hos_mem.c:34` | the system region committed; `hos_total_memory` = 9809 MB (a Switch 2 application's, `HOS_MEMORY_MB` overrides); the heap is placed at `HOS_HEAP_BASE` = `0x1000000000` when the game asks (`SetMemoryHeapSize`, `host/hos_mem.c:190`, log "heap: 6894 MB at 0x1000000000") |
| widescreen, mods | `host/hos_main.c:1513-1581` | `HOS_ASPECT` / the options file, `hos_mods_init` |
| the guest main thread | `hos_boot_main_thread`, `host/hos_thread.c:489` | a ThreadType and a 1 MB stack (`hos_main_stack_size`, `0x100000`, from the lift's `gen/hos_image.c`) in the system region, priority 44, ideal core 0, named "MainThread"; `setup_guest` (`host/hos_thread.c:368`): TLS block, TLR, kernel handle; takes a virtual core (`hos_vcore_back`) before any guest code |
| profiler, display | `host/hos_main.c:1586-1592` | `hos_prof_start` (`HOS_PROFILE`), `hos_display_start` |
| the guest calls | `call_guest_args`, `host/hos_main.c:224` | `a64_lookup(addr)` gives the lifted function; lr = 0, sp = the stack's top; the order is nn::init::Start's (`host/hos_main.c:1593-1611`): `_init_libc0` (`0x85717e50`), `nnosInitialize` (`0x8510f720`, x0 = the thread's handle), `hos_thread_adopt_guest` (`host/hos_thread.c:60`: the SDK's own ThreadType from TLR+0x1F8), `_init_libc1` (`0x85717e60`), `nninitStartup` (`0x8262bb10`), `_init_libc2` (`0x85717e80`), `DT_INIT` of main (`0x812377e0`), subsdk0 (`0x84ec0040`), sdk (`0x84fa0060`), then `nnMain` (`0x81222cd0`). `nninitInitializeSdkModule` (`0x850d5130`) is not run: it only opens the SDK's IPC service clients, which the HLE stands in for |

VERIFIED (log, `boot_iold3`): image 0.074 s, `_init_libc0` 0.122, main `DT_INIT` 0.131-0.216, `nnMain` 0.229.

Other host machinery the guest meets from here on:

- **Threads**: `nn::os::CreateThread` (`host/hos_thread.c:844`) makes a host thread per guest thread
  (`hos_thread_create`, `:549`); its start (`thread_main`, `:511`) runs the SDK's TLS start hook
  (`hos_thread_start_hook` from the lift) before the entry. Names come from `SetThreadName` (`:876`).
- **Virtual cores**: `hos_cores()` = 6 (`:119`); a guest thread holds one of the 6 virtual cores while it runs guest code
  and gives it up around every block; `vc_allowed` (`:1247`) is its core mask; a better waiter kicks the holder;
  equal-priority holders are kicked after `HOS_VCORE_SLICE_US` (2 ms). Details and measurements: cores.md.
  `GetCurrentCoreNumber` answers the held core (`:895`); `GetThreadAvailableCoreMask` 0x3f (`:896`).
- **NVN**: `nvnBootstrapLoader` (`host/nvn_native.c:3921`) hands out the native NVN layer's procs; the back end behind
  it is chosen at the first device call (log "rx: direct mode").
- **Services**: every `nn::*` import the game calls is an HLE function in `host/nn_*.c`; the unimplemented ones log
  "unimplemented import" and return 0.

## 2. The guest: from rtld to the framework

### 2.1 Before nnMain

- **rtld**: not run. The image is linked and relocated at build time (`nx2/image.py`, Eden's layout); rtld's three
  callbacks to nn::init are done by the host (above).
- **`nninitStartup`** (`0x8262bb10`, label `sys_nninit_startup`; VERIFIED decompile): `SetCpuBoostMode(1)`;
  `QueryMemoryInfo`; heap size = (total - used, 2 MB aligned) - `0xaf200000`; `SetMemoryHeapSize`;
  `AllocateMemoryBlock(heap)`; `f_814e6c30(block, 0x2400000)` (INFERRED: the C heap, 36 MB); the sead root arena
  starts at block + 36 MB (stored at `0x84df1058`); `SetMemoryAllocatorForThreadLocal`. The two constants are HeapDef's
  `Malloc` (36864 KB = `0x2400000`) and `VirtualAddressMemory` (2869248 KB = `0xaf200000`, kept outside the heap):
  VERIFIED (the numbers match exactly), the code does not read the file for them.
- **Static constructors**: main's `DT_INIT` runs **9,783** `init_array` entries (all distinct, `0x809e2c90`..
  `0x83be61b0`, 325k instructions in all, median 28; `image.json`), sdk's 96, subsdk0's none. Most register objects:
  5,575 call `f_80ca4a00` (a static intrusive-list head) and 3,203 link a node with `f_80ca4f70`; 2,162 register a
  destructor through `f_80e4d090` (the `__cxa_atexit` stub, returns -1); 1,799 insert into a keyed tree (`f_8068b050`)
  through the singleton `f_80ee1d80` (INFERRED: a type/parameter registry). The largest are Havok trace-metadata
  tables (`f_817c5e40` 3,128 instructions) and the game-parameter table setup (`f_832d4720`). They take 85 ms
  (VERIFIED log). One import is missing there: `nn::ro::ProtectRelro` (lr `0x81237908`, harmless).

### 2.2 nnMain (`f_81222cd0`, label `sys_nn_main`)

VERIFIED (decompile), in order:

| # | Call | What |
|---|---|---|
| 1 | `nn::oe::SetCpuBoostMode(1)` (first boot only) | |
| 2 | `f_80d6e4b0`, `f_80df3b50`, `f_80d83f90` | sead's static tables (INFERRED: math and hash tables; 2 + 14 tiny initialisers) |
| 3 | `f_80ec0e90` | **FPCR.FZ set** (flush denormals to zero) for this thread |
| 4 | `nn::oe::SetFocusHandlingMode(2)`, `f_83be0140` | oe notifications on: resume, operation mode, performance mode, graphics scale |
| 5 | `f_809db6a0(&{0x3000000, &arena})` | **sead framework systems**: `f_809ddf00` the HeapMgr (the root heap on the arena `{0x84df1058, heap - 36 MB}`: 6894 - 36 = 6858 MB on this host) and the heap "sead::ThreadMgr" with `f_80df5690` (the nn main thread becomes **sead::MainThread**: an object of vtable `0x84c18840`, TLS slot, its core number cached, and `ChangeThreadPriority(16)`); heaps "sead::SystemManagers", "sead::ResourceMgr" (`f_80b151f0`), "sead::FileDeviceMgr" (`f_80c49770`: the content mount, log "fs: mounted content" at 0.232 s); `nn::fs::SetAllocator` |
| 6 | `f_809df710(0, "HeapSizeDB", root)` + `f_80f70240(..., "System/HeapSize/")` | the **HeapDef** table read into a 1 MB work heap "HeapSizeDBWorkHeap" (file `System/HeapSize/Product.Nin_Ounce_NVN2.HeapDef.byml.zs`, 0.235 s), then the heap "MountROMCache" of HeapDef's `MountRomCache` (27 MB). `f_809df710` is sead's ExpHeap create (label `sead_exp_heap_create_b`): size 0 takes all the parent's free space, and the heap's virtual +0x28 (adjust) shrinks it to what was used |
| 7 | `f_80fb14b0`, `f_80fb3370`, `f_80fb29c0`, `f_80fb2670` | heap "RegionLanguageMgr": `System/RegionLangMask.txt`, `Local/System/RegionLangConfig.txt`; three singletons |
| 8 | `f_809d9a90` | heap "RomConfigMgrWorkHeap": `System/GameRomConfig/NormalProduct.game__RomConfig.bgyml` (0.239 s); `f_809d9de0` reads "BootupScene", "UseSceneSelector" |
| 9 | `f_83bf9db0` | heap "AddressTable": `System/AddressTable/Product.111.Nin_Ounce_NVN2.atbl.byml.zs` (0.240 s) |
| 10 | the framework's create argument | fields from HeapDef: "NvInitializeGraphics" (0 here; 16 MB if absent: the NV devtools allocator, not made at 0); the 1280 x 720 virtual canvas; the presentation flags |
| 11 | heap "Framework": `alloc(0x338)`, `f_80e57700` | **the framework object** (vtable `0x84c83f80`, named "sead::GameFrameworkOunce" by the window it makes) |
| 12 | heap "GraphicsSystem": `f_8348be50(fw, heap, 0, 0, {1280, 720})` | the framework's graphics initialisation (section 2.4) |
| 13 | heap "AccountMgr": `nn::account::TryOpenPreselectedUser` | |
| 14 | heap "GameSystem": `f_80eefdb0` | a game-system singleton (INFERRED) |
| 15 | `fw->vtable[0x20](fw, root, &runArg, &taskArg)` = `f_80f48c30` | **run**; never returns. The root task's factory in `taskArg` is `0x80f47540` (ModuleSystem) |

**Heaps** (HeapDef, sizes in KB in the file, shown in MB; 83 entries, `work/proto/boot/heapdef.json` has all; the
lookup is `f_805c2630`, label `config_heapdef_read`: `Heaps/<name>/Size`, KB << 10). The large ones:

| Heap | Parent | MB | | Heap | Parent | MB |
|---|---|---|---|---|---|---|
| ゲームグラフィックス (game graphics) | - | 3189 | | Event | - | 226 |
| TextureHeap | game graphics | 2400 | | Movie | Event | 206 |
| VirtualAddressMemory | Reserved | 2802 | | Sound | - | 200 |
| Graphics | - | 841 | | Program | Reserved | 137 |
| DynamicTextureAllocatorMem | Graphics | 305 | | Physics | - | 110 |
| Actor | - | 407 | | UI / GameUI | - | 54 / 37 |
| ActorInstanceHeap | Actor | 325 | | Resource | - | 42 |
| GameTerrain | - | 297 | | Malloc / ReservedByFramework | Reserved | 36 / 36 |
| Foliage / Bake | - | 250 / 250 | | TextureCompactionWorkHeap | game graphics | 32 |

DebugHeap and its children (aresDebug 1366 MB, ResourceDebug) have size 0 at the top: not made in this build
(INFERRED). Two sizes are seen again in the logs: the DTA pool "the game's 305 MB" (`host/nn_applet.c`, log line
"render target pool 512 MB (the game's 305 MB)") and the texture data pool 2432 MB (`agl::TextureDataMgr`, lr
`0x80f6cb20`; TextureHeap 2400 MB plus 32 MB, INFERRED the compaction work heap).

### 2.3 The framework object

`0x84c83f80` (no RTTI). The slots this document uses (VERIFIED decompile; thunks resolved in the lifted C):

| Slot | Function | What |
|---|---|---|
| +0x20 | `f_80f48c30` (label `sead_method_tree_mgr`) | run |
| +0x50 | `f_80df3c20` | run's pre-step (empty) |
| +0x58 | `f_8348d840` | run's post-step (empty) |
| +0x60 | `f_80966760` | runImpl: +0xc0, frame start tick, tail +0xd8 |
| +0x68 | `f_80f47b80` | creates the MethodTreeMgr (0x418 bytes, heap "sead::MethodTreeMgr") |
| +0x70 | `f_8348d430` | procReset: the reset delegates and the TaskMgr's request lists, when +0x8 is set |
| +0xc0 | `f_80966800` | the startup loop (section 2.5) |
| +0xd8 | `f_809667b0` | **the main loop** |
| +0xe0 | `f_83b6ea90` (label `frame_global_step`) | procFrame |
| +0xe8 | `f_83b6eb30` → `f_802eb020` | procDraw |
| +0xf0 | `f_83b6eb40` → `f_802eabd0` | procCalc |
| +0xf8 | `f_805902b0` → `f_805902c0` | the present's submit (the Presentation Thread's work; inline when presentation is not threaded) |
| +0x100 | `f_80547f20` | `nvnQueuePresentTexture` and the window's crop/sync bookkeeping |
| +0x108 | `f_8348ca50` | draws into the 1280 x 720 target at +0x110 (INFERRED: the debug/virtual canvas) |
| +0x110 | `f_8042cd10` → `f_8042cd20` | the end of the frame: GPU and display waits, the CPU gate |
| +0x118 | `f_83b6eb50` | after the waits (two indirect calls; INFERRED statistics) |
| +0x120 | `f_81207a50` | wait for the previous present, acquire the window texture |
| +0x128 | `f_804fbd60` → `f_804fbda0`, `f_83b6ef60` | the frame-rate controller: frame times (+0x148/+0x150) to the presentation interval (INFERRED; host/nn_vi.c calls it OoT's frame-rate controller) |
| +0x130 | `f_8348c680` | the frame body |

Fields used below: +0x38 the TaskMgr, +0x40 the MethodTreeMgr, +0x50 the presentation state (2 once frames run),
+0xfc the target frame rate, +0x100 / +0x108 the last frame's time and start tick, +0x148 the frame's CPU time, +0x188 a frame callback (`f_80441560`), +0x1b8 the
Presentation Thread, +0x1c8 the vsync system event, +0x1f8 the present-done event, +0x228 "presentation threaded",
+0x24f the present-skip flags, `[0x84cacc98]` the graphics device object (queue +0x68, gate primary +0x30..,
sync ring +0xb8, gate events +0xe0 + 0x40 * index, index +0x164, mutex +0x220).

### 2.4 Graphics initialisation (`f_8348be50`, label `sead_game_framework`)

VERIFIED (decompile), with the log lines of `boot_iold3` (0.243-0.247 s):

1. `nv::SetGraphicsAllocator`, `nv::InitializeGraphics(0, 0)`; the devtools allocator only if HeapDef gave it memory.
2. `nn::vi::Initialize`, `OpenDefaultDisplay`, `CreateLayer`, `GetDisplayVsyncEvent(fw+0x1c8)`,
   `SetDisplay120HzRefreshRateCapability`.
3. `nvnBootstrapLoader("nvnDeviceInitialize")`, `("nvnDeviceGetProcAddress")`; `f_812be290` fills the NVN proc
   pointers (once without a device, once with it; 0x5000 bytes of stores); the device builder and
   `nvnDeviceInitialize` into a 0x3000-byte device (log "native NVN device"); the API version is checked (0xe7 / 0x28).
4. The graphics device object (0x2b0 bytes, `f_80d34500`, stored at `[0x84cacc98]`) and its initialisation
   (`f_80966940`); `f_80f69040` (label `sead_device_queue_pools`): the queue (builder flags 9, command/control memory
   from the device's info), and the **window**: 3 textures 2560 x 1440 format 0x25 (log "window of 3 textures"), HDR
   format 0x29/0x3d when HDR is on; texture pool 33,068 and sampler pool 4,096 descriptors (log).
5. `f_8348fcc0`: a 0x25/0x35 colour+depth target of the canvas size at fw+0x110 (when fw+0xa4).
6. **The Presentation Thread**: a sead thread (0xe0 bytes, `f_8069dbe0`; name "Presentation Thread", priority
   fw+0xe8; INFERRED from the arguments: stack 0x10000, a message queue of 0x20) whose delegate (vtable `0x84c16950`, method `0x800ae150`) calls
   fw->vtable[0xf8]; core mask fw+0xec (0x3f: VERIFIED in the thread dump of `cc_base`, priority 17); started at once.

### 2.5 run, the TaskMgr and the startup loop

`f_80f48c30` (VERIFIED decompile): +0x50; heap "sead::MethodTreeMgr" and +0x68 → fw+0x40; `f_80f48eb0` makes the
**sead::TaskMgr** (`f_80f48ef0`; name "sead::TaskMgr"; root-task factory `0x80f47540`) → fw+0x38; +0x60 (runImpl);
+0x58.

The **startup loop** (`f_80966800`, slot +0xc0) repeats, every 10 ms: lock the device; `f_802eaa40` (a queued task's
**prepare**, its vtable +0x50, run on this thread); `f_802ead80` (create the queue's next task, or move a prepared one
to running); `f_802eaf10` (the TaskMgr's own tree and its destroy requests); unlock; until the TaskMgr has its root task
(+0x100) or a quit was asked. Then the MethodTreeMgr's +0x30(0) and the main loop (+0xd8).

So the root task **ModuleSystem** (factory `f_80f47540` → `alloc` + `f_80f47590`, vtable `0x84c85650`, a second base
at +0x1e8 with vtable `0x84c85730`) is created by the startup loop, and its prepare `f_83b797e0` runs **on
sead::MainThread, inside the startup loop**: there is no TaskMgr prepare thread in the thread list. That prepare is
boot-speed.md's 0.6-4.4 s of single-threaded init. VERIFIED (decompile: `f_802eaa40` calls the task's +0x50; run:
no such thread).

### 2.6 ModuleSystem's prepare (`f_83b797e0`, label `module_draw_names`)

VERIFIED (decompile) unless marked:

- heaps "GeneralHeap" (0x1100 bytes) and "ModuleSystem" (HeapDef size);
- the draw delegates for agl's layers: "ModuleSystem::draw2D_" (`0x8028f420`), "::drawTool2D_" (`0x8028f400`),
  "::drawTool2DSuper_" (`0x8028f3f0`), "::draw3DOpa_" (`0x802aaa80`), "::draw3DXlu_" (`0x802aae00`);
- four 0x470-byte stage descriptors of the module job graph (`f_80b5ebe0`, ModuleSystem +0x4c8, +0x4d0, +0x4d8,
  +0x4e0: moduleCalc_ hands +0x4c8 and +0x4d0 to `f_80070ea0` as its stage argument; the modules' nodes are added to
  +0x4d8 and +0x4e0 by `f_80b208e0`);
- the module registry `[0x84eb5f40]`: +0x20 creates the modules into ModuleSystem +0x1f8 (a pointer array, each
  module's index written at +0x2c), +0x28 and +0x30 initialise them; per module +0x70/+0x78 give its calc nodes,
  +0x68 its setup (`f_80b208e0`, `f_80b20010`: the job graph); +0x58/+0x60 finish the graph;
- `f_83b7a2c0` (label `module_system_init`): heaps "ExtraArgHeap", "PauseMgrHeap", and **`Pack/Bootup.Nin_Ounce_NVN2.pack`**
  (VERIFIED log: opened by "ARes Loading 0" at 0.398 s, right after `Pack/ZsDic` and `Pack/ResidentCommon`);
- the calc node **"ModuleSystem::moduleCalc_"** (delegate vtable `0x84c857e0`, method `f_80706be0`) attached with the
  gsys SystemTask's nodes under the ModuleSystem's own calc node (`f_80f48060` x3).

What the modules initialise in that window (the subsystems), with the log's times (`boot_iold3`, `boot_t1`):

| s | What | Evidence |
|---|---|---|
| 0.37 | the resource system: `System/Resource/ResourceSizeTable...rsizetable.zs`; the ARes loader threads load `Pack/ZsDic`, `Pack/ResidentCommon`, `Pack/Bootup`, the RSDB tables, `Lib/agl`, `Lib/gsys`, `Shader/*.bfsha`, `Pack/AI.Global`, fonts, `Mals`, effects (~500 files in the first second) | fs trace |
| 0.44-0.66 | threads: Presentation, VibrationThread, Resource Control/Memory, ARes Loading 0-3, ParallelDecompThread x4, GeneralFormatAsync_ x6, BootupBackgroundWorkThread x2, SceneLowPriority x2 | run (gdb, slowed) |
| 0.75 | the first queue submit (lr `0x812c3cf4`, a list begun at `0x812bd76c`) | log |
| 1.64-2.13 | `Sequence/FileEntry`, the title's event settings (`Dm_Title_*`, `Title_Background`) | fs trace |
| 1.83 | the audio renderer (log "audio: renderer opened") | log |
| 2.01 | save data mounted ("fs: mounted save"; BootupBackgroundWorkThread mounts the temp) | log |
| 2.08-2.66 | threads: engine::GraphicsModule, rmbl::VibrationThread, PreActorModelCompose x6, EventMgr::EventSettingLoad, UpdateTextBoxTextureThread, EngineUILowPriorityWorker, EffectResLoading/Unload, AudioFramework/UserTask, OpusDecoder, aal::*, gmd::GameDataMgr/SaveDataMgr, bake::ModuleMgr, aglTextureDataMgr::Compaction/Replace, BakeProbe, PlayReporter, ConcenteredRenderer::Prepare | run (gdb) |
| 2-3.5 | almost no files opened; the main thread runs lifted code (effect resources `f_812eed00`, `f_8017ed80`: boot-speed.md) | boot-speed.md |
| (play log) | the texture data pool 2432 MB (`agl::TextureDataMgr`, lr `0x80f6cb20`); the DTA's render target pool | log |
| 4.34 | audio input streams | log |
| 4.40 | input: `hid` reads npad 0 (lr `0x83489670`), six-axis handles | log |
| ~4.40 | the last threads: NavMeshDepWorker2, NavMeshCAStepDepWorker1, game::cull::Module, **ModuleSystemWorker1-5** (`f_80dbcd70`, label `thread_module_workers_create`), **BaseProcExecutor0-5** (`f_80b5e9e0`), BaseProcCreateAndDeleteThread x6, BancModule, Porter; then OptimizeChangeBanc, NfcDevice[0], NfpMgr, PlacementMgr in the first frames | run (gdb); log ("rt slots" from ModuleSystemWorker3 at 4.405) |
| 4.42 | **the first frame**: the first CPU `EventSignal` (lr `0x802eb420`, the gate primary) | log |

## 3. From the first frame to the title

- The sequence is data, not code. Within the first frames the game opens
  `Sequence/SequenceParam/FormalScene.engine__component__SequenceRef.bgyml` and **`Sequence/FormalSequence.root.ainb`**
  (4.506 s, VERIFIED fs trace), then its modules: Title, Main, Event, DestroySceneCommon.110, BeforeInitializeCommon,
  MinusMenu, BeforeTitle.110, NormalTitle, Opening, ... (111 files in `Sequence/`). Its nodes are the game's `Seq*`
  functions (for example `SeqOneShotFinishStartupLogo`, `f_832f26a0`).
- What runs it each frame is a module in the ModuleSystem's graph (INFERRED: the module and its thread were not
  identified).
- boot-speed.md's timeline (chiseled boot, frozen note, not edited): BeforeTitle(.110) 4.4-6.1 s (layout archives,
  resident events, the title event `Title_Background` = `Event/Movie/Dm_Title_0005.webm` reserved, loaded, requested
  and started; then `SeqOneShotFinishStartupLogo`, the "oe: startup logo finished" line, 6.29 s in `boot_iold3`);
  the new-game transition 6.1-8.6 s (NormalTitle, PrepareScreensOfLoading, CloseTitle, ResetGameData, Opening,
  ChangeBanc's unload); the black loading screen 8.6-13.2 s (banc load); the first scene frames from 13.2 s;
  playable at ~18 s. The main loop below runs through all of it: the title and the loads are frames.

## 4. The main loop

### 4.1 The loop and the frame, on sead::MainThread

`f_809667b0` (VERIFIED lifted C): `for (;;) { fw->vtable[0xe0](fw); }`.

The frame, in order. Order VERIFIED (run `m6j_order1`, two whole frames, identical) and the calls VERIFIED
(decompile); "blocks on" from the code and the `m6a_crit2_k` trace (Kokiri, pacing off; ms a frame are that run's).

| # | Function (call site lr) | What | Blocks on |
|---|---|---|---|
| 1 | `f_83b6ea90` procFrame | `QueryDisplayCurrentRefreshRate`, `QueryDisplayVrrSupported`; +0x128 the frame-rate controller; the global step `[0x84d61fc8]` = 60 / nominal rate; `f_83be0230` the nn::oe messages (focus, operation and performance mode); tail-calls the body | - |
| 2 | `f_8348c680` the frame body (label `frame_main_signal_gpu`) | `QueryDisplayHdrStatus` / paper white when HDR; then 3-9 | - |
| 3 | `f_802eaf10` (`0x8348c72c`) | the TaskMgr's own method tree (empty) and its destroy and finalize requests | the TaskMgr mutex |
| 4 | +0xf0 → `f_802eabd0` (`0x8348c73c`) **calc** | `f_802eaa40` / `f_802ead80` (task prepare and create, as in the startup loop); then the MethodTreeMgr's calc tree under its mutex (`f_802eacc0`, recursive): section 4.2 | the job stages (4.3) |
| 5 | +0x70 → `f_8348d430` (`0x8348c74c`) | procReset: nothing unless a reset is asked | - |
| 6 | +0x120 → `f_81207a50` (`0x8348c75c`) | `nn::os::WaitEvent(fw+0x1f8)`: the Presentation Thread's previous present is done; then `nvnWindowAcquireTexture` and `nvnWindowSetCrop`; the presentation-mode state machine (+0x248/+0x24c) | the present-done event; the acquire (the host's display pacing, `hos_vi_native_acquire`, `host/nn_vi.c`; off with `HOS_NO_PACING=1`) |
| 7 | +0xe8 → `f_802eb020` (`0x8348c76c`) **draw** (label `frame_gate_caller`) | `f_802eb390` the gate primary's begin (label `frame_gate_primary`: add memory, `BeginRecording`, the CPU `nvnEventSignal(gate, 0)` that resets the gate event, a recorded `WaitEvent(gate == 1)`, a timestamp, pools, scratch); the MethodTreeMgr's draw tree (4.2); a timestamp report, `EndRecording`, the command/control memory used; then **message 1 to the Presentation Thread** (`fw[0x37]->vtable[0x10](1, 0)`) when +0x228, else +0xf8 inline | - |
| 8 | tick: fw+0x148 = this frame's CPU time | | |
| 9 | +0x110 → `f_8042cd20` (`0x8348c78c`) (label `frame_limiter_sleep`) | (a) the frame callback fw+0x188 with 0 (`f_80441560` → `f_80441630` → the ModuleSystem's second-base delegate → `f_80070d30`: **the frame-end stage**, 4.3); (b) `nvnSyncWait(sync ring[+0x168], -1)` at `0x8042cde0`: the previous frame's Presentation fence; (c) when the window is presenting (fw+0x50 == 2), `nvnSyncWait(window sync, -1)`, else `f_8348c920`: `WaitSystemEvent(vsync)`, repeated while below +0xfc fps; (b) comes before (c) when +0xab is set, after it otherwise; (d) the callback with 1, +0x118; (e) **tail call `nvnEventSignal(gate event[+0x164], 0, 1)`** (the jump at `0x8042cec0`), so its lr is `0x8348c78c` | the fence; the window sync or vsync; the stage's own join |
| 10 | ticks: fw+0x100 frame time, fw+0x108 the frame's start | the present-skip flags (+0x24e/+0x24f) | |

When +0x24f has both present-skip bits (no presentation), 7 only signals +0x1f8 and 9 is a sleep to 1 / +0xfc
(the "frame limiter" of `host/nn_vi.c`); in play the bits are clear.

### 4.2 The method trees

VERIFIED (run `m6j_tree1`): identical at present 60 (the title's first second), 300 (the load) and 1200 (Kokiri
play). Nodes are sead MethodTreeNodes (name at +0x48, delegate at +0x50: invoker, object +0x58, method +0x60,
adjust +0x68; children through +0x28 / +0x30), walked by `f_802eacc0` (label `frame_draw_tv_caller`), which calls a
node, then its children.

```
calc (MethodTreeMgr +0x28), from f_802eabd0
  sead::RootCalc
    sead::SysCalc                                   (no children)
    sead::AppCalc
      ModuleSystem                       f_83b7b100   (ModuleSystem vtable +0xb0)
        gsys::SystemTask::PreCalc        f_80670d00   "gsys::SystemTask::preCalc_"
        ModuleSystem::moduleCalc_        f_80706be0   (label frame_step)
        gsys::SystemTask::PostCalc       f_8066fb80   "gsys::SystemTask::postCalc_"
draw (MethodTreeMgr +0x1d8), from f_802eb020
  sead::RootDraw
    sead::AppDraw
      ModuleSystem                       f_83b7bc70   (vtable +0xb8)
      gsys::SystemTask::DrawMainScreen   f_8066fc80   "Draw(TV)" (label frame_draw_tv)
    sead::AppDrawFinal                              (no children)
    sead::SysDraw
      gsys::SystemTask::PostDrawMainScreen f_836ce750 "PostDraw(TV)"
TaskMgr tree (TaskMgr +0x1e8), from f_802eaf10: a root with no children
```

The ModuleSystem's agl draw delegates (draw3DOpa_ etc.) are not tree nodes: they are called by the layer lists.

### 4.3 Inside moduleCalc_: the job stages

`f_80706be0` (VERIFIED decompile): the frame's step from the mode (60.0, 30.0 in the half-rate mode) and the
nominal rate (the double at ModuleSystem +0xd58 is the game clock, `nx2/oot_patches.py`), the module timing
(`f_83b6e300`, `f_83b89870`, `f_83b899c0`, `f_80877eb0` on ModuleSystem +0x480), the registry's +0x30 under a mutex,
then the module job graph (ModuleSystem +0x4b8) in **stages**: each stage is `f_80070ea0(graph, pool, stage list)`
(label `job_graph_build`) and `f_80070b20(pool)` (label `job_graph_run_and_join`): the calling thread runs the
scheduler `f_8007fe60` (label `job_scheduler`) itself, and the ModuleSystemWorkers run it through their `calc_`
`f_8007fe10` (message 1, then `SignalLightEvent(+0x1e8)`); `f_80070b20` then waits on each worker's light event
(engine-jobs.md section 2).

One frame (VERIFIED run `m6j_order1`; the functions each stage was seen running):

| Stage | Built at / run at | Runs | Threads |
|---|---|---|---|
| A | `0x80706d38` / `0x80706d40` (stage list +0x4c8 or +0x4d0) | **game logic**: the BaseProc executors `f_8004aae0` → `f_8004ab40` (label `actor_baseproc_executor`), the NavMesh workers' scheduler | main (scheduler), BaseProcExecutor0-5, NavMeshDepWorker2, NavMeshCAStepDepWorker1, one ModuleSystemWorker in `f_806afc40` |
| B | `0x80707310` / `0x80707318` | only the scheduler was seen (content not identified) | main, ModuleSystemWorker1-5 |
| C | `0x8070749c` / tail call (lr `0x802ead44`) | the calc jobs (`f_800998a0`, lr `0x80081268`, 3-5 calls); the **102 render jobs** (`f_80084de0`, label `render_job_record`, about 17 rounds of 6: main and five workers); a second BaseProc step (`f_83acfb90` on main and the workers, with BaseProcExecutor0-5 again); the **layer lists** (`f_8006cd60` x6, `f_8007ad40` x4, the 3D pipeline `f_80660320` x10, on the workers and main) | main, ModuleSystemWorker1-5, BaseProcExecutor0-5 |
| end | `f_80070d30` (`0x80441668`) at step 9(a): `f_80070ea0` (`0x80070d54`), `f_80070b20` (`0x80070d5c`), `f_80070ea0` (`0x80070dd0`) | the next frame's first work: one ModuleSystemWorker enters `f_806afc40` (a BaseProc step) and the BaseProcExecutors start while the main thread is still in steps 9-10 and 1-4 | main, ModuleSystemWorker1-5, BaseProcExecutor0-5 |

M6a's critical path is this sequence seen with times (Kokiri, median frame of 12.7 ms, VERIFIED trace): game logic
on the BaseProcExecutors 0-4.45 ms (stage A, kicked by the end stage), calc jobs 4.5-7.5 ms, render recording
7.5-11.9 ms (job lists, then the long layer list `0x8007b0c8` on one worker: stage C), the main thread's submits
and the gate signal 11.96-12.73 ms (steps 7 and 9).

What the main thread waits on inside the stages (`m6a_crit2_k`, ms a frame): the scheduler's condition variable
at `0x80080188` (6.75, 11.9 times), its 20 µs poll sleeps at `0x8009a478` in `f_800998a0` (0.81, 39 times), the
second BaseProc step's join at `0x83acfcf4` in `f_83acfb90` (0.63, once). The main thread runs 4.5 ms of 12.9 and is
blocked 8.3 ms.

### 4.4 The draw phase and the hand-over

- `f_8066fc80` "Draw(TV)" (VERIFIED decompile, run): `f_80665980` the queue heads (label `frame_head_begin`: the
  frame-start event signal at `0x80666598`, two head submits from the main thread at `0x80666be0`), then
  `f_802ab6c0` "Draw(drawMainScreen_)" → `f_807ccda0` → `f_807cd380` (label `layer_lists_call`): the one
  `CallCommands(cb, 9, handles)` of the layer lists recorded in stage C, and the final composite to the window
  texture (`f_80285b00`). engine-frame.md section 1 has the lists, events and queues; nothing there changes.
- `f_836ce750` "PostDraw(TV)" (SysDraw).
- `f_802eb020` ends the gate primary and sends message 1 to the Presentation Thread.

### 4.5 The Presentation Thread

`Presentation Thread` (priority 17, mask 0x3f): `f_834a2e50` (label `thread_sead_entry`) → run `f_80df2680` (label
`thread_sead_loop_b`: receive a message, call `calc_(msg)`) → `f_80a65370` (the delegate thread's `calc_`) → method
`0x800ae150` → fw->vtable[0xf8] → **`f_805902c0`** (label `frame_present_submit`). VERIFIED (run: the thread's object,
vtable `0x84c188d0`, delegate at +0x100/+0x110; decompile):

1. lock the device (`[0x84cacc98]` +0x220);
2. `nvnQueueWaitSync(queue, window sync)` (`0x80590314`);
3. the submits: when the object at fw+0x250 says so (its +0x10), its +0x00 submits (INFERRED: `f_80527f00`, label
   `frame_submit_queues`, the compute lists and heads at `0x805282f4`, then the gate primary, as engine-frame.md
   traced), else `nvnQueueSubmitCommands(queue, 1, gate primary)` (`0x8059036c`); then `nvnQueueFenceSync` into the
   ring (`0x805903a0`), `nvnQueueFlush`;
4. HDR metadata when HDR;
5. when threaded: poll `nvnEventGetValue(gate event) == 1` with 1 ms sleeps (`0x80590490`, the sleep at `0x80590520`):
   the main thread's step 9(e);
6. +0x100 → `f_80547f20` (label `frame_queue_present`): `nvnQueuePresentTexture` (`0x80547f7c`);
7. unlock, `nn::os::SignalEvent(fw+0x1f8)` (what step 6 of the next frame waits for).

`m6a_crit2_k`: it waits 11.5 ms a frame for the message (`0x834a3084`), 1.08 ms in the poll's sleep, 0.18 ms in the
present.

### 4.6 What else runs during a frame

Everything not in the table of section 6 below as "frame work" sleeps in its message queue or condition variable
most of the frame (`m6a_crit2_k`: 11-13 ms blocked of 12.9 for Resource Control/Memory, aal::*, SceneLowPriority,
PlayReporter, AudioFrameworkThread). In steady Kokiri play the ones that run a little each frame are the
NavMesh workers (stage A; NavMeshCAStepDepWorker1 ran 2.45 ms a frame and waited 3.8 ms for a virtual core),
BaseProcCreateAndDeleteThread x6 (actor creation and deletion, on demand), aal::PathFindingThread (~1 ms),
AudioFrameworkThread and AudioUserTaskThread (audio frames on their own clock), rmbl::VibrationThread (short sleeps),
OptimizeChangeBanc (16 ms sleeps), and the npln (network) threads (blocked).

## 5. Call tree

```
host main (host/hos_main.c:1625)                       process thread: SDL window loop
 └ game_thread → game_main (:1368)                     "Game", then the guest main thread
    ├ reserve_guest (:121), image read, hos_mem_init (hos_mem.c:34), hos_boot_main_thread (hos_thread.c:489)
    ├ _init_libc0 0x85717e50, nnosInitialize 0x8510f720, _init_libc1 0x85717e60
    ├ nninitStartup 0x8262bb10                          heap size, SetMemoryHeapSize, 36 MB C heap, root arena
    ├ _init_libc2 0x85717e80; DT_INIT main 0x812377e0 (9,783 ctors), subsdk0 0x84ec0040, sdk 0x84fa0060
    └ nnMain f_81222cd0
       ├ f_80ec0e90                                     FPCR.FZ
       ├ f_809db6a0                                     sead systems
       │  ├ f_809ddf00 → f_80df5690                     root heap; sead::MainThread (priority 16)
       │  └ f_80c49770                                  FileDeviceMgr: romfs mounted
       ├ f_80f70240                                     HeapDef
       ├ f_80fb14b0, f_809d9a90, f_83bf9db0             region, rom config, address table
       ├ f_80e57700                                     sead::GameFrameworkOunce (vtable 0x84c83f80)
       ├ f_8348be50                                     NVN, vi, window, Presentation Thread
       └ fw+0x20 f_80f48c30 run
          ├ fw+0x68 f_80f47b80                          MethodTreeMgr
          ├ f_80f48eb0 → f_80f48ef0                     TaskMgr (root task factory f_80f47540)
          └ fw+0x60 f_80966760 runImpl
             ├ fw+0xc0 f_80966800                       startup loop, 10 ms
             │  ├ f_802ead80 → f_809dbac0 → f_80f47540  creates ModuleSystem
             │  └ f_802eaa40 → task+0x50 f_83b797e0     ModuleSystem prepare: modules, threads, Pack/Bootup
             └ fw+0xd8 f_809667b0 main loop: for (;;)
                └ fw+0xe0 f_83b6ea90 procFrame
                   ├ fw+0x128 f_804fbd60                frame-rate controller
                   ├ f_83be0230                         nn::oe messages
                   └ f_8348c680 frame body
                      ├ f_802eaf10                      TaskMgr pre-pass
                      ├ fw+0xf0 f_802eabd0 calc
                      │  ├ f_802eaa40, f_802ead80       task prepare/create
                      │  └ f_802eacc0 calc tree
                      │     └ f_83b7b100 ModuleSystem
                      │        ├ f_80670d00             gsys PreCalc
                      │        ├ f_80706be0             moduleCalc_
                      │        │  ├ f_80070ea0 + f_80070b20 → f_8007fe60   stage A: BaseProc (f_8004ab40), NavMesh
                      │        │  ├ f_80070ea0 + f_80070b20 → f_8007fe60   stage B
                      │        │  └ f_80070ea0 + f_80070b20 → f_8007fe60   stage C: f_800998a0, f_80084de0 x102,
                      │        │                                            f_83acfb90, f_8006cd60/f_8007ad40 → f_80660320
                      │        └ f_8066fb80             gsys PostCalc
                      ├ fw+0x70 f_8348d430              reset
                      ├ fw+0x120 f_81207a50             WaitEvent(present done), nvnWindowAcquireTexture
                      ├ fw+0xe8 f_802eb020 draw
                      │  ├ f_802eb390                   gate primary begin
                      │  ├ f_802eacc0 draw tree
                      │  │  ├ f_83b7bc70                ModuleSystem draw
                      │  │  ├ f_8066fc80 Draw(TV)       f_80665980 heads; f_802ab6c0 → f_807cd380 CallCommands(9)
                      │  │  └ f_836ce750 PostDraw(TV)
                      │  └ message 1 → Presentation Thread
                      └ fw+0x110 f_8042cd20
                         ├ fw+0x188 f_80441560(0) → f_80441630 → f_80070d30   frame-end stage (f_80070ea0, f_80070b20)
                         ├ nvnSyncWait(previous fence); nvnSyncWait(window) or f_8348c920 (vsync)
                         └ tail nvnEventSignal(gate, 1)

Presentation Thread: f_834a2e50 → f_80df2680 → f_80a65370 → 0x800ae150 → fw+0xf8 → f_805902c0
   WaitSync(window), f_80527f00 submits, gate submit, FenceSync, poll gate, f_80547f20 present, SignalEvent(+0x1f8)
ModuleSystemWorkerN: f_834a2e50 → f_80df25c0 → f_8007fe10 → f_8007fe60 (stages A-C, end)
BaseProcExecutorN:   f_834a2e50 → f_80df25c0 → f_8004aae0 → f_8004ab40
```

## 6. Threads in steady play

Creator: the function that names the thread (it references the name string; `fnstr.json` of the engine-layers
graph). Entry: every sead thread starts in `f_834a2e50` (TLS, the cached core number, then run); "run / calc" are its
vtable's +0x68 / +0x70 (VERIFIED run `m6j_tree1`). Priority and mask: the `cc_base` thread dump (cores.md, VERIFIED
log). Blocks on: `m6a_crit2_k`, the guest lr of the thread's main blocking call and its ms a frame (VERIFIED trace).
`0x834a3084` is the sead message-queue receive (`f_834a3060`) of an idle thread.

| Thread | Run / calc | Creator | Prio | Mask | Blocks on |
|---|---|---|---|---|---|
| sead::MainThread | the nn main thread: `nnMain` → main loop | host; adopted by `f_80df5690` | 16 | 0x1 | the stages' scheduler (`0x80080188`, 6.7 ms), present-done (`0x81207a6c`), the fence and window waits in `f_8042cd20` |
| Presentation Thread | `f_80df2680` / `f_80a65370` → `f_805902c0` | `f_8348be50` | 17 | 0x3f | its message (`0x834a3084`, 11.5 ms), the gate poll (`0x80590520`, 1.1 ms), the present (`0x80547f7c`) |
| ModuleSystemWorker1-5 | `f_80df25c0` / `f_8007fe10` | `f_80dbcd70` | 16 | one of 0x2..0x20 | the scheduler (`0x80080188`, 3.7-7.4 ms), poll sleeps (`0x8009a478`), the BaseProc joins (`0x806afd14` MSW1 3.6 ms, `0x83acfcf4` 0.6 ms) |
| BaseProcExecutor0-5 | `f_80df25c0` / `f_8004aae0` → `f_8004ab40` | `f_80b5e9e0` | 16 | one of 0x1..0x20 | its message (`0x834a3084`, 7.0 ms), inside the executor (`0x8004af4c`, 1.7-2.7 ms) |
| NavMeshDepWorker2, NavMeshCAStepDepWorker1 | `f_80df25c0` / `f_8007fe10` | `f_8102f6e0`, `f_80f6ecf0` | 22 | 0x4, 0x2 | its message (6.6 ms); virtual cores (3.8 ms) |
| BaseProcCreateAndDeleteThread x6 | `f_80df2640` / `f_83ad0bf0` | `f_80ed7f60` | 23 | one each | its queue (`0x834a3084`) |
| PreActorModelCompose x6 | `f_80df2680` / `f_80a65370` | `f_80ed6f00` | 26 | one each | 100 ms sleeps (`0x80df26c0`) |
| GeneralFormatAsync_ x6 | `f_80df2680` / `f_836e9140` | `f_83bb3eb0` | 24 | one each | its queue |
| ARes Loading 0-3 | `f_80df2680` / `f_801be6e0` | `f_836d8ad0` | 19, 19, 18, 18 | 0x20/0x10 | its queue |
| Resource Control, Resource Memory | `f_80df2680` / `f_801be6e0` | `f_836d8ad0` | 20, 21 | 0x2 | its queue (12.7 ms) |
| ParallelDecompThread x4 | `f_80df2680` / `f_836dda20` | `f_8069a5e0` | 18-19 | 0x10/0x20 | its queue |
| tera::FileResourceMgr | `f_80df2680` / `f_80a65370` | `f_80f6cf50` | 25 | 0x2 | its queue |
| SceneLowPriority x2 | `f_80df2680` / `f_836e9140` | `f_811f75d0` | 28, 17 | 0x4, 0x3f | its queue |
| engine::GraphicsModule | `f_80df2680` / `f_80a65370` | `f_83b71980` | 23 | 0x2 | its queue |
| game::cull::Module | `f_80df2680` / `f_80a65370` | `f_80ed7a70` | 22 | 0x2 | its queue |
| EffectResLoadingThread x2, EffectResUnloadThread | `f_80df2680` / `f_80a65370` | `f_83b494b0`, `f_83b51a00` | 23-25 | 0x2, 0x4 | its queue |
| aglTextureDataMgr::Compaction, ::Replace | `f_80df2680` / `f_80a65370` | `f_80f6c580` | 25 | 0x2 | its queue |
| ConcenteredRenderer::Prepare | `f_80df2680` / `f_80a65370` | `f_809dd2f0` | 25 | 0x4 | its queue |
| UpdateTextBoxTextureThread | `f_80df2680` / `f_80e6dfd0` | `f_80a9f200` | 17 | 0x3f | its queue |
| EngineUILowPriorityWorker | `f_80df2680` / `f_80a65370` | `f_80ed86d0` | 20 | 0x4 | its queue |
| AudioFrameworkThread | `f_80df2680` / `f_836f51d0` | `f_81043860` | 4 | 0x2 | a condition variable (`0x836f5258`, audio frames) |
| AudioUserTaskThread | `f_80df2680` / `f_836f17a0` | `f_836fb420` | 3 | 0x2 | `0x836f1894` |
| OpusDecoder | `f_80df2680` / `f_836f4410` | `f_80ed9e90` | 14 | 0x2 | its queue |
| aal::PathFindingThread, aal::LowPrioWorkerThread | `f_80df2680` / `f_8370bbc0` | `f_80f336b0`, `f_81049f70` | 21, 17 | 0x3f | its queue |
| VibrationThread, rmbl::VibrationThread | `f_83489ad0`; `f_80a65370` | `f_80ed6970`, `f_80c7a080` | 15 | 0x2 | its queue; short sleeps (`0x80a65400`) |
| gmd::GameDataMgr, gmd::SaveDataMgr | `f_80df2680` / `f_80a65370` | `f_80e1f010`, `f_80b72340` | 27 | 0x4 | its queue |
| bake::ModuleMgr, BakeProbe | `f_80df2680` / `f_80a65370` | `f_80b15d80`, `f_80b15710` | 28 | 0x2 | its queue |
| BootupBackgroundWorkThread x2 | `f_80df2680` / `f_80e557a0` | `f_809ddc80` | - | - | its queue (gone or idle in play) |
| PlayReporter | `f_80df2680` / `f_805378b0` | `f_80e5eaf0` | 29 | 0x1 | its queue |
| BancModule, Porter, PlacementMgr | `f_80df2680` / `f_80a65370` | `f_809de390`, `f_809ddab0`, `f_80d7ac00` | 24, 28, 26 | 0x2, 0x2, 0x4 | its queue |
| OptimizeChangeBanc | `f_80df2680` / `f_80a65370` | `f_832b9720` | 27 | 0x38 | 16 ms sleeps (`0x80032f28`) |
| EventMgr::EventSettingLoad, ::ResidentEventLoad | `f_80df2680` / `f_80a65370` | `f_80ed7dc0`, `f_80f84710` | - | - | its queue |
| NfcDevice[0], NfpMgr | `f_80df2680` / `f_80a65370` | `f_80ed8a60`, `f_80ed8ee0` | 28 | 0x4 | its queue |
| nn.npln.*, npln-grpc * (8) | nn::os threads of the network library (not sead) | the SDK | 16 | 0x6 | condition variables, socket poll |
| Thread_824c2960 | `0x824c2960` (not sead) | - | 28 | 0x2 | - |

Title only: MovieOperateThread, MovieEventHandleThread, MovieMediaSourceClient (`f_83b7cec0`, `f_83b827e0`) and an
unnamed movie input thread; joined at the new-game transition (boot-speed.md). Core masks and priorities do not
change with `HOS_CORES` (cores.md).

## 7. Where to hook

The cleanest function boundary for a timing mark or a patch, per phase (an `ENTRY_HOOK(0x...)` runs at a lifted
function's entry, `host/hos.h:26`; a mark at the end of a phase is the entry of the next one).

| Phase | Boundary | Notes |
|---|---|---|
| a frame | `f_83b6ea90` entry (procFrame) | once a frame on sead::MainThread, before anything else of the frame; `nx2/oot_patches.py` already splices the real-time step here and in `f_80706be0` |
| the frame's CPU work ends | the tail `nvnEventSignal(value 1)` of `f_8042cd20` | already timed by the back end (the CPU event signal, `host/rx_backend.c`); M6a's frame boundary |
| task create/destroy | `f_802eaf10`, `f_802eaa40`, `f_802ead80` | rare work; a scene change's task churn shows here |
| calc | `f_802eabd0` entry / `f_8348d430` entry | everything of 4.3 |
| game logic | `f_80706be0` entry to the second `f_80070ea0` (lr `0x80707310`) | stage A; per thread `f_8004ab40` (BaseProc executor) |
| calc jobs, render recording | `f_80070ea0` with lr `0x8070749c` to the return of `f_802eacc0` into PostCalc (`f_8066fb80` entry) | stage C; per job `f_80084de0` (already hooked: `host/eng_jobs.c:51`), per layer list `f_8006cd60` / `f_8007ad40` |
| a stage | `f_80070b20(pool)` | entry and return bracket one stage on the main thread including its join; `f_8007fe10` brackets a worker's share |
| present wait and acquire | `f_81207a50` | the WaitEvent and the acquire are its only blocking calls |
| draw (gate primary) | `f_802eb020` entry to `f_8042cd20` entry | the main thread's recording of the frame primaries |
| frame-end stage | `f_80070d30` | the next frame's game logic starts here |
| GPU and display waits | `f_8042cd20` after the callback: `0x8042cde0` (fence) and `f_8348c920` (vsync) | |
| present | `f_805902c0` entry / `f_80547f20` | on the Presentation Thread; the gate poll is `0x80590490` |
| frame rate and step | `f_804fbda0` (controller), `f_83b6ea90` / `f_80706be0` (the step) | the host already overrides the interval (`hos_vi_native_interval`) and +0xfc (`host/nn_vi.c`) |
| boot | `f_80966800` (startup loop) and `f_83b797e0` (ModuleSystem prepare) | the 4 s single-threaded window |

## 8. Corrections to earlier notes

- **engine-jobs.md section 2** has one kick and join a frame (`f_80070d30` → `f_80070ea0` → `f_80070b20`). The frame has
  **four** stages of the module graph: three are built and run by moduleCalc_ itself (`f_80706be0` calls `f_80070ea0`
  and `f_80070b20` directly at `0x80706d38/40`, `0x80707310/18`, `0x8070749c` + a tail call), and `f_80070d30` runs only
  the frame-end stage, from the framework's frame callback inside `f_8042cd20` before the GPU wait (VERIFIED run).
- **M6a and crit_path.py** say the main thread starts the next frame's jobs right after its CPU event signal. The
  frame-end stage that starts them is launched before the signal, before the fence and window waits; the signal then
  follows those waits, and the workers' share runs on into the next frame (VERIFIED run: a ModuleSystemWorker in
  `f_806afc40` and the BaseProcExecutors running while the main thread is in `f_8348c680` and `f_802eaf10`). The frame
  boundary M6a chose still works as a mark.
- **engine-frame.md section 1**: "CPU gate `nvnEventSignal(ev, 1)` at `0x8348c78c` (after its sync wait)" is right,
  but `0x8348c78c` is the return address of the framework's +0x110 call: the signal is the tail call of `f_8042cd20`,
  not a call of `f_8348c680`. "main SyncWait `0x8042cde0` ... and on the window sync": the second wait is the window's
  sync when presenting, else the vsync event (`f_8348c920`). "`f_805902c0` <- `f_80df2680`": the full chain is
  `f_80df2680` → `f_80a65370` → `0x800ae150` → the framework's +0xf8 thunk `f_805902b0`. And the layer lists and
  render jobs are recorded in the **calc** phase (stage C under `f_802eabd0`), not in the draw phase: the draw phase
  only records the gate primary and calls them.
- **Labels of M6i** (nx2/oot_labels.txt, not changed here) that the code reads otherwise:

  | Address | Label | What the code says |
  |---|---|---|
  | `80f48c30` | `sead_method_tree_mgr` | the framework's run (it creates the MethodTreeMgr, the TaskMgr and runs the loop) |
  | `8348c680` | `frame_main_signal_gpu` | the frame body (calc, draw, waits); the GPU signal is in `f_8042cd20` |
  | `802eacc0` | `frame_draw_tv_caller` | the method tree's recursive call, for the calc and the draw trees |
  | `802eb020` | `frame_gate_caller` | procDraw: gate primary, draw tree, hand-over |
  | `8042cd20` | `frame_limiter_sleep` | the frame end: frame-end stage, fence, window or vsync waits, CPU gate; the sleep is only the no-present branch |
  | `83b797e0` | `module_draw_names` | ModuleSystem's prepare (modules, threads, nodes, Bootup pack) |
  | `83b7a2c0` | `module_system_init` | loads `Pack/Bootup` (heaps ExtraArgHeap, PauseMgrHeap) for the prepare |
  | `80706be0` | `frame_step` | ModuleSystem::moduleCalc_: the step and the three job stages |
  | `8348be50` | `sead_game_framework` | the framework's graphics initialisation (NVN, vi, window, Presentation Thread) |

## 9. Not known

- What each stage's job graph holds (the node lists of stage lists +0x4c8/+0x4d0, stage B's work), and which modules
  exist (the registry `[0x84eb5f40]` makes them; their names and order were not dumped).
- Which module runs the FormalSequence AINB, and on which thread.
- What `f_83acfb90` (the second BaseProc step in stage C) and `f_806afc40` (the BaseProc step on one worker) do apart
  from running `f_8004ab40` and joining.
- `f_83b6eb50` (+0x118, after the waits), the fw+0x250 submit hook's object, and `f_8348ca50` (+0x108).
- nnMain's `0x3000000` in the HeapMgr argument (not used by the arena path taken here).
- The ModuleSystem's second base (+0x1e8, vtable `0x84c85730`, `f_80070d30`) and `f_83b7b130` (vtable +0xc8, the same
  body): who else holds that delegate.
- The init window's per-subsystem order inside the prepare (graphics module, audio, save data, texture heap) is known
  only by the log's times, not by the calls.

## 10. Labels added (nx2/oot_labels.txt, "boot and the main loop")

| Address | Label | What |
|---|---|---|
| `809db6a0` | `sead_framework_init_systems` | heap mgr, thread mgr, resource mgr, file device mgr (romfs mount) |
| `809ddf00` | `sead_heap_mgr_init` | the root heap on nninitStartup's arena, the ThreadMgr |
| `80df5690` | `thread_adopt_main` | the nn main thread becomes sead::MainThread, priority 16 |
| `80f70240` | `config_heapdef_load` | reads System/HeapSize's HeapDef |
| `809d9a90` | `config_romconfig_load` | System/GameRomConfig |
| `83bf9db0` | `config_address_table_load` | region language, System/AddressTable |
| `80e57700` | `sead_framework_construct` | the framework object (vtable 0x84c83f80) |
| `80f48eb0` | `sead_task_mgr_create` | the TaskMgr with the root task's factory |
| `80966800` | `frame_startup_loop` | 10 ms polls of the TaskMgr until the root task runs |
| `809667b0` | `frame_main_loop` | for (;;) procFrame |
| `802eaf10` | `frame_task_mgr_pre` | the TaskMgr's tree and destroy requests |
| `802eabd0` | `frame_proc_calc` | task create, the calc tree |
| `802eaa40` | `sead_task_mgr_prepare_next` | a queued task's prepare on the calling thread |
| `802ead80` | `sead_task_mgr_create_next` | creates the queue's next task, or starts a prepared one |
| `8348d430` | `frame_proc_reset` | reset requests |
| `81207a50` | `frame_wait_present_acquire` | waits for the previous present, nvnWindowAcquireTexture |
| `8348c920` | `frame_wait_vsync` | the vsync event, more vsyncs below the target rate |
| `83be0230` | `frame_app_messages` | nn::oe notification messages |
| `804fbda0` | `frame_rate_controller` | frame times to the presentation interval (INFERRED) |
| `80f47540` | `module_root_task_create` | the root task's factory: a ModuleSystem |
| `80f47590` | `module_system_construct` | ModuleSystem (vtable 0x84c85650) |
| `83b7b100` | `module_system_task_calc` | the ModuleSystem's calc node |
| `83b7bc70` | `module_system_task_draw` | the ModuleSystem's draw node |
| `80670d00` | `gsys_system_task_precalc` | gsys::SystemTask::preCalc_ |
| `8066fb80` | `gsys_system_task_postcalc` | gsys::SystemTask::postCalc_ |
| `836ce750` | `gsys_system_task_post_draw` | "PostDraw(TV)" |
| `8004aae0` | `thread_baseproc_executor_calc` | BaseProcExecutor's calc_: runs the executor |
| `83acfb90` | `actor_baseproc_stage_late` | the second BaseProc step of stage C, with its join (INFERRED) |
| `806afc40` | `actor_baseproc_stage_logic` | a worker's BaseProc step that starts the game logic (INFERRED) |
| `80a65370` | `thread_delegate_calc` | a delegate thread's calc_ |
| `8069dbe0` | `thread_sead_construct` | a sead thread object |
| `80441560` | `frame_end_callback` | the framework's frame callback (+0x188) |
| `80441630` | `frame_end_stage_kick` | calls the ModuleSystem's frame-end stage |
| `80ec0e90` | `sys_fpcr_flush_to_zero` | FPCR.FZ for the main thread |
| `80d34500` | `gfx_device_construct` | the graphics device object at [0x84cacc98] |
| `812be290` | `nvn_procs_load` | fills the NVN proc pointers |
| `83ad0bf0` | `thread_baseproc_create_delete_calc` | BaseProcCreateAndDeleteThread's calc_ |
| `801be6e0` | `thread_resource_calc` | Resource Control/Memory and ARes Loading's calc_ |
| `836dda20` | `thread_parallel_decomp_calc` | ParallelDecompThread's calc_ |
| `836e9140` | `thread_general_format_calc` | GeneralFormatAsync_ and SceneLowPriority's calc_ |
| `80e557a0` | `thread_bootup_background_calc` | BootupBackgroundWorkThread's calc_ |
| `80e6dfd0` | `thread_textbox_texture_calc` | UpdateTextBoxTextureThread's calc_ |
| `836f51d0` | `thread_audio_framework_calc` | AudioFrameworkThread's calc_ |
| `836f17a0` | `thread_audio_user_task_calc` | AudioUserTaskThread's calc_ |
| `836f4410` | `thread_opus_decoder_calc` | OpusDecoder's calc_ |
| `8370bbc0` | `thread_aal_worker_calc` | aal::PathFindingThread and aal::LowPrioWorkerThread's calc_ |
| `805378b0` | `thread_play_reporter_calc` | PlayReporter's calc_ |
| `83489ad0` | `thread_vibration_calc` | VibrationThread's calc_ |

## Local files (work/proto/boot, not committed)

| File | What |
|---|---|
| `d1`..`d5/` | Ghidra output of the functions above |
| `tools/calls.py <addr>...` | a lifted function's direct calls, tail calls and indirect calls with their lr |
| `tools/s.py`, `tools/vt.py` | a string, a vtable from the image |
| `tools/byml.py`, `heapdef.json` | the HeapDef table |
| `tools/xref.py` | data words and code references to a function |
| `tools/blocks.py <run>` | each thread's blocking sites in a `HOS_DBG=crit` trace |
| `tools/order.py order1.txt` | the order trace grouped by the main thread's phases |
| `gdb/hookshot`, `gdb/tree.py`, `gdb/order.py` | the gdb wrapper (`EXE=` for `scripts/run-vk.sh`, `BT_GDB=` picks the script) |
| `tree1.txt`, `order1.txt` | the two runs' output |
