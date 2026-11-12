# M0: the seam audit (2026-11-12)

Analysis only; nothing was run. `nn` = `host/nvn_native.c`, `nl` = `host/nvn_list.c`, `rb` = `host/rx_backend.c`,
`api` = `gpu/vk/vk_api.h`, `rxh` = `gpu/render/rx_nvn.h`. Line numbers are of the tree at 23d0aa6.

## 1. Is cmd_alloc the single path?

Yes for every record an NVN call makes. All of them end in `cmd_alloc` (`nn:1660`) through `rec()` (`nn:2118`, used by
`REC`/`RECN`) or by hand (`cmd_alloc` + `put_hdr`: BEGIN `nn:1886`, END `nn:1974`, SIGNAL/PUSH_CONST `nn:2175`,
UPDATE_UB `nn:2240`, `eng_mark` `nn:1605`). Exceptions, all of which a direct mode has to deal with:

| # | Writer | Where | What |
|---|---|---|---|
| 1 | `fw_record` | `nn:1190-1203` | a hand-built BEGIN / WAIT_FENCE / END in a static ring of 1024; its address is passed as a handle (`nn:2919`, `nn:1206-1212`). Not an lrec, not in the handle set |
| 2 | `inline_recording_raw` | `nn:2087-2090` | default path: memcpy of the callee's records into the caller (CallCommands / CopyCommands) |
| 3 | `nvn_list_call` | `nl:495-534` | LIST_REF path: writes NOP_CALL straight into the writer (`nl:520-526`), or `expand_into` the callee past depth 7 (`nl:501-508`) |
| 4 | `inline_recording` | `nn:2055-2065` | NOP_PKT via `rec()` (HOS_ENG_LISTS), then (2) or (3) |
| 5 | patches after End | `resolve_deferred` `nn:2866-2889`; `def_rw` / `resolve_flat` `nl:315-361` | rewrite `h[]` / `r[]` of deferred binds from the game's array at submit |
| 6 | read-only | `eng_lists.c:162-240` (packet builder at End), `eng_jobs.c:87-93` (hands the flattened copy to the back end) | decode records, write none |

`NOP_JUMP` is never written (no writer in `host/`). `cmd_alloc` itself writes only a zero header (`nn:1669`); the payload is
filled by the caller after it returns. So "emit at put_hdr" is too early: emit needs a flush point after the fill (section 6).
`hw_fix` (`nn:1673`) and `o->hw_used -= ...` adjust only the byte accounting the game sizes its heaps from.

## 2. Ops

Payload structs: `rxh:164-248`. "Final at submit" = the payload holds a guest address or pointer read later.

| Op | Payload | NVN function(s) | Final at submit? |
|---|---|---|---|
| BEGIN / END | none | Begin/EndRecording `nn:1887,1975` | no |
| CALL, PKT | `nvn_p_call`, `nvn_p_pkt` | CallCommands, CopyCommands (`nn:2095,2109`) | no (host objects) |
| SET_RT | `nvn_p_set_rt` | SetRenderTargets `nn:2277` | no: tex pointers are pinned copies (`tex_copy` `nn:2662`), views copied |
| CLEAR_COLOR / CLEAR_DS / CLEAR_TEX | `nvn_p_clear_*` | ClearColor, ClearDepthStencil, ClearTexture[ui] | no |
| VIEWPORTS, SCISSOR, DEPTH_RANGE, DEPTH_CLAMP, RENDER_ENABLE, RASTER_DISCARD, STENCIL, POLY_OFFSET, PRIMITIVE_RESTART, BLEND_COLOR | small structs | SetViewport[s], SetScissor[s], SetDepthRange[s], ... `nn:2308-2737` | no |
| RENDER_COND | `addr` | SetRenderEnableConditional `nn:2350` | yes: counters read at replay (`rxh:170`) |
| BIND_PROGRAM | `const nvn_prog *` (guest object) | BindProgram `nn:2364` | pointer into guest memory; read when used |
| BIND_STATE / VATTR / VSTREAM / VBUF | bytes copied | Bind*State, BindVertex* `nn:2370-2403` | no |
| BIND_UBO / SSBO | `stage,index,addr,size` (+census trailer) | Bind[Uniform|Storage]Buffer[s] `nn:2408,2480,2491-2502` | no |
| BIND_BUFS (deferred) | `src` + `r[]` | Bind*BuffersDeferred `nn:2503-2517` | **yes**: `src` array re-read at submit (`rxh:185`, `nn:2884`) |
| BIND_TEX / SAMP / IMG | `src` + `h[]` | BindTexture[s], BindSeparate*, BindImage[s] (immediate, src 0) `nn:2547-2554`; `*Deferred` `nn:2557-2560` | deferred ones **yes** (`rxh:183`, `nn:2877`) |
| SET_TEX_POOL / SET_SAMP_POOL | `const nvn_dpool *` (guest) | SetTexturePool, SetSamplerPool `nn:2624` | pointer to guest object |
| DRAW, DRAW_IDX | `nvn_p_draw[_idx]`, `ibuf` | DrawArrays[Instanced], DrawElements[BaseVertex|Instanced] `nn:2580-2616` | no |
| DRAW_INDIRECT, DISPATCH_INDIRECT, DRAW_MESH | arg / count addresses | Draw*Indirect, MultiDraw*IndirectCount, DispatchComputeIndirect, DrawMeshTasks* `nn:3205-3234` | **yes**: arguments read at replay after GPU writes (`rxh:241`) |
| DISPATCH, BARRIER, PUSH_CONST | ints; push bytes copied | DispatchCompute, Barrier, PushComputeConstants `nn:2620-2623,2170` | no |
| UPDATE_UB | `dst,size` + data copied at record | UpdateUniformBuffer `nn:2233` | no (copy), written in GPU order |
| COPY_B2T / T2B / T2T / B2B, COPY_STRIDE, CLEAR_BUF | addresses, regions | CopyBufferToTexture ... `nn:2226,2680-2730` | no (addresses final) |
| REPORT_COUNTER, RESET_COUNTER | `type,addr` | ReportCounter, ResetCounter `nn:2194-2220` | addr is guest memory the replay writes |
| SIGNAL_EVENT, WAIT_EVENT | event gpu address, mode, value | SignalEvent, WaitEvent `nn:2128-2160` | no |
| WAIT_FENCE | fence value | none: `fw_record` only (QueueWaitSync under RX_VK_TIMELINES `nn:2184-2190`) | no |
| JOB, ENG | marks | BeginRecording/EndRecording (job), CallCommands at 3 call sites (`nn:2095-2107`, `eng_call_site` `nn:1596`) | no GPU work |
| none recorded | | CommandBufferWaitSync (`nn:2160`), SetShaderScratchMemory, SetRecordingMode (`nn:3078`), SetLineWidth ... | |

Only three things are not final at record: the deferred handle / buffer arrays, the indirect arguments and conditions
(GPU-written), and pointers to guest objects (`prog`, `dpool`). Everything else is value data.

## 3. Command-buffer lifecycle

- **Object**: `nvn_cmd` (`rxh:110-124`) is guest memory; the recording buffer lives in a host slot table, `cmd_rec`
  (`nn:1457-1468`, `cmd_adopt` `nn:1519`, `cmd_host` `nn:1538`; slots under `cmd_lock` `nn:1471`, or thread-local free
  lists with LIST_REF `nn:1501-1518`). `Initialize` takes a slot (`nn:1612-1622`); for a render job it also saves
  x25 / x24 (view record, cmd slot) when LR = `0x800852c0` (`nn:1573,1621`). `Finalize` releases it (`nn:1623`).
- **Memory**: `AddCommandMemory` only resets accounting (`nn:1676`). Memory callbacks are never called (`nn:1628`).
  `AddControlMemory` (`nn:1847`) releases every handle recorded over that control range (`release_copies` `nn:1762`, or
  `nvn_list_release` + `nvn_list_drain` with LIST_REF), then sets ctl and `ctl_used = 0`. Each End takes 64 bytes of
  control memory for the descriptor (`nn:1937,1997`) and writes a mark there (`COPY_MARK`), swept every 25,000
  recordings (`nn:1803-1840`).
- **BeginRecording** (`nn:1866-1897`): sets `recording`, opens the writer (`nvn_list_open` or `cur = seg`), writes BEGIN.
  Available: the guest context `c` of the calling thread: LR `c->x[30]` (already used: `eng_lists_kind(c->x[30])`
  `nn:1871`, table `eng_lists.c:46-56`: 6 load-time kinds), frame pointer `c->x[29]` (`guest_chain` `nn:1928`), sp,
  x22/x25/x28 for shapes and jobs (`nn:1875-1881`, `eng_jobs.c:73`), the host thread (`hos_self()` `nn:2455`).
  Job identity comes from a hook on `f_80084de0` that fills a thread-local `jt` (`eng_jobs.c:43-61`), with BeginRecording
  checking LR `0x8008533c` (`eng_jobs.c:16,66`). Kinds not yet in code (layer lists, gate, async, queue heads) are in
  `work/assets/native/engine-frame.md:30-66` (return addresses partly given there, not re-verified).
- **EndRecording** returns the handle (`nn:1965-2033`). Default: `flatten` copies BEGIN..END into a pool block with a
  prefix (`nn:1900`), filed in `rec_buckets` by control position under `copy_lock` (`nn:1997-2021`), added to the live
  set (`nn:2022`). LIST_REF: `end_recording_ref` (`nn:1935`) publishes the writer's own allocation as an lrec with a
  generation id (`nl:535-575`); no copy, no `copy_lock`. A handle is a host address (default) or lrec id; it is used only
  if in the live set (`check_handle` `nn:2035`).
- **CallCommands / CopyCommands** (`nn:2095,2109`): per handle `inline_recording`: default copies the callee's records
  into the caller (`nn:2072-2093`), LIST_REF records NOP_CALL + a reference (`nl:495`). Accounting adds the callee's flat
  size (`nn:2077-2082`): the game sizes heaps from it.
- **Lifetime**: handle valid until AddControlMemory over its position or an overwritten mark (`nn:1741-1745`,
  `host/nvn_list.h:18-35`); bytes live until `rx_nvn_replayed()` passes the submits pushed before the release
  (`free_dead_copies` `nn:1747-1761`, `dead[].after`); with LIST_REF also until no caller holds a reference. These rules
  need `rx_nvn_pushed` / `rx_nvn_replayed` from the back end.

## 4. Submit and sync

`nvnQueueSubmitCommands` (`nn:2891-2965`), on the guest thread that calls it (the Presentation Thread for the frame):

1. `hos_renderer_wait` once (`nn:2892`); copy up to 64 handles (`nn:2896`).
2. LIST_REF: `nvn_list_submit` (resolve deferred binds in place, or private copies on conflict, `nn:2910`, `nl:767`);
   default: per handle `check_handle` + `resolve_deferred` (`nn:2915-2918`). Both read the game's arrays now because the
   game rewrites them right after.
3. RX_VK_TIMELINES: `fw_take` + `fw_record` puts a WAIT_FENCE handle first (`nn:2920-2922`).
4. Unless HOS_NVN_HOLD (default off, `nn:1111`): `hos_vcore_away`, `rx_nvn_submit_q(h, n, queue)` (`nn:2931`), then
   `nvn_list_submitted(rx_nvn_pushed() + held_count())`. The replay runs inside that call (comment `nn:2926`).

| Call | Host work | rx_* calls |
|---|---|---|
| `nvnQueueFenceSync` `nn:1215` | `fw_flush`; `s->fence = rx_nvn_fence_q(queue)` | `rx_nvn_submit_q` (only a pending wait), `rx_nvn_fence_q`, `rx_nvn_fence` |
| `nvnSyncWait` `nn:1242` | unfenced: timeout; else poll then wait | `rx_nvn_fence`, `rx_nvn_wait` (twice) |
| `nvnQueueWaitSync` `nn:2184` | only `fw_put` (TIMELINES); else nothing | none |
| `nvnEventSignal` `nn:1291` | writes value to guest memory | `rx_nvn_event_signal(gpu, v)` |
| `nvnEventGetValue` `nn:1279` | `nvn_gpu_read` | none |
| `nvnQueueFlush/Finish` `nn:1329-1337` | `fw_flush`, `held_flush` | `rx_nvn_finish` |
| `nvnWindowAcquireTexture` `nn:3025` | paces the frame (`hos_vi_native_acquire`); marks the sync signalled | none |
| `nvnQueuePresentTexture` `nn:3038` | `fw_flush`, `held_flush`, `rt_slots_frame`, `hos_vi_native_presented` | `rx_nvn_present(tex, crop)` |
| CommandBuffer SignalEvent / WaitEvent `nn:2128,2145` | record only | none (replay orders them) |

## 5. Host <-> back end interface

Load and choice: `rx_start` (`rb:256-273`): RX_BACKEND unset or `vulkan` loads `hookshot_vk.dll`, `eden` loads
`mkw_eden.dll` (`load_vulkan` `rb:235`), anything else is WebGPU. The DLL's one export `mkwvk_init(host)` returns the
table (`api:195`); the DLL refuses a host of another version (`gpu/vk/vk_main.cpp:1079`), the host refuses a table of
another version (`rb:249`) and falls back to WebGPU. `MKWVK_API_VERSION` is 14 (`api:18`); a change to either table
bumps it (AGENTS.md). Optional exports are found by name: `mkwvk_close_asked` (`rb:284`), `mkwvk_eng_job_recorded`,
`mkwvk_eng_job_deferred` (`rb:295,306`). Texture and sampler registrations made before `start` are queued and replayed
(`rb:176-232`).

**MkwVkHost** (`api:59-134`, filled `rb:157-170`):

| Class | Members |
|---|---|
| object layer, from the start | `version, log, log_flush, guest_base, guest_size, gpu_ptr, gpu_guest, gpu_read, gpu_write, ticks, now_ns, gpu_pools, maps_generation, vmap_generation, depth_minus_w, fmt_bpb, reserved_cpus` |
| old renderer only (layout, write watch, readback) | `tex_layout, tex_level_offset, tex_level_tiles, tex_linear_stride` (no block-linear in the new back end, plan "Textures"), `collect_written, collect_written_in, free_pages, mark_written, watch_guest_writes, guest_alloc` (Eden's banks) |
| option / menu / overlay | `pad_raw, binding, bind, aspect_ratio, take_key_press, pointer, mic_count, mic_name, mic_select, gyro_status, prompt_face, romfs_dir` (stub: return 0 / null) |

**MkwVkRenderer** (`api:137-189`, routed `rb:316-479`):

| Class | Members |
|---|---|
| object layer, needed from the start | `start, resize, describe, nvn_register_texture, nvn_register_sampler, nvn_set_texture_pool_size, nvn_texture_created, nvn_texture_released, nvn_program_set, nvn_program_released, nvn_pool_created, nvn_pool_native, nvn_vmap_changing, nvn_invalidate, nvn_read_texels, nvn_write_texels, guest_imported` |
| sync, kept (host waits and handle lifetimes call them) | `nvn_fence, nvn_fence_q, nvn_pushed, nvn_replayed, nvn_wait, nvn_finish, nvn_event_signal, nvn_present` |
| command / submit, replaced by emit | `nvn_submit` (only `held_flush` and the non-`_q` fallback call it), `nvn_submit_q` (becomes the submit-time call) |
| option / menu | `option, option_set` (host passes defaults when null, `rb:454`), `stats` |
| old renderer only | `wait_writebacks_reading` (`host/nn_libc.c:27`, readback), plus the exports `mkwvk_eng_job_recorded/deferred` |

## 6. Smallest host change for direct mode

1. **Selection**: `RX_BACKEND=direct` loads a new DLL (add a case at `rb:257-259`); API version 15 adds to
   `MkwVkRenderer`: `list_begin(kind, lr, fp, view, thread) -> void*`, `emit(void *list, uint32_t op, const void *payload,
   uint32_t size)`, `list_end(list) -> void*` (sets `lrec->user`), `list_call(list, callee_user)`, `list_released(user)`.
   `nvn_submit_q` stays; the back end snapshots deferred arrays there from the `src` fields it kept at emit.
2. **Handle table**: direct mode rides on `list_ref()` (`nn:1352`): handles, control-range release, lifetime rules exist
   and `nvn_lrec.user` plus the hooks (`nn:1385-1387`, `nl:29`) already carry a per-list pointer. Add a "no bytes" flag so
   `cmd_reserve_rec` (`nn:1639-1643`) skips `nvn_list_reserve` and the lrec is a header only; `flat` is the host's
   `o->cmd_used - o->rec_start` at End. `eng_jobs_on` today refuses LIST_REF (`eng_jobs.c:25-26`): relax it, since the
   job tag (`jt`, view in x25) is the one input direct mode needs from that hook.
3. **BeginRecording** (`nn:1866`), after `nn:1871`: `r->dl = rx_list_begin(classify(c->x[30]), c->x[30], c->x[29], c->x[25], thread)`.
   `classify` extends `eng_lists_kind` (`eng_lists.c:46`) with job (`0x8008533c`), layer, gate, async, queue-head
   addresses (engine-frame.md); unknown returns "packet", counted.
4. **Emit point**: in `cmd_alloc` (`nn:1660`), if `r->dl`: first flush the thread's pending record (`emit(pending.list, op, p+8,
   size-8)`, op read back from the header), then hand out a thread-local scratch (64 KiB + 32, UPDATE_UB is at most
   64 KiB, `nn:2237`) instead of `r->cur`; keep the `cmd_used` / `hw_used` / `last_rec` accounting (`nn:1664-1668`) as is,
   since `hw_fix` and the game's heap sizing depend on it. Flush also at the top of EndRecording (`nn:1965`) and
   CallCommands / CopyCommands (`nn:2095,2109`), and before `nvnQueueSubmitCommands` (`nn:2891`). This covers `rec()`,
   BEGIN/END, and the hand-written puts (all four go through `cmd_alloc`). A table-level wrapper on the NVN dispatch
   (`nn:3458`) is the alternative; it flushes after every call at one indirect each.
5. **Exceptions from section 1**: (2)(3)(4) become `inline_recording_raw` (`nn:2072`) calling `rx_list_call(r->dl, lrec->user)`
   and adding the callee's `flat` to the accounting as now; (5) vanishes (the back end copies at submit); (1) `fw_record`
   stays a side record: either the new submit decodes it, or add `rx_queue_wait_fence(queue, fence)` and drop the handle.
6. **Skipped at submit** (`nn:2905-2918`): `check_handle`, `resolve_deferred`, `nvn_list_submit` and its private copies;
   `eng_lists_*` (E2 packets) and `eng_jobs_end`'s flattened copy (`nn:2026`).

**Recording-thread work that remains**: the accounting in `cmd_alloc`; `tex_copy` under an exclusive `pin_lock`
(`nn:2662-2678`, one per target per SET_RT, about 118 a frame, `engine-frame.md:35`); the handle publish, control-range
filing and the 64-byte mark write (`nn:1935-1964`, `nl:535-575`); `nvn_list_release` at AddControlMemory (`nn:1849`,
per-256-bucket spinlocks, `nl:603`); the sweep every 25,000 recordings (`nn:1954-1962`); logging counters
(`sc_add`, `InterlockedIncrement` in SignalEvent, ReportCounter, DrawMesh, `nn:2128-2220,3218`). **Skippable**: of the
3.3 ms under `copy_lock` (`layer1-plan.md:56-58`: `release_copies` 1.53, `flatten` 0.86, `inline_recording` 0.89), `flatten`
and `inline_recording` are gone (no bytes, a call is one `list_call`), and `release_copies` is replaced by the lrec
release, which layer1-plan S1 (`layer1-plan.md:214`) already expects to remove the lock contention of. Not measured here.

## Decision for the plan

The seam holds: `cmd_alloc` is the one write path for NVN commands, with the exceptions of section 1 (a side-record
ring for fence waits, call copies, submit-time patches), none of which hides an op. Two corrections to the plan's
text: emit cannot sit in `put_hdr` (the payload is filled after it returns), it needs a flush point (thread-local
pending record, flushed at the next alloc, End, Call and submit); and direct mode needs the LIST_REF handle table
(which also removes `copy_lock`), so `eng_jobs_on`'s LIST_REF refusal (`eng_jobs.c:25-26`) must be lifted. Three
payload classes are not final at record (deferred arrays, GPU-written indirect arguments, guest-object pointers
`prog` and `dpool`); the first is the plan's submit-time snapshot, the other two the back end reads when it uses them.
Host API: bump `MKWVK_API_VERSION` to 15, keep the object layer and the fence / pushed / replayed / wait calls (the
handle lifetimes and `SyncWait` depend on them), stub the option and menu members, add the five list calls. Open: the
return addresses of layer, gate, async and queue-head lists (engine-frame.md only), and `fw_record`'s handle form.
