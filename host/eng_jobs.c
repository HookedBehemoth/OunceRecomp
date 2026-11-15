/* The render-job recorder f_80084de0(ctx, drawctx, cmdpool, key, job, &range, stats) replaced for the direct back end.

   The job runners (f_8007fe60 at 0x80081548, f_80083960 at 0x8008410c) call it as a function on ModuleSystemWorker1-5 and
   the thread that kicks the pool. The replacement lets the direct back end refuse a pass whose formats it does not know
   yet (direct_passes_recorder), otherwise notes the arguments for this thread, runs the game's recorder (its lifted
   body, orig) and clears them. Inside it the recorder's BeginRecording
   (lr 0x8008533c) names the job's view record in x25, which an At row before that call hands to eng_jobs_view for
   eng_jobs_tag, for the back end. Nothing here runs without
   the direct back end. */
#include "eng_jobs.h"
#include "direct.h"
#include <string.h>
#include <windows.h>
void f_80084de0(Ctx *);   /* (lifted functions the host calls by name: a code pointer is a host function pointer) */
void direct_layer_ahead_job_done(uint64_t range, uint64_t lr);   /* direct_layer_ahead.c */


typedef struct {
    int active, tagged;
    uint64_t ctx, drawctx, key, view;
} job_tls;
static _Thread_local job_tls jt;
static _Thread_local uint64_t jt_cmd;   /* the guest command buffer the job on this thread records into */

/* the job recorder (ctx, drawctx, cmdpool, key, job, &range, stats): the game's (orig) with this thread's job noted; a
   pass whose formats are not known returns at once (its x0 as it came) */
PATCH_FUNC(0x80084de0, word(0xd503237f), body(0x961fa51430550d71), aapcs, x0, eng_jobs_record,
           orig, as(x0, ctx), as(x1, drawctx), as(x3, key), as(x5, range), as(x30, lr)) {
    if (jt.active) return guest_orig(orig);   /* (a recorder run inside one: the game's) */
    if (direct_passes_recorder(key)) { direct_layer_ahead_job_done(range, lr); return ctx; }
    memset(&jt, 0, sizeof jt);
    jt.active = 1;
    jt.ctx = ctx, jt.drawctx = drawctx, jt.key = key;
    const uint64_t r = guest_orig(orig);
    jt.active = 0;
    direct_layer_ahead_job_done(range, lr);
    return r;
}

/* (an At row in f_80084de0 before BeginRecording's blr, a PATCH_AT declaration, host/patch.h) the view record in x25 */
static _Thread_local uint64_t jt_view_arg;
PATCH_AT(0x80085334, word(0xf9400108), after, none, eng_jobs_view, as(x25, view)) { jt_view_arg = view; }

static int eng_jobs_begin(uint64_t cmd) {   /* (a JOB list's BeginRecording: f_80084de0's, by its row) */
    if (!jt.active || jt.tagged) return 0;
    jt.tagged = 1;
    jt_cmd = cmd;
    /* (the view record: one of the queue's, ctx +0x30 count, +0x38 array of 0xe0-byte records) */
    const uint64_t v = jt_view_arg;
    jt_view_arg = 0;
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

int eng_jobs_tag(uint64_t cmd, uint64_t *key, uint64_t *view) {
    if (!eng_jobs_begin(cmd)) return 0;
    *key = jt.key, *view = jt.view;
    return 1;
}
