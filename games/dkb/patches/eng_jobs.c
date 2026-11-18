/* The render-job recorder render_job_record f_800802e0(ctx, drawctx, cmdpool, key, job, &range, stats) replaced for the
   direct back end.

   Its one caller, render_job_record_caller (f_8007ee40), runs it on the job workers. The replacement lets the direct
   back end refuse a pass whose formats it does not know yet (passes_recorder), otherwise notes the arguments for this
   thread, runs the game's recorder (its lifted body, orig) and clears them. Inside it the recorder's BeginRecording
   (lr 0x80081634) has the job's view record in x27, which an At row before that call hands to game_job_list for the back
   end. Nothing here runs without the direct back end. */
#include "patches.h"
#include <string.h>

typedef struct {
    int active, tagged;
    uint64_t ctx, key, view;
} job_tls;
static _Thread_local job_tls jt;

/* the job recorder: the game's (orig) with this thread's job noted; a pass whose formats are not known returns at once
   (x0 as it came, as the recorder's own early return at 0x80081534) */
PATCH_FUNC(0x800802e0, word(0xd503237f), body(0xb79959ce74123018), aapcs, x0, eng_jobs_record,
           orig, as(x0, ctx), as(x3, key)) {
    if (jt.active) return guest_orig(orig);   /* (a recorder run inside one: the game's) */
    if (passes_recorder(key)) return ctx;
    memset(&jt, 0, sizeof jt);
    jt.active = 1;
    jt.ctx = ctx, jt.key = key;
    const uint64_t r = guest_orig(orig);
    jt.active = 0;
    return r;
}

/* the view record in x27, before the recorder's BeginRecording blr (0x80081630) */
static _Thread_local uint64_t jt_view_arg;
PATCH_AT(0x8008162c, word(0xf9400108), after, none, eng_jobs_view, as(x27, view)) { jt_view_arg = view; }

int game_job_list(uint64_t cmd, uint64_t *key, uint64_t *view) {
    (void)cmd;
    if (!jt.active || jt.tagged) return 0;
    jt.tagged = 1;
    /* (the view record: one of the queue's, ctx +0x30 count, +0x38 array of 0xe0-byte records) */
    const uint64_t v = jt_view_arg;
    jt_view_arg = 0;
    if (hos_guest_range(jt.ctx + 0x30, 0x10)) {
        const uint32_t cnt = G32(jt.ctx + 0x30);
        const uint64_t arr = G64(jt.ctx + 0x38);
        if (v >= arr && v < arr + (uint64_t)cnt * 0xe0 && (v - arr) % 0xe0 == 0 && hos_guest_range(v, 0xe0)) jt.view = v;
    }
    *key = jt.key, *view = jt.view;
    return 1;
}
