/* os.h on Linux (glibc). Depends on nothing of Hookshot's: libc, the kernel's futexes, and libdl. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static int err_of(int e) {
    switch (e) {
    case 0: return OS_OK;
    case ENOENT: case ENOTDIR: return OS_ENOENT;
    case EEXIST: return OS_EEXIST;
    case EACCES: case EPERM: case EISDIR: case EROFS: return OS_EACCES;
    case ENOMEM: return OS_ENOMEM;
    case ETIMEDOUT: return OS_ETIMEDOUT;
    case ENOTEMPTY: return OS_ENOTEMPTY;
    case EINVAL: return OS_EINVAL;
    default: return OS_EFAIL;
    }
}

/* a path with '\' made '/' */

/* ---- time ---- */
int64_t os_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static struct timespec ts_of(int64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    return ts;
}
void os_sleep_until_ns(int64_t deadline) {
    const struct timespec ts = ts_of(deadline);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
void os_sleep_ns(int64_t ns) {
    if (ns <= 0) { sched_yield(); return; }
    os_sleep_until_ns(os_now_ns() + ns);
}
void os_yield(void) { sched_yield(); }

/* The kernel's timer slack (50 us by default) is added to every sleep's and every timed wait's end: the game's short
   sleeps (a 100 us poll) would take half again as long. Per thread, and inherited by the threads a thread starts:
   set for the main thread here, before anything starts one. */
__attribute__((constructor)) static void precise_timers(void) { prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0); }

/* ---- futexes ---- */
static long futex(volatile uint32_t *w, int op, uint32_t v, const struct timespec *ts, uint32_t v3) {
    return syscall(SYS_futex, w, op, v, ts, NULL, v3);
}
/* sleeps while *w == expected, until the monotonic deadline (< 0: none): 0 or OS_ETIMEDOUT */
static int wait_until(volatile uint32_t *w, uint32_t expected, int64_t deadline) {
    struct timespec ts;
    if (deadline >= 0) ts = ts_of(deadline);
    const long r = futex(w, FUTEX_WAIT_BITSET_PRIVATE, expected, deadline >= 0 ? &ts : NULL, FUTEX_BITSET_MATCH_ANY);
    return r == -1 && errno == ETIMEDOUT ? OS_ETIMEDOUT : 0;
}
static int64_t deadline_of(int64_t timeout_ns) { return timeout_ns < 0 ? -1 : os_now_ns() + timeout_ns; }
static int past(int64_t deadline) { return deadline >= 0 && os_now_ns() >= deadline; }
int os_futex_wait(volatile uint32_t *w, uint32_t expected, int64_t timeout_ns) {
    return wait_until(w, expected, deadline_of(timeout_ns));
}
void os_futex_wake(volatile uint32_t *w, int n) { futex(w, FUTEX_WAKE_PRIVATE, (uint32_t)n, NULL, 0); }

/* ---- mutex: Drepper's three states ("Futexes Are Tricky", mutex3), with a short spin before sleeping ---- */
enum { SPINS = 100 };
void os_mutex_lock(os_mutex *m) {
    uint32_t c = os_atomic_cas(&m->state, 0u, 1u);
    if (!c) return;
    for (int i = 0; i < SPINS; i++) {
        os_pause();
        if (!__atomic_load_n(&m->state, __ATOMIC_RELAXED) && !(c = os_atomic_cas(&m->state, 0u, 1u))) return;
    }
    if (c != 2) c = os_atomic_xchg(&m->state, 2u);
    while (c) {
        wait_until(&m->state, 2, -1);
        c = os_atomic_xchg(&m->state, 2u);
    }
}
bool os_mutex_trylock(os_mutex *m) { return os_atomic_cas(&m->state, 0u, 1u) == 0; }
void os_mutex_unlock(os_mutex *m) {
    if (os_atomic_fetch_add(&m->state, (uint32_t)-1) != 1) {
        os_atomic_store(&m->state, 0u);
        os_futex_wake(&m->state, 1);
    }
}

/* ---- reader/writer lock: state = readers | RW_WRITER | RW_SLEEPERS (someone sleeps on the word) ---- */
#define RW_WRITER 0x40000000u
#define RW_SLEEPERS 0x80000000u
#define RW_READERS 0x3fffffffu
void os_rwlock_lock_shared(os_rwlock *l) {
    int spins = SPINS;
    for (;;) {
        uint32_t s = os_atomic_load(&l->state);
        if (!(s & RW_WRITER)) {
            if (os_atomic_cas(&l->state, s, s + 1) == s) return;
            continue;
        }
        if (spins-- > 0) { os_pause(); continue; }
        if (!(s & RW_SLEEPERS)) {
            if (os_atomic_cas(&l->state, s, s | RW_SLEEPERS) != s) continue;
            s |= RW_SLEEPERS;
        }
        wait_until(&l->state, s, -1);
    }
}
bool os_rwlock_trylock_shared(os_rwlock *l) {
    for (;;) {
        const uint32_t s = os_atomic_load(&l->state);
        if (s & RW_WRITER) return false;
        if (os_atomic_cas(&l->state, s, s + 1) == s) return true;
    }
}
void os_rwlock_unlock_shared(os_rwlock *l) {
    const uint32_t s = os_atomic_fetch_add(&l->state, (uint32_t)-1);
    /* (the last reader out, with sleepers: they are writers, or readers that saw a writer since gone) */
    if ((s & RW_READERS) == 1 && (s & RW_SLEEPERS) && os_atomic_cas(&l->state, RW_SLEEPERS, 0u) == RW_SLEEPERS)
        os_futex_wake(&l->state, OS_WAKE_ALL);
}
void os_rwlock_lock(os_rwlock *l) {
    int spins = SPINS;
    for (;;) {
        uint32_t s = os_atomic_load(&l->state);
        if (!(s & ~RW_SLEEPERS)) {
            if (os_atomic_cas(&l->state, s, s | RW_WRITER) == s) return;
            continue;
        }
        if (spins-- > 0) { os_pause(); continue; }
        if (!(s & RW_SLEEPERS)) {
            if (os_atomic_cas(&l->state, s, s | RW_SLEEPERS) != s) continue;
            s |= RW_SLEEPERS;
        }
        wait_until(&l->state, s, -1);
    }
}
bool os_rwlock_trylock(os_rwlock *l) {
    const uint32_t s = os_atomic_load(&l->state);
    return !(s & ~RW_SLEEPERS) && os_atomic_cas(&l->state, s, s | RW_WRITER) == s;
}
void os_rwlock_unlock(os_rwlock *l) {
    if (os_atomic_fetch_and(&l->state, ~(RW_WRITER | RW_SLEEPERS)) & RW_SLEEPERS) os_futex_wake(&l->state, OS_WAKE_ALL);
}

/* ---- condition variable: a sequence word, woken only when its waiter count says someone waits. A waiter counts
   itself and reads the sequence before it lets the lock go, so a signal made under the lock (or after a change made
   under it) sees the waiter and moves the sequence past the value it sleeps on. ---- */
static int cond_wait(os_cond *c, void (*unlock)(void *), void (*relock)(void *), void *l, int64_t timeout_ns) {
    os_atomic_inc(&c->waiters);
    const uint32_t seq = os_atomic_load(&c->seq);
    unlock(l);
    const int r = wait_until(&c->seq, seq, deadline_of(timeout_ns));
    relock(l);
    os_atomic_dec(&c->waiters);
    return r;
}
static void mutex_unlock_(void *l) { os_mutex_unlock((os_mutex *)l); }
static void mutex_lock_(void *l) { os_mutex_lock((os_mutex *)l); }
static void rw_unlock_(void *l) { os_rwlock_unlock((os_rwlock *)l); }
static void rw_lock_(void *l) { os_rwlock_lock((os_rwlock *)l); }
int os_cond_wait(os_cond *c, os_mutex *m, int64_t timeout_ns) { return cond_wait(c, mutex_unlock_, mutex_lock_, m, timeout_ns); }
int os_cond_wait_rw(os_cond *c, os_rwlock *l, int64_t timeout_ns) { return cond_wait(c, rw_unlock_, rw_lock_, l, timeout_ns); }
void os_cond_signal(os_cond *c) {
    if (!os_atomic_load(&c->waiters)) return;
    os_atomic_inc(&c->seq);
    os_futex_wake(&c->seq, 1);
}
void os_cond_broadcast(os_cond *c) {
    if (!os_atomic_load(&c->waiters)) return;
    os_atomic_inc(&c->seq);
    os_futex_wake(&c->seq, OS_WAKE_ALL);
}

/* ---- event: state 1 set; a waiter counts itself before it looks, a setter sets before it looks for waiters (both
   sequentially consistent: one of the two sees the other) ---- */
/* ---- once: state 0 not run, 1 running, 2 running with sleepers, 3 done ---- */
void os_once_run(os_once *o, void (*fn)(void *arg), void *arg) {
    uint32_t s = os_atomic_load(&o->state);
    if (s == 3) return;
    if (s == 0 && os_atomic_cas(&o->state, 0u, 1u) == 0) {
        fn(arg);
        if (os_atomic_xchg(&o->state, 3u) == 2) futex(&o->state, FUTEX_WAKE_PRIVATE, OS_WAKE_ALL, NULL, 0);
        return;
    }
    while ((s = os_atomic_load(&o->state)) != 3) {
        if (s == 1 && os_atomic_cas(&o->state, 1u, 2u) != 1) continue;
        futex(&o->state, FUTEX_WAIT_PRIVATE, 2, NULL, 0);
    }
}

void os_event_init(os_event *e, bool manual, bool set) {
    e->manual = manual;
    e->waiters = 0;
    os_atomic_store(&e->state, set ? 1u : 0u);
}
void os_event_set(os_event *e) {
    os_atomic_store(&e->state, 1u);
    if (os_atomic_load(&e->waiters)) os_futex_wake(&e->state, e->manual ? OS_WAKE_ALL : 1);
}
void os_event_reset(os_event *e) { os_atomic_store(&e->state, 0u); }
bool os_event_try(os_event *e) {
    if (e->manual) return os_atomic_load(&e->state) != 0;
    return os_atomic_load(&e->state) && os_atomic_cas(&e->state, 1u, 0u) == 1;
}
int os_event_wait(os_event *e, int64_t timeout_ns) {
    if (os_event_try(e)) return 0;
    if (!timeout_ns) return OS_ETIMEDOUT;
    const int64_t deadline = deadline_of(timeout_ns);
    os_atomic_inc(&e->waiters);
    int r = 0;
    while (!os_event_try(e)) {
        if (wait_until(&e->state, 0, deadline) == OS_ETIMEDOUT || past(deadline)) {
            r = os_event_try(e) ? 0 : OS_ETIMEDOUT;
            break;
        }
    }
    os_atomic_dec(&e->waiters);
    return r;
}

/* ---- semaphore ---- */
void os_sema_init(os_sema *s, uint32_t count) { s->waiters = 0; os_atomic_store(&s->count, count); }
void os_sema_post(os_sema *s, uint32_t n) {
    os_atomic_add(&s->count, n);
    if (os_atomic_load(&s->waiters)) os_futex_wake(&s->count, n >= OS_WAKE_ALL ? OS_WAKE_ALL : (int)n);
}
static bool sema_take(os_sema *s) {
    for (uint32_t c = os_atomic_load(&s->count); c;) {
        const uint32_t was = os_atomic_cas(&s->count, c, c - 1);
        if (was == c) return true;
        c = was;
    }
    return false;
}
int os_sema_wait(os_sema *s, int64_t timeout_ns) {
    if (sema_take(s)) return 0;
    if (!timeout_ns) return OS_ETIMEDOUT;
    const int64_t deadline = deadline_of(timeout_ns);
    os_atomic_inc(&s->waiters);
    int r = 0;
    while (!sema_take(s)) {
        if (wait_until(&s->count, 0, deadline) == OS_ETIMEDOUT || past(deadline)) {
            r = sema_take(s) ? 0 : OS_ETIMEDOUT;
            break;
        }
    }
    os_atomic_dec(&s->waiters);
    return r;
}

/* ---- threads ---- */
struct os_thread {
    pthread_t th;
    volatile uint32_t tid;
    os_event done;   /* (manual: set when the thread ends) */
    volatile int refs;
    int priority;
    os_thread_fn fn;
    void *arg;
    char name[64];
};
static __thread os_thread *self_thread;
static __thread uint32_t self_tid;
uint32_t os_tid(void) {
    if (!self_tid) self_tid = (uint32_t)syscall(SYS_gettid);
    return self_tid;
}
uint32_t os_pid(void) { return (uint32_t)getpid(); }
void os_thread_retain(os_thread *t) { os_atomic_inc(&t->refs); }
/* Threads this file starts are joinable, so their pthread descriptor (and stack) stays until the last reference goes:
   a detached thread's could be unmapped while a handle still names it (set_affinity, set_name, cpu_ns use t->th). */
void os_thread_release(os_thread *t) {
    if (!t || os_atomic_dec(&t->refs) != 0) return;
    if (t->fn) {   /* (one we started; a foreign thread's descriptor is not ours) */
        if (pthread_equal(t->th, pthread_self())) pthread_detach(t->th);
        else pthread_join(t->th, NULL);
    }
    free(t);
}
/* an alternate signal stack for the calling thread, so a stack overflow (a guard page hit) still reaches the fault
   handlers: the stack given, or NULL when the thread has one already */
#define ALT_STACK_SIZE (64u << 10)
static void *alt_stack_install(void) {
    stack_t old;
    if (sigaltstack(NULL, &old) == 0 && !(old.ss_flags & SS_DISABLE)) return NULL;
    stack_t ss = {0};
    ss.ss_sp = malloc(ALT_STACK_SIZE);
    ss.ss_size = ALT_STACK_SIZE;
    if (!ss.ss_sp || sigaltstack(&ss, NULL)) { free(ss.ss_sp); return NULL; }
    return ss.ss_sp;
}
static void alt_stack_remove(void *sp) {
    if (!sp) return;
    stack_t ss = {0};
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
    free(sp);
}
os_thread *os_thread_current(void) {
    if (!self_thread) {   /* (a thread this file did not start: the main thread, a library's) */
        alt_stack_install();   /* (kept for the thread's life) */
        os_thread *t = (os_thread *)calloc(1, sizeof *t);
        os_event_init(&t->done, true, false);
        t->th = pthread_self();
        t->tid = os_tid();
        t->refs = 1;   /* (the thread's own, never given back: the handle outlives it) */
        self_thread = t;
    }
    return self_thread;
}
static void thread_finish(void) {
    os_thread *t = self_thread;
    if (!t) return;
    os_event_set(&t->done);
    self_thread = NULL;
    os_thread_release(t);
}
static void *thread_entry(void *p) {
    os_thread *t = (os_thread *)p;
    self_thread = t;
    t->tid = os_tid();
    if (t->name[0]) {
        char shortname[16];   /* (the kernel keeps 15 characters) */
        snprintf(shortname, sizeof shortname, "%s", t->name);
        pthread_setname_np(pthread_self(), shortname);
    }
    void *alt = alt_stack_install();
    t->fn(t->arg);
    alt_stack_remove(alt);
    thread_finish();
    return NULL;
}
os_thread *os_thread_start(os_thread_fn fn, void *arg, size_t stack, const char *name) {
    os_thread *t = (os_thread *)calloc(1, sizeof *t);
    if (!t) return NULL;
    os_event_init(&t->done, true, false);
    t->fn = fn, t->arg = arg, t->refs = 2;   /* (the handle's and the thread's own) */
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack ? (stack + 0xFFFF) & ~(size_t)0xFFFF : (size_t)8 << 20);
    const int r = pthread_create(&t->th, &a, thread_entry, t);
    pthread_attr_destroy(&a);
    if (r) { free(t); return NULL; }
    return t;
}
int os_thread_join(os_thread *t, int64_t timeout_ns) { return os_event_wait(&t->done, timeout_ns); }
uint32_t os_thread_tid(os_thread *t) {
    while (!os_atomic_load(&t->tid) && !os_event_try(&t->done)) sched_yield();   /* (until it has started) */
    return t->tid;
}
void os_thread_set_name(os_thread *t, const char *name) {
    snprintf(t->name, sizeof t->name, "%s", name);
    char shortname[16];
    snprintf(shortname, sizeof shortname, "%s", name);
    if (os_thread_tid(t) && !os_event_try(&t->done)) pthread_setname_np(t->th, shortname);
}
void os_thread_name(os_thread *t, char *out, size_t cap) {
    if (!cap) return;
    snprintf(out, cap, "%s", t->name);
    if (!t->name[0] && cap >= 16) pthread_getname_np(t->th, out, cap);   /* (a foreign thread: the name the kernel has) */
}
/* (nice values; normal and above are nice 0: an unprivileged process cannot go below it, and taking a lowering back
   needs RLIMIT_NICE or CAP_SYS_NICE) */
bool os_thread_set_priority(os_thread *t, int prio) {
    const int nice = prio <= OS_PRIO_IDLE ? 19 : prio <= OS_PRIO_LOWEST ? 10 : prio == OS_PRIO_BELOW ? 5 : 0;
    const uint32_t tid = os_thread_tid(t);
    if (!tid || setpriority(PRIO_PROCESS, (id_t)tid, nice)) return false;
    t->priority = prio;
    return true;
}
int os_thread_priority(os_thread *t) { return t->priority; }
uint64_t os_thread_set_affinity(os_thread *t, uint64_t mask) {
    if (!mask) return 0;
    cpu_set_t old, want;
    CPU_ZERO(&old);
    pthread_getaffinity_np(t->th, sizeof old, &old);
    CPU_ZERO(&want);
    for (int i = 0; i < 64; i++) if (mask >> i & 1) CPU_SET(i, &want);
    if (pthread_setaffinity_np(t->th, sizeof want, &want)) return 0;
    uint64_t r = 0;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &old)) r |= (uint64_t)1 << i;
    return r ? r : 1;
}
int64_t os_thread_cpu_ns(os_thread *t) {
    clockid_t clk;
    struct timespec ts;
    if (os_event_try(&t->done) || pthread_getcpuclockid(t->th, &clk) || clock_gettime(clk, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
void os_thread_exit(void) {
    thread_finish();
    pthread_exit(NULL);
}
void os_exit(int code) {
    fflush(NULL);
    _exit(code);
}

/* ---- processors ---- */
int os_cpu_count(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }
/* The process's mask as it started, taken before main (Linux keeps no process mask apart from its threads': read
   later from a thread that pinned itself, it would be that thread's) */
static uint64_t process_affinity;
__attribute__((constructor)) static void init_process_affinity(void) {
    cpu_set_t s;
    CPU_ZERO(&s);
    if (sched_getaffinity(0, sizeof s, &s) == 0)
        for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &s)) process_affinity |= (uint64_t)1 << i;
}
uint64_t os_process_affinity(void) {
    if (!process_affinity) init_process_affinity();
    return process_affinity;
}
static int read_int(const char *path) {
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}
int os_cpu_cores(uint64_t *masks, int cap) {
    long cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (cpus > 64) cpus = 64;
    int keys[64][2], n = 0;
    uint64_t m[64];
    for (int i = 0; i < cpus; i++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
        int core = read_int(p);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
        int pkg = read_int(p);
        if (core < 0) core = i, pkg = 0;
        int k = 0;
        while (k < n && (keys[k][0] != pkg || keys[k][1] != core)) k++;
        if (k == n) keys[n][0] = pkg, keys[n][1] = core, m[n] = 0, n++;
        m[k] |= (uint64_t)1 << i;
    }
    for (int k = 0; k < n && k < cap; k++) masks[k] = m[k];
    return n;
}

/* ---- fibers: ucontext ---- */
struct os_fiber {
    ucontext_t uc;
    void *arg, *stack;
    size_t stack_size;
    os_fiber_fn fn;
};
static __thread os_fiber *cur_fiber;
os_fiber *os_fiber_from_thread(void) {
    if (cur_fiber) return cur_fiber;
    os_fiber *f = (os_fiber *)calloc(1, sizeof *f);
    cur_fiber = f;
    return f;
}
static void fiber_start(unsigned lo, unsigned hi) {
    os_fiber *f = (os_fiber *)(uintptr_t)((uint64_t)hi << 32 | lo);
    f->fn(f->arg);
    os_thread_exit();
}
os_fiber *os_fiber_create(size_t stack, os_fiber_fn fn, void *arg) {
    size_t size = stack < ((size_t)1 << 20) ? (size_t)1 << 20 : stack;
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    mprotect(mem, 4096, PROT_NONE);   /* (a guard page at the bottom) */
    os_fiber *f = (os_fiber *)calloc(1, sizeof *f);
    f->arg = arg, f->fn = fn, f->stack = mem, f->stack_size = size;
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = mem;
    f->uc.uc_stack.ss_size = size;
    f->uc.uc_link = NULL;
    const uint64_t p = (uint64_t)(uintptr_t)f;
    makecontext(&f->uc, (void (*)(void))fiber_start, 2, (unsigned)p, (unsigned)(p >> 32));
    return f;
}
void os_fiber_switch(os_fiber *to) {
    os_fiber *from = cur_fiber;
    if (!from || from == to) return;
    cur_fiber = to;
    swapcontext(&from->uc, &to->uc);
}
void os_fiber_delete(os_fiber *f) {
    if (!f) return;
    if (f == cur_fiber) os_thread_exit();
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
}
os_fiber *os_fiber_current(void) { return cur_fiber; }

/* ---- memory ---- */
size_t os_page_size(void) {
    static size_t page;
    if (!page) page = (size_t)sysconf(_SC_PAGESIZE);
    return page;
}
static int prot_of(int p) {
    return (p & OS_PROT_R ? PROT_READ : 0) | (p & OS_PROT_W ? PROT_WRITE | PROT_READ : 0) | (p & OS_PROT_X ? PROT_EXEC : 0);
}
static void page_span(const void *addr, size_t size, uintptr_t *lo, uintptr_t *hi) {
    const uintptr_t page = os_page_size();
    *lo = (uintptr_t)addr & ~(page - 1);
    *hi = ((uintptr_t)addr + size + page - 1) & ~(page - 1);
}
void *os_mem_map(void *addr, size_t size, int prot) {
    void *p = mmap(addr, size, prot_of(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (addr && p != addr) { munmap(p, size); return NULL; }
    return p;
}
int os_mem_unmap(void *addr, size_t size) { return munmap(addr, size) ? err_of(errno) : 0; }
int os_mem_protect(void *addr, size_t size, int prot) {
    uintptr_t lo, hi;
    page_span(addr, size, &lo, &hi);
    return mprotect((void *)lo, hi - lo, prot_of(prot)) ? err_of(errno) : 0;
}
int os_mem_discard(void *addr, size_t size) {
    uintptr_t lo, hi;
    page_span(addr, size, &lo, &hi);
    return madvise((void *)lo, hi - lo, MADV_DONTNEED) ? err_of(errno) : 0;
}
int os_mem_resident(const void *addr, size_t size, uint8_t *out) {
    return mincore((void *)addr, size, out) ? err_of(errno) : 0;
}
int os_mem_usage_get(os_mem_usage *out) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return err_of(errno);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned long long kb;
        if (sscanf(line, "VmRSS: %llu", &kb) == 1) out->resident = kb << 10;
        else if (sscanf(line, "VmHWM: %llu", &kb) == 1) out->peak_resident = kb << 10;
        else if (sscanf(line, "RssAnon: %llu", &kb) == 1) out->private_bytes = kb << 10;
    }
    fclose(f);
    return 0;
}

/* ---- faults: on SIGSEGV, SIGBUS, SIGILL, SIGFPE ---- */
static os_fault_fn handlers[16];
static int nhandlers;
static os_fault_fn last_handler;
static struct sigaction previous[32];
static os_mutex handlers_lock = OS_MUTEX_INIT;
static void on_signal(int sig, siginfo_t *si, void *uctx) {
    os_fault f;
    memset(&f, 0, sizeof f);
    f.kind = sig == SIGSEGV || sig == SIGBUS ? OS_FAULT_ACCESS : sig == SIGILL ? OS_FAULT_ILLEGAL
           : sig == SIGFPE ? OS_FAULT_DIVIDE : OS_FAULT_TRAP;
#if defined(__x86_64__)
    const ucontext_t *uc = (const ucontext_t *)uctx;
    f.pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    if (f.kind == OS_FAULT_ACCESS) f.write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(__aarch64__)
    const ucontext_t *uc = (const ucontext_t *)uctx;
    f.pc = (uintptr_t)uc->uc_mcontext.pc;
#endif
    if (f.kind == OS_FAULT_ACCESS) f.addr = (uintptr_t)si->si_addr;
    for (int i = 0; i < nhandlers; i++)
        if (handlers[i] && handlers[i](&f) == OS_FAULT_RESUME) return;
    if (last_handler && last_handler(&f) == OS_FAULT_RESUME) return;
    /* not ours: whoever had the signal before, else the default (the fault comes again and ends the process) */
    const struct sigaction *p = &previous[sig];
    if ((p->sa_flags & SA_SIGINFO) && p->sa_sigaction) { p->sa_sigaction(sig, si, uctx); return; }
    if (p->sa_handler != SIG_IGN && p->sa_handler != SIG_DFL && p->sa_handler) { p->sa_handler(sig); return; }
    signal(sig, SIG_DFL);
}
static void install_signals(void) {
    static int done;
    if (done) return;
    done = 1;
    alt_stack_install();   /* (the installing thread's, usually the main thread's) */
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_signal;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sigs[i], &sa, &previous[sigs[i]]);
    }
}
void os_fault_add(os_fault_fn fn, bool first) {
    os_mutex_lock(&handlers_lock);
    install_signals();
    if (nhandlers < 16) {
        if (first) {
            memmove(&handlers[1], &handlers[0], sizeof handlers[0] * (size_t)nhandlers);
            handlers[0] = fn;
        } else {
            handlers[nhandlers] = fn;
        }
        nhandlers++;
    }
    os_mutex_unlock(&handlers_lock);
}
void os_fault_remove(os_fault_fn fn) {
    os_mutex_lock(&handlers_lock);
    for (int i = 0; i < nhandlers; i++)
        if (handlers[i] == fn) {
            memmove(&handlers[i], &handlers[i + 1], sizeof handlers[0] * (size_t)(nhandlers - i - 1));
            nhandlers--;
            break;
        }
    os_mutex_unlock(&handlers_lock);
}
void os_fault_set_last(os_fault_fn fn) {
    os_mutex_lock(&handlers_lock);
    install_signals();
    last_handler = fn;
    os_mutex_unlock(&handlers_lock);
}

/* ---- modules ---- */
os_lib *os_lib_open(const char *path) {
    if (!path) return NULL;
    const char *p = path;
    return (os_lib *)dlopen(p, RTLD_NOW | RTLD_LOCAL);
}
void *os_lib_sym(os_lib *l, const char *name) { return l ? dlsym((void *)l, name) : NULL; }
void os_lib_close(os_lib *l) { if (l) dlclose((void *)l); }
void *os_sym(const char *name) { return dlsym(RTLD_DEFAULT, name); }
int os_exe_path(char *out, size_t cap) {
    if (!cap) return 0;
    const ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    out[n > 0 ? n : 0] = 0;
    return n > 0 ? (int)n : 0;
}
uintptr_t os_exe_base(void) {
    Dl_info di;
    return dladdr((void *)&os_exe_base, &di) ? (uintptr_t)di.dli_fbase : 0;
}
uintptr_t os_module_of(const void *addr, char *out, size_t cap) {
    Dl_info di;
    if (cap) out[0] = 0;
    if (!dladdr(addr, &di) || !di.dli_fbase) return 0;
    if (cap) {
        /* (the executable's dli_fname is how it was started, maybe a bare name: /proc/self/exe instead) */
        if ((uintptr_t)di.dli_fbase == os_exe_base() || !di.dli_fname || !strchr(di.dli_fname, '/')) os_exe_path(out, cap);
        else {
            char full[PATH_MAX];
            snprintf(out, cap, "%s", realpath(di.dli_fname, full) ? full : di.dli_fname);
        }
    }
    return (uintptr_t)di.dli_fbase;
}

/* ---- files ---- */
struct os_file { int fd; };
os_file *os_file_open(const char *path, int flags, int *err) {
    const char *p = path;
    int o = (flags & OS_READ) && (flags & OS_WRITE) ? O_RDWR : (flags & OS_WRITE) ? O_WRONLY : O_RDONLY;
    if (flags & OS_CREATE) o |= O_CREAT;
    if (flags & OS_EXCL) o |= O_CREAT | O_EXCL;
    if (flags & OS_TRUNC) o |= O_TRUNC;
    const int fd = open(p, o | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (err) *err = err_of(errno);
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        if (err) *err = OS_EACCES;
        return NULL;
    }
    os_file *f = (os_file *)malloc(sizeof *f);
    f->fd = fd;
    if (err) *err = 0;
    return f;
}
int64_t os_file_read_at(os_file *f, void *buf, size_t n, uint64_t at) {
    size_t done = 0;
    while (done < n) {
        const ssize_t r = pread(f->fd, (char *)buf + done, n - done, (off_t)(at + done));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return done ? (int64_t)done : err_of(errno);
        if (!r) break;
        done += (size_t)r;
    }
    return (int64_t)done;
}
int64_t os_file_write_at(os_file *f, const void *buf, size_t n, uint64_t at) {
    size_t done = 0;
    while (done < n) {
        const ssize_t r = pwrite(f->fd, (const char *)buf + done, n - done, (off_t)(at + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return r < 0 ? err_of(errno) : OS_EFAIL;
        done += (size_t)r;
    }
    return (int64_t)done;
}
int64_t os_file_size(os_file *f) {
    struct stat st;
    return fstat(f->fd, &st) ? err_of(errno) : (int64_t)st.st_size;
}
int os_file_resize(os_file *f, uint64_t size) { return ftruncate(f->fd, (off_t)size) ? err_of(errno) : 0; }
int os_file_flush(os_file *f) { return fdatasync(f->fd) && errno != EINVAL ? err_of(errno) : 0; }
void os_file_close(os_file *f) {
    if (!f) return;
    close(f->fd);
    free(f);
}
bool os_file_trylock(os_file *f) { return flock(f->fd, LOCK_EX | LOCK_NB) == 0; }
const void *os_file_map_read(os_file *f, size_t *size) {
    struct stat st;
    *size = 0;
    if (fstat(f->fd, &st) || st.st_size <= 0) return NULL;
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, f->fd, 0);
    if (p == MAP_FAILED) return NULL;
    *size = (size_t)st.st_size;
    return p;
}
int os_path_kind(const char *path) {
    const char *p = path;
    struct stat st, link;
    if (stat(p, &st)) return OS_PATH_NONE;
    int k = S_ISDIR(st.st_mode) ? OS_PATH_DIR : OS_PATH_FILE;
    if (lstat(p, &link) == 0 && S_ISLNK(link.st_mode)) k |= OS_PATH_LINK;
    return k;
}
int64_t os_path_size(const char *path) {
    const char *p = path;
    struct stat st;
    if (stat(p, &st)) return err_of(errno);
    return S_ISDIR(st.st_mode) ? OS_EACCES : (int64_t)st.st_size;
}
int os_mkdir(const char *path) {
    const char *p = path;
    return mkdir(p, 0755) ? err_of(errno) : 0;
}
int os_rmdir(const char *path) {
    const char *p = path;
    return rmdir(p) ? err_of(errno) : 0;
}
int os_remove(const char *path) {
    const char *p = path;
    return unlink(p) ? err_of(errno) : 0;
}
int os_rename(const char *from, const char *to, bool replace) {
    const char *a = from, *b = to;
    if (!replace) return renameat2(AT_FDCWD, a, AT_FDCWD, b, RENAME_NOREPLACE) ? err_of(errno) : 0;
    return rename(a, b) ? err_of(errno) : 0;
}
int os_copy_file(const char *from, const char *to, bool replace) {
    const char *a = from, *b = to;
    const int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0) return err_of(errno);
    const int out = open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (replace ? O_TRUNC : O_EXCL), 0644);
    if (out < 0) { const int e = err_of(errno); close(in); return e; }
    char buf[1 << 16];
    ssize_t n;
    int r = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { r = err_of(errno); break; }
    if (n < 0) r = err_of(errno);
    close(in), close(out);
    return r;
}
int os_chdir(const char *path) {
    const char *p = path;
    return chdir(p) ? err_of(errno) : 0;
}
struct os_dir { DIR *d; char path[PATH_MAX]; };
os_dir *os_dir_open(const char *path, int *err) {
    os_dir *d = (os_dir *)calloc(1, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    size_t n = strlen(d->path);
    while (n > 1 && d->path[n - 1] == '/') d->path[--n] = 0;
    d->d = opendir(d->path);
    if (!d->d) {
        if (err) *err = err_of(errno);
        free(d);
        return NULL;
    }
    if (err) *err = 0;
    return d;
}
bool os_dir_next(os_dir *d, os_dirent *e) {
    struct dirent *de;
    while ((de = readdir(d->d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char full[PATH_MAX + 260];
        snprintf(full, sizeof full, "%s/%s", d->path, de->d_name);
        struct stat st, link;
        if (stat(full, &st) == 0) {
            e->kind = S_ISDIR(st.st_mode) ? OS_PATH_DIR : OS_PATH_FILE;
            if (!S_ISDIR(st.st_mode)) e->size = (uint64_t)st.st_size;
        } else {
            e->kind = de->d_type == DT_DIR ? OS_PATH_DIR : OS_PATH_FILE;
        }
        if (lstat(full, &link) == 0 && S_ISLNK(link.st_mode)) e->kind |= OS_PATH_LINK;
        return true;
    }
    return false;
}
void os_dir_close(os_dir *d) {
    if (!d) return;
    closedir(d->d);
    free(d);
}
bool os_stderr_shown(void) {
    struct stat st;
    if (fstat(2, &st)) return false;
    if (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode)) return true;
    /* (a terminal is; /dev/null and the journal's socket, where a desktop launcher sends it, are not) */
    return S_ISCHR(st.st_mode) && isatty(2);
}

