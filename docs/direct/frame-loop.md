# The game's frame loop

This page describes how one frame of the game runs on the CPU: which threads do what, where the render jobs and layer
lists are recorded, and how the frame reaches the GPU. Read it before you profile frame time or place a patch or timing
mark in the frame. For the path from process start to the first frame, see [boot to the main loop](boot-to-main-loop.md).

The function names below are labels from `games/oot/labels.txt`; search that file for a label to find its address.

## The threads that do the frame's work

Steady play has about 84 guest threads, but the frame's work runs on a few of them:

| Thread | Work |
|---|---|
| `sead::MainThread` | the frame loop; runs a share of every job stage; records the gate primary |
| `ModuleSystemWorker1`-`5` | job stages: calc jobs, render jobs, layer lists |
| `BaseProcExecutor0`-`5` | game logic: actor calcs, physics and cloth tasks |
| `NavMeshDepWorker2`, `NavMeshCAStepDepWorker1` | navigation |
| `Presentation Thread` | submits the frame and presents |

The other threads (resource loading, audio, decompression, UI text) sleep in their message queues most of the frame.

## One frame on the main thread

The main loop (`frame_main_loop`) calls the framework's frame function forever. A frame is, in order:

1. **Frame start** (`frame_global_step`): the display rate, the frame-rate controller and the game step (60 / the
   presentation interval).
2. **Calc** (`frame_proc_calc`): the calc method tree. Its center is the ModuleSystem's `module_calc`, which runs three
   stages of the module job graph on the main thread and the five workers, and joins them:
   - **Stage A**, game logic on the BaseProc executors.
   - **Stage B**, short; its content is not identified.
   - **Stage C**: the calc jobs (culling: `job_calc_runner`), then the **render jobs** (`render_job_record`, about 17
     rounds of six), a second BaseProc step, then the **layer lists**, including the 3D pipeline.
3. **Wait and acquire** (`frame_wait_present_acquire`): wait until the Presentation Thread finished the previous
   present, then `nvnWindowAcquireTexture`. The host's display pacing acts here (`no_pacing` turns it off).
4. **Draw** (`frame_proc_draw`): record the gate primary. It begins with a GPU wait on the gate event, records the queue
   heads and one `CallCommands` of the nine layer lists recorded in stage C, then the final composite to the window
   texture. Then it sends message 1 to the Presentation Thread.
5. **Frame end** (`frame_end_wait_and_gate`): start the frame-end stage (the next frame's game logic, which runs on into
   the next frame), wait for the previous frame's fence and for the window or vsync, then signal the gate event from the
   CPU with `nvnEventSignal(value 1)`.

So **render jobs and layer lists are recorded in the calc phase**, not the draw phase. The draw phase only records the
gate primary and calls the lists.

## The Presentation Thread

The Presentation Thread (`frame_present_submit`) waits on the GPU for the window, submits the async-compute lists, the
queue heads and the gate primary, places a fence, then polls the gate event in 1 ms sleeps until the main thread's
signal. Then it presents (`frame_queue_present`) and signals the present-done event that step 3 of the next frame
waits for.

In the back end, everything is recorded by the time the Presentation Thread submits. The submit only orders the chunks
and hands them to the sender thread; see [the back end's architecture](architecture.md#submission).

## Inside stage C

Stage C's task graph is a chain of six-wide phases, one node per thread, each joined before the next opens:

```
calc jobs -> join -> render-job loops (x6) -> join -> layer lists (x6) -> join
```

The layer phase opens only when every render-job loop has ended. That is the game's order, not a data dependency: the
job-call sites do not wait for their jobs. One layer, the 3D layer, is much longer than the others and records alone at
the end of the stage (about 2 ms of 7.4 in the market). The `HOS_LAYER_AHEAD` patch starts it during the job phase;
see [render jobs](jobs.md#the-long-layer-runs-ahead).

**The calc jobs** run one function on all six threads. The occlusion grid is built by one thread while the other five
poll with 20 µs sleeps; the shape culling then spreads over all six.

## Inside stage A

Stage A's critical path is the player's calc: one actor update that runs alone for about 1.6 ms in the market while the
other executors wait, because the physics phase and the later actors depend on it. Havok's physics and cloth run as
tasks on the BaseProc executors; see [Havok](havok.md).

## Where to place a mark or a patch

| Phase | Boundary |
|---|---|
| a frame | entry of `frame_global_step` |
| the frame's CPU work ends | the tail `nvnEventSignal(value 1)` of `frame_end_wait_and_gate` |
| a job stage | entry and return of `job_graph_run_and_join` on the main thread |
| one render job | `render_job_record` (already hooked by `games/oot/patches/eng_jobs.c`) |
| present wait and acquire | `frame_wait_present_acquire` |
| present | `frame_present_submit` on the Presentation Thread |

To see the frame's critical path, run with `debug.crit`: it writes `crit.bin` in the run folder, and
`tools/crit_path.py` reads it.
