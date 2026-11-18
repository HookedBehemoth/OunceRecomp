/* Game patch, direct mode only: every render job starts with the system binder's storage buffers bound,
   so its draws do not read what the layer list bound passes earlier (docs/direct/jobs.md). A job is a rendering secondary that starts from nothing in the direct back end; on the console
   a job inherits the layer list's binds, and its task and mesh draws read slots 0-3 of stages 6 and 7 from there.

   The system binder f_80101ec0(info) binds those four slots (to every stage) with:
     slot 0: f_801060f0(info, [V + 0x538], 0xfb000000)   (0x80102450)
     slot 1: f_801060f0(info, [V + 0x540], 0xfb010101)   (0x80102470)
     slot 2: f_801060f0(info, *[0x84ca0fa8], 0xfb020202)
     slot 3: f_80100210([L + 0xc00], [info + 0xb0], 0xfb030303), L = [[[0x84c9e740]] + 0x1e8]
   with V = [[info + 0xe0] + 8], the scene's view object (each buffer object picks its own frame's copy). f_801060f0 reads
   only info + 0xb0 (the draw context, whose + 0xd8 is the command buffer) and info + 0xbc (the kind: 0x20 binds otherwise).
   So the patch needs only V, which a row at the binder's entry notes, and the job's draw context (games/oot/patches/eng_jobs.c).
   One V a frame is the game's: a frame whose binder calls see a second V is counted and logged, since its jobs would
   bind the last one, not their own pass's. The splice sits after the job prologue's SetDepthRange
   (the call returning to 0x800855ac in f_80084de0: an At row, a PATCH_AT declaration, runtime/patch.h, that calls guest code), as the
   M3 survey placed it; the four binds go to the job's own command buffer through a 0x100-byte block on the guest stack
   below the job's SP (info + 0xb0 = the job's draw context, + 0xbc = 0).

   Check that it applies: the 600-present line "direct patch mesh system" (jobs bound a frame, the view objects seen). */
#include "patches.h"
#include <string.h>
#include "os.h"
void f_801060f0(Ctx *), f_80100210(Ctx *);   /* (lifted functions the host calls by name: a code pointer is a host function pointer) */

uint64_t eng_jobs_drawctx(void);   /* eng_jobs.c */
extern volatile int64_t hos_present_count;   /* (display_sdl.c) */

static volatile uint64_t view_obj;   /* (V, the last the binder was called with) */
static volatile int64_t view_present;   /* (the present at which V last changed) */
static struct { volatile int64_t binder, views, jobs, no_view, no_ctx, two_views; } n;

PATCH_AT(0x80101ec0, word(0xd503237f), before, none, direct_mesh_system_binder, as(x0, info)) {   /* (info): V noted */
    os_atomic_inc(&n.binder);
    if (!hos_guest_range(info + 0xe0, 8) || !hos_guest_range(G64(info + 0xe0) + 8, 8)) return;
    const uint64_t v = G64(G64(info + 0xe0) + 8);
    if (v && v != view_obj) {
        if (view_obj && view_present == hos_present_count && os_atomic_inc(&n.two_views) <= 8)
            hos_log("direct patch mesh system: a second view object %#llx in one frame (with %#llx): its jobs bind one of them",
                    (unsigned long long)v, (unsigned long long)view_obj);
        view_present = hos_present_count;
        if (os_atomic_inc(&n.views) <= 8)
            hos_log("direct patch mesh system: the system binder's view object %#llx (was %#llx)", (unsigned long long)v,
                    (unsigned long long)view_obj);
        view_obj = v;
    }
}

/* (f_80084de0 after its blr x8 to SetDepthRange: sp the job's) */
PATCH_AT(0x800855a8, word(0xd63f0100), after, none, direct_mesh_system_bind, guest(0x801060f0, 0x80100210), as(sp, job_sp)) {
    const uint64_t v = view_obj, dc = eng_jobs_drawctx();
    if (!v || !hos_guest_range(v + 0x538, 16)) { os_atomic_inc(&n.no_view); return; }
    if (!dc) { os_atomic_inc(&n.no_ctx); return; }
    const uint64_t sp = (job_sp - 0x200) & ~15ull, info = sp + 0x100;
    if (!hos_guest_range(sp, 0x200)) { os_atomic_inc(&n.no_ctx); return; }
    memset(GP(info), 0, 0x100);
    G64(info + 0xb0) = dc;
    guest_call(g, sp, CODE_PTR(f_801060f0), info, G64(v + 0x538), 0xfb000000u, 0, 0);
    guest_call(g, sp, CODE_PTR(f_801060f0), info, G64(v + 0x540), 0xfb010101u, 0, 0);
    if (hos_guest_range(0x84ca0fa8, 8) && hos_guest_range(G64(0x84ca0fa8), 8))
        guest_call(g, sp, CODE_PTR(f_801060f0), info, G64(G64(0x84ca0fa8)), 0xfb020202u, 0, 0);
    if (hos_guest_range(0x84c9e740, 8) && hos_guest_range(G64(0x84c9e740), 8) && hos_guest_range(G64(G64(0x84c9e740)) + 0x1e8, 8)) {
        const uint64_t l = G64(G64(G64(0x84c9e740)) + 0x1e8);
        if (l && hos_guest_range(l + 0xc00, 8) && G64(l + 0xc00)) guest_call(g, sp, CODE_PTR(f_80100210), G64(l + 0xc00), dc, 0xfb030303u, 0, 0);
    }
    os_atomic_inc(&n.jobs);
}

void direct_mesh_system_log(double frames) {
    static int64_t last[6];
    const int64_t now[6] = {n.binder, n.views, n.jobs, n.no_view, n.no_ctx, n.two_views};
    hos_log("direct patch mesh system (the system binder's storage slots 0-3 bound at every job's prologue, f_80084de0 after "
            "0x800855ac): a frame %.2f jobs bound, %.2f system binder calls; view object changes since the start %lld, "
            "changes within a frame (two views: wrong binds) %lld; jobs without a view object %lld, without a draw context %lld",
            (now[2] - last[2]) / frames, (now[0] - last[0]) / frames, (long long)now[1], (long long)now[5], (long long)(now[3] - last[3]),
            (long long)(now[4] - last[4]));
    memcpy(last, now, sizeof last);
}
