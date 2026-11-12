/* (native-vk engine cut E3) HOS_ENG_JOBS=1: the render-job recorder hooked. See eng_jobs.h and gpu/vk/eng_job.h.

   f_80084de0(ctx, drawctx, cmdpool, key, job, &range, stats) (work/assets/native/engine-jobs.md section 1): called as a
   function by the job runners (f_8007fe60 at 0x80081548, f_80083960 at 0x8008410c), on ModuleSystemWorker1-5 and the thread
   that kicks the pool. The hook notes the arguments for this thread, runs the game's recorder (the lifted function, found by
   a64_lookup: the hook sees it again and lets it run), and times it. Inside it the recorder's BeginRecording (lr 0x8008533c)
   names the job's view record in x25 (CONFIRMED: d_80084de0.s, the view search's x27 moved to x25 before the cmd slot is
   taken; engine-jobs.md "the host sees Initialize from 0x800852c0 with x25 = view record") and its EndRecording hands the
   records to the back end, on this thread. */
#include "eng_jobs.h"
#include "direct.h"
#include "../gpu/render/rx_nvn.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define JOB_BEGIN_LR 0x8008533cull   /* f_80084de0's BeginRecording */

static int on = -1;
int eng_jobs_on(void) {
    if (on < 0) {
        const char *e = getenv("HOS_ENG_JOBS"), *l = getenv("HOS_NVN_LIST_REF");
        const int want = e && atoi(e) == 1;
        const int lr = l && atoi(l) != 0;
        on = want && !lr;
        if (rx_direct()) {   /* (direct mode: the hook names each job's pass key and view for the back end, eng_jobs_tag) */
            hos_log("eng: render jobs: the recorder hooked for the direct back end (its pass key and view record)");
            on = 2;
            return on;
        }
        if (want) hos_log("eng: render jobs (HOS_ENG_JOBS): %s", lr ? "off (HOS_NVN_LIST_REF=1: not with lists by reference)" :
                          "the recorder hooked, each job recorded as a Vulkan secondary on its thread");
    }
    return on;
}

typedef struct {
    int active;
    uint32_t serial;
    rx_eng_job_info info;
    int64_t handed_ns;   /* (the back end's time in this job, inside the game's recorder) */
} job_tls;
static _Thread_local job_tls jt;
static _Thread_local uint64_t jt_cmd;   /* (direct mode: the guest command buffer the job on this thread records into) */
static volatile uint32_t serials;

static struct {
    volatile LONG64 calls, tagged, untagged, no_view, handed, game_ns, handed_ns, max_ns;
} n;

ENTRY_HOOK(0x80084de0) {   /* (ctx, drawctx, cmdpool, key, job, &range, stats) */
    if (!eng_jobs_on() || jt.active) return 0;   /* (off, or the game's recorder run from here: its body) */
    if (on == 2 && direct_passes_recorder(c)) return 1;   /* (direct mode, M3a: the pass's formats not known: the early return) */
    static A64Fn orig;
    if (!orig) orig = a64_lookup(0x80084de0);
    if (!orig) return 0;
    memset(&jt, 0, sizeof jt);
    jt.active = 1;
    jt.info.ctx = ARG(0), jt.info.drawctx = ARG(1), jt.info.key = ARG(3), jt.info.job = ARG(4);
    if (hos_guest_range(ARG(5), 8)) jt.info.range_start = G32(ARG(5)), jt.info.range_count = G32(ARG(5) + 4);
    const int64_t t0 = hos_now_ns();
    orig(c);
    const int64_t dt = hos_now_ns() - t0;
    InterlockedIncrement64(&n.calls);
    InterlockedAdd64(&n.game_ns, dt - jt.handed_ns);
    InterlockedAdd64(&n.handed_ns, jt.handed_ns);
    if (!jt.serial) InterlockedIncrement64(&n.untagged);   /* (skipped: nothing to draw for this pass, no recording) */
    for (LONG64 m = n.max_ns; dt > m && InterlockedCompareExchange64(&n.max_ns, dt, m) != m; m = n.max_ns) {}
    jt.active = 0;
    return 1;
}

uint32_t eng_jobs_begin(Ctx *c) {
    if (!jt.active || jt.serial || c->x[30] != JOB_BEGIN_LR) return 0;
    uint32_t s;
    do s = __atomic_add_fetch(&serials, 1, __ATOMIC_RELAXED);
    while (!s);
    jt.serial = s;
    jt.info.serial = s;
    jt_cmd = c->x[0];
    /* (the view record: one of the queue's, ctx +0x30 count, +0x38 array of 0xe0-byte records) */
    const uint64_t v = c->x[25], ctx = jt.info.ctx;
    if (hos_guest_range(ctx + 0x30, 0x10)) {
        const uint32_t cnt = G32(ctx + 0x30);
        const uint64_t arr = G64(ctx + 0x38);
        if (v >= arr && v < arr + (uint64_t)cnt * 0xe0 && (v - arr) % 0xe0 == 0 && hos_guest_range(v, 0xe0)) {
            jt.info.view = v;
            memcpy(jt.info.rect, GP(v + 0x78), sizeof jt.info.rect);
        }
    }
    if (!jt.info.view) InterlockedIncrement64(&n.no_view);
    InterlockedIncrement64(&n.tagged);
    return s;
}

/* (direct mode, M3a: host/direct_job_sync.c) the guest command buffer of the job this thread records now and its view record
   (0: none) */
uint64_t eng_jobs_current(uint64_t *view) {
    if (!jt.active || !jt.serial) return 0;
    if (view) *view = jt.info.view;
    return jt_cmd;
}

int eng_jobs_tag(Ctx *c, uint64_t *key, uint64_t *view) {
    if (!eng_jobs_begin(c)) return 0;
    *key = jt.info.key, *view = jt.info.view;
    return 1;
}

void eng_jobs_end(uint32_t serial, const uint8_t *rec, size_t bytes, uint64_t handle) {
    if (!serial || !jt.active || jt.serial != serial) return;
    const int64_t t0 = hos_now_ns();
    rx_eng_job_recorded(&jt.info, rec, (uint64_t)bytes, handle);
    jt.handed_ns += hos_now_ns() - t0;
    InterlockedIncrement64(&n.handed);
}

void eng_jobs_log(double frames) {
    if (eng_jobs_on() != 1) return;
    static LONG64 last[8];
    const LONG64 now[8] = {n.calls, n.tagged, n.untagged, n.no_view, n.handed, n.game_ns, n.handed_ns, n.max_ns};
    const double calls = (double)(now[0] - last[0]);
    hos_log("eng: render jobs a frame: %.1f calls (%.1f recordings, %.1f with nothing to record, %.2f without a view record); "
            "the game's recorder %.3f ms a frame (%.1f us a job), the Vulkan recording in it %.3f ms (%.1f us a job); longest %.2f ms",
            calls / frames, (now[1] - last[1]) / frames, (now[2] - last[2]) / frames, (now[3] - last[3]) / frames,
            (now[5] - last[5]) / frames * 1e-6, calls ? (now[5] - last[5]) / calls * 1e-3 : 0.0,
            (now[6] - last[6]) / frames * 1e-6, calls ? (now[6] - last[6]) / calls * 1e-3 : 0.0, now[7] * 1e-6);
    memcpy(last, now, sizeof last);
    n.max_ns = 0;
}
