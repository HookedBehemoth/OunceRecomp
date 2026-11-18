/* Game patch, direct mode only: a render job is recorded only when its pass's target formats are known
.

   f_80084de0 (the job recorder) is replaced by games/oot/patches/eng_jobs.c's PATCH_FUNC; in direct mode it asks here first. Its
   pass key (x3) is the pass's identity. If the back end's format table does not know the key, the recorder returns at entry,
   as its own early return at 0x8008693c does (no view record, no slot): the layer list then finds no record and draws the
   pass through the game's inline path f_80681320, after its own SetRenderTargets. At rows before its two calls name
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
#include "os.h"

extern volatile int64_t hos_present_count;
static struct {
    volatile int64_t asked, early, inl[2], inl_no_jobs[2], inl_no_list;
} n;

/* the recorder's keys by the first 0x70 bytes of the key (a few dozen; written by the recorders, read at the inline path) */
#define NKEY 256
static struct { uint64_t key, hash; } keys[NKEY];
static int nkeys;
static os_rwlock keys_lock = OS_RWLOCK_INIT;

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
    os_rwlock_lock_shared(&keys_lock);
    int found = 0;
    for (int i = 0; i < nkeys; i++)
        if (keys[i].key == key) { found = keys[i].hash == h ? 1 : 2; break; }
    os_rwlock_unlock_shared(&keys_lock);
    if (found == 1) return;
    os_rwlock_lock(&keys_lock);
    int i = 0;
    for (; i < nkeys; i++)
        if (keys[i].key == key) break;
    if (i == nkeys && nkeys < NKEY) nkeys++;
    if (i < NKEY) keys[i].key = key, keys[i].hash = h;
    os_rwlock_unlock(&keys_lock);
}
/* the recorder's pointers for key bytes at a: every key with those bytes (the game matches records by them, so they name
   one pass); n of them in out */
static int keys_by_bytes(uint64_t a, uint64_t *out, int cap) {
    const uint64_t h = key_hash(a);
    int k = 0;
    os_rwlock_lock_shared(&keys_lock);
    for (int i = 0; i < nkeys && k < cap; i++)
        if (keys[i].hash == h) out[k++] = keys[i].key;
    os_rwlock_unlock_shared(&keys_lock);
    return k;
}
static int key_known(uint64_t key) {
    int k = 0;
    os_rwlock_lock_shared(&keys_lock);
    for (int i = 0; i < nkeys; i++)
        if (keys[i].key == key) { k = 1; break; }
    os_rwlock_unlock_shared(&keys_lock);
    return k;
}

int direct_passes_recorder(uint64_t key) {
    const hsd_fns *d = rx_direct();
    if (!d) return 0;
    key_note(key);
    os_atomic_inc(&n.asked);
    if (d->pass_known(key)) return 0;
    os_atomic_inc(&n.early);
    {   /* HOS_DBG=capture (diagnosis): each early return, by key */
        static int on = -1;
        if (on < 0) on = hos_dbg_item("capture");
        if (on) hos_log("capture: recorder early return: key %#llx (bytes %#llx) at present %lld", (unsigned long long)key,
                        (unsigned long long)key_hash(key), (long long)hos_present_count);
    }
    return 1;   /* (as 0x8008693c: nothing recorded, no view record) */
}

/* The game's inline path of a pass, f_80681320(queue, drawctx, &key, owner, ...), from its two callers: At rows just
   before each call (a PATCH_AT declaration, runtime/patch.h) hand over the key and the list's draw context */
static void inline_pass(int site, int nk, const uint64_t *ks, uint64_t drawctx) {
    const hsd_fns *d = rx_direct();
    if (!d) return;
    {
        static int on0 = -1;
        if (on0 < 0) on0 = hos_dbg_item("capture");
        if (on0) hos_log("capture: inline site %d reached with %d keys at present %lld", site, nk, (long long)hos_present_count);
    }
    if (!nk) {
        os_atomic_inc(&n.inl_no_jobs[site]);
        return;
    }
    {
        static int on = -1;
        if (on < 0) on = hos_dbg_item("capture");
        for (int i = 0; on && i < nk; i++)
            hos_log("capture: inline pass (site %d): key %#llx at present %lld", site, (unsigned long long)ks[i], (long long)hos_present_count);
    }
    void *list = direct_cmd_list(hos_guest_range(drawctx + 216, 8) ? G64(drawctx + 216) : 0);
    if (!list) os_atomic_inc(&n.inl_no_list);
    for (int i = 0; i < nk; i++) d->pass_inline(list, ks[i]);
    os_atomic_inc(&n.inl[site]);   /* (the game's body draws the pass) */
}
/* f_806688c0 before its call (0x80668d80): x6 = [sp + 0x18] the key + 0xc0, x1 = [sp + 0x10] the list's draw context */
PATCH_AT(0x80668d7c, word(0xf90483e8), after, none, direct_passes_inline_job, as(x6, key_c0), as(x1, drawctx)) {   /* str x8, [sp, #0x900] */
    const uint64_t key = key_c0 - 0xc0;
    inline_pass(0, key_known(key), &key, drawctx);
}
/* f_8028a2a0 before its call (0x8028a588): x2 a copy of the key, x1 the draw context; keys with the same bytes (cube
   faces of two sets share them) all name this pass */
PATCH_AT(0x8028a584, word(0xaa1303e1), after, none, direct_passes_inline_key, as(x2, key_copy), as(x1, drawctx)) {   /* mov x1, x19 */
    uint64_t ks[8];
    inline_pass(1, keys_by_bytes(key_copy, ks, 8), ks, drawctx);
}

void direct_passes_log(double frames) {
    static int64_t last[8];
    const int64_t now[8] = {n.asked, n.early, n.inl[0], n.inl[1], n.inl_no_jobs[0], n.inl_no_jobs[1], n.inl_no_list, nkeys};
    hos_log("direct patch passes (f_80084de0 entry, f_80681320's calls in f_806688c0 and f_8028a2a0): a frame: recorder asked "
            "%.2f, early returns %.2f; inline passes of recorder keys %.2f (f_806688c0) %.2f (f_8028a2a0); inline passes no "
            "recorder asked for %.2f / %.2f; with no list %lld; %lld keys noted",
            (now[0] - last[0]) / frames, (now[1] - last[1]) / frames, (now[2] - last[2]) / frames, (now[3] - last[3]) / frames,
            (now[4] - last[4]) / frames, (now[5] - last[5]) / frames, (long long)now[6], (long long)now[7]);
    memcpy(last, now, sizeof last);
}
