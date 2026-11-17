/* host/os/os_linux.c under load: the locks exclude, the condition variable, event and semaphore lose no wake-up,
   timeouts end, sleeps are not rounded to milliseconds. Built and run by tests/test_os.py. */
#include "os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
enum { THREADS = 8, ROUNDS = 200000 };

static os_mutex mu = OS_MUTEX_INIT;
static os_rwlock rw = OS_RWLOCK_INIT;
static volatile uint64_t counter, readers_in, writer_in;
static os_event go;

static void mutex_worker(void *arg) {
    (void)arg;
    os_event_wait(&go, OS_FOREVER);
    for (int i = 0; i < ROUNDS; i++) {
        os_mutex_lock(&mu);
        counter++;
        os_mutex_unlock(&mu);
    }
}
static void rw_worker(void *arg) {
    const int writer = (int)(intptr_t)arg & 1;
    os_event_wait(&go, OS_FOREVER);
    for (int i = 0; i < ROUNDS / 4; i++) {
        if (writer) {
            os_rwlock_lock(&rw);
            CHECK(!os_atomic_load(&readers_in) && !os_atomic_load(&writer_in));
            writer_in = 1;
            counter++;
            writer_in = 0;
            os_rwlock_unlock(&rw);
        } else {
            os_rwlock_lock_shared(&rw);
            os_atomic_inc(&readers_in);
            CHECK(!os_atomic_load(&writer_in));
            os_atomic_dec(&readers_in);
            os_rwlock_unlock_shared(&rw);
        }
    }
}

/* a queue handed over with a condition variable: every item arrives */
static os_mutex qmu = OS_MUTEX_INIT;
static os_cond qcv = OS_COND_INIT;
static int queued, taken, producers_done;
static void producer(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS / 4; i++) {
        os_mutex_lock(&qmu);
        queued++;
        os_mutex_unlock(&qmu);
        os_cond_signal(&qcv);
    }
    os_mutex_lock(&qmu);
    producers_done++;
    os_mutex_unlock(&qmu);
    os_cond_broadcast(&qcv);
}
static void consumer(void *arg) {
    (void)arg;
    os_mutex_lock(&qmu);
    for (;;) {
        while (!queued && producers_done < THREADS / 2) os_cond_wait(&qcv, &qmu, OS_FOREVER);
        if (!queued) break;
        queued--, taken++;
    }
    os_mutex_unlock(&qmu);
}

/* ping-pong over two auto-reset events: no lost set */
static os_event ping, pong;
static void ponger(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS / 4; i++) {
        CHECK(os_event_wait(&ping, OS_FOREVER) == 0);
        os_event_set(&pong);
    }
}

static os_sema sem;
static volatile uint32_t sem_got;
static void sem_taker(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS / 8; i++) {
        CHECK(os_sema_wait(&sem, OS_FOREVER) == 0);
        os_atomic_inc(&sem_got);
    }
}

static os_once once = OS_ONCE_INIT;
static volatile uint32_t once_runs, once_seen;
static void once_fn(void *arg) {
    (void)arg;
    os_sleep_ns(20000000);   /* (the others arrive while it runs) */
    os_atomic_inc(&once_runs);
}
static void once_worker(void *arg) {
    (void)arg;
    os_event_wait(&go, OS_FOREVER);
    os_once_run(&once, once_fn, NULL);
    if (os_atomic_load(&once_runs) == 1) os_atomic_inc(&once_seen);   /* (returned only after it ran) */
}

static void run_all(os_thread_fn fn, int n, int tagged) {
    os_thread *t[THREADS];
    os_event_init(&go, true, false);
    for (int i = 0; i < n; i++) CHECK((t[i] = os_thread_start(fn, (void *)(intptr_t)(tagged ? i : 0), 0, "os_test")));
    os_event_set(&go);
    for (int i = 0; i < n; i++) {
        CHECK(os_thread_join(t[i], 60000000000LL) == 0);
        os_thread_release(t[i]);
    }
}

int main(void) {
    counter = 0;
    run_all(mutex_worker, THREADS, 0);
    CHECK(counter == (uint64_t)THREADS * ROUNDS);

    run_all(once_worker, THREADS, 0);
    CHECK(once_runs == 1 && once_seen == THREADS && os_once_done(&once));

    counter = 0;
    run_all(rw_worker, THREADS, 1);
    CHECK(counter == (uint64_t)(THREADS / 2) * (ROUNDS / 4));

    os_thread *t[THREADS];
    for (int i = 0; i < THREADS; i++) t[i] = os_thread_start(i < THREADS / 2 ? producer : consumer, NULL, 0, "q");
    for (int i = 0; i < THREADS; i++) { CHECK(os_thread_join(t[i], 60000000000LL) == 0); os_thread_release(t[i]); }
    CHECK(taken == (THREADS / 2) * (ROUNDS / 4) && !queued);

    os_event_init(&ping, false, false);
    os_event_init(&pong, false, false);
    os_thread *p = os_thread_start(ponger, NULL, 0, "pong");
    for (int i = 0; i < ROUNDS / 4; i++) {
        os_event_set(&ping);
        CHECK(os_event_wait(&pong, OS_FOREVER) == 0);
    }
    CHECK(os_thread_join(p, OS_FOREVER) == 0);
    os_thread_release(p);
    CHECK(!os_event_try(&ping) && !os_event_try(&pong));

    os_sema_init(&sem, 0);
    for (int i = 0; i < 4; i++) t[i] = os_thread_start(sem_taker, NULL, 0, "sem");
    for (int i = 0; i < 4 * (ROUNDS / 8); i++) os_sema_post(&sem, 1);
    for (int i = 0; i < 4; i++) { CHECK(os_thread_join(t[i], 60000000000LL) == 0); os_thread_release(t[i]); }
    CHECK(sem_got == 4 * (ROUNDS / 8) && os_sema_wait(&sem, 0) == OS_ETIMEDOUT);

    /* timeouts end, near their time */
    os_event never;
    os_event_init(&never, true, false);
    int64_t t0 = os_now_ns();
    CHECK(os_event_wait(&never, 20000000) == OS_ETIMEDOUT);
    int64_t took = os_now_ns() - t0;
    CHECK(took >= 20000000 && took < 200000000);
    os_mutex_lock(&qmu);
    t0 = os_now_ns();
    CHECK(os_cond_wait(&qcv, &qmu, 5000000) == OS_ETIMEDOUT);
    CHECK(os_now_ns() - t0 >= 5000000);
    os_mutex_unlock(&qmu);
    CHECK(os_sema_wait(&sem, 1000000) == OS_ETIMEDOUT);

    /* a 100 us sleep: at least 100 us, and (the median of 50) well under a millisecond */
    int64_t lat[50];
    for (int i = 0; i < 50; i++) {
        t0 = os_now_ns();
        os_sleep_ns(100000);
        lat[i] = os_now_ns() - t0;
        CHECK(lat[i] >= 100000);
    }
    for (int i = 0; i < 50; i++)
        for (int j = i + 1; j < 50; j++)
            if (lat[j] < lat[i]) { int64_t x = lat[i]; lat[i] = lat[j]; lat[j] = x; }
    printf("sleep 100 us: median %.1f us, max %.1f us\n", lat[25] / 1000.0, lat[49] / 1000.0);
    CHECK(lat[25] < 600000);

    /* threads: names, ids, the current one */
    os_thread *me = os_thread_current();
    CHECK(os_thread_tid(me) == os_tid() && os_tid() != 0);
    char name[64];
    os_thread_set_name(me, "os_test_main");
    os_thread_name(me, name, sizeof name);
    CHECK(!strcmp(name, "os_test_main"));
    CHECK(os_thread_cpu_ns(me) > 0);
    CHECK(os_process_affinity() != 0 && os_cpu_count() > 0);
    uint64_t cores[64];
    CHECK(os_cpu_cores(cores, 64) > 0);

    /* memory */
    const size_t pg = os_page_size();
    char *m = (char *)os_mem_map(NULL, 16 * pg, OS_PROT_NONE);
    CHECK(m);
    CHECK(os_mem_protect(m, 4 * pg, OS_PROT_RW) == 0);
    m[0] = 7;
    CHECK(os_mem_discard(m, pg) == 0 && m[0] == 0);
    CHECK(os_mem_unmap(m, 16 * pg) == 0);

    /* files and folders */
    char dir[256], path[300], path2[300];
    snprintf(dir, sizeof dir, "/tmp/os_test_%u", os_pid());
    CHECK(os_mkdir(dir) == 0 && os_mkdir(dir) == OS_EEXIST && os_path_kind(dir) == OS_PATH_DIR);
    snprintf(path, sizeof path, "%s/a.bin", dir);
    int err;
    os_file *f = os_file_open(path, OS_READ | OS_WRITE | OS_EXCL, &err);
    CHECK(f && !err);
    CHECK(!os_file_open(path, OS_WRITE | OS_EXCL, &err) && err == OS_EEXIST);
    CHECK(os_file_write_at(f, "hello", 5, 10) == 5 && os_file_size(f) == 15);
    char buf[16] = {0};
    CHECK(os_file_read_at(f, buf, 16, 10) == 5 && !memcmp(buf, "hello", 5));
    CHECK(os_file_resize(f, 3) == 0 && os_file_size(f) == 3 && os_file_flush(f) == 0);
    os_file_close(f);
    snprintf(path2, sizeof path2, "%s/b.bin", dir);
    CHECK(os_copy_file(path, path2, false) == 0 && os_copy_file(path, path2, false) == OS_EEXIST);
    CHECK(os_rename(path, path2, false) == OS_EEXIST && os_rename(path, path2, true) == 0);
    CHECK(os_path_kind(path) == OS_PATH_NONE && os_path_size(path2) == 3);
    os_dir *d = os_dir_open(dir, &err);
    CHECK(d);
    os_dirent e;
    int n = 0;
    while (os_dir_next(d, &e)) { n++; CHECK(!strcmp(e.name, "b.bin") && e.kind == OS_PATH_FILE && e.size == 3); }
    os_dir_close(d);
    CHECK(n == 1);
    CHECK(os_rmdir(dir) == OS_ENOTEMPTY && os_remove(path2) == 0 && os_rmdir(dir) == 0);
    CHECK(!os_dir_open(dir, &err) && err == OS_ENOENT);

    /* modules */
    char exe[OS_PATH_MAX];
    CHECK(os_exe_path(exe, sizeof exe) > 0);
    char where[OS_PATH_MAX];
    CHECK(os_module_of((void *)&main, where, sizeof where) == os_exe_base() && !strcmp(where, exe));
    printf("os_test: all passed\n");
    return 0;
}
