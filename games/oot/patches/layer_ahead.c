/* Game patch: the long layer of calc stage C starts with the render-job phase instead of after it
   (docs/direct/jobs.md, docs/direct/frame-loop.md).

   Stage C's task graph opens the layer-list phase (six nodes of kind 0x8800) only after every render-job loop (kind
   0x400) has ended. The layer phase's nodes take their work from two queues of the work object W = [[sp + 0x48] + 0xd0]
   of the scheduler f_8007fe60: queue 1 (claim counter W + 0x4dc, count W + 0x4e8, items W + 0x4f0; the thread whose
   worker index is W + 0x4d8 skips it) and queue 2 (W + 0x4e0, + 0x4f8, + 0x500). An item's kind is item + 0x10 (0 a
   layer recorded by f_8006cd60, layer at item + 0x18) and the scheduler runs it as [[item] + 0x10](item). Queue 1's first
   item is the 3D layer (the pipeline, post effects: 3.6 ms in the market); the counters are reset at stage C's start.

   An At row (a PATCH_AT declaration, runtime/patch.h, a guest call) sits where a thread enters a render-job loop node (the bit-10 branch of f_8007fe60): the first thread of
   the frame that may take queue 1 claims its item 0 there (the same counter, 0 -> 1) and runs it before its job loop,
   so the five other threads record the jobs meanwhile. The rest of both queues stays with the layer phase.

   The game's job-call sites do not wait for their jobs (the graph's order did): f_806688c0 and f_8028a2a0 look the pass's
   view record up by its key's first 0x70 bytes and, with none yet, draw the pass inline (f_80681320) while its jobs are
   recorded for nothing. So on the thread running the layer ahead, both sites wait first until every render-job list entry
   with that key's bytes (entry + 0x10: the recorder's key pointer) has been through the recorder this frame (stamped in
   games/oot/patches/eng_jobs.c's hook). Keys at the sites: f_806688c0(owner, drawctx, i) x0 + 0x3830 + i * 0xd0; f_8028a2a0(owner,
   drawctx, key) x2. The wait sleeps on a condition variable that each recorded job wakes, and
   falls through after 50 ms (counted), as the game's own path would go on. */
#include "patches.h"
#include <string.h>
#include <stdlib.h>
#include "os.h"

uint64_t hos_tls_value(uint32_t slot);   /* hos_thread.c */


static struct { volatile int64_t runs, not_layer, skip_thread, other_caller; } n;

static volatile int64_t gen;   /* (moduleCalc_ calls: the frame) */
static _Thread_local int ahead_thread;

/* ---- the render-job list entries recorded this frame ---- */
enum { LISTS = 4, ENTRIES = 1024 };
static volatile int64_t done[LISTS][ENTRIES];   /* (gen of the frame the entry was recorded in) */
static os_mutex lk = OS_MUTEX_INIT;
static os_cond cv = OS_COND_INIT;
static volatile int32_t waiters;
static struct { volatile int64_t waits, wait_ns, timeouts, checks; } w;

static uint64_t job_lists(uint32_t *nl) {
    const uint64_t r = hos_guest_range(0x84ea2780ull, 8) ? G64(0x84ea2780ull) : 0;
    if (!r || !hos_guest_range(r + 0x30, 0x10)) return *nl = 0, 0;
    *nl = G32(r + 0x30) < LISTS ? G32(r + 0x30) : LISTS;
    return G64(r + 0x38);
}

/* (games/oot/patches/eng_jobs.c) the recorder took the entry whose range is at x5 (entry + 0x18), recorded or not */
void layer_ahead_job_done(uint64_t range, uint64_t lr) {
    if (lr != 0x8008154cull) os_atomic_inc(&n.other_caller);   /* (not f_8007fe60's job loop: f_80083960's twin) */
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
        os_mutex_lock(&lk);
        os_mutex_unlock(&lk);
        os_cond_broadcast(&cv);
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

/* ---- the job list in the order the layer calls its passes ----
   The list holds the passes' jobs in an order of its own (in the market the four depth-probe passes the 3D layer calls
   first are entries 78-101 of 102), and the job loops claim it from the front. Before the first claim of a frame, the
   entries are put in the order the layer ran ahead called their keys the frame before (the others after them, in their
   order). The order of recording changes nothing else: a pass calls its jobs sorted by their item ranges (+0x68). */
enum { SEQ = 64 };
/* (seq_next is written by the thread running the layer ahead and read by the next frame's first job-loop entrant: the
   layer has ended by then, since stage C's join waits for it and the next frame's job phase opens after that join) */
static uint64_t seq_use[SEQ], seq_next[SEQ];
static int n_use, n_next;
static os_mutex order_lk = OS_MUTEX_INIT;
static volatile int64_t ordered_gen;
static struct { volatile int64_t ordered, moved, claimed_before; } o;

static uint64_t key_hash(uint64_t a) {
    const uint8_t *p = (const uint8_t *)GP(a);
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 0x70; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void order_jobs(void) {
    if (__atomic_load_n(&ordered_gen, __ATOMIC_ACQUIRE) == gen) return;
    os_mutex_lock(&order_lk);
    if (ordered_gen != gen) {
        if (n_next) memcpy(seq_use, seq_next, sizeof seq_use), n_use = n_next, n_next = 0;
        uint32_t nl;
        const uint64_t arr = job_lists(&nl);
        for (uint32_t li = 0; li < nl && n_use; li++) {
            const uint64_t l = G64(arr + 8ull * li);
            if (!l || !hos_guest_range(l + 0x3c10, 0x80)) continue;
            const uint64_t base = G64(l + 0x3c18);
            const uint32_t cnt = G32(l + 0x3c28);
            if (!cnt || cnt > G32(l + 0x3c10) || cnt > ENTRIES || !hos_guest_range(base, 0x28ull * cnt)) continue;
            if (G32(l + 0x3c84) != 0) { os_atomic_inc(&o.claimed_before); continue; }
            static uint8_t tmp[ENTRIES * 0x28];
            static uint32_t rank[ENTRIES], idx[ENTRIES];
            for (uint32_t i = 0; i < cnt; i++) {
                const uint64_t ek = G64(base + 0x28ull * i + 0x10);
                uint32_t r = SEQ + i;
                if (ek && hos_guest_range(ek, 0x70)) {
                    const uint64_t h = key_hash(ek);
                    for (int k = 0; k < n_use; k++) if (seq_use[k] == h) { r = (uint32_t)k; break; }
                }
                rank[i] = r, idx[i] = i;
            }
            for (uint32_t i = 1; i < cnt; i++) {   /* (stable: ties keep the list's order) */
                const uint32_t v = idx[i];
                uint32_t j = i;
                while (j && rank[idx[j - 1]] > rank[v]) idx[j] = idx[j - 1], j--;
                idx[j] = v;
            }
            uint32_t moved = 0;
            for (uint32_t i = 0; i < cnt; i++) moved += idx[i] != i;
            if (!moved) continue;
            memcpy(tmp, GP(base), 0x28ull * cnt);
            for (uint32_t i = 0; i < cnt; i++) memcpy(GP(base + 0x28ull * i), tmp + 0x28ull * idx[i], 0x28);
            os_atomic_fetch_add(&o.moved, moved);
        }
        os_atomic_inc(&o.ordered);
        __atomic_store_n(&ordered_gen, gen, __ATOMIC_RELEASE);
    }
    os_mutex_unlock(&order_lk);
}

static void wait_for(uint64_t key) {
    if (!ahead_thread || !hos_guest_range(key, 0x70)) return;
    os_atomic_inc(&w.checks);
    {
        const uint64_t h = key_hash(key);
        if (n_next < SEQ && (!n_next || seq_next[n_next - 1] != h)) {
            int seen = 0;
            for (int k = 0; k < n_next; k++) seen |= seq_next[k] == h;
            if (!seen) seq_next[n_next++] = h;
        }
    }
    if (!pending(key)) return;
    const int64_t t0 = hos_now_ns();
    os_atomic_inc(&w.waits);
    os_mutex_lock(&lk);
    os_atomic_inc(&waiters);
    while (pending(key)) {   /* (each recorded job wakes it: no poll, only the 50 ms cap) */
        const int64_t left = t0 + 50000000 - hos_now_ns();
        if (left <= 0) { os_atomic_inc(&w.timeouts); break; }
        hos_cv_sleep(&cv, &lk, left);
    }
    os_atomic_dec(&waiters);
    os_mutex_unlock(&lk);
    os_atomic_fetch_add(&w.wait_ns, hos_now_ns() - t0);
}

/* (owner, drawctx, i): the pass's key at owner + 0x3830 + i * 0xd0 */
PATCH_AT(0x806688c0, word(0xd503237f), before, none, layer_ahead_pass, as(x0, owner), as(w2, i)) {
    if (ahead_thread) wait_for(owner + 0x3830 + (uint64_t)i * 0xd0);
}
PATCH_AT(0x8028a2a0, word(0xd503237f), before, none, layer_ahead_key, as(x2, key)) {   /* (owner, drawctx, key) */
    if (ahead_thread) wait_for(key);
}
PATCH_AT(0x80706be0, word(0xd503237f), before, none, layer_ahead_calc) {   /* moduleCalc_ */
    os_atomic_inc(&gen);
}

/* the worker index the scheduler compares with W + 0x4d8: its TLS slot's value - 1, else the core number */
static uint32_t worker_index(void) {
    const uint64_t v = hos_tls_value(G32(0x84e9f7acull));
    return v ? (uint32_t)(v - 1) : (uint32_t)hos_current_core(0);
}

/* (an At row in f_8007fe60, the job loop, on the TBZ's fall-through: sp the loop's, s its scheduler record from
   [sp + 0x48]) */
PATCH_AT(0x800809d0, word(0xb5004ee8), before, none, layer_ahead_run, guest(computed), sp, as(ld(sp, 0x48), s)) {
    order_jobs();
    if (!s || !hos_guest_range(s + 0xd0, 8)) return;
    const uint64_t w = G64(s + 0xd0);
    if (!w || !hos_guest_range(w + 0x4d8, 0x30)) return;
    volatile uint32_t *ctr = (volatile uint32_t *)GP(w + 0x4dc);
    if (*ctr != 0 || (int32_t)G32(w + 0x4e8) < 1) return;   /* (taken, or an empty queue) */
    const uint64_t item = G64(G64(w + 0x4f0));
    if (!item || !hos_guest_range(item, 0x20) || !hos_guest_range(G64(item) + 0x10, 8)) return;
    if (G32(item + 0x10) != 0) { os_atomic_inc(&n.not_layer); return; }
    if (worker_index() == G32(w + 0x4d8)) { os_atomic_inc(&n.skip_thread); return; }
    uint32_t zero = 0;
    if (!__atomic_compare_exchange_n(ctr, &zero, 1u, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
    /* (guest_call puts every register back: the lifted scheduler keeps values in caller-saved registers here) */
    ahead_thread = 1;
    guest_call(g, sp, G64(G64(item) + 0x10), item, 0, 0, 0, 0);
    ahead_thread = 0;
    os_atomic_inc(&n.runs);
}

void layer_ahead_log(double frames) {
    static int64_t last[11];
    const int64_t now[11] = {n.runs, n.not_layer, n.skip_thread, w.checks, w.waits, w.wait_ns, w.timeouts, o.ordered, o.moved, o.claimed_before,
                            n.other_caller};
    hos_log("direct patch layer ahead (f_8007fe60 at the job loop; f_806688c0, f_8028a2a0 wait): a frame: run ahead %.2f; queue 1's "
            "first item not a layer %.2f; the queue-1 skipping thread first %.2f; job calls checked %.2f, waited %.2f for %.3f ms; "
            "waits given up after 50 ms %lld; job lists ordered %.2f, entries moved %.1f, claimed before the order %lld; jobs recorded from another caller %lld",
            (now[0] - last[0]) / frames, (now[1] - last[1]) / frames, (now[2] - last[2]) / frames, (now[3] - last[3]) / frames,
            (now[4] - last[4]) / frames, (now[5] - last[5]) / 1e6 / frames, (long long)(now[6] - last[6]),
            (now[7] - last[7]) / frames, (now[8] - last[8]) / frames, (long long)(now[9] - last[9]),
            (long long)(now[10] - last[10]));
    memcpy(last, now, sizeof last);
}
