/* Game patch, direct mode only: the waits, barriers and dispatches a render job records move to the layer
   list that calls the job, so that a job is one rendering secondary (docs/direct/patch-sites.md 4; jobs-state.md B).

   Inside jobs, every frame and all in the first colour pass (key 0x113b647338) and one in 0x113b648038:
   - f_82c02f20 / f_82c03170 (the culling pre-draw hooks): WaitEvent (return 0x82c0301c / 0x82c0326c) and Barrier(0x28)
     (0x82c030c4 / 0x82c03314), on the job's command buffer;
   - f_80279400 (water): its call at 0x80279448 of f_831fa4a0, which tail-jumps to WaitEvent (return 0x8027944c);
   - f_8067c310 (the occlusion grid's dispatch), tail-called by f_8067c110 from a job's item callback.
   Each of those calls the NVN proc through a register loaded just before the call: an At row after that load (nx2/lift2/
   probes.py DIRECT) points the call
   at a host function instead, which, when the command buffer is the job's own (host/eng_jobs.c: the job this thread
   records), keeps (event, mode, value) or the barrier bits for the job's view record, and else calls the proc. f_8067c310 is
   hooked at its entry: inside a job its call (entry, flags) is kept the same way. The CPU side of the hooks (the event slot
   and value taken under the game's mutex) runs as before.

   At the job-call site, f_806688c0's CallCommands (return 0x80668c84; x0 the list's command buffer) is pointed at a host
   function too, and the row that does it hands over the view record found (x21) and the list's draw context ([sp + 0x10])
   for this thread, so the host function reads only its arguments: it records the kept waits and one barrier of
   the kept bits on the layer list's command buffer, then the kept dispatches (f_8067c310 with the list's draw context;
   M6d: before the job's commands, not after), then the CallCommands. The water wait may only go here (jobs-state.md B:
   earlier positions deadlock); f_8028a2a0's call
   site (0x8028a538) never calls a job that kept one (counted).

   Check that it applies: the 600-present line "direct patch job sync" (kept and placed a frame, by kind; dropped; calls of
   the procs that passed through). */
#include "direct.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>
void f_8067c310(Ctx *);   /* (lifted functions the host calls by name: a code pointer is a host function pointer) */

uint64_t nvn_native_proc(const char *name);   /* nvn_native.c */
uint64_t hos_hostcall(const char *name, void (*fn)(Ctx *, void *), void *user);   /* hos_hostcall.c */
uint64_t hos_call_guest(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t eng_jobs_current(uint64_t *view);   /* eng_jobs.c */
extern volatile LONG64 hos_present_count;

static uint64_t p_wait, p_barrier, p_call, s_wait, s_barrier, s_call;
static struct {
    volatile LONG64 waits, barriers, dispatches, placed_waits, placed_barriers, placed_dispatches, dropped, through, calls,
        calls_kept, other_site;
} n;

/* what a view record's jobs kept this frame */
#define NV_ 128
typedef struct {
    uint64_t view;
    int64_t frame;
    int nw, nd;
    uint32_t bits;
    struct { uint64_t ev; uint32_t mode, value; } w[8];
    struct { uint64_t entry, x2, x3; } d[8];
} Kept;
static Kept kept[NV_];
static SRWLOCK kept_lock = SRWLOCK_INIT;

static Kept *kept_of(uint64_t view) {   /* (lock held) the view record's entry this frame, emptied when it is an older frame's */
    Kept *free_one = NULL;
    for (int i = 0; i < NV_; i++) {
        if (kept[i].view == view) {
            if (kept[i].frame != hos_present_count) {
                if (kept[i].nw || kept[i].nd || kept[i].bits) InterlockedIncrement64(&n.dropped);
                memset(&kept[i], 0, sizeof kept[i]);
                kept[i].view = view, kept[i].frame = hos_present_count;
            }
            return &kept[i];
        }
        if (!free_one && (!kept[i].view || kept[i].frame + 4 < hos_present_count)) free_one = &kept[i];
    }
    if (!free_one) return NULL;
    if (free_one->view && (free_one->nw || free_one->nd || free_one->bits)) InterlockedIncrement64(&n.dropped);
    memset(free_one, 0, sizeof *free_one);
    free_one->view = view, free_one->frame = hos_present_count;
    return free_one;
}
/* the job this thread records, when cmd is its command buffer: its view record (0: not a job's) */
static uint64_t job_view(uint64_t cmd) {
    uint64_t view = 0;
    const uint64_t jc = eng_jobs_current(&view);
    return jc && jc == cmd ? view : 0;
}

static void wait_fn(Ctx *c, void *u) {   /* (cmd, event, mode, value) */
    (void)u;
    const uint64_t view = job_view(c->x[0]);
    if (view) {
        AcquireSRWLockExclusive(&kept_lock);
        Kept *k = kept_of(view);
        if (k && k->nw < 8) {
            k->w[k->nw].ev = c->x[1], k->w[k->nw].mode = (uint32_t)c->x[2], k->w[k->nw].value = (uint32_t)c->x[3], k->nw++;
            ReleaseSRWLockExclusive(&kept_lock);
            InterlockedIncrement64(&n.waits);
            return;
        }
        ReleaseSRWLockExclusive(&kept_lock);
    }
    InterlockedIncrement64(&n.through);
    hos_call_guest(c, p_wait, c->x[0], c->x[1], c->x[2], c->x[3]);
}
static void barrier_fn(Ctx *c, void *u) {   /* (cmd, bits) */
    (void)u;
    const uint64_t view = job_view(c->x[0]);
    if (view) {
        AcquireSRWLockExclusive(&kept_lock);
        Kept *k = kept_of(view);
        if (k) {
            k->bits |= (uint32_t)c->x[1];
            ReleaseSRWLockExclusive(&kept_lock);
            InterlockedIncrement64(&n.barriers);
            return;
        }
        ReleaseSRWLockExclusive(&kept_lock);
    }
    InterlockedIncrement64(&n.through);
    hos_call_guest(c, p_barrier, c->x[0], c->x[1], 0, 0);
}
/* the job call's view record and draw context, handed over by direct_job_sync_call just before the call */
static _Thread_local uint64_t call_view, call_drawctx;
/* f_806688c0's CallCommands (cmd, count, handles): the view record's kept ops around it */
static void call_fn(Ctx *c, void *u) {
    (void)u;
    const uint64_t cmd = c->x[0], cnt = c->x[1], handles = c->x[2], view = call_view, drawctx = call_drawctx;
    call_view = call_drawctx = 0;
    Kept k;
    int have = 0;
    AcquireSRWLockExclusive(&kept_lock);
    for (int i = 0; i < NV_; i++)
        if (kept[i].view == view && kept[i].frame == hos_present_count) {
            k = kept[i];
            kept[i].nw = kept[i].nd = 0, kept[i].bits = 0;
            have = k.nw || k.nd || k.bits;
            break;
        }
    ReleaseSRWLockExclusive(&kept_lock);
    InterlockedIncrement64(&n.calls);
    if (!have) {
        hos_call_guest(c, p_call, cmd, cnt, handles, 0);
        return;
    }
    InterlockedIncrement64(&n.calls_kept);
    /* The kept grid dispatches go before the job's commands. Inside the job they sat between its items, before most
       of its draws (the frozen back end runs them there: its corner table holds the depth drawn so far, 0.95-1.0); after
       the job (M3a's place) they sampled the finished depth, the same depth 8df993dd samples later, so every corner was
       marked unchanged (-1) and the grid compute wrote every tile negated: the CPU culler then culled almost nothing
       (Kokiri 1079 draws a frame against the frozen back end's 685). Before the job they see the depth before its draws
       (1.0, cleared): the grid is the frozen one's and the frame draws 684. Exact would be the
       job cut at the dispatch; not done */
    for (int i = 0; i < k.nw; i++) hos_call_guest(c, p_wait, cmd, k.w[i].ev, k.w[i].mode, k.w[i].value);
    if (k.bits) hos_call_guest(c, p_barrier, cmd, k.bits, 0, 0);
    for (int i = 0; i < k.nd; i++) hos_call_guest(c, CODE_PTR(f_8067c310), k.d[i].entry, drawctx, k.d[i].x2, k.d[i].x3);
    hos_call_guest(c, p_call, cmd, cnt, handles, 0);
    InterlockedAdd64(&n.placed_waits, k.nw);
    InterlockedAdd64(&n.placed_barriers, k.bits ? 1 : 0);
    InterlockedAdd64(&n.placed_dispatches, k.nd);
}

static int ready(void) {
    static volatile LONG state;   /* 0 not yet, 1 on, 2 off */
    if (state) return state == 1;
    if (!rx_direct()) return InterlockedExchange(&state, 2), 0;
    p_wait = nvn_native_proc("nvnCommandBufferWaitEvent");
    p_barrier = nvn_native_proc("nvnCommandBufferBarrier");
    p_call = nvn_native_proc("nvnCommandBufferCallCommands");
    s_wait = hos_hostcall("direct_job_sync wait", wait_fn, NULL);
    s_barrier = hos_hostcall("direct_job_sync barrier", barrier_fn, NULL);
    s_call = hos_hostcall("direct_job_sync call", call_fn, NULL);
    const LONG on = p_wait && p_barrier && p_call && s_wait && s_barrier && s_call ? 1 : 2;
    if (on != 1) hos_log("direct patch job sync: the procs were not found: off");
    InterlockedExchange(&state, on);
    return on == 1;
}
/* the call target just loaded (f_82c02f20, f_82c03170: x8 after each ldr x8, [x8]; f_831fa4a0: x4, the same wait code
   as f_82c02f20's, which tail-jumps to WaitEvent through x4 (f_80279400, water, calls it at 0x80279448: WaitEvent's
   return is 0x8027944c)): the wait and barrier procs pointed at the host's */
PATCH_AT(0x82c02f94, word(0xf9400108), after, x8, direct_job_sync_retarget, as(x8, proc)) {
    if (!ready()) return proc;
    return proc == p_wait ? s_wait : proc == p_barrier ? s_barrier : proc;
}
PATCH_AT_SITE(0x82c03014, word(0xf9400108), after, x8, direct_job_sync_retarget, x8);
PATCH_AT_SITE(0x82c03024, word(0xf9400108), after, x8, direct_job_sync_retarget, x8);
PATCH_AT_SITE(0x82c030bc, word(0xf9400108), after, x8, direct_job_sync_retarget, x8);
PATCH_AT_SITE(0x82c031e4, word(0xf9400108), after, x8, direct_job_sync_retarget, x8);
PATCH_AT_SITE(0x82c03264, word(0xf9400108), after, x8, direct_job_sync_retarget, x8);
PATCH_AT_SITE(0x82c03274, word(0xf9400108), after, x8, direct_job_sync_retarget, x8);
PATCH_AT_SITE(0x82c0330c, word(0xf9400108), after, x8, direct_job_sync_retarget, x8);
PATCH_AT_SITE(0x831fa57c, word(0xf9400104), after, x4, direct_job_sync_retarget, x4);   /* ldr x4, [x8] */
/* the job-call site, f_806688c0 after each ldr x8, [x8]: its CallCommands (the only call of that proc there through
   x8) pointed at call_fn, with the view record (x21) and the list's draw context ([sp + 0x10]) for it */
PATCH_AT(0x80668c7c, word(0xf9400108), after, x8, direct_job_sync_call, as(x8, proc), as(x21, view), as(ld(sp, 0x10), drawctx)) {
    if (!ready() || proc != p_call) return proc;
    call_view = view, call_drawctx = drawctx;
    return s_call;
}
PATCH_AT_SITE(0x80668dd8, word(0xf9400108), after, x8, direct_job_sync_call, x8, x21, ld(sp, 0x10));
PATCH_AT_SITE(0x80668fb0, word(0xf9400108), after, x8, direct_job_sync_call, x8, x21, ld(sp, 0x10));

ENTRY_HOOK(0x8067c310) {   /* (grid entry, drawctx, flag, flag): the occlusion grid's dispatch */
    if (!ready()) return 0;
    const uint64_t view = hos_guest_range(a->x[1] + 216, 8) ? job_view(G64(a->x[1] + 216)) : 0;
    if (!view) return 0;
    AcquireSRWLockExclusive(&kept_lock);
    Kept *k = kept_of(view);
    if (k && k->nd < 8) {
        k->d[k->nd].entry = a->x[0], k->d[k->nd].x2 = a->x[2], k->d[k->nd].x3 = a->x[3], k->nd++;
        ReleaseSRWLockExclusive(&kept_lock);
        InterlockedIncrement64(&n.dispatches);
        return 1;
    }
    ReleaseSRWLockExclusive(&kept_lock);
    return 0;
}

void direct_job_sync_log(double frames) {
    static LONG64 last[11];
    const LONG64 now[11] = {n.waits, n.barriers, n.dispatches, n.placed_waits, n.placed_barriers, n.placed_dispatches, n.dropped,
                            n.through, n.calls, n.calls_kept, n.other_site};
    LONG64 d[11];
    for (int i = 0; i < 11; i++) d[i] = now[i] - last[i];
    hos_log("direct patch job sync (f_82c02f20, f_82c03170, f_831fa4a0, f_8067c310 -> f_806688c0's job call): a frame: kept in "
            "jobs %.2f waits, %.2f barriers, %.2f dispatches; placed at the job call %.2f waits, %.2f barriers, %.2f dispatches "
            "(%.2f of %.2f job calls had some); dropped %lld; proc calls passed through (not a job's) %.2f",
            d[0] / frames, d[1] / frames, d[2] / frames, d[3] / frames, d[4] / frames, d[5] / frames, d[9] / frames, d[8] / frames,
            (long long)d[6], d[7] / frames);
    memcpy(last, now, sizeof last);
}
