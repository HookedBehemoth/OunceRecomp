/* The render-job recorder f_80084de0(ctx, drawctx, cmdpool, key, job, &range, stats) hooked for the direct back end.

   The job runners (f_8007fe60 at 0x80081548, f_80083960 at 0x8008410c) call it as a function on ModuleSystemWorker1-5 and
   the thread that kicks the pool. The hook lets the direct back end refuse a pass whose formats it does not know yet
   (direct_passes_recorder), otherwise notes the arguments for this thread, runs the game's recorder (the lifted function,
   found by a64_lookup: the hook sees it again and lets it run) and clears them. Inside it the recorder's BeginRecording
   (lr 0x8008533c) names the job's view record in x25, which eng_jobs_tag reads for the back end. Nothing here runs without
   the direct back end. */
#include "eng_jobs.h"
#include "direct.h"
#include <string.h>
#include <windows.h>
void direct_layer_ahead_job_done(uint64_t range);   /* direct_layer_ahead.c */

#define JOB_BEGIN_LR 0x8008533cull   /* f_80084de0's BeginRecording */

typedef struct {
    int active, tagged;
    uint64_t ctx, drawctx, key, view;
} job_tls;
static _Thread_local job_tls jt;
static _Thread_local uint64_t jt_cmd;   /* the guest command buffer the job on this thread records into */

ENTRY_HOOK(0x80084de0) {   /* (ctx, drawctx, cmdpool, key, job, &range, stats) */
    if (!rx_direct() || jt.active) return 0;   /* (not direct mode, or the game's recorder run from here: its body) */
    const uint64_t range = ARG(5);
    if (direct_passes_recorder(c)) { direct_layer_ahead_job_done(range); return 1; }   /* (the pass's formats not known: the early return) */
    static A64Fn orig;
    if (!orig) orig = a64_lookup(0x80084de0);
    if (!orig) return 0;
    memset(&jt, 0, sizeof jt);
    jt.active = 1;
    jt.ctx = ARG(0), jt.drawctx = ARG(1), jt.key = ARG(3);
    orig(c);
    jt.active = 0;
    direct_layer_ahead_job_done(range);
    return 1;
}

static int eng_jobs_begin(Ctx *c) {
    if (!jt.active || jt.tagged || c->x[30] != JOB_BEGIN_LR) return 0;
    jt.tagged = 1;
    jt_cmd = c->x[0];
    /* (the view record: one of the queue's, ctx +0x30 count, +0x38 array of 0xe0-byte records) */
    const uint64_t v = c->x[25];
    if (hos_guest_range(jt.ctx + 0x30, 0x10)) {
        const uint32_t cnt = G32(jt.ctx + 0x30);
        const uint64_t arr = G64(jt.ctx + 0x38);
        if (v >= arr && v < arr + (uint64_t)cnt * 0xe0 && (v - arr) % 0xe0 == 0 && hos_guest_range(v, 0xe0)) jt.view = v;
    }
    return 1;
}

/* (host/direct_job_sync.c) the guest command buffer of the job this thread records now and its view record (0: none) */
uint64_t eng_jobs_current(uint64_t *view) {
    if (!jt.active || !jt.tagged) return 0;
    if (view) *view = jt.view;
    return jt_cmd;
}

/* (host/direct_mesh_system.c) the draw context of the job this thread records now (0: none) */
uint64_t eng_jobs_drawctx(void) { return jt.active ? jt.drawctx : 0; }

int eng_jobs_tag(Ctx *c, uint64_t *key, uint64_t *view) {
    if (!eng_jobs_begin(c)) return 0;
    *key = jt.key, *view = jt.view;
    return 1;
}
