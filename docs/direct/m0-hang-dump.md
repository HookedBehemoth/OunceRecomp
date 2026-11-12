# M0: the first devcoredump of the task/mesh hang (2026-11-12)

Analysis only; nothing was run. Inputs: `work/runs/devcoredump_h1.txt` (amdgpu devcoredump, Hyrule Field, native task +
mesh, time 755.77, pid 380965 thread `mkw_vk order`, vmid 7), `work/runs/m0_dump_h1/{kernel,log}.txt` (grep only).
Decoders (PM4 opcodes from `sid.h`, register fields from Mesa's `gfx103.json`; the three CP stall registers Mesa has
no GFX10 table for were checked against a local kernel tree's `gc_10_3_0_sh_mask.h`): `work/proto/direct-m0/coredump/{pm4,regs,hqd}.py`. Mesa: `../mesa` is **main
(27.0.0-devel)**, not the host's 26.2.4. The shallow clone still has the `mesa-26.2.4` tag, so the 26.2.4 code quoted
below was read with `git show mesa-26.2.4:<path>`.

## 1. Decoded state

**Rings** (rptr/wptr in dwords; ring size 2048 dw, so wptr is taken mod 0x800):

| Queue (dump block) | rptr | wptr mod size | State |
|---|---|---|---|
| gfx_0.0.0 (RB0, **guilty**) | reg 0x54d (header 0x500) | 0x700 | 2 jobs pending: **job A** at 0x500 (seq 48381 = 0xbcfd, 4 IBs), job B at 0x600 (48382, 2 IBs) |
| gfx_0.1.0 (RB1, other process) | 0x615 | 0x000 | blocked behind RB0: it shares the 3D engine (victim) |
| comp_1.3.0 (mec0 pipe3 q0, IB_VMID 7) | 0x400 | 0x400 | **drained**; last IB 0x8008_78904000, IB_RPTR 0x10; EOP rptr = wptr 0x178, EOP_EVENTS 0 |
| comp_1.1.0 (mec0 pipe1 q0, IB_VMID 7) | 0x500 | 0x500 | drained; EOP rptr = wptr 0x040 |
| other comp_1.x, kiq | rptr = wptr | | idle (other VMIDs or empty) |

**Job A on gfx_0.0.0** (all packets decoded): `COND_EXEC`/`WAIT_REG_MEM` (the kernel's gfx-pipe handshake on reg
0x3898), `CONTEXT_CONTROL`, `FRAME_CONTROL`, then four `INDIRECT_BUFFER`s: **IB#0** 0x8008_c0364000 56 dw (in every job
of ours: RADV's queue preamble), **IB#1** 0x8008_788c8000 16 dw, **IB#2** 0x8008_78850000 3648 dw (the command buffer),
**IB#3** 0x8008_788dc000 16 dw, then `RELEASE_MEM`s for the fence. Four IBs with two 16-dw IBs around the command buffer
match RADV's gang submit (leader preamble `WRITE_DATA sem[0]=1`, cmdbuf, leader postamble `WAIT_REG_MEM PFP sem[1]>=1`).
INFERRED from the sizes, since the IB contents are not in the dump.

**Key registers** (all from the dump):

| Register | Value | Meaning |
|---|---|---|
| CP_RB0_RPTR | 0x54d | the PFP has consumed the ring up to just after IB#2's packet; IB#3 (the postamble) **not yet fetched** |
| CP_IB1_BASE / BUFSZ / CMD_BUFSZ | 0x8008_78850000 / 896 left / 3648 | the CP is **inside IB#2, the application command buffer**; the PFP has prefetched 2752 of 3648 dw |
| CP_IB2_BASE / BUFSZ / CMD_BUFSZ | 0x8001_00025000 / 0 / 168 | a 168-dw IB2 (a secondary: `vkCmdExecuteCommands`), fully fetched |
| GRBM_STATUS | 0xe7f10028 | GE, SPI, SC, PA, DB, CB, SX, BCI, CP busy; **TA idle** |
| GRBM_STATUS_SE0,1,3 / SE2 | 0x0d000000 / 0xed400000 | PA, SX, SPI busy on all four SEs; SE2 also has SC, DB, CB, BCI busy |
| CP_STALLED_STAT2 | 0x00210802 | **GFX_CNTX_NOT_AVAIL_TO_ME** (every context is held by draws in flight), **QU_STALLED_ON_EOP_DONE_PULSE** (an end-of-pipe event waits for the pipe to drain), PFP/MEQ back-pressure (GFX10.3 field names, checked against `gc_10_3_0_sh_mask.h`) |
| CP_BUSY_STAT | 0x00448080 | PFP parsing, GFX context busy, EOP done busy; no SEM_* (semaphore) bits |
| CP_CPF_STALLED_STAT1 | 0x3 | fetcher waiting on ring/IB1 data (back-pressure) |
| CP_CPC_STATUS / CPC_BUSY_STAT / CPC_STALLED_STAT1 | 0 / 0 / 0 | **the compute CP (MEC) is idle** |
| GCVM_L2_PROTECTION_FAULT_STATUS, *_UTCL1_STATUS | 0 | no VM fault (the "page fault at 0x0" line is the empty header) |

The dump has **no wave state** (no SQ wave registers or PCs). It names user IBs, but all four IBs read as **zeros**
(the kernel could not read them), so the draws and the ACE packets cannot be decoded.

## 2. Answers

1. **The graphics ring is stuck, inside the application command buffer, with the 3D pipe holding draws that never
   drain.** The ME is not polling memory. It is starved of contexts and waits for an SC end-of-pipe. The ACE (comp_1.3.0,
   the vmid-7 queue whose last IB sits beside job A's IBs) has consumed its ring and retired every EOP event: it is
   idle and done. This is not "ACE waits for a semaphore graphics never writes", and not "graphics waits on ACE".
2. No wave state in the dump. Indirect: the MEC is idle and its EOP queue is empty, so **no task wave is running**
   (a CS-done EOP would still be pending). SPI busy + TA idle on the gfx side fits waves that hold resources without
   fetching memory, or a GE/SPI that waits to launch.
3. Only the kernel rings and the register snapshot are usable; the user IBs are zeros. **Not determinable**: which
   draw (program, task or not), the IB2's contents, the ACE dispatch count. To get them: per-draw markers on the gfx
   ring (our `RX_VK_CHECKPOINTS=1` AMD markers, which `eng_job.cpp`'s secondaries do not emit yet), or
   `RADV_DEBUG=hang` (crashed on 26.2.4; try it on main), or `umr --waves` during the hang (needs root and a longer
   lockup timeout).
4. RADV's waits (same in 26.2.4 and main): (a) ACE preamble `WAIT_REG_MEM sem[0] >= 1` (preemptable), released by the
   leader preamble; (b) gfx postamble `WAIT_REG_MEM PFP sem[1] >= 1`, released by the ACE postamble's bottom-of-pipe
   `RELEASE_MEM`; (c) in-cmdbuf `radv_wait_gang_leader` on the ACE for barriers whose dst has TASK or DRAW_INDIRECT.
   In a graphics command buffer the leader **never** waits for the follower (`gang_follower_flags = 0`). (d) Implicit:
   each `DISPATCH_TASKMESH_GFX` makes the firmware wait for the draw-ring ready bit that invocation 0 of each task
   workgroup writes (`ac_nir_lower_taskmesh_io_to_mem.c`: "If the task shader doesn't write this bit, the HW hangs").
   **(a)-(c) are not pending in this dump; (d) cannot be ruled out.** NIR appends a `launch_mesh_workgroups(0)` to every
   task shader, and all task waves finished, so (d) needs a count mismatch between the gfx and ACE packets. RADV emits
   them in pairs, also through secondaries. The MEC "indirect count 0" bug is off: MEC fw 0x86 = 134 >= 100.
   **Main vs 26.2.4**: the gang semaphore BO is now GL2-bypassed on every IP ("the gang wait preamble may run before a
   cache flush ... stale values"; in 26.2.4 only with an SDMA leader). That fix targets waits (a)/(b), a stuck ACE
   preamble or a stuck gfx postamble. It fits the **comp_1.x-named losses** of earlier runs, **not this one**.
   `radv_gang_barrier` was also rewritten (fallbacks.md 3c).

## 3. Confirmed vs inferred

- CONFIRMED (registers): gfx inside IB#2 of job A, postamble not fetched; 3D pipe busy with contexts exhausted; MEC
  idle; both vmid-7 compute queues drained with empty EOP queues; no VM fault; no wave or IB data.
- INFERRED: IB#1/#3 are the gang pre/postamble; comp_1.3.0 is job A's follower; the IB2 is one of our secondaries
  (`vk_replay.cpp` executes engine-job secondaries).
- Against earlier hypotheses: the **B/B' runaway task loop is not this event** (no task wave alive; and the clamps did
  not help). A gang-semaphore deadlock is not this event.

## 4. Verdict

A **graphics-side draw that never completes**, most likely a **native mesh-stage draw** (the variable that differs:
the fallback replaces the mesh stage too, not only the task stage). Two readings remain: (i) **shader/data or our
SPIR-V**: a mesh workgroup that does not finish its NGG exports (output counts above the declared max of 126/126,
`SetMeshOutputsEXT` misuse, a loop bounded by payload words) or a task payload the mesh misreads; (ii) **driver or
firmware**: the draw ring waits for an entry the ACE never produced. Confidence: high that the gfx ring is the stuck
one and that the ACE had finished; medium that it is a mesh draw; low between (i) and (ii). A Mesa-main test is worth
it later for the comp_1.x-named losses (the GL2-bypass fix), but it is not expected to fix this signature.

## 5. Next experiment (one loss, user captures the dump within 5 min)

Name the draw. Add the AMD buffer markers (`RX_VK_CHECKPOINTS=1` path, `vk_draw.cpp` marker words) to the draws
recorded in `eng_job.cpp` secondaries, with the program key and task/mesh flag. Run m0_n_h1's configuration with
`RX_VK_CHECKPOINTS=1`; the user captures the devcoredump. Read: the first marker reached but not finished is the stuck
draw. If it is a task+mesh program, dump its mesh SPIR-V output counts against `max v/p 126` and test (i) offline with
`tmtool`; if it is not a mesh draw, the mesh hypothesis is wrong. Risk: markers changed timing on NVIDIA (design.md
S4). If the hang vanishes with markers, spend the second loss on Mesa main + `RADV_DEBUG=hang` (private prefix,
fallbacks.md 3c step 3) for the IB dump of both rings.
