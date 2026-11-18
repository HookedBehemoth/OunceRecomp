# The direct back end: rules and open work

This page states what the direct back end is, the rules every change to it keeps, and the work that is still open. Read
it before you change anything under `renderer`, `renderer/api` or the `runtime/direct_*.c` patches. For how the back end
works, see [the back end's architecture](direct/architecture.md); for what is known about the game itself, see the
[renderer pages index](direct/README.md).

## What the direct back end is

The direct back end is the only renderer in the tree. It records the game's NVN commands as Vulkan commands. NVN is the
console's graphics API; the back end replaces it on a PC with Vulkan.

- It lives in `renderer` and builds as `ounce_renderer.so`, which `runtime/rx_backend.c` loads.
- It shares its interface with the host through `renderer/api` (`vk_api.h`, `rx_nvn.h`) and `renderer/api/vk_list_api.h`.
- It targets one PC: an AMD RX 6950 XT on Mesa's RADV driver, with every extension Mesa offers. RADV is Mesa's Vulkan
  driver for AMD GPUs.
- It runs one game build, the one `games/oot/game.toml` names.

Each NVN command-buffer call is recorded as Vulkan on the thread that makes it. No later thread walks the commands
again, and no step looks back to decide what an earlier command was.

## Rules

1. **One path.** The back end has no per-feature switches. A debug aid is a build option or an item of `HOS_DBG=<list>`.
2. **No fallback.** Every miss has one defined behavior: wait (at load time only), or skip the work and count it (in
   play). `HOS_DBG=strict` stops at the first miss. Steady play counts zero.
3. **Known when recorded.** If a command needs data that is not final when it is recorded, move the data: to load time,
   to a read on the GPU, or to one flat copy at the guest's submit. Never add a per-draw step at submit.
4. **Prepare offline.** The romfs can be changed. If a build-time tool can remove a problem, don't solve it at run time.
5. **Cut at the NVN call.** Replace engine code above NVN only where NVN's semantics force a CPU readback, a copy or a
   serial order.
6. **No second renderer in the process.** Checks use stored reference pictures, the Vulkan validation layers, counters
   and GPU asserts.
7. **Patch the game, not the back end.** The back end implements what Vulkan needs, plainly. When the game does
   something awkward for it, fix it with a patch of the game (a patch declaration from `runtime/patch.h` in a
   `runtime/direct_*.c` file), a replaced shader, or changed romfs data. The back end gets no special case for one pass, one
   program or one call site.

## Open work

Each item is open at the time of writing. Remove an item when you close it.

### Correctness

- **Pictures.** The per-pass picture checks (`scripts/direct_check.sh`, `scripts/direct_picture_check.py`) do not pass
  on every scene yet. Work through the failing pass states, earliest pass first.
- **Eviction destroys textures still drawn.** The VRAM eviction patch (`games/oot/patches/texdata.c`, a 1.5 GB margin) can
  destroy a texture that the game released in the same present while recorded draws still sample it ("texture with no
  image sampled"). Fix the patch so it does not, or lower the margin. The margin was sized when render targets still
  cost VRAM at a scene change; the [transient pool](direct/transient-pool.md) now holds them at a constant size.
- **A frame at each step up of the dynamic resolution** (`HOS_DYNRES=1`, off by default). The first frame at a larger 3D
  scale shows the previous picture at the old scale in the top-left part of the screen. Find which image the final
  upscale reads on that frame: either the game's own one-frame lag, or a target made again at the larger size over the
  old target's storage while the pass still reads the old image.
- **The render-target id patch** (`games/oot/patches/rt_ids.c`, log line "direct patch rt ids") keeps no calls: the
  system-texture entry it watches arrives with a changed id every frame, so every call takes the game's path. The
  submit-time snapshot of the deferred tables covers it. Find what resets the id, or delete the patch.
- **The SMAA edge list.** The SMAA pass uses a transient render target's storage address as a buffer
  (`ndsr::smaa_edge_list`). No patch gives it a buffer of its own; check what the pass reads and writes there, and give
  it a host-owned buffer.
- **The dump's three damaged terrain files** draw a placeholder and stop strict mode in the market. Give them a
  neutral record at the tile patch.

### Stability

- **The mesh-stage hang fix rests on few runs.** See [task and mesh shaders](direct/task-mesh.md). Eleven other mesh
  stages sit at the register limit without spilling, the scan covers three scenes and no fragment stage, and why a
  spilling mesh stage hangs the GPU is inferred, not known. A report to Mesa is not written.
- **The transient pool's size** (twice the game's pool) is measured on the market walk and a six-stop scene cycle only.
  The full scene cycle and a long soak under `HOS_DBG=strict` are not run.

### Frame time

- **A hitch when a command slot is added in play.** In the market benchmark, after a frame with a double game step,
  three recording threads each create a fourth command slot, with new command buffers and query pools. The frame that
  does it costs about 17-18 ms. Create the slots up front so no slot is made in play (`renderer/vk_cmd.cpp`).
- **The long layer of calc stage C** records alone at the end of the stage. `HOS_LAYER_AHEAD` starts it with the render
  jobs and saves 0.2-0.5 ms a frame in the market; Hyrule Field shows no gain and has bursts. See
  [the frame loop](direct/frame-loop.md).
- **Shader cost.** Work through the [shader census](direct/shader-costs.md)'s ranking: the heavy compute pass
  `fd029a317a9a4b94` first, then per-command overhead, then translator-wide costs. Don't rewrite stages one by one.
- **Native task shaders** are not used: RADV runs a task stage on an async-compute ring gang-submitted with the graphics
  ring, so the back end runs the task stage as a compute step. A native run on Mesa main would show whether the compute
  step can go.

### Memory and features

- **Memory accounting per owner** in the periodic log. The per-slot arenas, task pools and query pools never shrink.
- **A temporal upscaler.** The game upscales with a fixed-ratio network and writes no motion vectors. Rendering at the
  window's size times a bias needs an upscaler of ours in place of the network; see
  [render targets](direct/render-targets.md#upscaling).

## Checks

| What | How |
|---|---|
| Quick run | `scripts/smoke.sh` |
| Scene routes | `scripts/direct_walk_check.sh`, `scripts/direct_check.sh`; `scripts/direct_scene_cycle.sh` needs the removed `!warp` and `!cheat` commands ([parked patches](parked-patches.md)) |
| Pictures per pass | `HOS_DBG=dump:<present>` dumps the targets; `scripts/direct_dump_compare.py` and `scripts/direct_picture_check.py` compare them with stored references |
| Validation | `HOS_DBG=validation` runs the Vulkan validation layers with synchronization validation; any message is a failure |
| Counters | the `direct:` log lines every 600 presents: skipped draws and jobs, unclassified lists, objects created a frame, cross-list uniform overlaps; all 0 in steady play |
| Game patches | `HOS_DBG=logpatch`: one line per patch every 600 presents, which shows that it still applies |
| Hangs | `HOS_DBG=marks` writes GPU breadcrumbs for a lost device; save the kernel log into the run folder after each loss |

If you take timing, turn the frame cap off and run one game process on an idle machine. Frames per second at the cap
are not a result.
