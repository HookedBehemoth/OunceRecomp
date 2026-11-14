/* Game patch (HOS_LAYER_AHEAD=1): the long layer of calc stage C starts with the render-job phase instead of after it
   (docs/direct/boot-to-main-loop.md 4.3, docs/direct/patch-sites.md).

   Stage C's task graph opens the layer-list phase (six nodes of kind 0x8800) only after every render-job loop (kind
   0x400) has ended. The layer phase's nodes take their work from two queues of the work object W = [[sp + 0x48] + 0xd0]
   of the scheduler f_8007fe60: queue 1 (claim counter W + 0x4dc, count W + 0x4e8, items W + 0x4f0; the thread whose
   worker index is W + 0x4d8 skips it) and queue 2 (W + 0x4e0, + 0x4f8, + 0x500). An item's kind is item + 0x10 (0 a
   layer recorded by f_8006cd60, layer at item + 0x18) and the scheduler runs it as [[item] + 0x10](item). Queue 1's first
   item is the 3D layer (the pipeline, post effects: 3.6 ms in the market); the counters are reset at stage C's start.

   The splice sits where a thread enters a render-job loop node (the bit-10 branch of f_8007fe60): the first thread of
   the frame that may take queue 1 claims its item 0 there (the same counter, 0 -> 1) and runs it before its job loop,
   so the five other threads record the jobs meanwhile. The rest of both queues stays with the layer phase. */
#include "direct.h"
#include <string.h>
#include <stdlib.h>
#include <windows.h>

uint64_t hos_tls_value(uint32_t slot);   /* hos_thread.c */

static int on = -1;
static int enabled(void) {
    if (on < 0) {
        const char *e = getenv("HOS_LAYER_AHEAD");
        on = e && atoi(e) > 0;
    }
    return on;
}

static struct { volatile LONG64 runs, not_layer, skip_thread, frames; } n;

static volatile LONG64 gen;   /* (moduleCalc_ calls: the frame) */
int direct_tail_prof_calc(Ctx *c) __attribute__((weak));
ENTRY_HOOK(0x80706be0) {   /* moduleCalc_ */
    InterlockedIncrement64(&gen);
    return direct_tail_prof_calc ? direct_tail_prof_calc(c) : 0;
}

/* the worker index the scheduler compares with W + 0x4d8: its TLS slot's value - 1, else the core number */
static uint32_t worker_index(void) {
    const uint64_t v = hos_tls_value(G32(0x84e9f7acull));
    return v ? (uint32_t)(v - 1) : (uint32_t)hos_current_core(0);
}

SPLICE_AFTER(0x8007fe60, "if (!((c->x[23] >> 10) & 1)) { goto L_800809d4; }") {
    if (!enabled()) return;
    const uint64_t sp = c->x[31];
    if (!hos_guest_range(sp + 0x48, 8)) return;
    const uint64_t s = G64(sp + 0x48);
    if (!s || !hos_guest_range(s + 0xd0, 8)) return;
    const uint64_t w = G64(s + 0xd0);
    if (!w || !hos_guest_range(w + 0x4d8, 0x30)) return;
    volatile uint32_t *ctr = (volatile uint32_t *)GP(w + 0x4dc);
    if (*ctr != 0 || (int32_t)G32(w + 0x4e8) < 1) return;   /* (taken, or an empty queue) */
    const uint64_t item = G64(G64(w + 0x4f0));
    if (!item || !hos_guest_range(item, 0x20) || !hos_guest_range(G64(item) + 0x10, 8)) return;
    if (G32(item + 0x10) != 0) { InterlockedIncrement64(&n.not_layer); return; }
    if (worker_index() == G32(w + 0x4d8)) { InterlockedIncrement64(&n.skip_thread); return; }
    uint32_t zero = 0;
    if (!__atomic_compare_exchange_n(ctr, &zero, 1u, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
    /* (the guest state around the call: the lifted scheduler keeps values in caller-saved registers here) */
    Ctx *save = (Ctx *)malloc(sizeof *save);
    if (!save) hos_fatal(NULL, "layer ahead: a context copy");
    memcpy(save, c, sizeof *save);
    c->x[0] = item;
    c->x[30] = 0;
    a64_icall(c, G64(G64(item) + 0x10));
    memcpy(c, save, sizeof *save);
    free(save);
    InterlockedIncrement64(&n.runs);
}

void direct_layer_ahead_log(double frames) {
    static LONG64 last[3];
    const LONG64 now[3] = {n.runs, n.not_layer, n.skip_thread};
    hos_log("direct patch layer ahead (f_8007fe60 at the job loop): a frame: run ahead %.2f; queue 1's first item not a layer %.2f; "
            "the queue-1 skipping thread first %.2f",
            (now[0] - last[0]) / frames, (now[1] - last[1]) / frames, (now[2] - last[2]) / frames);
    memcpy(last, now, sizeof last);
}
