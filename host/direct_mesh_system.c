/* Game patch, direct mode only: every render job starts with the system binder's storage buffers bound,
   so its draws do not read what the layer list bound passes earlier (docs/direct/patch-sites.md 3, part 1;
   jobs-state.md A3/A4). A job is a rendering secondary that starts from nothing in the direct back end; on the console
   a job inherits the layer list's binds, and its task and mesh draws read slots 0-3 of stages 6 and 7 from there.

   The system binder f_80101ec0(info) binds those four slots (to every stage) with:
     slot 0: f_801060f0(info, [V + 0x538], 0xfb000000)   (0x80102450)
     slot 1: f_801060f0(info, [V + 0x540], 0xfb010101)   (0x80102470)
     slot 2: f_801060f0(info, *[0x84ca0fa8], 0xfb020202)
     slot 3: f_80100210([L + 0xc00], [info + 0xb0], 0xfb030303), L = [[[0x84c9e740]] + 0x1e8]
   with V = [[info + 0xe0] + 8], the scene's view object (each buffer object picks its own frame's copy). f_801060f0 reads
   only info + 0xb0 (the draw context, whose + 0xd8 is the command buffer) and info + 0xbc (the kind: 0x20 binds otherwise).
   So the patch needs only V, which the binder is hooked at its entry to note (every V it is called with is counted: one
   is expected), and the job's draw context (host/eng_jobs.c). The splice sits after the job prologue's SetDepthRange
   (the call returning to 0x800855ac in f_80084de0), as the M3 survey placed it; the four binds go to the job's own
   command buffer through a 0x100-byte block on the guest stack (info + 0xb0 = the job's draw context, + 0xbc = 0).

   Check that it applies: the 600-present line "direct patch mesh system" (jobs bound a frame, the view objects seen). */
#include "direct.h"
#include <string.h>
#include <windows.h>

uint64_t hos_call_guest(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);   /* hos_hostcall.c */
uint64_t eng_jobs_drawctx(void);   /* eng_jobs.c */

static volatile uint64_t view_obj;   /* (V, the last the binder was called with) */
static struct { volatile LONG64 binder, views, jobs, no_view, no_ctx; } n;

ENTRY_HOOK(0x80101ec0) {   /* (info): V noted, the body runs */
    if (!rx_direct()) return 0;
    InterlockedIncrement64(&n.binder);
    const uint64_t info = c->x[0];
    if (!hos_guest_range(info + 0xe0, 8) || !hos_guest_range(G64(info + 0xe0) + 8, 8)) return 0;
    const uint64_t v = G64(G64(info + 0xe0) + 8);
    if (v && v != view_obj) {
        if (InterlockedIncrement64(&n.views) <= 8)
            hos_log("direct patch mesh system: the system binder's view object %#llx (was %#llx)", (unsigned long long)v,
                    (unsigned long long)view_obj);
        view_obj = v;
    }
    return 0;
}

SPLICE_AFTER(0x80084de0, "c->x[30] = 0x800855acULL; A64_ICALL(c, _t); }") {
    if (!rx_direct()) return;
    const uint64_t v = view_obj, dc = eng_jobs_drawctx();
    if (!v || !hos_guest_range(v + 0x538, 16)) { InterlockedIncrement64(&n.no_view); return; }
    if (!dc) { InterlockedIncrement64(&n.no_ctx); return; }
    uint64_t saved[32];
    memcpy(saved, c->x, sizeof saved);
    const int z = c->z, nf = c->n, cf = c->c, vf = c->v;
    const uint64_t sp = (c->x[31] - 0x200) & ~15ull, info = sp + 0x100;
    if (!hos_guest_range(sp, 0x200)) { InterlockedIncrement64(&n.no_ctx); return; }
    memset(GP(info), 0, 0x100);
    G64(info + 0xb0) = dc;
    c->x[31] = sp;
    hos_call_guest(c, 0x801060f0ull, info, G64(v + 0x538), 0xfb000000u, 0);
    hos_call_guest(c, 0x801060f0ull, info, G64(v + 0x540), 0xfb010101u, 0);
    if (hos_guest_range(0x84ca0fa8, 8) && hos_guest_range(G64(0x84ca0fa8), 8))
        hos_call_guest(c, 0x801060f0ull, info, G64(G64(0x84ca0fa8)), 0xfb020202u, 0);
    if (hos_guest_range(0x84c9e740, 8) && hos_guest_range(G64(0x84c9e740), 8) && hos_guest_range(G64(G64(0x84c9e740)) + 0x1e8, 8)) {
        const uint64_t l = G64(G64(G64(0x84c9e740)) + 0x1e8);
        if (l && hos_guest_range(l + 0xc00, 8) && G64(l + 0xc00)) hos_call_guest(c, 0x80100210ull, G64(l + 0xc00), dc, 0xfb030303u, 0);
    }
    memcpy(c->x, saved, sizeof saved);
    c->z = z, c->n = nf, c->c = cf, c->v = vf;
    InterlockedIncrement64(&n.jobs);
}

void direct_mesh_system_log(double frames) {
    static LONG64 last[5];
    const LONG64 now[5] = {n.binder, n.views, n.jobs, n.no_view, n.no_ctx};
    hos_log("direct patch mesh system (the system binder's storage slots 0-3 bound at every job's prologue, f_80084de0 after "
            "0x800855ac): a frame %.2f jobs bound, %.2f system binder calls; view objects seen since the start %lld; jobs "
            "without a view object %lld, without a draw context %lld",
            (now[2] - last[2]) / frames, (now[0] - last[0]) / frames, (long long)now[1], (long long)(now[3] - last[3]),
            (long long)(now[4] - last[4]));
    memcpy(last, now, sizeof last);
}
