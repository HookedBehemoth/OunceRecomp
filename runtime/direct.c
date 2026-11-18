/* Direct mode's host side (direct.h): where a recording is classified, and the thread's record that waits for
   its payload. An NVN command function takes its record from cmd_alloc and fills the payload after it returns, so a record
   goes to the back end at the next cmd_alloc on the same thread, or at the flush points. */
#include "direct.h"
#include <stdlib.h>
#include <string.h>
#include "os.h"

/* ---- the list kinds: the function that begins a recording names it, by an At row just before its BeginRecording call
   (a PATCH_AT declaration, runtime/patch.h: direct_list_kind); a recording no row named is a PACKET list (counted, named in the log).
   1-6: the game's load-time lists (the list's load kind): 1 vertex list (f_80457e60, at its call of agl_cmdbuf_begin
   f_804581b0), 2 draw list (f_80293890 <- f_804566e0), 3 per-instance shape list (f_806002e0 <- f_8009bb80), 4 material
   list (f_80445020 <- f_80443000), 5 the vertex list's measuring copy (f_8044e8e0 <- f_8044e020), 6 the draw list's
   measuring copy (f_80293190 <- f_8044cb30); 16 JOB (f_80084de0, the render-job recorder), 17 LAYER (f_8006cd60, an
   agl::lyr layer's list; f_8007ad40; f_8007b520, a layer job's too: 5 a frame, never called, M1 run), 18 GATE
   (f_802eb390, the gate primary), 19 COMPUTE (f_806650d0, an async-compute list), 20 HEAD (f_80665980, a queue's head,
   command-lists.md), 21 IMMEDIATE (a one-shot list the game submits itself and waits for: agl_cmdbuf_begin's other
   callers, the save picture's copy f_80bd22b0, the immediate context's begin f_834bb830 and f_83a6d390's second
   command buffer; f_812bd720, the begin of the effect noise upload: vfx_curl_noise_staging submits and finishes it).
   f_804581b0 is shared, so its callers name the kind at their call, not a row inside it.
   The return address and frame pointer are kept in the info as labels for the logs. ---- */
static _Thread_local uint32_t tl_kind;
PATCH_AT(0x80458044, word(0x9400005b), before, none, direct_list_kind, as(k(1), kind)) { tl_kind = kind; }
PATCH_AT_SITE(0x80293b0c, word(0xf9400108), after, none, direct_list_kind, k(2));
PATCH_AT_SITE(0x806006d0, word(0xf9400108), after, none, direct_list_kind, k(3));
PATCH_AT_SITE(0x8044530c, word(0xf9400108), after, none, direct_list_kind, k(4));
PATCH_AT_SITE(0x8044ea18, word(0xf9400108), after, none, direct_list_kind, k(5));
PATCH_AT_SITE(0x80293458, word(0xf9400108), after, none, direct_list_kind, k(6));
PATCH_AT_SITE(0x80085334, word(0xf9400108), after, none, direct_list_kind, k(16));
PATCH_AT_SITE(0x8006d250, word(0xf9400108), after, none, direct_list_kind, k(17));
PATCH_AT_SITE(0x8007b0c0, word(0xf9400108), after, none, direct_list_kind, k(17));
PATCH_AT_SITE(0x8007b9f8, word(0xf9400108), after, none, direct_list_kind, k(17));
PATCH_AT_SITE(0x802eb3f4, word(0xf94002e8), after, none, direct_list_kind, k(18));
PATCH_AT_SITE(0x80665578, word(0xf9400108), after, none, direct_list_kind, k(19));
PATCH_AT_SITE(0x80665ff4, word(0xf9400108), after, none, direct_list_kind, k(20));
PATCH_AT_SITE(0x80bd2490, word(0x97e21748), before, none, direct_list_kind, k(21));
PATCH_AT_SITE(0x834bb874, word(0x973e724f), before, none, direct_list_kind, k(21));
PATCH_AT_SITE(0x83a6d3f4, word(0x9727ab6f), before, none, direct_list_kind, k(21));
PATCH_AT_SITE(0x812bd764, word(0xf9400108), after, none, direct_list_kind, k(21));
void hsd_classify(Ctx *c, hsd_list_info *info) {
    info->lr = c->x[30], info->fp = c->x[29];
    const uint32_t k = tl_kind;
    tl_kind = 0;
    if (k >= 1 && k <= 6) {
        info->kind = HSD_LIST_LOAD, info->load_kind = k;
        return;
    }
    switch (k) {
    case 16: info->kind = HSD_LIST_JOB; break;
    case 17: info->kind = HSD_LIST_LAYER; break;
    case 18: info->kind = HSD_LIST_GATE; break;
    case 19: info->kind = HSD_LIST_COMPUTE; break;
    case 20: info->kind = HSD_LIST_HEAD; break;
    case 21: info->kind = HSD_LIST_IMMEDIATE; break;
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
