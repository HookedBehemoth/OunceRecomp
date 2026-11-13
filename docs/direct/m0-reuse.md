# M0: reuse audit for `gpu/direct/` (2026-11-12)

Analysis only (tree at 23d0aa6, clean). Paths are under `gpu/vk/` unless they start with `host/`, `nx2/` or `tests/`. "Left behind" =
`vk_replay.cpp`, `vk_draw.cpp`, `vk_texture.cpp`, `vk_state.h`, `dev` fields beyond the ones named, `RX_VK_*`, stats/log/prof helpers.
Verdicts: **L** lifts cleanly, **L-n** lifts with n named cuts, **R** entangled (rewrite, using the old code as a reference).
Baseline: `gpu/vk/tests` 4/4 pass; `tests/run_all.py` 270 pass, 3 fail (unrelated), 40 skip (no Vulkan headers found); with the headers set ~14 more fail at HEAD (i): "keeps passing" means against a recorded baseline.

| | Component | Verdict | One line |
|---|---|---|---|
| a | device, swapchain, present | **L-12** (device) / **R-small** (present) | `vk_main.cpp` is clean in its core, ~250 lines of start-up screen and ~12 vendor/menu hooks go; present is a 120-line piece inside `vk_replay.cpp` |
| b | layer 0 | **L-7** (`vk_pools.cpp`) + a **~250-line extract** of `vk_memory.cpp` | `layer0_windows.h` verbatim |
| c | descriptor heap | **L-6**, about 40% of it | the device/heap/ring/encode half lifts; the lazy per-draw resolve half is the design the plan drops |
| d | BDA | **L-3** + CPU side **R** | table and counters lift; the draw-time half sits in `vk_draw.cpp` |
| e | timelines / syncs | header **L**; logic **R** | `vk_timelines.h` is pure; ~700 lines of ordering in `vk_replay.cpp` are the replay's shape |
| f | shader path | Eden **L** (link, do not copy yet); `vk_shader.cpp` **L-9**; `vk_shader_object.cpp` **L-4**; `vk_bank.cpp` **L-3** | caches are stamped by source: name the files apart |
| g | utilities | **L** | `vk_formats.cpp`, `vk_loader.h` (prune), `vk_paths.h`, `vk_jobs.*`, `vk_present_rect.h` |

## a. Device, instance, queues, swapchain, present

| Item | Where | Needs from not taken (cut) |
|---|---|---|
| Loader and PFN tables | `vk_loader.h:20-184`, defined `vk_main.cpp:32-70` | none; the lists carry replay-only entries (`:100-150`): prune |
| `Device` (`dev`) | `vk_internal.h:37-110`, `vk_main.cpp:77` | keep ~25 fields; drop DLSS/framegen/hdr/present_queue/checkpoints/shader_binary fields (the shader and heap files read `dev.{device,props,memory,physical,driver_id,mesh_*,sg32_stages,subgroup_*,native_mma,shader_object,shader_binary_*,vk12,null_descriptor}`, from a grep of each file) |
| Instance, device pick | `vk_main.cpp:97-217` | cuts: `dlss_instance_extensions` :131, `menu_init`/`menu_set_gpu_count`/`opts.gpu_device` :151,169,182 |
| `create_device` | `:219-526` | cuts: `dlss_/reflex_/antilag_/fsr4_*` :266-271,370-371,513-514; `eng_pass_device_extensions` :369; `buffer_markers_init` :510; the `RX_VK_*` reads (switches become constants); present queue :280-310 (off by default, `want=false` :~290). Keeps: `layer0_wanted()` :321 sparse queue, `desc_heap_*` :368,436,511, `coopmat_init` :512 |
| Swapchain | `create_swapchain` `:559-731`, `unorm_of` :528 | cuts: `opts.hdr` :595-606 (HDR), `reflex_*` :682,688,729, `swap_views_*` (ImGui overlays), `device_wait_idle_locked` :687 (replay: needs a queue-idle under the present lock) |
| Entry, table | `mkwvk_init` `:1077-1111`, `start` `:979-1036`, `resize` :1038 (only stores an atomic) | cuts: `fill_replay_table` :1109, `replay_start` :883, mesh warning :1086 |
| Start-up shader screen | `:733-977`, `mkwvk_close_asked` :1063 | **drop** (menu/startup_screen/`prepare_run`/`precompile_run`, all in `vk_draw.cpp:8913-9264`); rewrite a plain prepare loop |
| Present | `present_image` `vk_replay.cpp:4900-5126`, `Frame` :3770, `image_barrier` :3907, `blit_crop_to_rect` :3943, `vk_present_rect.h` | keep acquire :4943, barrier+blit :4955-4999, submit/present :5068-5101, unseen-window rule :4930-4950. **Drop** menu :4923,4926,5046; framegen/HDR :4980-4996; frame log :5001-5012, geometry trace :5013-5043, `RX_VK_FRAME_LOG` :5108; reflex/antilag :5081,5090; `queue_flush_locked` :5068 (batching) |

Threads: `start`/`mkwvk_init` on the host's start thread; `resize` on the window thread (atomic only, `vk_main.cpp:1038`); `present_image` one thread
at a time under `present_image_mu` (`vk_replay.cpp:4899`); queue use under `dev.present_mu` (`vk_internal.h:33-35`).

## b. Layer 0 memory

| Piece | Where | Verdict / cuts |
|---|---|---|
| Window arithmetic | `layer0_windows.h:1-197` (includes std only) | **L**; tested by `tests/layer0_windows_test.cpp` |
| Windows, binds, VRAM class, prefetch worker | `vk_pools.cpp:128-780`, API `vk_pools.h:12-45` | **L-7**: (1) `memory_guest_dma_buf`/`memory_dma_buf_limit` :239,613 (below); (2) `memory_game_pool` :573; (3) `guest_pages_set` :218; (4) `vk_timeline_completed/wait/pushed` :405-407,475,696,713 (`vk_replay.cpp`); (5) `queue_flush_locked` + `dev.present_mu` in the no-sparse-queue branch :200-203 (drop it: require the sparse queue); (6) `bda_window_made` :159 (keep if BDA); (7) `RX_VK_LAYER0*` reads :586-640, stats :760-780. Needs `dev.{device,memory,queue,sparse_queue,sparse_queue_mu,sparse_buffers,features}` |
| dma-buf import | `vk_memory.cpp:147-195` (`dma_buf_memory`), `:270-300` (`dmabuf_ready`: `hos_posix_guest_memfd` by `dlsym` :277, `/dev/udmabuf` :283), `:454-470` | extract (~110 lines). **Linux only**: Windows has no memfd window, so layer 0 is off there (`vk_pools.cpp:612`) |
| Pool table, lookups | `memory_sync` `vk_memory.cpp:592-628` (reads `host->gpu_pools`, `maps_generation`), `game_pool_of` :2656, `guest_span_*` :2666-2682, `memory_game_pool` :2716, `gpu_pool_start/end` :2683-2715 | extract (~120 lines); needs `pools_mu`/`guest_pools` only |
| Page table of guest memory | `guest_pages_set` :484, `memory_guest_pages` :542-569 (`create_buffer` 12 GB table), deferred flush :504-540 | extract; the deferred path calls `upload_command_buffer` (`vk_texture.cpp:2288`): cut it, write at once (layer 0 binds before use) |
| Guest -> (buffer, offset, address) | `gpu_buffer` :2598, `layer0_bind/buffer` `vk_pools.cpp:658-669` | `gpu_buffer_bind` :2760 is replay bookkeeping (write-back notes :2775): rewrite as `layer0_buffer` + a pool check |
| VMA | `vk_vma.cpp:17-35` (`vma_init`), `:138-195` (buffers), `:100-130` images | **L-2**: drop image pools :39-98, budget/stat helpers :196-260; needs `dev.{device,instance,physical,memory,memory_budget}`; `-DVMA_IMPLEMENTATION` in this one file, header from `HOOKSHOT_VMA_HEADERS` |

Threads: any. `layer0_pool_native` runs on the game thread that makes the pool (`host/nvn_native.c:338`); bindings on recording threads; own mutex
`mu` :98 plus one detached worker :332,649; "never hold `mu` while taking `pools_mu`" (:98). Nothing needs the replay thread.

## c. Descriptor heap (`vk_desc_heap.{h,cpp}`, 196 + 888 lines)

| Part | Where | Verdict |
|---|---|---|
| Extension, features, heaps, null entries, strides, ring slots | `.cpp:59-106,429-526`, `.h:30-60` | **L** (only `dev.{device,memory,null_descriptor,physical}`) |
| Encode image/sampler/buffer descriptors, offsets, ring alloc, bind, push | `.cpp:576-770`, `.h:100-160` | **L** |
| `HeapProgram` (binding mappings from `Shader::Info`) | `.h:74-87`, `make_program` `.cpp:287-420` | **L-2**: calls `texture_view_type`, `texture_handle_is_pointer` (`vk_draw.cpp`, declared `.cpp:20-21`) |
| Lazy resolve: `HeapEntry{Texture*}`, reverse maps, in-place / upload / in-stream put | `.h:117-148`, `.cpp:228-280,771-860` | **R**: its subject is the per-draw scan and `Texture` (`vk_texture.h`). Direct writes at registration (`HOS_ENG_REG` is the frozen precedent, `.h:34-44`) |
| Monkey-patched classic descriptor calls | `.cpp:146-160,520-523` (replaces `vkx::vkCmdPushDescriptorSetKHR` etc.) | **drop** (no classic path) |
| Hooks into others | `desc_heap_view_made` in `vk_texture.cpp:493-520`, `_sampler_made`, `_registered` (`vk_replay.cpp`), `_frame` | cuts, with the Texture/sampler code |

Needs `VK_EXT_descriptor_heap` (headers 1.4.363+ present here: `/usr/include/vulkan/vulkan_core.h:16728`) and shader objects
(`.cpp:447-455`). Threads: one recording thread per ring slot (`.h:101`, "off with `RX_VK_PARALLEL`" `.cpp:451`) plus a job ring any thread may use
(`.cpp:611`): with per-thread frame slots (plan, Frame resources) the ring becomes per thread.

## d. BDA

`vk_bda.{h,cpp}` (194 lines): window table (`make_table` :47, a 4 GB null sparse buffer + 4 KB table), `bda_on` :88, `bda_window_made` :143, counters.
**L-3**: `layer0_on`/`layer0_window_address` (:93,67) stay; counters/`BDA_CHECK`/`bda_why` :115-194 drop; `RX_VK_DESC_HEAP_BDA` :83 becomes constant.
Translator side is in Eden: `eden/shader_recompiler/backend/spirv/bda_emit.h` (281 lines: `kWindows` :26, `Pushed` :108, `Translate` :115, `kWideAllowed` :58),
set up in `vk_shader.cpp:548-553` and the stamp :1091. **CPU side is not in `vk_bda.cpp`**: `bda_virtual` `vk_draw.cpp:3169`, `bda_storage_read` :3188,
`_guess` :3208, the 4 push words :4121-4122, `eng_bda_refusal` :5273. Plan's "storage by address for every stage" removes the guess/refusal logic; rewrite ~100 lines.

## e. Timelines, NVN syncs, events

| Piece | Where | Verdict |
|---|---|---|
| `QueueSignals`, `Monotone`, `WaitPlan`, `Census` | `vk_timelines.h:1-146` (no Vulkan), `tests/timelines_test.cpp:13` | **L** |
| Queue timeline semaphores, `queue_signals_into` | `vk_replay.cpp:100-155` | **L-small**: copy 50 lines |
| Event semantics (value map, wait met, signal as GPU write) | `ev_cpu_apply` :659, `event_wait_met` :667, `NOP_SIGNAL_EVENT` :1416-1437, `gpu_write` :701 | reference only |
| Order/hold machine (`qruns`, `QueueRun`, `finish_recording`, `resume_queue`, `submit_job`, `fence_q`, `wait`, `finish`) | `:2899-3600` (~700 lines) | **R**: built around suspend/resume of a serial walk (`suspend_at`, `suspend_calls` :1432-1470). Plan replaces it with chunks cut at Signal/Wait and one ordered `vkQueueSubmit2` (Submission) |
| Back-end table the host calls | `fill_replay_table` :5646-5682 (`nvn_fence_q`, `nvn_wait`, `nvn_event_signal` :5532) | the 8 sync entries are the contract to keep |
| Host side (taken as is) | `host/nvn_native.c`: `nvnQueueFenceSync` :1215, `nvnSyncWait` :1242 (polls with `rx_nvn_wait(fence, 1)`), `nvnEventSignal` :1291 (CPU write then `rx_nvn_event_signal`), `fw_record` :1190, `nvnQueueWaitSync` :2184 | no change needed for the first cut; events live in guest memory, so with layer 0 a GPU event write is a `vkCmdUpdateBuffer` into a window |

## f. Shader path

| Piece | Size | Needs from not taken | Verdict |
|---|---|---|---|
| Eden + sirit + compat | 163 `.cpp`, 32.5k lines, 2.2 MB; 326 objects in `work/sa/build/obj` | none | **L**. Do not copy at M1: link the existing objects (below). Copy when the first translator change lands, since `nx2/build.py:884-896` stamps the caches by these files |
| `vk_shader.cpp` | 2815 | `texture_of`/`texture_slot_version` (`vk_bank.cpp:182,220`; the variant "answers" :320-340), `ub_read`/`ub_generation` (`vk_replay.cpp:900,768`: stream uniforms), `gpu_pool_end`, `vk_jobs`, `vk_draw.h` (:56; `prepare_run` etc. live in `vk_draw.cpp:8913`), `vk_bda`, `vk_shader_object`, `coopmat` globals, `dev.{driver_id,mesh_*,native_mma,sg32_stages,subgroup_*,props}` | **L-9** |
| `vk_shader_object.cpp` | 649 | `program_layout`, `fallback_prefetch`, `so_push_size` (`vk_draw.cpp`, declared `vk_shader_object.h:57-61`), `desc_heap_program` :236, `vk_jobs`, `ub_thread_off` | **L-4** |
| `vk_bank.cpp` | 434 | `gpu_pool_start/end` :287,297 (layer 0), `host->gpu_read` | **L-3**: registration tables `:124-232` (texture/sampler by pool id, versions) are what the shader answers need |
| `vk_coopmat.cpp` | 411 | `dev.queue` for a GPU self-test :110 | **L-1**; sets Eden globals the translator and the cache stamp read (`vk_shader.cpp:1112-1121`) |
| `vk_jobs.*` | 137 | none | **L** (2-4 workers :62) |

What a pick needs of `NvnState` (`vk_state.h:14-60`): `prog` + `has_prog` (stage code via `host->gpu_read(sd.control/data)` `vk_shader.cpp:150-152`),
`vattr[16]`+`nvattr` (:1612,1992), `ubo[stage][16]` (c3.. words :613-624), and for banks `tex/samp/img/ssbo/ubo/cs_push/grid`
(`vk_bank.cpp:79-105,234-380`), plus `bind_gen`/`tex_gen` only for the pick memo (:2069-2141, droppable). Plus the fragment `color_types[8]` from the pass.
A direct `PickState` of ~8 fields replaces it; the draw fills it from the recorded packet.

Caches: stamp = `HOOKSHOT_SHADER_STAMP` (sha1 of eden/sirit/compat + `vk_shader.cpp`, `vk_bank.cpp`, `vk_state.h`, `vk_api.h`, `nx2/build.py:884-896`)
+ depth mode, mesh flags, lanes, BDA bits, struct sizes, coopmat flags, env prefixes (`vk_shader.cpp:1072-1126`). A file with another stamp is
**truncated and rewritten** (`:1243-1252`; shader objects: `vk_shader_object.cpp:80-135`, keyed by driver UUID). Files: `hookshot_programs.bin/.idx`
(:1131), `hookshot_shader_objects.bin`, `hookshot_computes.bin`, pipeline notes; the seed `hookshot_shaders.bin` (:2398) is input only.
**So `gpu/direct/` must name its own cache files** (e.g. `hookshot_direct_*.bin`) or a switch of `RX_BACKEND` wipes a multi-GB cache each time.
Prefetch: `so_program_set` (:632) queues on `prefetch_thread` (:524) from `nvn_program_set` (game thread); the pick runs on whichever thread draws.

## g. Small utilities

| Item | Where | Verdict |
|---|---|---|
| `logf` | `vk_main.cpp:80` (to `host->log`) | **L** |
| `create_buffer`, `create_device_buffer`, mapped/flush | `vk_vma.cpp:138,183,171-179` | **L** (with b) |
| NVN -> VkFormat, ASTC native check, block size | `vk_formats.cpp:1-162` (`dev.physical` only; `host->fmt_bpb` :144) | **L**, verbatim |
| Sampler creation | `sampler_make` `vk_draw.cpp:1859-1960` (`sampler_of` `vk_bank.cpp:227`) | rewrite small (~80 lines), from `nvn_samp_par` |
| `game_module_path` | `vk_paths.h:1-37` | **L** |

## h. Build

`nx2/build.py` builds `hookshot_vk.so` into `work/sa/build/` (`VK_LIB` :66): `vk_sources()` :218 (Eden, sirit, `gpu/vk/*.cpp`, two suyu decoders),
flags `vflags` :897-908 (`-std=c++20 -O2 -DVK_NO_PROTOTYPES -DMKWVK_BUILDING`, `-I` eden/sirit/compat/gpu/vk/`gen`, `HOOKSHOT_SPIRV_HEADERS`,
`HOOKSHOT_VMA_HEADERS`, Vulkan headers :856-858), one job per file into `obj/vk_<path>.o` :951-964 with an sha1 stamp (`compile_one` :689), link :1115-1136
(`clang++ -shared -fuse-ld=lld -static-libstdc++ @vk_objs.rsp`, then `check_vk_symbols` :69). `RX_VK_ONLY=1` skips the lift and the exe (:746,975).
Headers: `HOOKSHOT_SPIRV_HEADERS=$PWD/work/deps/SPIRV-Headers HOOKSHOT_VMA_HEADERS=$PWD/work/deps/VulkanMemoryAllocator` (docs/development.md, "Iterating quickly").

Smallest change for a second library (~25 lines, no existing line changed):
1. `nx2/build.py`, **after** the `if build_vk:` branch: `direct_sources()` globs `gpu/direct/*.cpp`; one loop `for dsrc in ...` (not named `src`/`vk_sources`) appends jobs to a new
   `direct_jobs` with `vflags` + `-I gpu/direct` first (a same-named header in `gpu/direct` wins by quote-include rules; `vk_api.h` still comes from `gpu/vk`) and objects `obj/dx_<name>.o`;
   link `hookshot_direct.so` from `direct_jobs` + the already-built `vk_eden_*`/`vk_sirit_*` objects + `vk_posix_win32_posix.o`, same link line and `check_vk_symbols`;
   its own `-DHOOKSHOT_SHADER_STAMP` of the direct tree. No imgui, zstd, fonts, NGX, FSR.
2. `host/rx_backend.c:257-260`: `direct = b && !_stricmp(b,"direct")`, add it to the `if` (today anything but unset/`vulkan`/`eden` falls to WebGPU) and
   `dll = direct ? "hookshot_direct.dll" : ...`. On Linux `LoadLibraryA` maps `x.dll` to `x.so` (`host/posix/win32_posix.c:771`). Extra exports (`mkwvk_close_asked`
   style) are looked up by name (`rx_backend.c:285-300`); the table is `vk_api.h`'s, so no `MKWVK_API_VERSION` bump until the seam needs a struct change.
3. Constraint: `tests/test_shader_stamp_cache.py:24-34` execs the single `vflags` branch and `tests/test_dlss_sdk_cache.py:25-29` needs exactly one `for src in vk_sources()` loop:
   leave both untouched and put (1) outside the branch.

Incremental time (estimate from `work/build*.log`, not measured): one `.cpp` 2-10 s (`vk_draw.cpp` 10 s, the worst), link <1 s (`linked ... (0s)`), plus header hashing; host-only change ~40 s.

## i. Tests

`tests/` has 359 files; 183 mention `gpu/vk`. By source read as text (count of test files naming it): `vk_draw` 56, `vk_replay` 45, `vk_menu` 22, `vk_texture` 21,
`vk_memory` 20, `vk_shader` 20, `vk_main` 12, `vk_api` 11, `vk_framegen` 5, `vk_bank` 4, `vk_xfer` 4, `vk_jobs` 4, `vk_vma` 3, `vk_pools` 2 (`test_layer0_windows.py:45,91`,
`test_bda_emit.py:181`), `vk_timelines` 2. All hard-code `gpu/vk`; two glob `gpu/vk/*.cpp` (`test_submit_thread.py:39`, `test_wb_diff_cpu_writes.py:42`). They keep passing as long
as the frozen tree is not edited; copies into `gpu/direct/` are invisible to them. `nx2/build.py` is read by `test_dlss_sdk_cache.py`, `test_shader_stamp_cache.py`,
`test_build_times.py`, `test_cpm_discovery.py`, `test_compile_retry_jobs.py` (h.3).
Would run against a new directory with a path argument: `tests/layer0_windows_test.cpp` (header only), `gpu/vk/tests/timelines_test.cpp` (`#include "../vk_timelines.h"`),
`present_rect_test.cpp`, the `test_source_by_text` kind with `--source` flags (`test_import_span.py:28`, `test_import_write_watch.py:99`). `tests/run_all.py:244-270` lists the C++ unit tests
by name: a new test needs a line there. Failing at HEAD with headers found: `nx2/test_texture_init_barrier.py` and `tests/test_parallel_init_order.py` (both cut `create()` of `vk_texture.cpp`: `no member clear_pending`, source drift), `tests/test_bda_emit.py` (SPIR-V header path), and 8 `nx2/test_*` SPIR-V checks (not investigated).

## j. What the new back end still needs from `vk_texture.cpp`, small

| Need | Old code | Note |
|---|---|---|
| NVN format -> Vulkan, block size | `vk_formats.cpp:34-150` | lifts (g) |
| Image from a `nvn_texdesc`: key, class, usage, mutable/block-texel/cube/2D-array flags | `texture_key` :2313, `create` :1824-2130 (usage :1979-2016, flags :1986-2026), `format_class` :138, `widened` :101, `linear_twin` :86 | ~150 lines of the 300; drop sparse :3303-3440, eviction, takeover :1771, pools :244-300, BC3/ASTC re-encode :454-770; ASTC is native here (`vk_formats.cpp:36`) |
| View: target, levels, layers, swizzle, X-format alpha, 3D->2D, storage format | `texture_view` :2687-2810, `make_view` :493, `component` :147, `view_level/layer` :2298-2310 | lifts almost whole (~120 lines); `desc_heap_view_made` :493 stays for the heap |
| Render-target images | `create(..., as_target)` :1824, :2041-2084 (pool key), `texture_for_present` :3503 | targets come from `host/rt_slots.c`; one `create_image` with COLOR/DEPTH usage |
| Initial layout/clear | `create` :2113-2130, `texture_clear_now` :2385 | plan: load ops (Passes) |
| WriteTexels / ReadTexels (CPU, block-linear walk) | `texture_read_texels` :4089-4160, `texels_fit` :4071 | lifts (~70 lines); cut `guest_cpu_wrote`/`memory_stamp`/`by_gpu` :4146-4152; `read_texels`/`write_texels` wrappers `vk_replay.cpp:5207,5266` |
| CopyBufferToTexture / T2B / T2T | `copy_b2t` `vk_xfer.cpp:169`, `copy_t2t` :240, `Sub` :18 | rewrite small from the same `VkBufferImageCopy` arithmetic; drop BC re-encode (`texture_copy_into_bc`) |
| Samplers, texel buffer views | `sampler_make` `vk_draw.cpp:1859`, `texel_buffer_view` `vk_texture.cpp:3474` | rewrite small |

## Decision for the plan: `gpu/direct/` at M1

M1 needs a, b, e and g only (every list is a counted no-op; no shaders, no textures).

| File | Origin | Verdict |
|---|---|---|
| `direct_main.cpp`, `direct_internal.h` | `vk_main.cpp:32-526,559-731,1077-1111`; slim `Device`/`BufferRef`/`logf` | copy + 12 cuts |
| `vk_loader.h`, `vk_paths.h` | verbatim; prune loader lists | L |
| `vk_vma.cpp` | `vk_vma.cpp:17-35,100-195` | L-2 |
| `layer0_windows.h`, `vk_pools.{h,cpp}` | verbatim, 7 cuts (b) | L / L-7 |
| `direct_memory.cpp` | extract from `vk_memory.cpp:147-195,270-300,454-570,592-628,2656-2735` | new, ~250 lines |
| `direct_present.cpp` | `vk_replay.cpp:3770-3990,4900-5126` minus menu/framegen/reflex/log | new, ~150 lines |
| `vk_present_rect.h`, `vk_timelines.h`, `vk_jobs.{h,cpp}` | verbatim | L |
| `direct_sync.cpp` | new per Submission; reuse `vk_replay.cpp:100-155` and the event rules :659-700,1416-1470 | R |
| `vk_formats.cpp` | verbatim (needed by M2; harmless now) | L |

M2 adds `vk_shader.cpp`, `vk_shader_object.cpp`, `vk_bank.cpp`, `vk_coopmat.cpp`, `vk_desc_heap.*` (L-6), `vk_bda.*`, `direct_image.cpp` (j), with the direct cache
file names (f) and Eden **linked from the frozen objects**; fork `eden/` into `gpu/direct/eden` at the first translator edit (M3), at the same time as the one planned cache
invalidation. Build per h. Windows is out of scope here: layer 0 and the dma-buf path are Linux-only (b), which matches the plan's single-PC target.
