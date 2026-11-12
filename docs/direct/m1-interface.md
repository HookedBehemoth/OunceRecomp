# M1: the host / back end interface and the skeleton (2026-11-12)

Fixed by the plan's owner from `m0-seam.md` and `m0-reuse.md`. M1's goal (plan, Milestones): the game boots to Kokiri
and Hyrule Field on the direct back end with a cleared window and correct syncs; every list is recorded as counted
no-ops. It yields the game's own frame time with a free renderer.

## Selection

`RX_BACKEND=direct` loads `hookshot_direct.so` (sources `gpu/direct/*.cpp`, built by a block added after the
`if build_vk:` branch of `nx2/build.py`: m0-reuse.md h). The frozen library and `gpu/vk/vk_api.h` are not changed:
`MKWVK_API_VERSION` stays 14, so the frozen back end's caches and tests are untouched. The direct library answers
`mkwvk_init` with a version-14 table for the object layer, sync and present, and adds the exports below, which the
host finds by name (as it finds `mkwvk_close_asked`). Its cache files, when it gets any, have their own names.

## The list calls (`gpu/direct/direct_api.h`, C, included by the host)

```c
enum { HSD_LIST_PACKET, HSD_LIST_LOAD, HSD_LIST_JOB, HSD_LIST_LAYER, HSD_LIST_GATE, HSD_LIST_COMPUTE, HSD_LIST_HEAD };
typedef struct hsd_list_info {
    uint32_t kind;        /* HSD_LIST_*; PACKET = not classified (counted, its lr logged once) */
    uint32_t load_kind;   /* HSD_LIST_LOAD: eng_lists_kind's value */
    uint64_t lr, fp;      /* the guest return address and frame pointer of BeginRecording */
    uint64_t cmd;         /* the guest NVNcommandBuffer object */
    uint64_t pass_key;    /* HSD_LIST_JOB: the recorder's pass key pointer (x3 of f_80084de0: m0-job-formats.md) */
    uint64_t view;        /* HSD_LIST_JOB: the view record (x25 at BeginRecording), for logs only */
} hsd_list_info;

void *hsd_list_begin(const hsd_list_info *info);                         /* recording thread */
void  hsd_emit(void *list, uint32_t op, const void *payload, uint32_t size);   /* recording thread; payload valid during the call */
void  hsd_list_call(void *list, void *const *callees, uint32_t n);       /* CallCommands / CopyCommands, in order */
void  hsd_list_end(void *list);                                           /* complete; valid until released */
void  hsd_list_released(void *list);                                      /* the handle is gone (any thread) */
void  hsd_submit(uint64_t queue, void *const *lists, uint32_t n);         /* the guest's submit, in handle order */
void  hsd_queue_wait_fence(uint64_t queue, uint64_t fence);               /* nvnQueueWaitSync, instead of fw_record's side record */
```

`op` and the payloads are `gpu/render/rx_nvn.h`'s records. Deferred binds arrive with their `src` field; the back end
reads the game's array at `hsd_submit`, never the host.

## Host changes (`host/`), all behind the one boot-time mode

Direct mode is one flag read once at start (`rx_backend.c` knows the choice). With it off, nothing in the host may
behave differently: the frozen back end is the oracle.

1. Lists by reference are forced on (`list_ref()`), with a "no bytes" list: `cmd_reserve_rec` reserves nothing, the
   list record is a header whose `user` is the back end's list pointer. The byte accounting the game sizes its heaps
   from (`cmd_used`, `hw_used`, `hw_fix`, the callee's flat size at a call) stays exactly as it is.
2. `cmd_alloc` hands out a thread-local scratch record and flushes the thread's pending record to `hsd_emit` at the
   next `cmd_alloc`, at EndRecording, at CallCommands / CopyCommands and before a submit (m0-seam.md 6.4).
3. BeginRecording classifies by return address and calls `hsd_list_begin`. The job hook (`host/eng_jobs.c`) is allowed
   with lists by reference in direct mode and also notes the recorder's x3 (the pass key pointer).
4. CallCommands calls `hsd_list_call`; the submit calls `hsd_submit` with the lists' `user` pointers and skips
   `check_handle`, `resolve_deferred`, `nvn_list_submit`'s copies, the E2 packets and the E3 job hand-off.
5. `nvnQueueWaitSync` calls `hsd_queue_wait_fence`.

The classification table (job, layer, gate, compute, queue head, load kinds) is verified in M1 by a run: the log
names every return address seen per kind and every unclassified one. engine-frame.md's addresses are the starting
point, not the truth.

## The back end at M1 (`gpu/direct/`)

Files as m0-reuse.md's decision table (device and swapchain, layer 0 pools, present, timelines, formats).

- A list is an object with per-op counters and, in order, the ops the frame level needs: SIGNAL_EVENT, WAIT_EVENT,
  REPORT_COUNTER, RESET_COUNTER, calls. Everything else is counted and dropped.
- `hsd_submit` walks the lists in call order, makes each event signal take effect in order (the value the game polls),
  writes report results the game can live with (timestamps that advance; sample counts are stated in the M1 report,
  whatever is chosen), and sends one real, empty `vkQueueSubmit2` that signals the queue's timeline value, so fences
  and `nvnSyncWait` are honest waits on the GPU queue.
- Present clears the swapchain image to a fixed colour and presents it; the host's frame pacing is unchanged.
- Pools: layer 0 as today (guest pools as Vulkan memory); nothing reads them yet.
- Texture, sampler and program registrations are accepted and counted.

Rules that hold from the first line: no `getenv` switches in `gpu/direct/` (one `HOS_DBG` list for debug aids), no
fallback to the frozen renderer, no per-site special case (plan rules 1, 2, 8), per-thread state for anything the
recording threads touch, and a clean run under the Vulkan validation layer.

## Done when

- `chisel_kokiri` and the Hyrule Field overlay run 60 s each with `RX_BACKEND=direct`: no crash, no hang, the game's
  frame counter advancing, a cleared window presented.
- A log line every 600 presents: lists a frame by kind, ops a frame by op, unclassified lists (0, or each named),
  the submit's ms on the guest's submitting thread, registrations, and the frame time.
- The frame time of the game with the pacing off (the ceiling), for both scenes, measured on an idle machine.
- One run of each scene under `VK_LAYER_KHRONOS_validation`: no messages.
- With `RX_BACKEND` unset the frozen back end's smoke verdict for Kokiri is unchanged, and `tests/run_all.py` gives
  the same results as a baseline recorded before the change.
