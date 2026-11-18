
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <emmintrin.h>
typedef long LONG;
typedef struct { pthread_mutex_t m; } os_mutex;
typedef struct { pthread_cond_t c; } os_cond;
typedef struct HosThread HosThread;
typedef struct { uint64_t x[31]; } Ctx;
static void os_mutex_lock(os_mutex *l) { pthread_mutex_lock(&l->m); }
static void os_mutex_unlock(os_mutex *l) { pthread_mutex_unlock(&l->m); }
static void os_cond_signal(os_cond *c) { pthread_cond_signal(&c->c); }
static void os_cond_broadcast(os_cond *c) { pthread_cond_broadcast(&c->c); }
static void crit_wake(void *p) { (void)p; }
#define os_atomic_inc(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
static int64_t hos_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static int hos_cv_sleep(void *cv, void *lk, unsigned ms) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += (long)ms * 1000000L; t.tv_sec += t.tv_nsec / 1000000000L; t.tv_nsec %= 1000000000L;
    return pthread_cond_timedwait(&((os_cond *)cv)->c, &((os_mutex *)lk)->m, &t) == 0;
}
static HosThread *hos_self(void) { return NULL; }
static const char *hos_thread_name(HosThread *t) { (void)t; return "?"; }
static int logs;
static void hos_log(const char *f, ...) { (void)f; logs++; }
static void hos_fatal(Ctx *c, const char *f, ...) { (void)c; puts(f); exit(1); }
static uint8_t guest[4096] __attribute__((aligned(64)));
#define GP(a) ((void *)(guest + (uint32_t)(a)))
enum { K_MUTEX = 1 };
typedef struct Obj { uint64_t key; int kind; os_mutex lk; os_cond cv; int count, mwait, reserved;
                     int spin_misses, spin_waits; } Obj;
static Obj objs[16];
static Obj *obj_nockpt(uint64_t key, int kind) { Obj *o = &objs[key / 64]; o->key = key; o->kind = kind; return o; }
static int64_t spin_ns(void) { return 2000; }
static int spin_begin(Obj *o, int *misses) { *misses = o->spin_misses; return 1; }
static void spin_end(Obj *o, int misses, int ok) { (void)o; (void)misses; (void)ok; }
@SECTION@
enum { THREADS = 8, ROUNDS = 200000, MUTEXES = 4 };
static long counters[MUTEXES];
static void *worker(void *arg) {
    unsigned r = (unsigned)(uintptr_t)arg * 2654435761u;
    for (int i = 0; i < ROUNDS; i++) {
        r = r * 1103515245u + 12345u;
        const uint64_t key = 64 * (1 + (r >> 8) % MUTEXES);
        mx_lock(key, NULL);
        if (r & 0x10000) mx_lock(key, NULL);   /* (again by its owner) */
        counters[key / 64 - 1]++;
        if (r & 0x10000) mx_unlock(key);
        mx_unlock(key);
    }
    return NULL;
}
static volatile int stage;
static void *owed_waiter(void *arg) {   /* (a condition variable's waiter the signal reserved the mutex for) */
    (void)arg;
    while (stage < 1) _mm_pause();
    mx_relock(obj_nockpt(64 * 5, K_MUTEX), 1, 1);
    stage = 2;
    mx_unlock(64 * 5);
    return NULL;
}
int main(void) {
    for (int i = 0; i < 16; i++) {
        pthread_mutex_init(&objs[i].lk.m, NULL);
        pthread_cond_init(&objs[i].cv.c, NULL);
    }
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    long total = 0;
    for (int i = 0; i < MUTEXES; i++) total += counters[i];
    if (total != (long)THREADS * ROUNDS) { printf("FAIL: %ld increments under the mutexes, want %ld\n", total, (long)THREADS * ROUNDS); return 1; }
    for (int i = 1; i <= MUTEXES; i++)
        if (*mx_word(64 * i) & ~MX_SLOW) { printf("FAIL: mutex %d left held (%#x)\n", i, *mx_word(64 * i)); return 1; }

    /* counts past the field's 2047 */
    const uint64_t deep = 64 * 6;
    for (int i = 0; i < 3000; i++) mx_lock(deep, NULL);
    for (int i = 0; i < 2999; i++) mx_unlock(deep);
    if (!(*mx_word(deep) & MX_CNT)) { puts("FAIL: a mutex locked 3000 times was free after 2999 unlocks"); return 1; }
    mx_unlock(deep);
    if (*mx_word(deep)) { printf("FAIL: a mutex locked and unlocked 3000 times is %#x\n", *mx_word(deep)); return 1; }

    /* reserved for a woken waiter: a fast lock and a try by another thread both fail; the waiter takes it */
    const uint64_t res = 64 * 5;
    mx_lock(res, NULL);
    pthread_t w;
    pthread_create(&w, NULL, owed_waiter, NULL);
    Obj *m = obj_nockpt(res, K_MUTEX);
    mx_reserve(m);
    mx_give(res, mx_word(res));   /* (the signaller's unlock) */
    uint32_t v = 0;
    if (__atomic_compare_exchange_n(mx_word(res), &v, 99 | MX_ONE, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        puts("FAIL: a fast lock took a mutex owed to a woken waiter"); return 1;
    }
    if (mx_take(m, mx_word(res), mx_id(), 0)) { puts("FAIL: a slow lock took a mutex owed to a woken waiter"); return 1; }
    stage = 1;
    pthread_join(w, NULL);
    if (stage != 2 || m->reserved) { puts("FAIL: the owed waiter did not take its mutex"); return 1; }
    mx_lock(res, NULL);   /* (and after its unlock, anyone) */
    mx_unlock(res);
    if (*mx_word(res)) { printf("FAIL: the reserved mutex is %#x at the end, want 0\n", *mx_word(res)); return 1; }

    /* a word naming no thread (memory never initialized as a mutex) is taken as free */
    const uint64_t junk = 64 * 7;
    *mx_word(junk) = 0x00012345u | (3u << 20);
    mx_lock(junk, NULL);
    if ((*mx_word(junk) & MX_ID) != mx_id()) { puts("FAIL: a word naming no thread was not taken"); return 1; }
    mx_unlock(junk);
    printf("ok: %d threads x %d recursive lock pairs on %d mutexes\n", THREADS, ROUNDS, MUTEXES);
    return 0;
}
