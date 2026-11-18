/* Direct mode's host side (renderer.h): where a recording is classified, and the thread's record that waits for
   its payload. An NVN command function takes its record from cmd_alloc and fills the payload after it returns, so a record
   goes to the back end at the next cmd_alloc on the same thread, or at the flush points. */
#include "renderer.h"
#include <stdlib.h>
#include <string.h>
#include "os.h"

/* ---- the list kinds: the game's patches name the kind of the next recording a thread begins (hsd_name_next_list,
   from an At row before the BeginRecording call of the function that begins it); a recording nobody named is a PACKET
   list (counted, named in the log). The return address and frame pointer are kept in the info as labels for the logs. ---- */
static _Thread_local uint32_t tl_kind, tl_load_kind;   /* (tl_kind: HSD_LIST_* + 1, 0 none) */
void hsd_name_next_list(uint32_t kind, uint32_t load_kind) { tl_kind = kind + 1, tl_load_kind = load_kind; }
void hsd_classify(Ctx *c, hsd_list_info *info) {
    info->lr = c->x[30], info->fp = c->x[29];
    info->kind = tl_kind ? tl_kind - 1 : HSD_LIST_PACKET;
    if (info->kind == HSD_LIST_LOAD) info->load_kind = tl_load_kind;
    tl_kind = 0;
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
static volatile int64_t dropped;

void hsd_flush(void) {
    if (!tl.size) return;
    const uint32_t size = tl.size;
    tl.size = 0;
    __atomic_store_n(tl.pending, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(tl.slot, __ATOMIC_ACQUIRE) != tl.list) {   /* (ended or dropped on another thread meanwhile) */
        if (os_atomic_inc(&dropped) <= 8) hos_log("direct: a record dropped: its list was ended or let go on another thread");
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
