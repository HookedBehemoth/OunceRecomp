/* Direct mode only: a render job is recorded only when its pass's target formats are known.

   render_job_record (f_800802e0) is replaced by eng_jobs.c's PATCH_FUNC; in direct mode it asks here first. Its pass key
   (x3) is the pass's identity. If the back end's format table does not know the key, the recorder returns at entry, as
   its own early return does (no view record): pass_job_caller (f_8034c860) then finds no record with the key's CRC and
   tail-calls the game's inline path pass_draw_inline (f_8036425c), which sets its own targets. An At row before that
   tail call names the pass's key to the back end, which takes the formats from the targets the list sets
   (gpu_pass_inline). A key's first frame therefore draws inline, the frames after it as jobs.

   The game matches records by the CRC of the key's first 0x70 bytes, so the recorder's keys are noted here by those
   bytes and the inline key names every recorder key with the same bytes. An inline pass no recorder ever asked for is
   the game's own inline pass, left alone.

   Check that it applies: the 600-present line "direct patch passes" (debug.logpatch) and the back end's "jobs a
   frame" line. */
#include "patches.h"
#include <string.h>
#include "os.h"

static struct {
    volatile int64_t asked, early, inl, inl_no_jobs, inl_no_list;
} n;

/* the recorder's keys by the hash of their first 0x70 bytes (a few dozen; written by the recorders, read at the inline
   path) */
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
/* the recorder's pointers for the key bytes at a; n of them in out */
static int keys_by_bytes(uint64_t a, uint64_t *out, int cap) {
    const uint64_t h = key_hash(a);
    int k = 0;
    os_rwlock_lock_shared(&keys_lock);
    for (int i = 0; i < nkeys && k < cap; i++)
        if (keys[i].hash == h) out[k++] = keys[i].key;
    os_rwlock_unlock_shared(&keys_lock);
    return k;
}

int passes_recorder(uint64_t key) {
    const gpu_fns *d = rx_direct();
    if (!d) return 0;
    key_note(key);
    os_atomic_inc(&n.asked);
    if (d->pass_known(key)) return 0;
    os_atomic_inc(&n.early);
    return 1;
}

/* pass_job_caller before its tail call of pass_draw_inline (ctx, drawctx x1, key x2, job, 0) */
PATCH_AT(0x8034cd88, word(0x2a1f03e4), before, none, passes_inline, as(x1, drawctx), as(x2, key)) {   /* mov w4, wzr */
    const gpu_fns *d = rx_direct();
    if (!d) return;
    uint64_t ks[8];
    const int nk = keys_by_bytes(key, ks, 8);
    if (!nk) {
        os_atomic_inc(&n.inl_no_jobs);
        return;
    }
    void *list = cmd_list(hos_guest_range(drawctx + 0xd8, 8) ? G64(drawctx + 0xd8) : 0);
    if (!list) os_atomic_inc(&n.inl_no_list);
    for (int i = 0; i < nk; i++) d->pass_inline(list, ks[i]);
    os_atomic_inc(&n.inl);
}

void passes_log(double frames) {
    static int64_t last[6];
    const int64_t now[6] = {n.asked, n.early, n.inl, n.inl_no_jobs, n.inl_no_list, nkeys};
    hos_log("direct patch passes (render_job_record's entry, pass_job_caller's inline tail call): a frame: recorder asked "
            "%.2f, early returns %.2f; inline passes of recorder keys %.2f, inline passes no recorder asked for %.2f; with "
            "no list %lld; %lld keys noted",
            (now[0] - last[0]) / frames, (now[1] - last[1]) / frames, (now[2] - last[2]) / frames,
            (now[3] - last[3]) / frames, (long long)now[4], (long long)now[5]);
    memcpy(last, now, sizeof last);
}
