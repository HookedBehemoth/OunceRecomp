
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long LONG;
typedef struct { int x; } os_mutex;
typedef struct { int x; } os_cond;
#define OS_MUTEX_INIT {0}
#define OS_COND_INIT {0}
static int lock_depth, lookups_locked, target_lookups, wakes;
static int fail_next_realloc;
static void *test_realloc(void *p, size_t n) {
    if (fail_next_realloc) { fail_next_realloc = 0; return NULL; }
    return realloc(p, n);
}
#define realloc test_realloc
#define os_atomic_load(p) (*(p))
static void *mw_cv_at, *mw_lock_at;   /* (set in main) */
static int mw_held;                  /* (mw_lock itself held: any sleep then must let it go) */
static void os_mutex_lock(os_mutex *l) { lock_depth++; if ((void *)l == mw_lock_at) mw_held++; }
static void os_mutex_unlock(os_mutex *l) { lock_depth--; if ((void *)l == mw_lock_at) mw_held--; }
static void os_cond_broadcast(os_cond *cv) { (void)cv; wakes++; }
static void os_cond_signal(os_cond *cv) { (void)cv; wakes++; }
typedef struct Obj {
    uint64_t key, event; int sem_max, kind; struct Obj **links; int nlinks, caplinks; struct Obj *mw;
    int64_t due_ns, sig_ns; int sem, qlen, qcap, signaled, autoclear; os_mutex lk; os_cond cv;
} Obj;
typedef unsigned long DWORD;
#define OS_FOREVER ((int64_t)-1)
#define os_atomic_inc(p) (++*(p))
#define os_atomic_dec(p) (--*(p))
#define os_atomic_add(p, v) (*(p) += (v))
static int64_t hos_now_ns(void) { static int64_t t = 1000; return t += 100000; }   /* (0.1 ms a call: timed waits end) */
/* (scheduling points and blocking waits: counted when a lock is held; a sleep there never ends in this fixture) */
static int sched_locked;
static void (*on_sleep)(void);
static int hos_cv_sleep(void *cv, void *lk, unsigned ms) {
    (void)cv; (void)lk; (void)ms;
    if (lock_depth > 1) { puts("FAIL: a sleep under mw_lock (WaitAny's scan)"); exit(1); }
    if (mw_held && lk != mw_lock_at) { puts("FAIL: a sleep with mw_lock held that does not let mw_lock go"); exit(1); }
    if (on_sleep) on_sleep();   /* (an untimed WaitAny: something gets signaled while it sleeps) */
    return 0;
}
static void hos_vcore_checkpoint(void) { if (lock_depth) sched_locked++; }
static void hos_vcore_yield(void) { if (lock_depth) sched_locked++; }
static int wake_lat_on(void) { return 1; }
static void wake_lat_note(int how, int64_t lat_ns) { (void)how; (void)lat_ns; }
enum { K_EVENT = 1, K_TIMER, K_SEMA, K_QUEUE, K_MULTIWAIT, K_HOLDER };
@HOLDER_ENUM@
static Obj objs[128];
static Obj *obj(uint64_t key, int kind) {   /* (the real one is a scheduling point; a target lookup can resurrect it) */
    if (lock_depth) lookups_locked++;
    if (key < 8) target_lookups++;
    objs[key].key = key; objs[key].kind = kind;
    return &objs[key];
}
static Obj *obj_nockpt(uint64_t key, int kind) {   /* (no checkpoint: not a scheduling point) */
    objs[key].key = key; objs[key].kind = kind;
    return &objs[key];
}
static void spin_until_set(Obj *o, volatile int *flag) { (void)o; (void)flag; if (lock_depth) sched_locked++; }
static int wait_until(Obj *o, int64_t deadline) { (void)o; (void)deadline; if (lock_depth) sched_locked++; return 0; }
typedef struct { uint64_t x[8]; } Ctx;
#define ARG(i) (c->x[i])
#define HLE(name) static void name(Ctx *c)
static os_mutex mw_lock = OS_MUTEX_INIT;
static os_cond mw_cv = OS_COND_INIT;
static volatile int32_t mw_waiting;
@FUNCTIONS@
static int bad;
static int count(uint64_t k) { int i = mw_watched_at_locked(k); return i < 0 ? 0 : mw_watched[i].n; }
static void expect(const char *step, int e1, int e2) {
    if (count(1) != e1 || count(2) != e2) {
        printf("FAIL after %s: event1 watched by %d (want %d), event2 by %d (want %d)\n", step, count(1), e1, count(2), e2);
        bad = 1;
    }
}
static int poked(uint64_t k, int waiting) { mw_waiting = waiting; wakes = 0; poke_multiwait(&objs[k]); return wakes; }
static void call(void (*f)(Ctx *), uint64_t a, uint64_t b) { Ctx c = {{a, b}}; f(&c); }
int main(void) {
    mw_cv_at = &mw_cv, mw_lock_at = &mw_lock;
    const uint64_t A = 10, B = 11, h1 = 20, h2 = 21, h3 = 22, h4 = 23;
    objs[1].key = 1; objs[2].key = 2;
    objs[h1].event = 1; objs[h1].sem_max = H_EVENT;   /* (two holders over event 1, one over event 2, one over a thread) */
    objs[h2].event = 1; objs[h2].sem_max = H_EVENT;
    objs[h3].event = 2; objs[h3].sem_max = H_EVENT;
    objs[h4].event = 3; objs[h4].sem_max = H_THREAD;
    if (poked(1, 1)) { puts("FAIL: an unwatched change woke WaitAny"); bad = 1; }
    for (uint64_t h = h1; h <= h4; h++) call(LINK, A, h);
    expect("four links", 2, 1);
    if (poked(1, 1) != 1 || poked(2, 1) != 1) { puts("FAIL: a watched change did not wake WaitAny"); bad = 1; }
    if (poked(5, 1)) { puts("FAIL: an unwatched change woke WaitAny with holders linked"); bad = 1; }
    if (poked(1, 0)) { puts("FAIL: a wake with no WaitAny waiting"); bad = 1; }
    call(UNLINK, h2, 0);
    expect("unlink one", 1, 1);
    call(UNLINK, h2, 0);
    expect("unlink it again", 1, 1);
    memset(&objs[1], 0, sizeof objs[1]);   /* (event 1 initialized again, as fresh() does, its holder still linked) */
    objs[1].key = 1;
    if (poked(1, 1) != 1) { puts("FAIL: a re-initialized watched event no longer wakes WaitAny"); bad = 1; }
    call(MOVEALL, B, A);
    expect("move all", 1, 1);
    if (objs[h1].mw != &objs[B] || objs[A].nlinks) { puts("FAIL: move all did not move the holders"); bad = 1; }
    objs[2].kind = 0;   /* (event 2 finalized before its holder is unlinked) */
    call(UNLINKALL, B, 0);
    expect("unlink all", 0, 0);
    if (objs[2].kind) { puts("FAIL: unlink brought a finalized event back"); bad = 1; }
    call(LINK, A, h1);
    call(UNLINKALL, B, 0);
    expect("link again, unlink all of the other", 1, 0);
    call(UNLINK, h1, 0);
    expect("unlink the last", 0, 0);
    if (mw_nwatched) { printf("FAIL: %d addresses still watched\n", mw_nwatched); bad = 1; }
    /* Keep the holder array allocated so the injected failure hits the watched-address table. */
    objs[40].links = (Obj **)realloc(NULL, 32 * sizeof *objs[40].links); objs[40].caplinks = 32;
    for (uint64_t k = 1; k <= 17; k++) {
        objs[k].key = k; objs[64 + k].event = k; objs[64 + k].sem_max = H_EVENT;
        if (k <= 16) call(LINK, 40, 64 + k);
    }
    fail_next_realloc = 1;
    call(LINK, 40, 81);
    {   /* WaitAny's scan holds mw_lock: a lookup there that is a scheduling point (obj: a vcore checkpoint) can give the
           core up while the lock is held, and poke_multiwait's signallers, holding cores, then block on mw_lock until the
           250 ms vcore escape. TryWaitAny over a holder of every kind holder_ready looks up */
        const int before = lookups_locked;
        const int kinds[] = {H_EVENT, H_TIMER, H_SEMA, H_QUEUE_NOT_EMPTY, H_QUEUE_NOT_FULL};
        for (int k = 0; k < 5; k++) {
            objs[100 + k].event = 110 + k; objs[100 + k].sem_max = kinds[k];
            call(LINK, 42, 100 + k);
        }
        objs[111].due_ns = (int64_t)1 << 60;   /* (a timer not yet due: the scan reads its deadline too) */
        /* (every state of event_wait's try path the scan can meet: the event manual or auto-clear, unset (try, 5 ms timed,
           and untimed until it is set during a sleep) and set (try); mw_lock is held throughout each scan) */
        for (int ac = 0; ac < 2; ac++) {
            objs[110].signaled = 0, objs[110].autoclear = ac;
            if (wait_any(42, 0)) { puts("FAIL: TryWaitAny found a holder ready when none was"); bad = 1; }
            if (wait_any(42, 5000000)) { puts("FAIL: a timed WaitAny found a holder ready when none was"); bad = 1; }
            static int sleeps;
            sleeps = 0;
            on_sleep = [] { if (++sleeps == 3) objs[110].signaled = 1; };
            if (wait_any(42, -1) != 100 || sleeps < 3) { puts("FAIL: an untimed WaitAny did not return the event set while it slept"); bad = 1; }
            on_sleep = nullptr;
            objs[110].signaled = 1;
            if (wait_any(42, 0) != 100) { puts("FAIL: TryWaitAny missed the set event's holder"); bad = 1; }
            if (objs[110].signaled != !ac) { puts("FAIL: an auto-clear event not cleared by TryWaitAny"); bad = 1; }
        }
        objs[110].signaled = 0, objs[110].autoclear = 0;
        if (sched_locked) { printf("FAIL: WaitAny's scan reached %d spins, timed waits or vcore calls under mw_lock\n", sched_locked); bad = 1; }
        if (lookups_locked != before) {
            printf("FAIL: WaitAny's scan made %d lookups that are scheduling points under mw_lock\n", lookups_locked - before);
            bad = 1;
        }
        call(UNLINKALL, 42, 0);
    }
    if (fail_next_realloc || poked(17, 1) != 1) { puts("FAIL: failed watch registration lost a wake"); bad = 1; }
    call(UNLINK, 65, 0);   /* free a table slot, then link a second holder over address 17 */
    objs[82].event = 17; objs[82].sem_max = H_EVENT;
    call(LINK, 40, 82);
    call(UNLINK, 81, 0);
    if (poked(17, 1) != 1) { puts("FAIL: unlink of an uncounted holder hid another live holder"); bad = 1; }
    call(UNLINKALL, 40, 0);
    if (mw_nwatched) { puts("FAIL: watches remain after OOM-case cleanup"); bad = 1; }
    if (target_lookups) { printf("FAIL: %d lookups of a watched object (link, unlink, poke look up none)\n", target_lookups); bad = 1; }
    if (lookups_locked) { printf("FAIL: %d lookups (scheduling points) under mw_lock\n", lookups_locked); bad = 1; }
    puts(bad ? "FAILED" : "ok");
    return bad;
}
