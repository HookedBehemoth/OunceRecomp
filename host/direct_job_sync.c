/* Game patch, direct mode only: the waits, barriers and dispatches a render job records move to the layer
   list that calls the job, so that a job is one rendering secondary (docs/direct/patch-sites.md 4; jobs-state.md B).

   Inside jobs, every frame and all in the first colour pass (key 0x113b647338) and one in 0x113b648038:
   - f_82c02f20 / f_82c03170 (the culling pre-draw hooks): WaitEvent (return 0x82c0301c / 0x82c0326c) and Barrier(0x28)
     (0x82c030c4 / 0x82c03314), on the job's command buffer;
   - f_80279400 (water): its call at 0x80279448 of f_831fa4a0, which tail-jumps to WaitEvent (return 0x8027944c);
   - f_8067c310 (the occlusion grid's dispatch), tail-called by f_8067c110 from a job's item callback.
   Each of those calls the NVN proc through a register loaded just before the call (BLR x8). An instead row on that BLR
   (host/patch.h PATCH_AT) calls a host function with the proc and the call's arguments in its place, which, when the
   command buffer is the job's own (host/eng_jobs.c: the job this thread records), keeps (event, mode, value) or the
   barrier bits for the job's view record, and else calls the proc (guest_call). f_831fa4a0 tail-jumps to the proc (BR
   x4): the same row stands in place of that tail call, and the function returns after it. f_8067c310 is replaced (PATCH_FUNC, its body the orig):
   inside a job its call (entry, flags) is kept the same way. The CPU side of the hooks (the event slot and value
   taken under the game's mutex) runs as before. Each row asserts that the proc it is in place of is the one it names.

   At the job-call site, an instead row on f_806688c0's CallCommands (0x80668c80; x0 the list's command buffer) gets the
   view record found (x21) and the list's draw context ([sp + 0x10]) as operands: it records the kept waits and one
   barrier of the kept bits on the layer list's command buffer, then the kept dispatches (f_8067c310 with the list's draw
   context; M6d: before the job's commands, not after), then the CallCommands. The water wait may only go here
   (jobs-state.md B: earlier positions deadlock); f_8028a2a0's call site (0x8028a538) never calls a job that kept one
   (counted).

   Check that it applies: the 600-present line "direct patch job sync" (kept and placed a frame, by kind; dropped; calls of
   the procs that passed through). */
#include "direct.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>
void f_8067c310(Ctx *);   /* (lifted functions the host calls by name: a code pointer is a host function pointer) */

uint64_t nvn_native_proc(const char *name);   /* nvn_native.c */
uint64_t eng_jobs_current(uint64_t *view);   /* eng_jobs.c */
extern volatile LONG64 hos_present_count;

static struct {
    volatile LONG64 waits, barriers, dispatches, placed_waits, placed_barriers, placed_dispatches, dropped, through, calls,
        calls_kept;
} n;

/* the host's proc of name (nvn_native_proc, the same each time: a race only looks it up twice) */
static uint64_t proc_of(uint64_t *p, const char *name) {
    uint64_t v = __atomic_load_n(p, __ATOMIC_RELAXED);
    if (!v) __atomic_store_n(p, v = nvn_native_proc(name), __ATOMIC_RELAXED);
    return v;
}
static uint64_t p_wait_, p_barrier_, p_call_;
#define P_WAIT proc_of(&p_wait_, "nvnCommandBufferWaitEvent")
#define P_BARRIER proc_of(&p_barrier_, "nvnCommandBufferBarrier")
#define P_CALL proc_of(&p_call_, "nvnCommandBufferCallCommands")
/* a row's call is of the proc it names: the game's code is the patch's */
static void expect(uint64_t proc, uint64_t want, uint64_t site, const char *name) {
    if (proc != want)
        hos_fatal(NULL, "direct job sync: the call at %#llx is of %#llx, not %s (%#llx)", (unsigned long long)site,
                  (unsigned long long)proc, name, (unsigned long long)want);
}

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

/* a wait on cmd kept for its job's view record (1), or not a job's (0: the caller calls the proc) */
static int keep_wait(uint64_t cmd, uint64_t event, uint32_t mode, uint32_t value) {
    const uint64_t view = job_view(cmd);
    if (!view) return 0;
    AcquireSRWLockExclusive(&kept_lock);
    Kept *k = kept_of(view);
    if (k && k->nw < 8) {
        k->w[k->nw].ev = event, k->w[k->nw].mode = mode, k->w[k->nw].value = value, k->nw++;
        ReleaseSRWLockExclusive(&kept_lock);
        InterlockedIncrement64(&n.waits);
        return 1;
    }
    ReleaseSRWLockExclusive(&kept_lock);
    return 0;
}

/* f_82c02f20, f_82c03170: the WaitEvent call (cmd, event, mode, value), blr x8; f_831fa4a0 (the same wait code as
   f_82c02f20's) tail-jumps to it, br x4 (f_80279400, water, calls f_831fa4a0 at 0x80279448: WaitEvent's return is
   0x8027944c): the row in place of the tail call, then f_831fa4a0 returns */
PATCH_AT(0x82c03018, word(0xd63f0100), instead, none, direct_job_sync_wait, guest(computed), sp, as(k(0x82c03018), site),
         as(x8, proc), as(x0, cmd), as(x1, event), as(w2, mode), as(w3, value)) {
    expect(proc, P_WAIT, site, "nvnCommandBufferWaitEvent");
    if (keep_wait(cmd, event, mode, value)) return;
    InterlockedIncrement64(&n.through);
    guest_call(g, sp, proc, cmd, event, mode, value, 0);
}
PATCH_AT_SITE(0x82c03268, word(0xd63f0100), instead, none, direct_job_sync_wait, guest(computed), sp, k(0x82c03268), x8,
              x0, x1, w2, w3);
PATCH_AT_SITE(0x831fa588, word(0xd61f0080), instead, none, direct_job_sync_wait, guest(computed), sp, k(0x831fa588), x4,
              x0, x1, w2, w3);

/* f_82c02f20, f_82c03170: the Barrier call (cmd, bits) */
PATCH_AT(0x82c030c0, word(0xd63f0100), instead, none, direct_job_sync_barrier, guest(computed), sp,
         as(k(0x82c030c0), site), as(x8, proc), as(x0, cmd), as(w1, bits)) {
    expect(proc, P_BARRIER, site, "nvnCommandBufferBarrier");
    const uint64_t view = job_view(cmd);
    if (view) {
        AcquireSRWLockExclusive(&kept_lock);
        Kept *k = kept_of(view);
        if (k) {
            k->bits |= bits;
            ReleaseSRWLockExclusive(&kept_lock);
            InterlockedIncrement64(&n.barriers);
            return;
        }
        ReleaseSRWLockExclusive(&kept_lock);
    }
    InterlockedIncrement64(&n.through);
    guest_call(g, sp, proc, cmd, bits, 0, 0, 0);
}
PATCH_AT_SITE(0x82c03310, word(0xd63f0100), instead, none, direct_job_sync_barrier, guest(computed), sp, k(0x82c03310),
              x8, x0, w1);

/* the job-call site, f_806688c0's CallCommands (cmd, count, handles) with the view record (x21) and the list's draw
   context ([sp + 0x10]): the view record's kept ops around it */
PATCH_AT(0x80668c80, word(0xd63f0100), instead, none, direct_job_sync_call, guest(computed), sp, as(x8, proc),
         as(x0, cmd), as(w1, count), as(x2, handles), as(x21, view), as(ld(sp, 0x10), drawctx)) {
    expect(proc, P_CALL, 0x80668c80, "nvnCommandBufferCallCommands");
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
        guest_call(g, sp, proc, cmd, count, handles, 0, 0);
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
    for (int i = 0; i < k.nw; i++) guest_call(g, sp, P_WAIT, cmd, k.w[i].ev, k.w[i].mode, k.w[i].value, 0);
    if (k.bits) guest_call(g, sp, P_BARRIER, cmd, k.bits, 0, 0, 0);
    for (int i = 0; i < k.nd; i++) guest_call(g, sp, CODE_PTR(f_8067c310), k.d[i].entry, drawctx, k.d[i].x2, k.d[i].x3, 0);
    guest_call(g, sp, proc, cmd, count, handles, 0, 0);
    InterlockedAdd64(&n.placed_waits, k.nw);
    InterlockedAdd64(&n.placed_barriers, k.bits ? 1 : 0);
    InterlockedAdd64(&n.placed_dispatches, k.nd);
}

/* the occlusion grid's dispatch (grid entry, drawctx, flag, flag): kept for the view's job (its x0 left as it came), or
   the game's (orig) */
PATCH_FUNC(0x8067c310, word(0xb9400808), body(0xf7211f1c433e65c6), aapcs, x0, direct_job_sync_grid_dispatch,
           orig, as(x0, entry), as(x1, drawctx), x2, x3) {
    const uint64_t view = hos_guest_range(drawctx + 216, 8) ? job_view(G64(drawctx + 216)) : 0;
    if (!view) return guest_orig(orig);
    AcquireSRWLockExclusive(&kept_lock);
    Kept *k = kept_of(view);
    if (k && k->nd < 8) {
        k->d[k->nd].entry = entry, k->d[k->nd].x2 = x2, k->d[k->nd].x3 = x3, k->nd++;
        ReleaseSRWLockExclusive(&kept_lock);
        InterlockedIncrement64(&n.dispatches);
        return entry;
    }
    ReleaseSRWLockExclusive(&kept_lock);
    return guest_orig(orig);
}

void direct_job_sync_log(double frames) {
    static LONG64 last[10];
    const LONG64 now[10] = {n.waits, n.barriers, n.dispatches, n.placed_waits, n.placed_barriers, n.placed_dispatches, n.dropped,
                            n.through, n.calls, n.calls_kept};
    LONG64 d[10];
    for (int i = 0; i < 10; i++) d[i] = now[i] - last[i];
    hos_log("direct patch job sync (f_82c02f20, f_82c03170, f_831fa4a0, f_8067c310 -> f_806688c0's job call): a frame: kept in "
            "jobs %.2f waits, %.2f barriers, %.2f dispatches; placed at the job call %.2f waits, %.2f barriers, %.2f dispatches "
            "(%.2f of %.2f job calls had some); dropped %lld; proc calls passed through (not a job's) %.2f",
            d[0] / frames, d[1] / frames, d[2] / frames, d[3] / frames, d[4] / frames, d[5] / frames, d[9] / frames, d[8] / frames,
            (long long)d[6], d[7] / frames);
    memcpy(last, now, sizeof last);
}
