# M1 report: boot with no picture (2026-11-12)

Branch `agent/m1`. Runs in `work/runs/m1_*` (local), RX 6950 XT, RADV, idle machine (one game process).

## What was built

| Part | Files (lines) |
|---|---|
| Interface | `gpu/direct/direct_api.h` (38): the spec's calls, plus `HSD_LIST_KINDS` closing the enum |
| Device, table, registrations counted | `direct_main.cpp` (281), `direct_internal.h` (95): libvulkan by `dlopen`, every core feature but robust access, one graphics+compute queue and the sparse-binding queue |
| Lists | `direct_lists.{h,cpp}` (38, 187): per-list op counts; kept in order: SIGNAL_EVENT, WAIT_EVENT, REPORT_COUNTER, RESET_COUNTER, calls (callees ref-counted); a table of BeginRecording return addresses with how their lists were used |
| Submit, fences, events | `direct_sync.cpp` (326): see "Syncs" |
| Present | `direct_present.cpp` (281): clears the swapchain image (or an offscreen one when headless) to one colour; the 600-present lines |
| Layer 0 | `direct_pools.cpp` (621) from `vk_pools.cpp` + the dma-buf part of `vk_memory.cpp`; `layer0_windows.h` (202) copied |
| Host | `host/direct.{c,h}` (68, 34): classification, the thread's pending record; `rx_backend.c` +53 (`rx_direct()`, read once, loads the library at the first NVN record); `nvn_native.c` +123; `eng_jobs.c` +12 (the hook stays on in direct mode, `eng_jobs_tag` gives x3 and x25) |
| Build | `nx2/build.py` +29 (block after `if build_vk:`, Linux only, `dx_*.o`, own link and symbol check); `scripts/linux/appimage.sh` +1 |

Syncs: one timeline semaphore on the one queue. An NVN queue's submissions run in order. At run time an event signal writes
the event word in guest memory, a report writes `{4096 for samples passed / 0, tick-clock timestamp}`, and an unmet
WaitEvent holds that queue on the host until another queue's signal or `nvnEventSignal` meets it. A submission that has
run is one empty `vkQueueSubmit2` signalling the next value. A fence is `(queue index + 1) << 40 | sequence`. `nvnSyncWait`
first waits for its submission to be sent, then on the semaphore. `nvnQueueWaitSync` holds the next submission until the
fence's submission is sent.

## Done when

| Item | Run | Result |
|---|---|---|
| 60 s Kokiri, 60 s Hyrule Field (`chisel_hf_market`), `RX_BACKEND=direct` | `m1_d3`, `m1_hf1` | no crash or hang; 3000 presents each; cleared window: headless offscreen clear; `m1_win` (WINDOW=1) made a 1280x720 mailbox swapchain, 60 fps, validation clean |
| Line every 600 presents | all | see the figures below |
| Ceiling: pacing off (`HOS_NO_PACING=1 HOS_FPS_MAX=240 HOS_FPS_TARGET=1000`) | `m1_ceil_kokiri`, `m1_ceil_hf` | Kokiri **9.2 ms** mean (108-109 fps), median 9.1, p99 11.0; HF **9.4 ms** (106.8 fps), median 9.3, p99 11.2-11.6 |
| Validation layer and synchronization validation, both scenes | `m1_val2_kokiri`, `m1_val2_hf` | 0 messages (the loader log shows the layer loaded). The first runs (`m1_val_*`) gave 8 of VUID-VkBufferCreateInfo-size-06409, layer 0's 4 GB windows against RADV's maxBufferSize of 4 GB - 4; fixed in dae4648 |
| Frozen back end unchanged | `m1_after2` smoke vs `m1_gold` | PASS: 0 bad pictures, 0 new log groups; tests 289 pass / 5 fail / 19 skip, the same 5 failures as the baseline (`test_texture_init_barrier`, `test_lift_cleanup_error`, `test_make_game_cache`, `test_parallel_init_order`, `test_recomplauncher_json`) |

Steady frame, Kokiri (HF in brackets): lists 100.5 job (101), 14 layer, 1 gate, 7 compute, 5 head, 0 load (0.1), 0
unclassified. Ops 1259 calls (3062), 430 DRAW_IDX, 43 DRAW_MESH, 83 DISPATCH, 12 DISPATCH_INDIRECT, 124 SET_RT, 141
BARRIER, 80 UPDATE_UB, 2420 BIND_TEX, 2323 BIND_UBO, 19 SIGNAL_EVENT, 25 WAIT_EVENT, 626 REPORT_COUNTER (1766), 619
RESET_COUNTER, 269 RENDER_COND (the full list is in the log). Submits: 6 a frame, 0.034 ms a frame in `hsd_submit` (0.06),
longest 0.06 ms (1.5 ms once in HF). Registrations at 3000 presents: about 58k textures, 190 samplers, 12.5k programs
set, 4.6k pools.

## Settled by evidence

**Classification** (how each return address's lists were used over 3000 frames, `m1_d2`, `m1_hf1`):

| Kind | Return address | A frame | Used |
|---|---|---|---|
| job | `0x8008533c` | 100.5 / 101 | called by layer lists only |
| layer | `0x8006d258`, `0x8007b0c8` | 5 + 4 | called by the gate only |
| layer | `0x8007ba00` (`f_8007b520`) | 5 | **never called or submitted**: not in engine-frame.md's table; taken as layer (a layer job's list) |
| gate | `0x802eb3fc` | 1 | submitted |
| compute | `0x80665580` | 7 | submitted |
| head | `0x80665ffc` (`f_80665980` Begin, from command-lists.md; engine-frame.md gives only Init) | 5 | submitted |
| load 1-6 | `0x804582ac`, `0x80293b14`, `0x806006d8`, `0x80445314`, `0x8044ea20`, `0x80293460` | 0 in play | shape and draw lists called by jobs (about 900 a frame) and layers; vertex lists by load lists; 4-6 never called |
| unclassified | `0x812bd76c` (1, submitted once at boot), `0x834c0600` (1, movie thread), `0x80bd25bc` (7, at load) | 0 in play | named one-shots; left as PACKET |

**What the game needs to run with nothing drawn**: event signals applied in queue order, unmet waits held, and every
report written when its submission runs. About 4 submissions a frame are held at a wait (the CPU gate's waits), each held
12-24 ms. Reports used samples = 4096 and the real tick clock. Other report values were not tried.

**Pacing off** for the ceiling, with existing host switches only: `HOS_NO_PACING=1` (acquire), `HOS_FPS_TARGET=1000`
(raises the game's own limiter at `+0xfc`), `HOS_FPS_MAX=240` (frame-time step). Nothing was added to the back end.

## Deviations from the spec

- `HSD_LIST_KINDS` was added to the kind enum. `hsd_list_released` also drops a list that was begun and never ended
  (Initialize again, Finalize).
- Records go out through a thread-local pending record, as m0-seam 6.4 describes. A record whose command buffer's list
  changed before its flush is dropped and counted; that happens only when one list is recorded on two threads, and it
  logged 0 in all runs.
- Reports and events are written by the CPU when a submission runs, not on the GPU. That is enough at M1; M5 replaces it.
- Not copied (unused at M1): `vk_formats`, `vk_jobs`, `vk_timelines.h`, `vk_vma`, `vk_paths`, `vk_present_rect`. Layer 0 is
  cut to what pool creation does. It has no per-binding path, no shader page table and no BDA window table: M2 adds
  them with the bindings. Layer 0's windows are a 64 KB granule short of 4 GB, so the longest range one window holds whole
  is 2 GB - 64 KB (`layer0_windows.h`).
- The frozen-back-end check ran with `RX_BACKEND=vulkan`, which `run-vk.sh` always sets; it takes the same code path as
  unset.
- `gpu/direct` builds on Linux only (dlopen, udmabuf).

## Open problems

- `0x8007ba00`'s 5 lists a frame are recorded and never used. Before M2, check whether they belong to the frame or are
  wasted work worth a patch.
- An NVN fence is not a timeline value. `nvn_fence`, `nvn_pushed` and `nvn_replayed` return timeline values, which only
  the host's list lifetimes use.
- The ceiling includes the host's recording cost: `cmd_alloc` plus one emit call per record, about 15k records a frame in Kokiri.
  It has not been profiled.
- A wait that is never met holds its queue for good, with no guard (rule 2). None occurred.
