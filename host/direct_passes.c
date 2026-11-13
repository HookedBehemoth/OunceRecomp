/* Game patch, direct mode only: a render job is recorded only when its pass's target formats are known
.

   f_80084de0 (the job recorder) is entered through host/eng_jobs.c's ENTRY_HOOK; in direct mode it asks here first. Its
   pass key (x3) is the pass's identity. If the back end's format table does not know the key, the recorder returns at entry,
   as its own early return at 0x8008693c does (no view record, no slot): the layer list then finds no record and draws the
   pass through the game's inline path f_80681320, after its own SetRenderTargets. The entry hook of f_80681320 below names
   that pass's key to the back end, which takes the formats from the targets the list set (hsd_pass_inline). A key's first
   frame therefore draws inline, the frames after it as jobs.

   The two inline callers name the key differently: f_806688c0 (return 0x80668d84) holds key + 0xc0 at [sp + 0x18] (the
   recorder's pointer); f_8028a2a0 (return 0x8028a58c) passes a copy of the key on its own stack (x2), which the game itself
   matches to records by the key's first 0x70 bytes. The recorder's keys are noted here by those bytes, so that copy names
   the recorder's pointers (every key with those bytes: the two cube-face sets share them). A pass no recorder ever asked for (about 20 of f_8028a2a0's a frame: local passes with no jobs) is
   left alone: it is the game's own inline pass, not a miss.

   Check that it applies: the 600-present line "direct patch passes" (recorder asked, early returns, inline passes named per
   site) and the back end's "jobs a frame" line. */
#include "direct.h"
#include <string.h>
#include <windows.h>

static struct {
    volatile LONG64 asked, early, inl[2], inl_no_jobs[2], inl_other, inl_no_list;
} n;

/* the recorder's keys by the first 0x70 bytes of the key (a few dozen; written by the recorders, read at the inline path) */
#define NKEY 256
static struct { uint64_t key, hash; } keys[NKEY];
static int nkeys;
static SRWLOCK keys_lock = SRWLOCK_INIT;

static uint64_t key_hash(uint64_t a) {
    if (!hos_guest_range(a, 0x70)) return 0;
    const uint8_t *p = (const uint8_t *)GP(a);
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 0x70; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h | 1;
}
static void key_note(uint64_t key) {
    const uint64_t h = key_hash(key);
    if (!h) return;
    AcquireSRWLockShared(&keys_lock);
    int found = 0;
    for (int i = 0; i < nkeys; i++)
        if (keys[i].key == key) { found = keys[i].hash == h ? 1 : 2; break; }
    ReleaseSRWLockShared(&keys_lock);
    if (found == 1) return;
    AcquireSRWLockExclusive(&keys_lock);
    int i = 0;
    for (; i < nkeys; i++)
        if (keys[i].key == key) break;
    if (i == nkeys && nkeys < NKEY) nkeys++;
    if (i < NKEY) keys[i].key = key, keys[i].hash = h;
    ReleaseSRWLockExclusive(&keys_lock);
}
/* the recorder's pointers for key bytes at a: every key with those bytes (the game matches records by them, so they name
   one pass); n of them in out */
static int keys_by_bytes(uint64_t a, uint64_t *out, int cap) {
    const uint64_t h = key_hash(a);
    int k = 0;
    AcquireSRWLockShared(&keys_lock);
    for (int i = 0; i < nkeys && k < cap; i++)
        if (keys[i].hash == h) out[k++] = keys[i].key;
    ReleaseSRWLockShared(&keys_lock);
    return k;
}
static int key_known(uint64_t key) {
    int k = 0;
    AcquireSRWLockShared(&keys_lock);
    for (int i = 0; i < nkeys; i++)
        if (keys[i].key == key) { k = 1; break; }
    ReleaseSRWLockShared(&keys_lock);
    return k;
}

int direct_passes_recorder(Ctx *c) {
    const hsd_fns *d = rx_direct();
    if (!d) return 0;
    const uint64_t key = c->x[3];
    key_note(key);
    InterlockedIncrement64(&n.asked);
    if (d->pass_known(key)) return 0;
    InterlockedIncrement64(&n.early);
    return 1;   /* (as 0x8008693c: nothing recorded, no view record) */
}

ENTRY_HOOK(0x80681320) {   /* (queue, drawctx, &key, owner, ...): the game's inline path of a pass */
    const hsd_fns *d = rx_direct();
    if (!d) return 0;
    const uint64_t lr = c->x[30], sp = c->x[31];
    int site, nk = 0;
    uint64_t ks[8], cmd;
    if (lr == 0x80668d84ull) {   /* f_806688c0: [sp + 0x18] = key + 0xc0, the list's draw context at [sp + 0x10] */
        site = 0;
        ks[0] = G64(sp + 0x18) - 0xc0, cmd = G64(G64(sp + 0x10) + 216);
        nk = key_known(ks[0]);
    } else if (lr == 0x8028a58cull) {   /* f_8028a2a0: x2 a copy of the key, x1 the draw context; keys with the same bytes
                                           (cube faces of two sets share them) all name this pass */
        site = 1;
        nk = keys_by_bytes(c->x[2], ks, 8), cmd = G64(c->x[1] + 216);
    } else {
        InterlockedIncrement64(&n.inl_other);
        return 0;
    }
    if (!nk) {
        InterlockedIncrement64(&n.inl_no_jobs[site]);
        return 0;
    }
    void *list = direct_cmd_list(cmd);
    if (!list) InterlockedIncrement64(&n.inl_no_list);
    for (int i = 0; i < nk; i++) d->pass_inline(list, ks[i]);
    InterlockedIncrement64(&n.inl[site]);
    return 0;   /* (the game's body draws the pass) */
}

void direct_passes_log(double frames) {
    static LONG64 last[9];
    const LONG64 now[9] = {n.asked, n.early, n.inl[0], n.inl[1], n.inl_no_jobs[0], n.inl_no_jobs[1], n.inl_other, n.inl_no_list, nkeys};
    hos_log("direct patch passes (f_80084de0 entry, f_80681320 entry): a frame: recorder asked %.2f, early returns %.2f; inline "
            "passes of recorder keys %.2f (f_806688c0) %.2f (f_8028a2a0); inline passes no recorder asked for %.2f / %.2f; other "
            "callers %.2f; with no list %lld; %lld keys noted",
            (now[0] - last[0]) / frames, (now[1] - last[1]) / frames, (now[2] - last[2]) / frames, (now[3] - last[3]) / frames,
            (now[4] - last[4]) / frames, (now[5] - last[5]) / frames, (now[6] - last[6]) / frames, (long long)now[7], (long long)now[8]);
    memcpy(last, now, sizeof last);
}
