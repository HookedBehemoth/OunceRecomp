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
   so the five other threads record the jobs meanwhile. The rest of both queues stays with the layer phase.

   The game's job-call sites do not wait for their jobs (the graph's order did): f_806688c0 and f_8028a2a0 look the pass's
   view record up by its key's first 0x70 bytes and, with none yet, draw the pass inline (f_80681320) while its jobs are
   recorded for nothing. So on the thread running the layer ahead, both sites wait first until every render-job list entry
   with that key's bytes (entry + 0x10: the recorder's key pointer) has been through the recorder this frame (stamped in
   host/eng_jobs.c's hook). Keys at the sites: f_806688c0(owner, drawctx, i) x0 + 0x3830 + i * 0xd0; f_8028a2a0(owner,
   drawctx, key) x2. The wait sleeps on a condition variable (the virtual core given up) that each recorded job wakes, and
   falls through after 50 ms (counted), as the game's own path would go on. */
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
static _Thread_local int ahead_thread;

/* ---- the render-job list entries recorded this frame ---- */
enum { LISTS = 4, ENTRIES = 1024 };
static volatile LONG64 done[LISTS][ENTRIES];   /* (gen of the frame the entry was recorded in) */
static SRWLOCK lk = SRWLOCK_INIT;
static CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
static volatile LONG waiters;
static struct { volatile LONG64 waits, wait_ns, timeouts, checks; } w;

static uint64_t job_lists(uint32_t *nl) {
    const uint64_t r = hos_guest_range(0x84ea2780ull, 8) ? G64(0x84ea2780ull) : 0;
    if (!r || !hos_guest_range(r + 0x30, 0x10)) return *nl = 0, 0;
    *nl = G32(r + 0x30) < LISTS ? G32(r + 0x30) : LISTS;
    return G64(r + 0x38);
}

/* (host/eng_jobs.c) the recorder took the entry whose range is at x5 (entry + 0x18), recorded or not */
void direct_layer_ahead_job_done(uint64_t range) {
    if (!enabled()) return;
    uint32_t nl;
    const uint64_t arr = job_lists(&nl);
    for (uint32_t li = 0; li < nl; li++) {
        const uint64_t l = G64(arr + 8ull * li);
        if (!l || !hos_guest_range(l + 0x3c10, 0x10)) continue;
        const uint64_t base = G64(l + 0x3c18), e = range - 0x18;
        const uint32_t cap = G32(l + 0x3c10);
        if (e < base || e >= base + 0x28ull * cap || (e - base) % 0x28) continue;
        const uint64_t i = (e - base) / 0x28;
        if (i < ENTRIES) __atomic_store_n(&done[li][i], gen, __ATOMIC_SEQ_CST);
        break;
    }
    if (__atomic_load_n(&waiters, __ATOMIC_SEQ_CST)) {
        AcquireSRWLockExclusive(&lk);
        ReleaseSRWLockExclusive(&lk);
        WakeAllConditionVariable(&cv);
    }
}

/* entries with the key's bytes not yet recorded this frame */
static int pending(uint64_t key) {
    uint32_t nl;
    const uint64_t arr = job_lists(&nl);
    const void *kb = GP(key);
    int k = 0;
    for (uint32_t li = 0; li < nl; li++) {
        const uint64_t l = G64(arr + 8ull * li);
        if (!l || !hos_guest_range(l + 0x3c10, 0x20)) continue;
        const uint64_t base = G64(l + 0x3c18);
        const uint32_t cnt = G32(l + 0x3c28), cap = G32(l + 0x3c10);
        for (uint32_t i = 0; i < cnt && i < cap && i < ENTRIES; i++) {
            const uint64_t ek = G64(base + 0x28ull * i + 0x10);
            if (ek && hos_guest_range(ek, 0x70) && !memcmp(GP(ek), kb, 0x70) && __atomic_load_n(&done[li][i], __ATOMIC_SEQ_CST) != gen) k++;
        }
    }
    return k;
}

static void wait_for(uint64_t key) {
    if (!ahead_thread || !hos_guest_range(key, 0x70)) return;
    InterlockedIncrement64(&w.checks);
    if (!pending(key)) return;
    const int64_t t0 = hos_now_ns();
    InterlockedIncrement64(&w.waits);
    AcquireSRWLockExclusive(&lk);
    InterlockedIncrement(&waiters);
    while (pending(key)) {
        if (hos_now_ns() - t0 > 50000000) { InterlockedIncrement64(&w.timeouts); break; }
        hos_cv_sleep(&cv, &lk, 2);
    }
    InterlockedDecrement(&waiters);
    ReleaseSRWLockExclusive(&lk);
    InterlockedExchangeAdd64(&w.wait_ns, hos_now_ns() - t0);
}

ENTRY_HOOK(0x806688c0) {   /* (owner, drawctx, i): the pass's key at owner + 0x3830 + i * 0xd0 */
    if (ahead_thread) wait_for(c->x[0] + 0x3830 + (uint64_t)(uint32_t)c->x[2] * 0xd0);
    return 0;
}
ENTRY_HOOK(0x8028a2a0) {   /* (owner, drawctx, key) */
    if (ahead_thread) wait_for(c->x[2]);
    return 0;
}
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
    ahead_thread = 1;
    a64_icall(c, G64(G64(item) + 0x10));
    ahead_thread = 0;
    memcpy(c, save, sizeof *save);
    free(save);
    InterlockedIncrement64(&n.runs);
}

void direct_layer_ahead_log(double frames) {
    static LONG64 last[7];
    const LONG64 now[7] = {n.runs, n.not_layer, n.skip_thread, w.checks, w.waits, w.wait_ns, w.timeouts};
    hos_log("direct patch layer ahead (f_8007fe60 at the job loop; f_806688c0, f_8028a2a0 wait): a frame: run ahead %.2f; queue 1's "
            "first item not a layer %.2f; the queue-1 skipping thread first %.2f; job calls checked %.2f, waited %.2f for %.3f ms; "
            "waits given up after 50 ms %lld",
            (now[0] - last[0]) / frames, (now[1] - last[1]) / frames, (now[2] - last[2]) / frames, (now[3] - last[3]) / frames,
            (now[4] - last[4]) / frames, (now[5] - last[5]) / 1e6 / frames, (long long)(now[6] - last[6]));
    memcpy(last, now, sizeof last);
}
