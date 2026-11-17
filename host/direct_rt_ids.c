/* Game patch, direct mode only: a system texture keeps its descriptor id while its texture and view are
   unchanged (docs/direct-backend-plan.md, open work).

   f_80667d20 (the per-view table of 30 system textures) finds one entry's dirty bit set every frame and calls
   f_800634b0(entry, view, 0) at 0x806680d8 (return 0x806680dc) with a texture view built on its stack. Given a view,
   f_800634b0 frees the entry's id (+0x68) and registers the texture again under a new one (agl's descriptor ring), even when
   nothing changed: table word 4 gets a fresh id every frame, a heap entry is written and the deferred table copied at the
   submit changes. Here, at that call only (an At row in place of the BL, a PATCH_AT declaration, host/patch.h; it makes the call
   itself through guest_call when the call is to run): when the entry has an id, and the entry's first 0xc0 bytes (the texture
   pointer and settings at +0x00..+0x64, the id at +0x68, the copies f_800634b0 keeps of them from +0x70), the view's 16
   bytes and the texture object (descriptor, layout, storage address) are all what they were right after this row last let
   the call run, the call is skipped: the id stays, nothing is freed or registered. Any difference runs the game's path (new
   id, the old one freed after the frames in flight). An id is reused only for an identical descriptor.

   Check that it applies: the 600-present line "direct patch rt ids" (calls kept / run, and why run). */
#include "direct.h"
#include "../gpu/api/rx_nvn.h"
#include <stddef.h>
#include <string.h>
#include "os.h"
void f_800634b0(Ctx *);   /* (lifted functions the host calls by name: a code pointer is a host function pointer) */

#define ENTRY_BYTES 0xc0
typedef struct {
    uint8_t entry[ENTRY_BYTES];
    uint8_t view[16];
    uint8_t tex[offsetof(nvn_tex, serial)];
} Snap;
#define NE 64
static struct { uint64_t entry; int have; Snap s; } ents[NE];
static os_rwlock lock = OS_RWLOCK_INIT;
static struct { volatile int64_t calls, kept, run_first, run_no_id, run_entry, run_view, run_tex, run_other_caller; } n;

static int snap(uint64_t entry, uint64_t view, Snap *s) {
    memset(s, 0, sizeof *s);
    if (!hos_guest_range(entry, ENTRY_BYTES) || !hos_guest_range(view, 16)) return 0;
    memcpy(s->entry, GP(entry), ENTRY_BYTES);
    memcpy(s->view, GP(view), 16);
    const uint64_t t = G64(entry + 16);
    if (t && hos_guest_range(t, sizeof(nvn_tex))) memcpy(s->tex, GP(t), sizeof s->tex);
    return 1;
}

#define REREGISTER CODE_PTR(f_800634b0)  /* f_800634b0 desc_reregister (entry, view, flag) */

/* f_80667d20 at 0x806680d8 in place of its bl f_800634b0: sp the caller's, x0-x2 the call's arguments */
PATCH_AT(0x806680d8, word(0x97e7ecf6), instead, none, direct_rt_ids_reregister, guest(0x800634b0), sp, as(x0, entry), as(x1, view), as(x2, flag)) {
    if (!view) { guest_call(g, sp, REREGISTER, entry, view, flag, 0, 0); return; }
    os_atomic_inc(&n.calls);
    Snap now;
    if (!snap(entry, view, &now)) { guest_call(g, sp, REREGISTER, entry, view, flag, 0, 0); return; }
    os_rwlock_lock_shared(&lock);
    int i = 0;
    for (; i < NE && ents[i].entry && ents[i].entry != entry; i++) {}
    int same = 0, why = 0;
    if (i == NE || !ents[i].have) why = 1;
    else if (G32(entry + 0x68) == 0xffffffffu) why = 2;
    else if (memcmp(ents[i].s.entry, now.entry, ENTRY_BYTES)) why = 3;
    else if (memcmp(ents[i].s.view, now.view, 16)) why = 4;
    else if (memcmp(ents[i].s.tex, now.tex, sizeof now.tex)) why = 5;
    else same = 1;
    if (why >= 3) {   /* (the first bytes that differ, named a few times) */
        static volatile int32_t told;
        const uint8_t *a = why == 3 ? ents[i].s.entry : why == 4 ? ents[i].s.view : ents[i].s.tex;
        const uint8_t *b = why == 3 ? now.entry : why == 4 ? now.view : now.tex;
        const int len = why == 3 ? ENTRY_BYTES : why == 4 ? 16 : (int)sizeof now.tex;
        int k = 0;
        while (k < len && a[k] == b[k]) k++;
        if (os_atomic_inc(&told) <= 12)
            hos_log("direct patch rt ids: entry %#llx: %s differs from byte %#x (%02x -> %02x)", (unsigned long long)entry,
                    why == 3 ? "the entry" : why == 4 ? "the view" : "the texture", k, a[k], b[k]);
    }
    os_rwlock_unlock_shared(&lock);
    if (same) {
        os_atomic_inc(&n.kept);
        return;   /* (the id and its registration stay; f_800634b0's result is not read at 0x806680dc) */
    }
    os_atomic_inc(why == 1 ? &n.run_first : why == 2 ? &n.run_no_id : why == 3 ? &n.run_entry : why == 4 ? &n.run_view : &n.run_tex);
    guest_call(g, sp, REREGISTER, entry, view, flag, 0, 0);
    Snap after;
    if (snap(entry, view, &after)) {
        os_rwlock_lock(&lock);
        int j = 0;
        for (; j < NE && ents[j].entry && ents[j].entry != entry; j++) {}
        if (j < NE) ents[j].entry = entry, ents[j].s = after, ents[j].have = 1;
        os_rwlock_unlock(&lock);
    }
}

void direct_rt_ids_log(double frames) {
    static int64_t last[7];
    const int64_t now[7] = {n.calls, n.kept, n.run_first, n.run_no_id, n.run_entry, n.run_view, n.run_tex};
    int64_t d[7];
    for (int i = 0; i < 7; i++) d[i] = now[i] - last[i];
    hos_log("direct patch rt ids (f_800634b0 from 0x806680dc): a frame: %.2f calls, id kept %.2f; the game's path: first %.2f, no id "
            "%.2f, entry changed %.2f, view changed %.2f, texture changed %.2f",
            d[0] / frames, d[1] / frames, d[2] / frames, d[3] / frames, d[4] / frames, d[5] / frames, d[6] / frames);
    memcpy(last, now, sizeof last);
}

