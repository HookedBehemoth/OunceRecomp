# M2: the UI frame (structure and stages, 2026-11-12)

M1 landed (97e6d1e, `m1-report.md`): the game runs on the direct back end with nothing drawn, 9.2 ms a frame in Kokiri
and 9.4 ms in Hyrule Field with pacing off. That is the ceiling: about 7 ms a frame are left for rendering work on the
critical path at 60 fps.

M2's goal (plan, Milestones): the UI layers and the final composite of the chiseled scenes, and the title screen and
menus on a normal boot, match the frozen back end's pictures; validation clean. The 3D layer's draws, compute lists,
mesh draws and conditions are still counted and skipped.

M2 is built in stages, each landed on its own.

## Modules (`gpu/direct/`)

| File | Owns | Stage |
|---|---|---|
| `direct_cmd` | per-thread command pools and bump arenas per frame slot, reset whole when the frame's timeline value has passed; a list's command buffers and chunks | M2a |
| `direct_image` | one image per NVN texture, made when the texture is created or registered, never at a draw; views per NVN view (format, swizzle, levels, layers); region copies for WriteTexels and the buffer/texture copies; the placeholder | M2a |
| `direct_pass` | SetRenderTargets noted, rendering begun lazily at the first draw or job call so the clears that follow become load ops; ended by a dispatch, a copy, the next SetRenderTargets or the list's end; barriers from the NVN bits | M2a |
| `direct_present` | the swapchain image is a blit of the NVN window texture with the crop the host gives | M2a |
| `direct_dump` | `HOS_DBG=dump:<present>`: every pass's targets of one frame written to files, in the frozen back end's `RX_VK_DUMP` naming so one tool compares both | M2a |
| `direct_assets` | the prepared texture store's reader (`nx2/prepare_textures.py`'s docstring is the format); upload into an image on the registering thread's command buffer | A-rt |
| `direct_heap` | descriptor-heap entries at the NVN pool ids, written at registration | A-rt |
| `direct_state` | the NVN state of a list, updated by the emitted ops | M2b |
| `direct_shader` | programs to shader objects through the translator, a lock-free table by program, the direct back end's own cache files | M2b |
| `direct_draw` | a draw: program, dynamic state, vertex and index buffers by layer 0, push data and bank words, the draw call | M2b |

## Decisions that hold for all stages

- **Images exist before use.** A render target's image is made when its NVN texture is initialised; an asset texture's
  when it is registered, from the prepared store; a texture with no prepared record gets the placeholder and is
  counted and named. No code path reads texel bytes from guest memory except the explicit CPU write calls
  (WriteTexels, the buffer-to-texture copies), which arrive linear.
- **Layouts:** every image is used in `VK_IMAGE_LAYOUT_GENERAL` for now. It is valid everywhere and needs no tracker;
  what it costs on this GPU is measured at M6 and changed then if it matters. Synchronisation comes from the game's
  NVN barriers, mapped to pipeline barriers; the validation layer's synchronisation check is the test that they
  suffice. A hazard it reports is fixed by a patch of the game's pass (plan rule 8), not by a tracker.
- **Threads:** a list's commands go into a command buffer of the recording thread's pool for the current frame slot.
  Nothing a recording thread touches is shared without a lock; the registration paths (loader threads) submit their
  uploads through the queue's mutex with their own timeline value, and a frame's submission waits for the uploads its
  textures need.
- **Frame slots:** three. A slot's pools and arenas are reset when its last submission's timeline value has passed.
  No Vulkan object is created per frame in steady play (counted).
- **Submission:** as M1, each NVN queue's lists run in order with unmet waits held on the host, but a submission is
  now the lists' command buffers in call order, in one `vkQueueSubmit2`. Events and reports stay CPU-written until M5.
- **No frozen header changes.** `gpu/vk`, `gpu/render` and `tests/` are not edited. What the host must tell the back
  end beyond the version-14 table goes through new exports in `direct_api.h`, found by name.
- Rules as M1: no `getenv` but `HOS_DBG`; no fallback; no special case for one pass, program or site; a validation run
  with no messages.

## Stages and their checks

**M2a: targets, passes, present.** Done when, in Kokiri and Hyrule Field: every SetRenderTargets of the frame opens
a real rendering on real images with the game's clears as load ops; the window shows the NVN window texture (the
game's own clear colours, since nothing draws yet); `HOS_DBG=dump` writes the frame's passes and a comparison script
lists, per pass, the frozen back end's target (format, size) beside the direct one's with matching counts; images
made a frame in steady play: 0; validation clean; the M1 numbers (lists, submit ms, ceiling frame time) reported again.

**A-rt: prepared textures at registration.** Done when: every registered asset texture of both scenes has its image
filled from the store before its first possible use (placeholders: 0, or each named); the game's texel copy into the
resource pool is skipped for prepared textures and the pool's pages stay unbacked (resident pages reported); uploads
never run on the Presentation Thread; VRAM used by textures reported for both scenes.

**M2b: programs and draws.** First deliverable, before code: `m2-shader-contract.md`, what a translated program
expects at a draw (push data, bank words, heap indices, addresses) and how the frozen back end's job-thread draw path
(`eng_draw_state`, `eng_draw_descriptors` in `vk_draw.cpp`) provides it. Done when: the UI layers' and the final
composite's draws are drawn; the per-pass comparison of those passes against the frozen back end is within its
run-to-run noise in Kokiri, Hyrule Field, and on the title screen of a normal boot; skipped draws are counted by reason
(mesh, compute, not yet handled) and no UI draw is among them.
