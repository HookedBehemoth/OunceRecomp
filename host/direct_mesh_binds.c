/* (native-vk M4b) Game patch, direct mode only: the GPU-culled mesh drawers bind the task and mesh storage buffers they
   read, so a job's mesh draws do not depend on what an earlier job of the pass left bound (docs/direct/m3-patch-sites.md 3,
   part 2; m0-jobs-state.md A3/A4). A job is a rendering secondary that starts from nothing in the direct back end, so a
   slot nobody binds in the job reads the null window there.

   f_82c02bc0 (the culled-instance drawer) binds names 4 and 5 itself: f_801a2d70(ctx, item, 4, o + 0x1b0, 0) and
   f_801a6a40(ctx, item, 5, o + 0xd70, 0), o = [[param_1 + 0x18] + 0x338], ctx = [[item] + 0x28], when [o + 0xd40] == 1.
   f_801a43e0, f_801a46a0 and f_801a6720 draw the same culled lists and bind neither (on the console they read the
   values f_82c02bc0 left from an earlier job of the pass). Each is hooked at its entry: the two binds are made first, as
   f_82c02bc0 makes them, then the drawer's body runs. A binder binds nothing when the item's program has no slot for the
   name (its name table's four bytes 0xff), so a program that does not read the names is not changed.

   f_801a6720 was left open by the M3 survey (its draws saw either value set depending on job order): the hook logs the
   name-table words of names 4 and 5 of the programs it draws (the first 32 distinct ones) and counts the calls whose
   program lists either name.

   Check that it applies: the 600-present line "direct patch mesh binds" (calls a frame per drawer, binds made, skipped
   because the culled-instance object is not active). */
#include "direct.h"
#include <string.h>
#include <windows.h>

uint64_t hos_call_guest(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);   /* hos_hostcall.c */

enum { D_43E0, D_46A0, D_6720, D_N };
static const char *const names[D_N] = {"f_801a43e0", "f_801a46a0", "f_801a6720"};
static struct { volatile LONG64 calls, bound, inactive, lists45, unreadable; } n[D_N];

/* the name table word of a name (four bytes, one a stage; all 0xff: not read) of the item's program; 0: unreadable */
static int name_word(uint64_t item, int name, uint32_t *w) {
    if (!hos_guest_range(item + 0x10, 8)) return 0;
    const uint64_t a = G64(item + 0x10);
    if (!hos_guest_range(a + 8, 8)) return 0;
    const uint64_t b = G64(a + 8);
    if (!hos_guest_range(b + 8, 8)) return 0;
    const uint64_t t = G64(b + 8);
    if (!hos_guest_range(t + (uint64_t)name * 4 + 4, 4)) return 0;
    *w = G32(t + (uint64_t)name * 4 + 4);
    return 1;
}

static void log_names(uint64_t item, uint32_t w4, uint32_t w5) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static uint64_t seen[32];
    static int nseen;
    const uint64_t prog = hos_guest_range(item + 0x10, 8) ? G64(item + 0x10) : 0;
    AcquireSRWLockExclusive(&lock);
    int known = 0;
    for (int i = 0; i < nseen; i++) known |= seen[i] == prog;
    if (!known && nseen < 32) seen[nseen++] = prog;
    ReleaseSRWLockExclusive(&lock);
    static int say = -1;
    if (say < 0) say = hos_log_switch("logpatch");
    if (!known && nseen <= 32 && say)
        hos_log("direct patch mesh binds: f_801a6720 draws program info %#llx: name 4 %08x, name 5 %08x (ffffffff: not read)",
                (unsigned long long)prog, w4, w5);
}

static int drawer(Ctx *c, int which) {
    if (!rx_direct()) return 0;
    const uint64_t p1 = c->x[0], item = c->x[1];
    InterlockedIncrement64(&n[which].calls);
    uint32_t w4 = ~0u, w5 = ~0u;
    if (!name_word(item, 4, &w4) || !name_word(item, 5, &w5)) InterlockedIncrement64(&n[which].unreadable);
    else if (w4 != ~0u || w5 != ~0u) InterlockedIncrement64(&n[which].lists45);
    if (which == D_6720) log_names(item, w4, w5);
    if (!hos_guest_range(p1 + 0x18, 8) || !hos_guest_range(G64(p1 + 0x18) + 0x338, 8) || !hos_guest_range(item, 8)) return 0;
    const uint64_t o = G64(G64(p1 + 0x18) + 0x338);
    if (!o || !hos_guest_range(o + 0xd40, 1) || *(const uint8_t *)GP(o + 0xd40) != 1) {
        InterlockedIncrement64(&n[which].inactive);
        return 0;
    }
    const uint64_t ctx = hos_guest_range(G64(item) + 0x28, 8) ? G64(G64(item) + 0x28) : 0;
    if (!ctx) return 0;
    uint64_t saved[19];
    memcpy(saved, c->x, sizeof saved);
    c->x[4] = 0;
    hos_call_guest(c, 0x801a2d70ull, ctx, item, 4, o + 0x1b0);
    c->x[4] = 0;
    hos_call_guest(c, 0x801a6a40ull, ctx, item, 5, o + 0xd70);
    memcpy(c->x, saved, sizeof saved);   /* (the drawer's arguments and the caller-saved registers as they were) */
    InterlockedIncrement64(&n[which].bound);
    return 0;
}
ENTRY_HOOK(0x801a43e0) { return drawer(c, D_43E0); }
ENTRY_HOOK(0x801a46a0) { return drawer(c, D_46A0); }
ENTRY_HOOK(0x801a6720) { return drawer(c, D_6720); }

void direct_mesh_binds_log(double frames) {
    static LONG64 last[D_N][5];
    char line[768];
    int k = snprintf(line, sizeof line, "direct patch mesh binds (names 4 and 5 bound before the drawer, as f_82c02bc0 binds them), a frame:");
    for (int i = 0; i < D_N; i++) {
        const LONG64 now[5] = {n[i].calls, n[i].bound, n[i].inactive, n[i].lists45, n[i].unreadable};
        k += snprintf(line + k, k < (int)sizeof line ? sizeof line - k : 0,
                      " %s %.2f calls, %.2f bound, %.2f the culled object inactive, %.2f with a program naming 4 or 5, %.2f names unreadable;",
                      names[i], (now[0] - last[i][0]) / frames, (now[1] - last[i][1]) / frames, (now[2] - last[i][2]) / frames,
                      (now[3] - last[i][3]) / frames, (now[4] - last[i][4]) / frames);
        memcpy(last[i], now, sizeof now);
    }
    hos_log("%s", line);
}
