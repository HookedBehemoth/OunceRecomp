/* Direct mode's host side (direct.h): where a recording is classified, and the thread's record that waits for
   its payload. An NVN command function takes its record from cmd_alloc and fills the payload after it returns, so a record
   goes to the back end at the next cmd_alloc on the same thread, or at the flush points. */
#include "direct.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* The game's load-time lists (vertex, draw, shape, material and the two measuring copies of the first two), by where their
   recording began: the guest return address after BeginRecording; 0: not one of them. The value is the list's load kind. */
static int load_kind(uint64_t lr) {
    switch (lr) {
    case 0x804582acull: return 1;   /* vertex list: f_804581b0 <- f_80457e60 */
    case 0x80293b14ull: return 2;   /* draw list: f_80293890 <- f_804566e0 */
    case 0x806006d8ull: return 3;   /* per-instance shape list: f_806002e0 <- f_8009bb80 */
    case 0x80445314ull: return 4;   /* material list: f_80445020 <- f_80443000 */
    case 0x8044ea20ull: return 5;   /* vertex list's measuring copy: f_8044e8e0 <- f_8044e020 */
    case 0x80293460ull: return 6;   /* draw list's measuring copy: f_80293190 <- f_8044cb30 */
    default: return 0;
    }
}

/* ---- the list kinds, by the return address of BeginRecording (this build of the game; engine-frame.md section 1 as
   checked by a run). An address not here begins a PACKET list (counted, named in the log) ---- */
void hsd_classify(Ctx *c, hsd_list_info *info) {
    const uint64_t lr = c->x[30];
    info->lr = lr, info->fp = c->x[29];
    const int load = load_kind(lr);
    if (load) {
        info->kind = HSD_LIST_LOAD, info->load_kind = (uint32_t)load;
        return;
    }
    switch (lr) {
    case 0x8008533cull: info->kind = HSD_LIST_JOB; break;       /* f_80084de0, the render-job recorder */
    case 0x8006d258ull:                                         /* f_8006cd60, an agl::lyr layer's list */
    case 0x8007b0c8ull:                                         /* f_8007ad40 */
    case 0x8007ba00ull: info->kind = HSD_LIST_LAYER; break;     /* f_8007b520, a layer job's too: 5 a frame, never called (M1 run) */
    case 0x802eb3fcull: info->kind = HSD_LIST_GATE; break;      /* f_802eb390, the gate primary */
    case 0x80665580ull: info->kind = HSD_LIST_COMPUTE; break;   /* f_806650d0, an async-compute list */
    case 0x80665ffcull: info->kind = HSD_LIST_HEAD; break;      /* f_80665980, a queue's head (command-lists.md) */
    default: info->kind = HSD_LIST_PACKET; break;
    }
}

/* ---- the record that waits on this thread ---- */
enum { SCRATCH = 65536 + 64 };   /* (UpdateUniformBuffer: 64 KiB of data and its header, the largest record) */
typedef struct {
    uint8_t *buf;
    void *list;
    void *volatile *slot;   /* (the command buffer's list: a record whose list is not its command buffer's any more is dropped) */
    volatile int32_t *pending;
    uint32_t size;
} pending_rec;
static _Thread_local pending_rec tl;
static volatile LONG64 dropped;

void hsd_flush(void) {
    if (!tl.size) return;
    const uint32_t size = tl.size;
    tl.size = 0;
    __atomic_store_n(tl.pending, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(tl.slot, __ATOMIC_ACQUIRE) != tl.list) {   /* (ended or dropped on another thread meanwhile) */
        if (InterlockedIncrement64(&dropped) <= 8) hos_log("direct: a record dropped: its list was ended or let go on another thread");
        return;
    }
    uint32_t op;
    memcpy(&op, tl.buf, 4);
    rx_direct()->emit(tl.list, op, tl.buf + 8, size - 8);
}

uint8_t *hsd_scratch(void *volatile *list, volatile int32_t *pending, uint32_t size) {
    hsd_flush();
    if (!tl.buf) {
        tl.buf = (uint8_t *)malloc(SCRATCH);
        if (!tl.buf) hos_fatal(NULL, "direct: a record's scratch");
    }
    if (size > SCRATCH) hos_fatal(NULL, "direct: a record of %u bytes", size);
    tl.list = *list, tl.slot = list, tl.pending = pending, tl.size = size;
    __atomic_store_n(pending, 1, __ATOMIC_RELEASE);
    return tl.buf;
}
