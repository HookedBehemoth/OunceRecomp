# M2a report: targets, passes, present (2026-11-12)

Branch `agent/m2a`. Runs in `work/runs/m2a_*` (local), RX 6950 XT, RADV, one game process at a time.

## What was built

| Part | Files (lines) |
|---|---|
| Command buffers | `direct_cmd.{h,cpp}` (48, 168): a command pool per recording thread and frame slot (three), its buffers kept and handed out again after the slot's reset (at the thread's first list of a new frame, once the slot's last submission is done and no list holding its buffers is alive; otherwise counted and not reset); a per-thread ring of four upload buffers sent at once with their own timeline value |
| Images | `direct_image.{h,cpp}` (80, 460), `direct_formats.cpp` (164, copy of `vk_formats.cpp`), `direct_vma.cpp` (68, VMA): an image per NVN texture at `nvnTextureInitialize` (or registration), keyed by storage and shape as `vk_texture.cpp` keys it, so re-initialized transient targets keep theirs; made in GENERAL and cleared to zero on the making thread's upload; refcounted by live textures, destroyed 8 presents after the last one went and the GPU passed it; attachment views cached per image; the placeholder (ASTC, no Vulkan format, refused); WriteTexels as a region copy at the call; `image_sub` (copy regions, from `vk_xfer.cpp`) |
| Passes | `direct_pass.{h,cpp}` (16, 426): rules below |
| Submission | `direct_sync.cpp` (+47): a submission is its lists' command buffers in call order (`list_buffers`, callees in place) in one `vkQueueSubmit2`; each slot gets the value |
| Present | `direct_present.cpp` (+53 net): the host's present is queued behind its queue's submissions (new export `hsd_present_queue`, 1 line each in `host/nvn_native.c`, `rx_backend.c`, `direct.h`) and blits the window texture with the crop (`vk_present_rect.h`) |
| Dump | `direct_dump.{h,cpp}` (47, 360): `HOS_DBG=dump:<present>[+<frames>]`, `vk_dump.cpp`'s copies, line format and PNGs; `HOS_DBG=dumpdir:<dir>`; `HOS_DBG=rtlog` names 200 SetRenderTargets |
| Compare | `scripts/direct_dump_compare.py` (148) |
| Layer 0 | `direct_pools.cpp` (+27): `layer0_span`, a guest range as window and offset, imported at its first use when not bound ahead |

Pass rules: SET_RT notes the targets; the rendering begins at the first draw, job call, or call of a list that draws
(the load-time draw lists: without this the cube-probe passes never began). Clears before the begin covering the whole
area (no scissor or one past it) are load ops; others `vkCmdClearAttachments`. A barrier, dispatch, copy or texture
clear ends the rendering, keeping the targets (the next draw begins it again, loading); SET_RT and the list's end end the
pass. Each NVN barrier is one full memory barrier. Layer, gate, compute and head lists record; job, load and packet
lists record nothing (counted: 2 BARRIERs a frame, in jobs). A call of a list with command buffers ends the caller's
buffer (0 such calls cut a rendering).

## Done when

| Item | Run | Kokiri | Hyrule Field |
|---|---|---|---|
| SET_RT opens real renderings, clears as load ops | `m2a_ceil_*` | 123.8 SET_RT a frame: 101.7 renderings, 37.5 load-op clears, 0.2 in-pass clears, 0.8 SET_RT with no target, 0 targets without an image, 0 masked clears; 2 copies, 2 texture clears | 121.8: 97.8, 36.1, 0.1, 0.8, 0, 0 |
| Window shows the NVN window texture | `m2a_k1`, `m2a_win` | 600 of 600 presents blit an image, 0 without; `m2a_win` (WINDOW=1, validation) 1280x720 mailbox swapchain, 0 messages | same |
| Images by class (live, steady) | `m2a_ceil_*` | 3182: 488 colour, 41 depth, 2653 BC; 1756 MB; 164 rendered, 4 copied/cleared, 43 texel-written, 2971 never written (assets: zero or unfilled, A-rt's); 3890 ASTC placeholders | 3294: 487, 36, 2771; 1984 MB; 154, 4, 43, 3093; 5005 ASTC placeholders |
| Images made a frame in steady play | `m2a_ceil_*` (9600 / 8400 presents) | 0 in 5 of 6 steady 600-present windows, 1 in one; Vulkan objects +2 per 1200 presents; 224 command buffers since start, constant | 0 in all 6 steady windows; command buffers 217 -> 220 over 3600 presents (pools growing to their peak) |
| M1 numbers | `m2a_ceil_*` | lists 100.5 job, 14 layer, 1 gate, 7 compute, 5 head; 6 submits, 12 command buffers a frame, 0.040 ms a frame in `hsd_submit` (longest 0.07); ceiling 9.56-9.73 ms mean, median 9.46-9.64, p99 11.2-11.8 (M1: 9.2 / 9.1 / 11.0) | 101 job; 0.064-0.067 ms (longest 0.52-0.88); 9.53-9.85 mean, median 9.40-9.71, p99 11.6-12.0 (M1: 9.4 / 9.3 / 11.2-11.6) |
| Validation with synchronisation | `m2a_val_*` | 0 messages other than sync; 3 sync hazards, each at the layer's 10-message limit (every frame) | the same 3 |
| Frozen back end unchanged | `m2a_after3` vs `m2a_gold2` | tests 290 pass / 5 fail, the baseline's 5; smoke 0 new log groups, numbers equal, 1 bad picture (distance 16), but the baseline itself fails against its own golden the same way (`m2a_base2`: 2 bad, 14 and 16): the scene drifts more than the threshold | |

The window texture holds the game's clears only; whether every clear colour is right is M2b's picture check.

**The hazards** (all three at the two `nvnCommandBufferCopyTextureToTexture` a frame in the layer list begun from
`0x8007b0c8`): the scene depth (D32F 1536x864) copied right after its pass ends, preceded by SignalEvent and WaitEvent
but no barrier (READ-AFTER-WRITE on the store op); the copy's destination was last a depth-only pass's attachment
(WRITE-AFTER-WRITE); and the next pass's store follows the copy's read (WRITE-AFTER-READ). The second copy (D32F 768x432,
after that list's SetRenderTargets 40) is the same shape. No game barrier orders these; see candidate patches.

## Per-pass comparison

`scripts/direct_dump_compare.py <frozen log> <direct log> --frames F D`: target lists aligned in order, passes (runs over
the same targets merged, since the frozen back end restarts renderings within a SET_RT) compared by their targets' kind,
format and size, per frame. Frozen `RX_VK_DUMP=1800+8` holds only 2 (Kokiri) and 4-5 (HF) frames; the direct dumps 8.
- **Kokiri** (`m2a_fdump_k8`, `m2a_ddump_k8`): frozen 51 distinct passes, all 51 in the direct dump; 42 with the same
  count a frame. Differences: the cube-probe mip chain (six targets a level) is time-sliced, 1 frame in 4 in the direct
  run and in both frozen frames; direct-only: depth D32F 384x384 (1 a frame) and B10G11R11F 128x128 + D32F 128x128
  (0.75); RGBA8 1536x864 + depth 3 vs 2, RGBA16F 512x512 1.9 vs 1.5.
- **Hyrule Field** (`m2a_fdump_hf`, `m2a_ddump_hf`, frozen as 4 frames): 50 of 51 frozen distinct passes in the
  direct dump, 29 with the same count; the 1.25-vs-1 rows are the frozen dump's partial fifth frame; real differences:
  the cube chain as above, and a depth-only D32F 2432x2432 pass (1 a frame) the direct dump lacks (it has the RGBA8
  1536x864 + D32F 2432x2432 one both have).
The direct-only passes are most likely ones whose draws the frozen back end skipped by a render condition (direct
ignores conditions until M5); not checked.

## Deviations

- A **barrier ends the rendering** too (Vulkan allows no general barrier inside one); the targets stay set.
- **Calls of lists that draw** begin the caller's rendering, not only job calls.
- **No bump arenas**: nothing at M2a writes one; they come with their first user (M2b's push data).
- **No sampled views** (swizzle, levels, layers): their user is the descriptor heap (A-rt / M2b); attachment views only.
- **Masked colour clears** are counted and skipped (they need a draw: M2b); 0 occurred. Partial texture clears, 3D or
  depth texture clears, CLEAR_BUF and COPY_B2B are counted and skipped (0, 0, 1, 0 a frame).
- **RADV has no D24 attachments**: D24 formats get D32F twins (a capability of the device, not a special case).
- The present runs in its queue's order, needing one new export; held submissions delay it.
- Dumps record copies from two presents before the frame on and are written 2 presents after it (held submissions).

## Candidate game patches

1. A barrier (ORDER_FRAGMENTS | INVALIDATE_TEXTURE) before and after the two depth CopyTextureToTexture in the layer
   list begun from `0x8007b0c8` (`f_8007ad40`), as the validation run requires (the callers to be found from the LR).

## Open problems

- The HF depth-only 2432x2432 pass and Kokiri's direct-only passes are not explained.
- Images of textures over the same storage in another shape are separate images (aliasing is not shared).
- A list recorded on two threads would share a pool across threads (counted: 0 in all runs).
- The ceiling rose 0.3-0.5 ms against M1 (recording real commands, image lookups at SET_RT); not profiled.
- Command pools grow by a few buffers over minutes in HF before their peak; the counter is not yet exactly 0.
