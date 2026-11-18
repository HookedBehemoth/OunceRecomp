/* Game patch, direct mode only: the GPU-culled mesh drawers bind the task and mesh storage buffers they
   read, so a job's mesh draws do not depend on what an earlier job of the pass left bound (docs/direct/jobs.md). A job is a rendering secondary that starts from nothing in the direct back end, so a
   slot nobody binds in the job reads the null window there.

   f_82c02bc0 (the culled-instance drawer) binds names 4 and 5 itself: f_801a2d70(ctx, item, 4, o + 0x1b0, 0) and
   f_801a6a40(ctx, item, 5, o + 0xd70, 0), o = [[param_1 + 0x18] + 0x338], ctx = [[item] + 0x28], when [o + 0xd40] == 1.
   f_801a43e0, f_801a46a0 and f_801a6720 draw the same culled lists and bind neither (on the console they read the
   values f_82c02bc0 left from an earlier job of the pass). Each has a row at its entry (PATCH_AT before): the two binds are made first, as
   f_82c02bc0 makes them, then the drawer's body runs. A binder binds nothing when the item's program has no slot for the
   name (its name table's four bytes 0xff), so a program that does not read the names is not changed.

   Check that it applies: the 600-present line "direct patch mesh binds" (calls a frame per drawer, binds made, skipped
   because the culled-instance object is not active). */
#include "direct.h"
#include <stdio.h>
#include <string.h>
#include "os.h"
void f_801a2d70(Ctx *), f_801a6a40(Ctx *);   /* (lifted functions the host calls by name: a code pointer is a host function pointer) */


enum { D_43E0, D_46A0, D_6720, D_N };
static const char *const names[D_N] = {"f_801a43e0", "f_801a46a0", "f_801a6720"};
static struct { volatile int64_t calls, bound, inactive; } n[D_N];

static void drawer(GuestCall *g, uint64_t sp, uint64_t p1, uint64_t item, int which) {
    os_atomic_inc(&n[which].calls);
    if (!hos_guest_range(p1 + 0x18, 8) || !hos_guest_range(G64(p1 + 0x18) + 0x338, 8) || !hos_guest_range(item, 8)) return;
    const uint64_t o = G64(G64(p1 + 0x18) + 0x338);
    if (!o || !hos_guest_range(o + 0xd40, 1) || *(const uint8_t *)GP(o + 0xd40) != 1) {
        os_atomic_inc(&n[which].inactive);
        return;
    }
    const uint64_t ctx = hos_guest_range(G64(item) + 0x28, 8) ? G64(G64(item) + 0x28) : 0;
    if (!ctx) return;
    guest_call(g, sp, CODE_PTR(f_801a2d70), ctx, item, 4, o + 0x1b0, 0);   /* (the drawer's registers as they were after each) */
    guest_call(g, sp, CODE_PTR(f_801a6a40), ctx, item, 5, o + 0xd70, 0);
    os_atomic_inc(&n[which].bound);
}
/* (at each drawer's entry, on the call's stack: the binds, then its body) */
PATCH_AT(0x801a43e0, word(0xd503237f), before, none, direct_mesh_binds_43e0, guest(0x801a2d70, 0x801a6a40), sp, x0, x1) {
    drawer(g, sp, x0, x1, D_43E0);
}
PATCH_AT(0x801a46a0, word(0xd503237f), before, none, direct_mesh_binds_46a0, guest(0x801a2d70, 0x801a6a40), sp, x0, x1) {
    drawer(g, sp, x0, x1, D_46A0);
}
PATCH_AT(0x801a6720, word(0xd503237f), before, none, direct_mesh_binds_6720, guest(0x801a2d70, 0x801a6a40), sp, x0, x1) {
    drawer(g, sp, x0, x1, D_6720);
}

void direct_mesh_binds_log(double frames) {
    static int64_t last[D_N][3];
    char line[768];
    int k = snprintf(line, sizeof line, "direct patch mesh binds (names 4 and 5 bound before the drawer, as f_82c02bc0 binds them), a frame:");
    for (int i = 0; i < D_N; i++) {
        const int64_t now[3] = {n[i].calls, n[i].bound, n[i].inactive};
        k += snprintf(line + k, k < (int)sizeof line ? sizeof line - k : 0, " %s %.2f calls, %.2f bound, %.2f the culled object inactive;",
                      names[i], (now[0] - last[i][0]) / frames, (now[1] - last[i][1]) / frames, (now[2] - last[i][2]) / frames);
        memcpy(last[i], now, sizeof now);
    }
    hos_log("%s", line);
}
