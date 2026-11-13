# The Hyrule Field hang (2026-11-13)

Branch `agent/hfhang` (from native-vk 1aa8282). RX 6950 XT, RADV Mesa 26.2.4, kernel 7.2.11. Game runs in `work/runs/hfh_*`,
harness runs in `work/meshlab_hf/runs/` (both local: they hold game data). Every statement is marked **measured** (a run or a
file says so) or **inferred**.

## Conclusion

The loss is **not garbage data**. It is a fault of the driver or the hardware with one compiled shader: the mesh stage
`40d0c0768e874600` of program `cfebb562ec378f87` (the ivy of the Kokiri passage) as RADV compiles the translator's SPIR-V
for it. It is reproduced outside the game in 2 seconds, and it ends when that stage is replaced by a hand-written one that
emits the same triangles.

- **Measured:** the losing draw's triangles are ordinary. Three losing frames were captured (`hfh_t_losttest` present 1949,
  `hfh_f_plainfs1` present 1897, and present 3596 of `hfh_a_lost`, three frames before its loss): 525-527 mesh groups,
  about 27,000 triangles, none non-finite, 0.10-0.22 of the target covered in sum, the largest triangle under 310 pixels.
  The task stage run again on the captured inputs places the same number of groups as the GPU's header says (525 of 525,
  527 of 527).
- **Measured:** the game's fragment stage is not needed. With a fragment stage of ours that writes a constant
  (`HOS_DBG=progfs`) the walk lost the device at present 1897 (`hfh_f_plainfs1`).
- **Measured:** standalone reproducer. The captured draw drawn twice in one rendering, first with depth written
  (compare less-or-equal, no colour), then as the game draws it (depth equal, colour written), a constant fragment stage:
  hung at submission 5, 2.0 s (`run_f1897_prepass_simple.txt`). Without the first draw every fragment fails the depth test
  against the cleared depth and the same draw runs 235,573 times in 30 s (`run_t1949_fsreal.txt`).
- **Measured:** it is this stage, not the class. Under the same conditions a larger depth-equal task+mesh draw of Hyrule
  Field whose mesh stage has no spills (`063b4c21712f159c`, 1,102 groups) ran 156,740 times in 40 s
  (`run_hf305_prepass_simple.txt`).
- **Measured:** the same draw with a hand-written mesh stage in place of the translated one (`meshlet_lit.mesh`,
  `MODEL_SLOT=2`: the same 26,829 triangles and the same statistics on the losing frame, `geo_f1897_repl.txt` against
  `geo_f1897.txt`) ran 846,382 times in 40 s (`run_f1897_prepass_repl.txt`).
- **Measured:** what tells `40d0` apart in RADV's compile: of the five depth-equal task+mesh programs of the Hyrule Field
  frame it is the only one whose mesh stage spills registers to scratch memory (8 `scratch_store_dword`, 8
  `scratch_load_dword`; the other four: 0). The hand-written stage: 0. (M6e: it also keeps outputs in RADV's mesh scratch
  ring.)
- **Inferred:** the scratch spills (or the ring together with them) are the trigger, and it needs the draw's fragments to
  pass, that is pixel work behind the mesh waves. Not shown: which of the two, and what the hardware does. No page fault in
  the kernel log at any of these losses; `ring gfx_0.0.0 timeout` about 2 s after the frame.

## What was ruled out, and by what

| Idea | Result | Evidence |
|---|---|---|
| Stale or garbage geometry at the map change | no | the three captures above (**measured**); `HOS_DBG=progprobe` (below): 10,792 draws of the program over 240 s, the clipper took 22,000-30,383 primitives a draw, never more (**measured**, `hfh_p_probe1`) |
| An unusual group count | no | `HOS_DBG=tmlog`: 515-567 groups in the frames before a loss, falling or rising by a few a frame (`hfh_a_lost`, `hfh_t_losttest`) |
| The map change itself | no | two of this stage's three walk losses came in the first walk cycle, before any change back (presents 1949 and 1897, 42-44 s) |
| The game's fragment stage (its light loop) | no | `hfh_f_plainfs1` lost with the constant fragment stage. Its loop is bounded: `min(cluster count, c6[1808]) + 1` trips, the translator's cap above that (read from the other agent's IR print; **inferred** from reading) |
| M6f (textures kept and freed) | **not run**; **inferred** no | the reproducer has no textures, no game process and one draw |

A correction to M6e's report: its isolation runs never ran a fragment. The harness cleared depth to 1 and the lit draws
test depth for equality, so every fragment failed. "Two overlapping mesh draws are not enough" was shown only for draws
without pixel work. `run --prepass` closes that hole.

## Runs

Walk: `OVERLAY=work/overlay/chisel_hf RUN=timing scripts/run-vk.sh <name> 240 -- RX_BACKEND=direct HOS_DBG=<list>
HOS_LOG_TIME=1 HOS_INPUT_SCRIPT=30:LDOWN/6000,37:LUP/12000,55:LDOWN/6000,62:LUP/12000,...(every 25 s to 205)
HOS_INPUT_EXCLUSIVE=1`; each run's `command.txt` has its exact line. K = `cfebb562ec378f87`.

| Run | HOS_DBG | Result |
|---|---|---|
| `hfh_a_lost` | `marks,meshcaplost,meshcapprog:K,tmlog:K` | **lost** at present 3599 (524 groups). The capture was cut by a second thread's abort after one frame (3596); fixed (`device_lost`) |
| `hfh_t_losttest` (46 s, meant as a test of the loss path without a loss) | the same + `meshcaplosttest:2030` | **lost** at present 1949 (527 groups), in the first cycle; the losing frame captured |
| `hfh_p_probe1` | the same + `progprobe:K` (the program's draws in a 128 x 128 box, no depth or stencil test, constant fragment stage, a statistics query each) | survived 240 s, 13,800 presents; most fragments in the box 7,142 |
| `hfh_f_plainfs1` | `marks,meshcaplost,meshcapprog:K,tmlog:K,progfs:K` | **lost** at present 1897 (525 groups); the losing frame captured |

Harness (`tools/meshlab/gpu_run.sh` with `MESHLAB=work/meshlab_hf/meshlab`):

| Output | Arguments | Result |
|---|---|---|
| `geo_sane_230.txt`, `geo_sane_230_tasks.txt` | `hang_hf_cap` frame, `geo 230 [--tasks]` | 262 groups, 13,344 triangles, 0.18 screens; the task stage run again places 262 |
| `geo_a_3596_tasks.txt`, `geo_t_1949.txt`, `geo_t_1948.txt`, `geo_f1897.txt` | `geo --present N` | as in the conclusion |
| `run_t1949_fsreal.txt` | `run --present 1949 --fs real` | survived, 235,573 draws in 30 s (no fragment passes) |
| `run_f1897_prepass_simple.txt` | `run --present 1897 --fs simple --prepass` | **HUNG** at submission 5 |
| `run_hf305_prepass_simple.txt` | `hang_hf_cap` frame, `run 305 --fs simple --prepass` | survived, 156,740 draws in 40 s |
| `run_f1897_prepass_repl.txt` | as the hung one + `--repl work/meshlab_hf/repl40d0` | survived, 846,382 draws in 40 s |

One run each: the hung configuration was not repeated (the rule), so "hangs at once" rests on one harness run and three
game losses; the two survivals are 40 s each against a hang in 2 s.

## Device losses: 4 of 6

| # | Time | Run | Recovery |
|---|---|---|---|
| 1 | 09:16:45 (reset 32) | `hfh_a_lost` | vulkaninfo after 3 min |
| 2 | 09:22:30 (reset 33) | `hfh_t_losttest` (not meant to lose: the first cycle had not lost before) | the same |
| 3 | 09:36:04 (reset 34) | `hfh_f_plainfs1` | the same |
| 4 | 09:39:13 (reset 35) | harness `run_f1897_prepass_simple` | the same |

Kernel logs are in each run's folder (`kernel.txt`, `*.kernel.txt`). No configuration lost twice.

## Not done

- Step 3's 1-pixel scissor and discard for the one program (`progscissor`, `progdiscard` exist, not run): the probe run and
  the reproducer decided it.
- Step 4, the build before M6f: not run (see the table).
- The barrier between the reproducer's two draws (`--prepass-barrier`, exists): not run. So it is open whether the
  reproducer is the game's single draw or needs the two draws' overlap; in the game the draw before `cfebb`'s had finished.
- Which of spills and ring; a synthetic shader without game data for a Mesa report; the group count at which it starts.
- No fix in the game. The hand-written stage emits position, normal, tangent, world position and one texture coordinate
  set (16 of `40d0`'s 28 output components); the rest (the other agent's note `hf-mesh-program.md`: the probe lookup) is
  not written, so swapping it in now would draw the ivy wrongly lit.

## About the task stage as a compute step

That design (m0-task-mesh.md) is what the back end already does since M4c: the task stage is a compute dispatch and the mesh
stage is native. It does not avoid this loss; the fault is in the native mesh stage's compile.

## Recommendation

1. Finish the hand-written `40d0` stage (the 12 missing components), check it with `meshlab compare` on the Hyrule Field
   and market captures, and swap it in by code key as M6e's section 6 designs. Prove it with two 240 s walks. This is a
   replaced shader, not a back-end special case.
2. Before that lands, check every mesh stage's compile for scratch spills (`RADV_DEBUG=shaders meshlab <frame> stats`,
   count `scratch_store_dword` in the mesh stage) over the three scenes' captures, and run each spilling one through
   `run --prepass`: `f69141e92f08bc7b` uses `40d0` too, and M6e lists it in the market's lost job.
3. Run the market's job with `--prepass` and no barrier: if it hangs, M6d's wait before every task+mesh draw can be
   judged against the same cause, and perhaps dropped once the spilling stages are replaced.
4. A Mesa report needs a synthetic stage that spills and hangs under `--prepass`; `tools/meshlab/probes/` is the place.

## Code

| What | Where |
|---|---|
| The capture as a roll: `meshcap:<first>-<last>`, `meshcapprog:<key>`, `meshcaplost` (the kept draws written when a submit reports the device lost, the unfinished command's frame first), `meshcaplosttest:<present>`; record `MC_FRAME` | `gpu/direct/direct_meshcap.{h,cpp}`, `direct_meshcap_format.h` |
| One thread reports a lost device | `device_lost` in `gpu/direct/direct_marks.{h,cpp}`; `direct_sync.cpp`, `direct_present.cpp` |
| `tmlog:<key>` (a program's mesh groups a frame) | `gpu/direct/direct_task.cpp` |
| `progdiscard:<key>`, `progscissor:<key>`, `progfs:<key>`, `progprobe:<key>`; `shaders/diag_plain.frag` and its header | `gpu/direct/direct_draw.{h,cpp}`, `direct_cmd.cpp` |
| meshlab: rolls (`--present`), `geo [--tasks]` (a draw's triangles in numbers, rasterizer off), `state`, `run --prepass` / `--prepass-barrier`, a cut roll read as far as it is whole | `tools/meshlab/meshlab.cpp`, `capture.h` |
| `gpu_run.sh`: `MESHLAB=`, `MESHLAB_TIMEOUT=` | `tools/meshlab/gpu_run.sh` |

All switches are off by default. After the last game run the two key helpers were moved out of `draw_emit`
(`test_direct_strict` reads that function); that build was compiled and not run in the game. Tests: 296 pass / 5 fail (the
baseline five).
