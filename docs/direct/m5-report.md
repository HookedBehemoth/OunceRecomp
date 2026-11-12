# M5 report: reports and render conditions on the GPU (2026-11-12)

Branch `agent/m5` (from native-vk 248a5e5). Runs `work/runs/m5_*` (local), RX 6950 XT, RADV, `RUN=timing`, one game
process at a time; `chisel_kokiri`, `chisel_hf` (dumps), `chisel_hf_market` (timing, validation). Pacing off for timing
(`HOS_NO_PACING=1 HOS_FPS_MAX=240 HOS_FPS_TARGET=1000`). **No device was lost.** Tests: 291 pass / 5 fail (the baseline).

## What was built

| Part | Where |
|---|---|
| **Samples passed** as occlusion queries (precise). A list's ResetCounter starts its counter; while it counts, a query is open in the list's rendering (begun lazily at the first draw, ended at the rendering's end, so a counter spans renderings as stretches); ReportCounter ends the stretch and makes a record: guest address, type, the stretches since the reset (summed). The counter keeps running after a report, as NVN's does | `direct_query.{h,cpp}` (new) |
| **Every report** also writes a timestamp query where it is recorded (`TOP_OF_PIPE` for TIMESTAMP_TOP) | same |
| **Results in guest memory**: at the end of the rendering the report was recorded in (a job's: its caller's rendering; outside a rendering: at once), the queries' results are copied (`vkCmdCopyQueryPoolResults`, 64-bit, wait) into the command slot's buffer and one compute step (`shaders/report_resolve.comp`, shader object, push data, BDA) writes each report's 16 bytes at its guest address through layer 0's window table: `{counter, timestamp}`, timestamps on the guest's 19.2 MHz counter clock on the host tick epoch (VK_KHR_calibrated_timestamps, re-calibrated every 600 presents). Barriers before and after | same |
| Query pools (4096 occlusion, 4096 timestamp) and the record buffer **per command slot** (thread x frame slot), made at a slot's first report, **reset on the host with the slot** (`vkResetQueryPool`) | `direct_cmd.{h,cpp}` +4 |
| Job secondaries begun with `occlusionQueryEnable` (a job without counters of its own runs inside its caller's query); a job with its own counters is executed with the caller's stretch ended (counted) | `direct_cmd.cpp` +2, `direct_pass.cpp` |
| **Render conditions** as `VK_EXT_conditional_rendering` on the counter's low word in guest memory (window buffers get `CONDITIONAL_RENDERING` usage): mode 1 (render if differ) plain, mode 0 inverted. Begun inside the rendering before a gated draw or clear, ended at the rendering's end, before a job's execution, at the next condition or render enable; in a job secondary begun and ended inside it. A clear under a condition is never a load op | `direct_query.cpp`, `direct_pass.cpp` (+25 lines of hooks), `direct_pools.cpp` +1, `direct_main.cpp` +3 |
| Removed: the CPU's report writes at the submit (`write_report`, the REPORT/RESET frame ops), the "drawn under a render condition (not evaluated)" path | `direct_sync.cpp`, `direct_lists.cpp`, `direct_draw.cpp` |
| Debug aids (`HOS_DBG`): `replog` (a frame's reports and conditions read back 4 presents later, with their pools), `condcheck`, `poolof:<a>+<b>`, A/B: `qfixed` (samples read 4096), `qnocond`, `qcputs`. Event holds now say what ended them | `direct_query.cpp`, `direct_sync.cpp`, `direct_pools.cpp` (`layer0_pool_of`) |

## Done-items

| Item | Kokiri (`m5_t_k_real`) | Hyrule Field (`m5_t_hf_real`) |
|---|---|---|
| Reports a frame / written by the GPU | 607.5 / 607.5 (600.8 samples, 588 of them in jobs) | 1765.5 / 1765.5 |
| Occlusion queries / resets / stretches summed / no reset in their list | 615 / 601 / 0 / 0 | 1782 / 1759 / 0 / 0 |
| Flushes / resolve dispatches a frame | 15.6 / 31.4 | 15.5 / 30.7 |
| Conditions a frame / begun on the GPU / gated draws | 268.7 / 258.0 / 332.0 (264 in jobs) | 839.5 / 829.5 / 925.5 |
| Not written, no room, not in layer 0, read before written, after a foreign job's counter | all 0 | all 0 |
| CPU-written report values | **0** (`reports written by the CPU 0.0`) | 0 |
| Frame time, pacing off, mean / p99 (steady blocks) | **12.0-12.1 / 13.9-15.0 ms**; A/B `qfixed,qnocond` 12.1-12.9 / 14.1-15.1 (`m5_t_k_ab`) | **13.4-13.6 / 15.3-15.8**; A/B 13.4-13.5 / 15.3-16.0 |
| hsd_submit on the submitting threads | 0.061 ms a frame (longest 0.15) | 0.075 (longest 0.71) |
| Vulkan objects a frame in steady play | M5's: 0 (pools only at a slot's first report, none logged after present 1200). The total still grows ~100 per 600 presents from command-buffer batches, the same in the A/B run: not M5's | same |
| Validation + sync validation (`m5_val_k`, `m5_val_hf`) | one type only: the known `Undefined-Value-ShaderOutputNotConsumed-DynamicRendering` (10, the layer's limit); no query, conditional-rendering or hazard message | the same: that one type only (10) |

**The readers see real values** (`m5_c_k`, `HOS_DBG=replog`, turn script): standing, 625 samples reports, 467 zero,
counts 11 to 737,080 (`844 1955 33277 11 4295 0 22765 ...`), steady from frame to frame; turned (present 2100), 1,721
reports, 1,384 zero, others 5 to 517,041; of 270 conditions 79 render, turned 81 of 327, back 58-62 of 231-234.
Timestamps arrive 8-30 ms before they are read (one stale slot at 3900: a list recorded and not run, open).

**Turning the camera** (`m5_turn_real` vs `m5_turn_ab`, a shot every 3 presents through two 4 s turns; sheets of
1647-1692 looked at): objects come in at the frame's edge as the camera moves (lanterns, shelves, the hollow's opening),
none appears late in the interior and none blinks. Gated draws are decided on the GPU in the same frame from the
same frame's proxies, so they cannot pop; the CPU culler (`f_812342c0`) reads counts 1-2 frames old (below), and no
late appearance from it is visible at this sampling.

**Per pass against the frozen back end.** The frozen back end evaluates conditions on the CPU from reports it writes
as 4096 (`RX_VK_OCC_FIXED` default): its dump log has all 305 Kokiri conditions in mode 1, 298 on, the 7 off ones the
clear flags (`0x100d16xxxx`, 0). So like with like is direct `qfixed` (4096, conditions on the GPU). The gated passes
are the light accumulation into the lit B10G11R11F 1536x864 target (frozen dumps 230-278 per program). Kokiri
(`m5_dd_k_fx` / `m5_dd_k_real` vs `m2b_fd_kokiri_b`): those rows 0.10 / 0.09, 17.6-17.9 / 17.5-17.8, 6.0 / 6.0, then
2.15 / 2.16; M4d before (`m4d_dd_k`) 0.04, 17.6-17.8, 6.0, 2.14: **unchanged by M5**: the 17.7 rows are the lighting
inputs M4 left (the sky LUT). Final lit means frozen 0.0222/0.0130/0.0041, fx 0.0220/0.0145/0.0041, real
0.0220/0.0144/0.0038, M4d 0.0226/0.0157/0.0049. Real vs fx: median 0.01, mean 0.47, max 11.8. HF (`m5_dd_hf_*` vs
`m2b_fd_hf_b`): the lit rows 12.3 / 9.6 / 9.4 / 10.5 in both, M4d 12.2 / 9.5 / 9.4 / 10.4; real vs fx median 0.01,
max 5.2. Composite Kokiri: real 7.7, fx 7.0, M4d 7.1.

## Report kinds (all the game reports: types 1, 0, 16; `nvn: the game reports counter type`)

| Kind | A frame K / HF | Written |
|---|---|---|
| SAMPLES_PASSED (lr 0x80684f00, in job secondaries) | 601 / 1759 | GPU: summed occlusion queries + timestamp |
| TIMESTAMP_TOP (`f_8074b190` frame-start timer at +0x10; the clear-request flags) | 3.7 / 3.5 | GPU: counter 0, top-of-pipe timestamp |
| TIMESTAMP (`f_8074b190` start +0x28 / end +0x40: the dynamic resolution's GPU time) | 3.0 / 3.0 | GPU: bottom-of-pipe timestamp, counter 0 |

## The CPU readers

All their memory is host memory in layer 0 (`HOS_DBG=poolof`, `replog`): report rings `0x102f647000+0x181000`,
`0x104e5c6000`-like 1 MB pools and `0x100d6c8000` (flags 0x22 / 0x12, CPU_UNCACHED), the culling lists
`0x117cf04000`, `0x117d03c000` and the exposure block `0x11371e6000` and `0x113c895000` (0x24, CPU_CACHED). No VRAM
pool, so no placement fix. Time (`perf`, Kokiri standing, 15 s): `f_812342c0` **1.28 ms a frame**, `f_808226e0` (the
grid's CPU occluder) 0.36, `f_80365f00`, `f_80362ff0`, `f_80822260` about 0.005 each, `f_80357060` and `f_8074b190` ~0.
When: `f_812342c0` and the grid read whatever is in memory, no wait (1-2 frames old: the frame before is usually
still on the GPU); `f_80362ff0` and `f_80357060` read after the game's own `nvnSyncWait` at 0x8042cde0 on the frame
before (readback2.md), which never blocks here (`CPU fence waits 0.0`); the timestamps are read later by the
resolution logic (the targets stayed 1536x864). **We add no CPU wait.**

## Events

Per frame on the CPU, inside `hsd_submit` / `nvnEventSignal`, the guest's thread never blocked: 19 event words written
when the chunk before their signal is sent, 25 waits checked (one CPU read each), and **4 submissions held, all ended
by the game's own `nvnEventSignal`** (2 a frame): its CPU gate, intended. Held 0.3-0.5 ms a frame in all, longest
1-2 ms (M1's 12-24 ms were frames with nothing drawn). A hold parks the submission and returns; hsd_submit costs
0.06-0.08 ms a frame. GPU order is the chunks' (one queue, a full barrier after a wait). Nothing of ours to fix.

## Deviations

- **No predicate compute step and no gating patch:** `condcheck` found the second counter word non-zero in 0 of
  ~270 / 840 conditions a frame and the first word past 32 bits in 0, so conditional rendering on the report's own
  low word is exact. If either ever counts, the compute step of the plan is the fix.
- Reports are written by a compute step, not by a copy straight into guest memory: NVN's word is
  `{counter, timestamp}`, and the timestamp needs the clock conversion and a sum of stretches (copy alone cannot).
- Results are written at the end of the rendering, not at the report: a report read later in the same rendering
  would see the old value (counted, 0).

## Candidate game patches

1. The culler `f_812342c0` (1.28 ms) and the grid occluder (0.36 ms) as GPU work (readback2.md 6A), by measurement.
2. None needed for conditions.

## Open problems

The counter stays open after a report until the next reset (~2% more queries, cheap); one timestamp slot read stale
(a recorded list not run); a report in a job that runs inside a caller's counter with counters of its own would not be
NVN's value (0 seen); the total object count still grows from command-buffer batches (pre-M5).
