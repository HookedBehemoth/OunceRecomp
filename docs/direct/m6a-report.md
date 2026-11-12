# M6a report: where a frame's time goes on the critical path (2026-11-12)

Branch `agent/m6a` (from native-vk af88d00). Runs `work/runs/m6a_*` (local), RX 6950 XT, RADV, `RUN=timing`, one game process
at a time, `RX_BACKEND=direct`, pacing off (`HOS_NO_PACING=1 HOS_FPS_TARGET=1000 HOS_FPS_MAX=240`), Kokiri `chisel_kokiri`,
Hyrule Field (HF) `chisel_hf`, as M4c ran them. A measurement stage: nothing was optimised. No device was lost. Tests: 292 pass
/ 5 fail (the baseline five).

## Summary

- **The frame is CPU-bound in both scenes, and its period is set by sead::MainThread's frame loop, not by the present.** The
  main thread starts the next frame's jobs right after its CPU event signal (`nvnEventSignal`, value 1) at the end of the
  frame. The present runs beside it on the Presentation Thread and gates nothing (measured: an A/B that removes the
  present's poll latency gave no gain).
- **The critical path is a chain through the game's job threads** (about 50-80 thread changes a frame). For the median
  frame it has four phases: game logic jobs on the six BaseProcExecutor threads, calc jobs on main and the five
  ModuleSystemWorker threads, the render recording jobs (ending in one long layer list recorded by one thread), and the main
  thread's submits and CPU event signal.
- **The back end's time on the path** (traced, slightly inflated, see Method) is **2.24 ms in Kokiri and 2.88 ms in HF**.
  About 2.0 and 2.6 ms of that is real once the tracer's own cost is taken out. The largest items:
  - the emits and calls of one layer list (BeginRecording returns at `0x8007b0c8`), which one worker records alone: 0.83
    ms (Kokiri) and 1.31 ms (HF);
  - the vkQueueSubmit2 inside the main thread's CPU event signal: 0.57 / 0.53 ms, 88 % of it in the amdgpu CS ioctl;
  - the job lists' emits and packets on the path: 0.59 / 0.72 ms.
- **The game's own time on the path is 9.9 / 8.6 ms, and waits are 0.74 / 0.51 ms** (virtual-core waits 0.44 / 0.28).
- **GPU:** busy 7.5 ms (Kokiri) and 8.6 ms (HF) of each frame. The queue idles 4.9 / 3.4 ms a frame waiting for the CPU. The
  CPU waits 0.004 ms a frame for the GPU.
- **Distribution, frames-only runs:**

  | Scene | Steady play | mean | p99 | max | Frames over 16.67 ms |
  |---|---|---|---|---|---|
  | Kokiri | 124 s | 12.19 ms | 14.00 ms | 18.4 ms | 0.02 % |
  | HF | 134 s and 147 s | 12.02 ms | 15.18 ms | 23-44 ms | 0.45-0.56 % |

  HF's slow frames come in bursts of 4-12. The warm-up run's HF windows had a p99 of 13.0-13.3 ms. The 60 FPS headroom at
  p99 is therefore 2.7 ms in Kokiri and 1.5-3.6 ms in HF.

## Method and how far to trust each number

| Source | What | Cost to the frame (checked how) |
|---|---|---|
| `HOS_DBG=crit:<first present>:<presents>` (new, `host/direct_crit.{c,h}`) | every guest thread's blocks (what it waited on, its guest caller), its waits for a virtual core, the wakes of the objects waited on (condition variables, the arbiter, virtual cores handed over), and every call into the direct back end (record emits merged into bursts), as TSC spans over 600-1500 presents; `scripts/crit_path.py` walks the critical path | window frames 12.89 ms against 12.15 without (Kokiri, +6 %), 12.04 against 11.65 (HF, +3 %). The back end's own emit counter rose 4 % (2.64-2.69 against 2.55 ms a frame); the trace's emit spans are 12 % above the back end's counters (about 30 ns of wrapper per call), so the back-end items below are about 10 % high |
| `HOS_DBG=frames` (new) | every present's interval in `direct_frames.csv` | Kokiri 12.10-12.21 ms against 12.09-12.18 without: none measurable |
| `HOS_DBG=gputime` (new, `direct_sync.cpp`) | every batch submit and every present bracketed by two timestamps (top of pipe before its first command, all commands after its last): GPU busy time as the union of the brackets, idle gaps between them | +0.3 ms CPU: the extra command buffers make vkQueueSubmit2 slower (hsd_submit 0.34 against 0.056 ms a frame, vkQueueSubmit2 0.84 against 0.54). GPU numbers are taken with it, CPU numbers without |
| counters added to the `submits` line (always on) | waits for the submit lock, the present's fence wait | none: two clock reads per submit or present |
| Superluminal (`m6a_sl_k`, `m6a_sl_hf`, 30-35 s of steady play) | the game's functions by thread, the back end's functions, the kernel's share | frames 14.1 ms (Kokiri) and 13.6 ms (HF): +17 %. Its ms are given per frame of the capture, so read them as shares |

How the critical path is walked (`scripts/crit_path.py`). A frame runs from the end of one of the main thread's value-1 CPU
event signals to the end of the next. The path is walked backwards from that end:
- running time is taken as it is;
- a block ended by another thread's wake jumps to the waker at the moment of the wake (condition variable, arbiter, or a
  virtual core handed over);
- a run of the job system's 20 µs poll sleeps jumps to the thread that had been running longest when the poll ended. This
  is a heuristic: the job a poll waits for is not in the trace;
- any other block (a timed wait, a host wait) stays on its thread as a wait.

The poll heuristic is the weakest part: it moves time between the thread groups of the job phases, not between game and
back end. Two Kokiri trace runs agree within 0.1 ms on every category (`m6a_crit_k` with gputime, `m6a_crit2_k` without).

The profiler and the counters agree:
- hsd_emit + hsd_list_call: 4.59 ms a frame under the profiler against 4.25 ms from the counters (2.55 + 1.70), a ratio of
  1.08 against the profiler's 1.17 slowdown;
- the CPU event signal: 0.53 ms (profiler) against 0.57 ms (trace).

One disagreement: hsd_submit is 0.027 ms in the profiler against 0.056 ms from the counter. It is small either way and was
not pursued.

## 1. The critical path, median frames

Per scene, steps over 0.05 ms, from the frame's start. "back end" is the back end's share of the running time. The median
frame is the trace frame nearest the median (`crit_path.py --dump -2 --merged`, `m6a_crit2_*`).

**Kokiri (frame of 12.73 ms; trace mean 12.89):**

| ms | Phase | Thread(s) | back end |
|---|---|---|---|
| 0.00-4.45 (4.45) | game logic jobs: BaseProc executor `f_8004ab40` | BaseProcExecutor 0-5 handing the path between them | 0 |
| 4.52-7.49 (2.97) | calc jobs: job runner `f_800998a0` (the CPU culler `f_8009bb80` → `f_812342c0`, the grid occluder `f_80822260` → `f_808226e0`, others) | main, ModuleSystemWorker 1-5 | 0 |
| 7.49-11.92 (4.42) | render recording: job lists (`f_80084de0`), then the long layer list (BeginRecording → `0x8007b0c8`), recorded by one worker alone | ModuleSystemWorker5 | **2.20** |
| 11.96-12.07 (0.12) | the main thread's submits (compute and head lists, gate) | main | 0.04 |
| 12.08-12.73 (0.65) | `nvnEventSignal` value 1: the held chunks sent (vkQueueSubmit2) | main | **0.58** |

**HF (frame of 11.97 ms; trace mean 12.04):**

| ms | Phase | Thread(s) | back end |
|---|---|---|---|
| 0.00-3.79 (3.79) | game logic jobs | BaseProcExecutor 0-4 | 0 |
| 3.89-6.54 (2.65) | calc jobs, NavMesh worker | main, ModuleSystemWorker1, NavMeshCAStepDepWorker1 | 0 |
| 6.54-11.22 (4.68) | render recording, including a 0.48 ms wait of the NavMesh worker for a virtual core in the middle; the long layer list on one worker | ModuleSystemWorker1 | **2.36** |
| 11.26-11.35 (0.08) | submits | main | 0.04 |
| 11.44-11.97 (0.53) | `nvnEventSignal` value 1 | main | **0.51** |

Averaged over the trace (599 frames each), ms a frame:

| On the path | Kokiri | HF |
|---|---|---|
| frame (trace) | 12.89 | 12.04 |
| running, BaseProcExecutor threads | 4.12 | 3.66 |
| running, ModuleSystemWorker threads | 4.56 | 5.11 |
| running, sead::MainThread | 1.69 | 2.15 |
| running, NavMesh worker | 1.35 | 0.27 |
| running, other threads | 0.43 | 0.34 |
| of the running time, the back end | **2.24** | **2.88** |
| **the game's own running time** | **9.91** | **8.65** |
| waits for a virtual core | 0.44 | 0.28 |
| other waits (timed waits, unmatched blocks, wake latencies) | 0.27 | 0.23 |
| poll latency | 0.03 | 0.00 |

**The game's serial part, by function** (Superluminal, run time summed over a phase's threads, ms per profiled frame; these
are shares, not path times):
- **Game logic, BaseProcExecutor threads** (Kokiri / HF):
  - `f_800b3ce0` → `f_82c3fab0` → `f_82c415e0` → `f_80186a00` / `f_80168320`: 2.83 / 1.62
  - `f_8016b320` → `f_8016b620` → `f_81b47080`: 1.62 / 1.82
  - `f_8042b0c0`: 1.46 / 1.48
  - `f_82c3d740`: 1.32 / 0.44
  - `f_8073a4a0`: 0.59 / 0.95
  - `f_8191ee00`: 0.70 / 0.58
  - In all, 20 / 14 ms of run time across the six threads, spread over many functions.
- **Calc jobs:**
  - `f_8009bb80` (1.56 / 1.81), with the culler `f_812342c0` (0.64 run, 0.77 inclusive over all threads)
  - `f_808226e0` (0.39)
  - HF also `f_83647b20` (0.35)
  - The job system's poll sleeps (`f_800998a0` → `SleepThread` → `sched_yield`) burn 3.2 / 1.6 ms of host CPU, with no
    virtual core held.
- **Render recording:**
  - the job recorder `f_80084de0`: 4.6 ms over the threads, back end included;
  - the long layer list: its recording job is `f_8006cd60` → `f_8006e160` → `f_80a47660` → `f_80076cd0` → `f_806602e0` →
    `f_80660320`, 2.3 / 2.6 ms with the back end. That list has 3,344 records (Kokiri) and 6,002 (HF), and its begin-to-end
    wall time is 2.70 ms (p99 3.33) and 3.02 ms (p99 3.30) a frame;
  - inside it the host's `nn::os` mutex lookup `obj_lookup` takes 0.29 ms over all threads.

## 2. Back-end time on the serial thread(s)

**On the critical path,** ms a frame (`m6a_crit2_*`; emits and calls about 10 % high, see Method):

| Group | Kokiri | HF |
|---|---|---|
| emits into the long layer list (`0x8007b0c8`) | 0.62 | 1.05 |
| calls in that layer list: job secondaries executed and packets expanded | 0.21 | 0.26 |
| emits into job lists (`0x8008533c`) | 0.33 | 0.44 |
| packets expanded inline in jobs (`hsd_list_call` from jobs) | 0.25 | 0.28 |
| emits into compute lists | 0.09 | 0.14 |
| emits into the gate and head lists | 0.02 | 0.03 |
| list begin / end / released (begin includes the slot's command-pool reset at a thread's first list) | 0.07 / 0.01 / 0.02 | 0.07 / 0.01 / 0.02 |
| submit (`hsd_submit`, the deferred-word copy included) | 0.03 | 0.04 |
| **CPU event signal**: `progress` → `batch_flush` → one vkQueueSubmit2 of the chunks held at the game's CPU gate; 0.46 of 0.52 ms in the kernel's CS ioctl (profiler) | **0.57** | **0.53** |
| sync waits (`nvnSyncWait`) | 0.001 | 0.001 |
| present (0.18 ms a present, on the Presentation Thread) | off the path | off the path |
| **all** | **2.24** | **2.88** |

- **Inside the emits** (profiler, all threads, ms per profiled frame, inclusive):
  - pass_emit 3.90 → draw_emit 1.96;
  - ubo_address 0.40, state_emit 0.40, query_op + query_draw 0.45, bind_data 0.47;
  - shader_pick 0.15, compute_state 0.12, heap_texture_registered 0.12, set_dynamic 0.12, set_vertex_input 0.08,
    layer0_span 0.08.
- **Passes:** 109 renderings begun a frame (Kokiri). Pass begin and end are inside pass_emit and are not split out further.
- **Locks:**
  - the submit lock: 0.002 ms a frame of waits;
  - virtual cores: 0.44 / 0.28 ms on the path. The NavMesh worker waits 3.8 / 2.3 ms a frame for a core in all. The
    better-priority job threads keep taking it back: they kick it at their 20 µs polls;
  - the game's own mutexes (`mutex_lock`): 0.26 ms inclusive over all threads.
- **The four holds** are held on the GPU timeline, not on any CPU thread: 0.23-0.43 ms a frame held, the longest 1.4-3 ms.
  They are the game's CPU gate: the waits of the two queues on the event the main thread signals twice a frame (value 0
  after the frame's recording, value 1 at its end). It signals as late as it does by design, and the CPU never blocks on
  it. What the CPU pays is that the signal's thread submits the released chunks (the 0.53-0.57 ms above).
- **On the parallel threads,** in all, the back end's time per thread a frame (Kokiri / HF):
  - main: 1.61 / 1.77;
  - ModuleSystemWorker 1-5: 0.80-0.97 / 1.00-1.19 each;
  - Presentation Thread: 0.23 / 0.25, off the path.

  The main thread waits for the ModuleSystemWorker threads in the recording phase, so only the path's share counts: the
  0.59 / 0.72 ms of job-list emits and calls in the table.

## 3. GPU side

`m6a_crit_*` (`HOS_DBG=gputime`, 600-present windows; frame 12.45-12.5 / 11.9 ms with gputime's own +0.3):

| | Kokiri | HF |
|---|---|---|
| GPU busy a frame (the union of the submits' brackets) | 7.49-7.56 ms | 8.52-8.58 ms |
| GPU idle a frame | 4.85-5.2 ms in 4.0 gaps | 3.34-3.40 ms in 4.0 gaps |
| the present's fence wait (CPU waits for the GPU, 3 frames back) | 0.004 ms a frame | 0.004 ms a frame |
| CPU fence waits (`nvn_wait`), upload waits | 0 | 0 |

Both scenes are CPU-bound. The GPU waits for the CPU four times a frame: at the CPU gate's two waits, and between the
batches. Halving the CPU frame would make HF GPU-bound at about 8.6 ms (116 fps). The GPU work of the task steps and
reports is inside the busy time and was not split out.

## 4. What the 2.9 ms added since "nothing drawn" consists of

M1 measured 9.2 ms (Kokiri) and 9.4 ms (HF) with nothing drawn. Now, without tracing, the frames are 12.15 and 11.65 ms,
so +2.95 and +2.25 ms. The M1 build no longer exists, so this is the decomposition of today's path, not an A/B.

| Item | Kokiri | HF | |
|---|---|---|---|
| recording on the path (emits, packets, begin/end; about 10 % trace cost taken out) | ≈1.45 | ≈2.05 | measured now; M1 only counted the records (estimated ≤0.2 on the path) |
| the CPU gate's vkQueueSubmit2 in the main thread's event signal | 0.57 | 0.53 | measured now; M1 sent empty submissions (hsd_submit 0.034 ms a frame) |
| submit, sync: the deferred words, the slot resets | ≈0.1 | ≈0.1 | measured |
| **back end, new on the path** | **≈1.9-2.1** | **≈2.5-2.7** | |
| the rest | ≈0.9-1.1 | ≈-0.3-0 | estimated, not measured. It would be the game's own growth: real occlusion counts for the culler since M5 (M5 measured `f_812342c0` at 1.28 ms a frame), more virtual-core contention as the back end holds cores longer, cache effects. HF's M1 figure had a p99 of 11.2-11.6, so its residual is within noise |

## 5. Frame-time distribution (pacing off, `HOS_DBG=frames`)

| | Kokiri (`m6a_dist_k`) | HF (`m6a_dist_hf`) | HF (`m6a_dist2_hf`) |
|---|---|---|---|
| steady play | 124 s (presents 9000-19200, 10,201 frames) | 134 s (11,124 frames) | 147 s (12,216 frames) |
| mean / p50 | 12.19 / 12.10 ms | 12.03 / 11.83 | 12.02 / 11.83 |
| p90 / p99 / p99.9 | 12.75 / 14.00 / 15.61 | 12.77 / 15.18 / 21.09 | 12.74 / 15.18 / 19.01 |
| max | 18.41 | 23.02 | 43.96 |
| over 16.67 ms | 2 (0.02 %) | 62 (0.56 %) | 55 (0.45 %) |

The HF warm-up run (`m6a_warm_hf`, no switch) had 600-present windows of 11.51-11.70 ms mean and 12.5-13.3 ms p99, with
none of the bursts. Whether `HOS_DBG=frames` or the scene's state makes the difference is not settled. The switch costs
one `fprintf` a present, and Kokiri showed no cost.

**The worst 1 %** (trace windows, compared with the median 10 %):
- **Kokiri** (`m6a_crit2_k`, 5 frames, 16.4-21.0 ms; the same in `m6a_crit_k`, 15.3-20.1 ms):
  - the extra time is the game's: BaseProcExecutor running +1.8 ms, main +1.6, ModuleSystemWorker +1.1. The back end's
    emits rose 0.3 ms, and list begin 0.6 ms (a thread's first list in a frame resets its slot's command pool).
  - in the 124 s distribution run the top 1 % recur every 196 presents (2.4 s: presents 9660, 9856, 10052, ...), with
    normal neighbours (12.5 ms). That is a periodic game task.
- **HF** (`m6a_crit3_hf`, 1,500 frames, 14 frames of 14.2-27.2 ms):
  - ModuleSystemWorker running +3.9 ms, of it the back end's emits +0.9 and calls +0.4: more recorded work in the render
    jobs. Main -0.9.
  - the bursts of the distribution runs are runs of 4-12 frames at 16-23 ms. None fell into a trace window whole, so their
    cause is open.

## 6. Ranked list

ms given back to the frame (to the path, not to a thread sum). "Measured" is the item's cost on the path. "Estimated" is
what a change would give back.

| # | Item | Kind | Kokiri | HF | Basis |
|---|---|---|---|---|---|
| 1 | **The long layer list (`0x8007b0c8`, recorded by `f_8006cd60` → … → `f_80660320`) is recorded serially by one thread**: 2.70 / 3.02 ms wall on the path, of which the back end 0.83 / 1.31 | game patch (split its passes over the workers) + back end (cheaper emits) | 1.0-1.5 if split in two (estimated); the back end's share alone is ≤0.83 | 1.2-1.8 split (estimated); back end ≤1.31 | wall and emits measured (trace); split gain estimated as half the wall less the join |
| 2 | **The CPU gate's vkQueueSubmit2 on the main thread** (`nvnEventSignal` → `batch_flush`; 88 % in the amdgpu CS ioctl) | back end: hand the batch to a submit thread, the event word still written after its chunk is sent; or fewer, cheaper submits (why the ioctl costs 0.46 ms is open: the buffer list or the number of IBs) | ≈0.5 | ≈0.5 | cost measured (trace 0.57 / 0.53, profiler 0.53 / 0.48); gain estimated at the cost less a hand-off |
| 3 | **Job-list emits and packet expansion on the path** | back end (the draw path: pass_emit / draw_emit, address tables, bank copies) | ≤0.59, ≈0.3 for a 2x cheaper path | ≤0.72, ≈0.36 | measured on the path; gain estimated proportional |
| 4 | **Virtual-core waits on the path**: the NavMesh worker kicked off its core by the job threads' 20 µs polls | host (virtual-core scheduler: a poll sleep could keep its core away from an equal or better thread, or `HOS_VCORE_BY_PRIO`) | ≤0.44 | ≤0.28 | measured on the path; gain estimated; such a change also alters the game's per-core guarantees |
| 5 | **CPU culling**: `f_812342c0` (0.64-0.77 ms over the threads) and the grid occluder `f_808226e0` (0.39-0.49), in the calc jobs on the path | game patch (the plan's GPU culler, readback2.md 6A) | ≈0.2-0.5 | ≈0.2-0.5 | thread time measured (profiler); path share not separable: estimated |
| 6 | **Compute-list emits** on the path | back end | ≤0.09 | ≤0.14 | measured |
| 7 | **List begin / end / released** on the path (the slot's command-pool reset at a thread's first list) | back end | ≤0.10 | ≤0.10 | measured |
| 8 | **The host's `nn::os` object lookup** (`obj_lookup`) in mutex calls, 0.29 ms over the threads | host | ≤0.1 | ≤0.1 | profiler; path share estimated |
| - | the present's poll latency (1 ms sleeps polling the frame's event) | - | 0 | 0 | off the path: `HOS_PRESENT_POLL=1` A/B (`m6a_poll_k`): 12.30 ms mean against 12.19, no gain |
| - | hsd_submit's deferred-word copy (2,610 words a frame) | back end | ≤0.03 | ≤0.04 | not worth a change |
| - | GPU, fence and upload waits | - | 0 | 0 | the GPU idles 3.4-4.9 ms a frame |
| - | asset preparation | - | 0 | 0 | no upload or texel write in steady play (uploads 0.0 a frame) |

Items 1-3 together are the back end's 2.0 / 2.6 ms. If 2 and 3 are done and 1's back-end half is halved, the frame drops by
about 1.2 ms (Kokiri) and 1.5 ms (HF), to about 11 and 10 ms (estimated). Splitting the layer list is the largest single
item, and it is a game patch.

**60 FPS headroom now** (budget 16.67 ms):

| Scene | p99 | Margin at p99 | Mean | Margin at the mean | Frames over the budget |
|---|---|---|---|---|---|
| Kokiri | 14.00 ms | 2.67 ms | 12.19 ms | 4.5 ms | 0.02 % |
| HF, distribution runs | 15.18 ms | 1.49 ms | 12.0 ms | 4.6 ms | 0.45-0.56 %, all in the bursts of section 5 |
| HF, warm-up run | 13.0-13.3 ms | 3.4-3.7 ms | | | |

Both scenes meet the plan's p99 goal with pacing off. HF's bursts are what would show at 60 fps.

## Code added (measurement only; nothing changes with HOS_DBG unset)

| Part | Where |
|---|---|
| `HOS_DBG=crit[:<first>[:<presents>]]`: the trace. Blocks are named by the blocking call (`hos_vcore_away_why`: condition variable, arbiter, sleep, or a host site id); core waits and core releases go in `vc_acquire` and `vc_yield`; wakes at `hos_thread_wake` and the `nn::os` wakes; the back end's calls through timed wrappers of the host's function table; the CPU event signal, sync waits and presents in `rx_backend.c`. Writes `crit.bin` and `crit_threads.txt` into the run folder at the window's end. The functions the tests cut out (`hos_vcore_away`/`back`, `vc_grant`, `vc_release`, `poke_multiwait`) are unchanged | `host/direct_crit.{c,h}` (new), `host/hos_thread.c`, `host/nn_os_sync.c`, `host/rx_backend.c`, `host/hos.h`, the blocking sites in `host/nvn_native.c`, `nn_fs.c`, `nn_applet.c`, `hos_svc.c` |
| `scripts/crit_path.py`: the path walk, the categories by median, top 1 % and all frames, the back end on the path by list, a frame step by step (`--dump`, `--merged`), a frame as character rows (`CRIT_GANTT=<frame>`) | new |
| `HOS_DBG=gputime`: GPU busy and idle from timestamps around each batch submit and present (a line every 600 presents); `HOS_DBG=frames`: `direct_frames.csv` | `gpu/direct/direct_sync.cpp`, `direct_present.cpp` |
| the `submits` line: waits for the submit lock, the present's fence wait | `direct_sync.cpp`, `direct_present.cpp` |

Profiler captures: `m6a_sl_k.linux` (Kokiri, 70 s) and `m6a_sl_hf.linux` (HF, 80 s), in the session's scratchpad (local).

## Runs

| Run | What |
|---|---|
| `m6a_warm_k`, `m6a_warm_hf` | warm-up and baseline, no switch: 12.09-12.18 / 11.51-11.70 ms |
| `m6a_crit_k`, `m6a_crit_hf` | trace + gputime: the GPU numbers, the first path |
| `m6a_crit2_k`, `m6a_crit2_hf` | trace without gputime: the path tables |
| `m6a_crit3_hf` | trace over 1,500 frames: HF's worst 1 % |
| `m6a_dist_k`, `m6a_dist_hf`, `m6a_dist2_hf` | distributions |
| `m6a_poll_k` | `HOS_PRESENT_POLL=1` A/B |
| `m6a_sl_k`, `m6a_sl_hf` | Superluminal |

## Open

- What the slow-frame bursts in HF are (4-12 frames at 16-23 ms, every 300-800 frames). A trace window that holds one whole
  would say.
- Why one vkQueueSubmit2 costs 0.46 ms in the kernel. Compare its buffer list and IB count with the two cheap ones.
- The job phases' path split between threads rests on the poll heuristic (see Method).
- How large the residual of section 4 really is. Only a run with recording switched off would say, and none exists.
