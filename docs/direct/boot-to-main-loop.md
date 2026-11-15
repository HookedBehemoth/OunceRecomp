# From process start to the main loop

This page traces the path from the host's `main` to the game's first frame: what the host sets up, what the game
initializes, and where boot time goes. Read it when you work on boot speed or on anything the game sets up once. What
happens in each frame after that is in [the frame loop](frame-loop.md).

Function names are labels from `games/oot/labels.txt`.

## The host

1. The process's `main` (`host/hos_main.c`) reads `hookshot_env.txt`, opens the SDL window on the process thread, and
   starts the game on a thread of its own. The process thread then runs the window's events.
2. The game thread reserves the 512 GB guest window, reads `image.bin` (the lifted image) to its base address, and
   becomes the guest's main thread.
3. It calls the guest's start-up functions in the order the console's `nn::init` uses: the C library's three steps,
   `nnosInitialize`, `nninitStartup`, every module's `DT_INIT`, then `nnMain`. The SDK module's own service set-up is
   not run: every `nn::*` service the game calls is implemented by the host (`host/nn_*.c`). A call of an SDK function
   the host does not implement stops the game and names the function.

Every guest thread runs on its own host thread. There is no scheduler: `hos_cores()` answers 6, the game's own number,
because its per-core arrays have six entries.

## The game's initialization

- **`nninitStartup`** sizes the heap from the system's free memory and sets up the C heap.
- **Static constructors.** The main module's `DT_INIT` runs almost 10,000 constructors (about 85 ms). Most register
  objects in static lists and registries.
- **`nnMain`** (`sys_nn_main`) builds the sead framework: the heap manager, `sead::MainThread`, the file devices (the
  romfs is mounted here), the heap sizes from `System/HeapSize/*.HeapDef.byml`, region and ROM config, and the framework
  object. Its graphics initialization creates the NVN device, the queue, a window of three textures and the
  **Presentation Thread**. Then it calls the framework's `run`, which never returns.
- **`run`** makes the method tree manager and the task manager with the root task, the ModuleSystem, then polls the
  task manager every 10 ms (`frame_startup_loop`).
- **The ModuleSystem's prepare** (`module_system_prepare`) runs inside that loop, on `sead::MainThread`: it creates the
  engine modules and their threads (the worker pools last) and loads `Pack/Bootup`. This is the single-threaded stretch
  of boot, from about 0.6 s to 4.4 s.

The first frame starts at about 4.4 s. From then on, the main loop runs, and the title screen and the scene loads are
ordinary frames.

## The heaps that matter to the renderer

The heap sizes come from the romfs file `System/HeapSize/Product.Nin_Ounce_NVN2.HeapDef.byml.zs`. Two of them set
expectations for the back end:

| Heap | Size | What uses it |
|---|---|---|
| `TextureHeap` (+ `TextureCompactionWorkHeap`) | 2,400 MB (+ 32 MB) | asset textures; see [texture streaming](texture-streaming.md) |
| `DynamicTextureAllocatorMem` | 305 MB on the console | transient render targets; the host enlarges it with the output size (`hos_rt_pool_size`); see [render targets](render-targets.md) |

## From the first frame to the title

The game's sequence is data, not code: `Sequence/FormalSequence.root.ainb` and its modules (title, opening, scene
changes) run as frames go by. Which module and thread run it is not identified.

## Where boot time goes

Boot time is mostly the ModuleSystem's prepare on one thread: resource loads, effect resources and the creation of
the engine's threads. To see it, read the log's time stamps (on by default; `HOS_LOG_TIME=0` turns them off) and run
with `HOS_FS_TRACE=1` to log every file the game opens.
