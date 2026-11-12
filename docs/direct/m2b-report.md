# M2b report: programs and draws (2026-11-12)

Branch `agent/m2b`, rebased onto native-vk with A-rt (004acab). Runs are in `work/runs/m2b_*` (local). RX 6950 XT, RADV, one game process at a time.

## What was built

| Part | Files (lines) |
|---|---|
| State tracker | `direct_state.{h,cpp}` (65, 217): the NVN state of a list, starting from defaults, with generation counters; the graphics stages' driver banks c0/c2 (`vk_bank.cpp`'s layout) |
| Programs | `direct_shader.{h,cpp}` (81, 1154): Eden is linked from the frozen objects (`nx2/build.py` +14) and is unchanged. The code reading/lowering, `Env`, profile (BDA=2 as constants), the key and the answers are lifted from `vk_shader.cpp`. Slots are keyed by NVN serial in a lock-free open table; variants are a prepend-only list per slot. Two workers do the stage scan at `nvnProgramSetShaders` (code keys, headers, cached variants made), and translate on a miss. The cache file is this back end's own, `hookshot_direct_programs.bin`, stamped by `HOOKSHOT_DIRECT_SHADER_STAMP`; a file with another stamp is truncated. The heap mapping and the shader objects are made per variant |
| Heap, sampled views | `direct_heap.{h,cpp}` (88, 515): the M2b device part (shader objects, descriptor heap, EDS3 and the other extensions, feature chaining in `direct_main.cpp` +8). 7 view-type regions × 65,536 ids, plus a zero tail; 2 × 4,096 samplers. Entries are written at `nvnTexturePoolRegister*` in every region the target allows, as view create infos (no VkImageView). Each id has an answer byte. Entries are nulled when their image is destroyed |
| Draw | `direct_draw.{h,cpp}` (36, 918): dynamic state (`dynamic_state`/`so_state` without the replay's special cases), vertex input, vertex/index buffers through layer 0, a per-slot frame arena (bank copies, address tables), push data, the draw. Draws in lists with no command buffer (jobs, load lists) are resolved up to the Vulkan calls and counted. Skips are counted by reason, by list kind and by return address. `HOS_DBG=drawlog:<present>` logs the draws of one frame |
| Other | `direct_pools.cpp` +13 (`layer0_address`, the BDA window table entry), `direct_pass.cpp` (barriers wait for the list's next command; a shared barrier buffer for a list's trailing barrier; a barrier after texture clears), `direct_cmd` (arena per slot; command buffers in batches of 16), `direct_sync.cpp` +1 (deferred words at the guest's submit), `scripts/direct_dump_compare.py --pictures` |

## Done-items

| Item | Runs | Kokiri | Hyrule Field |
|---|---|---|---|
| UI (`0x8006d258`) and composite (gate `0x802eb3fc`) drawn | `m2b_ceil_k2`, `m2b_ceil_hf` | UI 20.0 drawn a frame, 0 skipped; composite 1.0; layer `0x8007b0c8` 113.7 drawn, 2 mesh, 0.8 with no targets | UI 20.0, composite 1.0; `0x8007b0c8` 372.9 drawn |
| Title, normal boot | `m2b_title9`, `m2b_ad_title3` | UI 15 drawn, composite 1, 0 skipped. The composite was empty until inline uniform updates were added (its vertex c3 comes from an UPDATE_UB) | |
| Same targets as frozen | `m2b_fd_*`, `m2b_ad_*` | 44 of 51 distinct frozen passes present; 29 at the same count a frame (M2a: 51/42) | 43 of 44, all 43 at the same count |
| Pictures (thumbnail mean abs. diff, 0-255) | noise: frozen vs frozen | median 0.00, max 13.4 (UI pass 0.77, composite 1.63) | median 0.01, max 9.1 (UI 0.83, composite 1.21) |
| | direct vs frozen (after A-rt) | median 2.0, max 85; UI pass 3.3, composite 13.0 | median 2.3, max 68; UI 16.2, composite 36.3; title UI 30.3, composite 57.2 |
| Validation (sync on, duplicate limit 200) | `m2b_val_kokiri`, `m2b_val_hf_market` | only the 3 known depth-copy hazards (RAW/WAR/WAW), plus 2 a frame BeginRendering-after-ClearColorImage RAW, now fixed by the barrier after texture clears (85272b0; not rerun under validation). One warning type remains: `ShaderOutputNotConsumed-DynamicRendering` (a fragment stage writes colour 0 in a pass that has no colour attachment) | same |
| Submit; frame time, pacing off | `m2b_ceil_k2`, `m2b_ceil_hf` | 0.060 ms a frame in `hsd_submit`; 10.73 ms mean, 10.67 median, 12.71 p99 (M2a: 9.56-9.73 / 11.2-11.8) | 0.118 ms; 11.57 mean, 11.46 median, 13.72 p99 (M2a 9.53-9.85) |
| Recording threads' time in `hsd_emit`, summed | same | 1.88 ms a frame (1.10 of it in the draw path) | 2.92 ms (1.85 draw path) |
| Objects in steady play | `m2b_hf2`, `m2b_ceil_*` | 0 command buffers or pools (was +3/3600 presents). The remaining +2 objects per ~1200 presents are images made at texture initialisation | same |

The picture distances do not meet "within noise", and cannot at this stage. The UI and the composite draw over targets the 3D layer fills (jobs, M3) and, on the title, over the movie frame a compute dispatch writes (M4). Both are near black in the direct dumps (scene mean about 0.0008 vs 0.02). The comparison does confirm that the UI and composite passes exist, are drawn, and write the same alpha coverage (0.64). Matching pictures needs M3/M4.

## Skipped draws a frame, by reason (`m2b_ceil_k2` / `m2b_ceil_hf`)

| Reason | Kokiri | HF |
|---|---|---|
| UI / composite lists | 0 / 0 | 0 / 0 |
| layer: mesh (M4); no render targets set | 2.0; 0.83 | 2.0; 0.84 |
| job: mesh (M4) | 41 | 47 |
| job: under a render condition (M5) | 114 | 316 |
| job: step A cannot draw (all "texture handle from a handle program") | 31.9 | 17 |
| job: resolved, would draw (no command buffer: M3) | 171.9 | 645.2 |

Of the job lists' ordinary draws, 54% resolve fully in Kokiri (171.9 of 317.8) and 66% in HF (645 of 978). Those draws resolved with float colour types assumed, since jobs set no targets.

## Deviations from the contract

- **Uniform buffers.** RADV's push data is 256 B, so each uniform binding is `INDIRECT_ADDRESS`: one pushed address (at offset 120) points to an address table in the arena. The contract's `PUSH_ADDRESS` per buffer did not fit.
- **Handle textures in c3+ buffers** read through the buffer's pushed address. Their variant answers are not checked at the draw; the variant is taken as it was translated, so no CPU read is made.
- **Shader objects** are compiled from SPIR-V at load. There is no binary cache file; Mesa's disk cache serves.
- **Inline uniform updates** (planned for M3) are already in: the composite needs them. They become arena copies for the list's later draws. 35 a frame do not cover the bound range, and the rest of the buffer is then read from guest memory at record time; this is counted.
- Barriers wait for the list's next command. Lists of only barriers and skipped dispatches (the 7 compute lists) now make no command buffer.
- Each list starts at NVN defaults; there is no state inheritance across CALL. No drawn list needed it.

## Candidate game patches

1. The M2a barrier around the two depth `CopyTextureToTexture` in `f_8007ad40`'s list (still the only hazards).

## Open problems

- **Placeholders sampled by title UI draws** (`m2b_ad_title3`):
  - textures `0x7fcbe23200` and `0x7fcbe7b200` (683x683, RGTC1, ids 1551 and 1552);
  - texture `0x7fcbca9200` (341x341, RGTC1, id 1537).

  A-rt's counters name only 'DummyTextureForEvent' as a placeholder. The heap entry was most likely written at registration before the texture's prepared image was linked, and the heap is never rewritten when an image changes. To check: whether A-rt links these textures after their registration; if so, re-encode at that link.
- **The CPU cost.** Frame time with pacing off rose 1.1 ms (Kokiri) and 1.9 ms (HF) over M2a. The draw path costs about 2 µs a draw, and about 60% of resolved draws are job draws that record nothing. It has not been profiled.
- About 62 texture registrations a frame (the transient targets' new ids), each 7 descriptor writes.
- The timing and validation runs predate the rebase onto A-rt. The dumps were redone after it.

## What M3 needs next

Jobs recorded as rendering secondaries with their pass formats (645 HF draws a frame already resolve). Packets for load lists. Render conditions, or a patch at the gating sites (316 HF draws). Handle-program textures (step B, the translator fork). The title movie's compute (M4). A profile of the draw path. Heap entries refreshed when a texture's image is linked late.
