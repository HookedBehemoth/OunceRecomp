/* The host's diagnostic threads (Linux): HOS_THREAD_TIMES=<seconds> (threads over 5% of a core), HOS_WATCH=<file>
   (data breakpoints on guest addresses) and HOS_DUMP_FILE=<path> (every guest thread's frames on request).
   - CPU time per thread from /proc/self/task/<tid>/schedstat (stat's utime + stime without schedstats);
   - breakpoints with perf_event_open (PERF_TYPE_BREAKPOINT, sigtrap=1: a SIGTRAP in the thread that touched the
     address, si_code TRAP_PERF), allowed on our own threads at the default perf_event_paranoid (2).
   Frames are written as <module>+<offset from its load base>: hookshot and hookshot_direct.so,
   offsets as in their linker maps (position-independent, based at 0). */
#include "../hos.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "os.h"
#ifndef TRAP_PERF   /* (the kernel's since 5.13; glibc names it from 2.36) */
#define TRAP_PERF 6
#endif

int hos_thread_tids(uint32_t *out, const char **names, int max);   /* hos_thread.c */
void hos_prof_start(void);

/* ---- the process's threads: id, CPU time, name (the guest thread's, else the thread's own: os_thread_set_name) ---- */
typedef struct { int tid; uint64_t cpu_ns; char name[64]; int guest; } Task;

static uint64_t task_cpu_ns(int tid) {
    char p[64];
    unsigned long long ns = 0, ut = 0, st = 0;
    snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid);
    FILE *f = fopen(p, "r");
    if (f) {
        const int got = fscanf(f, "%llu", &ns);
        fclose(f);
        if (got == 1) return ns;
    }
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    f = fopen(p, "r");
    if (!f) return 0;
    char line[1024];
    const char *s = fgets(line, sizeof line, f) ? strrchr(line, ')') : NULL;
    fclose(f);
    /* (after "(comm)": state is field 3, utime 14, stime 15) */
    if (!s || sscanf(s + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return 0;
    return (ut + st) * (1000000000ull / (unsigned long long)sysconf(_SC_CLK_TCK));
}
static void task_comm(int tid, char *out, size_t cap) {
    char p[64];
    snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(out, (int)cap, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}
static int tasks(Task *out, int max) {
    static uint32_t gtid[1024];
    static const char *gn[1024];
    const int ng = hos_thread_tids(gtid, gn, 1024);
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) && n < max;) {
        const int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        Task *t = &out[n++];
        t->tid = tid;
        t->cpu_ns = task_cpu_ns(tid);
        t->guest = 0;
        t->name[0] = 0;
        for (int i = 0; i < ng; i++)
            if (gtid[i] == tid) { snprintf(t->name, sizeof t->name, "%s", gn[i]); t->guest = 1; break; }
        if (!t->name[0]) task_comm(tid, t->name, sizeof t->name);
        if (!t->name[0]) snprintf(t->name, sizeof t->name, "tid %d", tid);
    }
    closedir(d);
    return n;
}

/* HOS_THREAD_TIMES=<seconds>: every interval, the threads that used over 5% of a core */
static void thread_times_thread(void *p) {
    const uint32_t ms = (uint32_t)(uintptr_t)p * 1000;
    enum { MAXT = 512 };
    static struct { int tid; uint64_t cpu; } last[MAXT];
    int nlast = 0;
    for (;;) {
        os_sleep_ns((int64_t)(ms) * 1000000);
        static Task now[MAXT];
        const int n = tasks(now, MAXT);
        char line[4096];
        int len = snprintf(line, sizeof line, "threads in %u s:", (unsigned)(ms / 1000));
        for (int i = 0; i < n; i++) {
            uint64_t before = 0;
            int seen = 0;
            for (int k = 0; k < nlast; k++)
                if (last[k].tid == now[i].tid) before = last[k].cpu, seen = 1;
            const double pct = seen ? (double)(now[i].cpu_ns - before) / (ms * 1e6) * 100.0 : 0;
            if (pct >= 5.0 && len < (int)sizeof line - 96) len += snprintf(line + len, sizeof line - len, " %s %.0f%%;", now[i].name, pct);
        }
        for (int i = 0; i < n; i++) last[i].tid = now[i].tid, last[i].cpu = now[i].cpu_ns;
        nlast = n;
        hos_log("prof: %s", line);
    }
}

/* a code address as module + offset: the module's file name and load base (dladdr) */
typedef struct { uintptr_t base; char name[64]; } Module;
static Module mods[64];
static int nmods;
static int module_of(uintptr_t pc, uintptr_t *base) {
    Dl_info d;
    if (!dladdr((void *)pc, &d) || !d.dli_fbase) { *base = 0; return -1; }
    *base = (uintptr_t)d.dli_fbase;
    for (int i = 0; i < nmods; i++)
        if (mods[i].base == *base) return i;
    if (nmods >= 64) return -1;
    const char *n = d.dli_fname && d.dli_fname[0] ? d.dli_fname : "hookshot";
    const char *b = strrchr(n, '/');
    mods[nmods].base = *base;
    snprintf(mods[nmods].name, sizeof mods[nmods].name, "%s", b ? b + 1 : n);
    return nmods++;
}

/* HOS_WATCH=<file> (diagnostic): when <file> exists (up to 4 guest addresses in hex), data breakpoints on them on every
   guest thread: 4 s on reads and writes, then 4 s on writes; <file>.out gets, by address and the host place of the
   access, how many; then <file> is renamed <file>.done and the next one may come. */
static uint64_t watch_keys[8192];
static volatile int32_t watch_counts[8192];
static volatile int32_t watch_phase;
static struct sigaction watch_prev;
static void on_watch(int sig, siginfo_t *si, void *uctx) {
    if (si->si_code != TRAP_PERF) {   /* (a breakpoint of the game's or a debugger's: os_linux.c's fault handlers) */
        if (watch_prev.sa_flags & SA_SIGINFO) watch_prev.sa_sigaction(sig, si, uctx);
        else if (watch_prev.sa_handler != SIG_DFL && watch_prev.sa_handler != SIG_IGN) watch_prev.sa_handler(sig);
        else { signal(sig, SIG_DFL); raise(sig); }
        return;
    }
    /* (si_perf_data, the event's sig_data: the watch's index. glibc's siginfo_t has no name for it: the kernel puts it
       after si_addr, at 24) */
    const unsigned long idx = *(const unsigned long *)((const char *)si + 24) & 3;
    const uint64_t rip = (uint64_t)((const ucontext_t *)uctx)->uc_mcontext.gregs[REG_RIP];
    const uint64_t key = ((uint64_t)(watch_phase & 1) << 62) | ((uint64_t)idx << 58) | rip;
    for (uint32_t i = (uint32_t)((key >> 2) * 2654435761u) & 8191, n = 0; n < 8192; n++, i = (i + 1) & 8191) {
        uint64_t k = __atomic_load_n(&watch_keys[i], __ATOMIC_RELAXED);
        if (!k) {
            uint64_t expect = 0;
            if (__atomic_compare_exchange_n(&watch_keys[i], &expect, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) k = key;
            else k = expect;
        }
        if (k == key) { os_atomic_inc(&watch_counts[i]); break; }
    }
}
enum { WATCH_FDS = 1024 * 4 };
static int watch_fd[WATCH_FDS];
static int nwatch_fd;
static void watch_arm(const uint64_t *addr, int n, int rw) {
    for (int i = 0; i < nwatch_fd; i++) close(watch_fd[i]);
    nwatch_fd = 0;
    if (!rw) return;
    static uint32_t gtid[1024];
    static const char *gn[1024];
    const int ng = hos_thread_tids(gtid, gn, 1024);
    int failed = 0, err = 0;
    for (int t = 0; t < ng; t++) {
        const int tid = (int)gtid[t];
        if (!tid || tid == (int)os_tid()) continue;
        for (int k = 0; k < n && nwatch_fd < WATCH_FDS; k++) {
            struct perf_event_attr a;
            memset(&a, 0, sizeof a);
            a.type = PERF_TYPE_BREAKPOINT;
            a.size = sizeof a;
            a.bp_type = rw == 1 ? HW_BREAKPOINT_W : HW_BREAKPOINT_RW;
            a.bp_addr = (uint64_t)(uintptr_t)GP(addr[k]);
            a.bp_len = HW_BREAKPOINT_LEN_4;
            a.sample_period = 1;
            a.exclude_kernel = 1;
            a.exclude_hv = 1;
            a.sigtrap = 1;
            a.remove_on_exec = 1;
            a.sig_data = (uint64_t)k;
            const int fd = (int)syscall(SYS_perf_event_open, &a, tid, -1, -1, PERF_FLAG_FD_CLOEXEC);
            if (fd >= 0) watch_fd[nwatch_fd++] = fd;
            else failed++, err = errno;
        }
    }
    if (failed) hos_log("watch: %d breakpoints not set (perf_event_open: %s; /proc/sys/kernel/perf_event_paranoid)", failed, strerror(err));
}
static void watch_thread(void *p) {
    const char *path = (const char *)p;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_watch;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (;;) {
        os_sleep_ns(300LL * 1000000);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        uint64_t addr[4];
        int n = 0;
        unsigned long long v;
        while (n < 4 && fscanf(f, "%llx", &v) == 1) addr[n++] = v;
        fclose(f);
        struct sigaction cur;   /* (ours ahead of os_linux.c's, which it calls for every other SIGTRAP) */
        sigaction(SIGTRAP, NULL, &cur);
        if (cur.sa_sigaction != on_watch) sigaction(SIGTRAP, &sa, &watch_prev);
        memset(watch_keys, 0, sizeof watch_keys);
        memset((void *)watch_counts, 0, sizeof watch_counts);
        for (int k = 0; k < n; k++) hos_log("watch: %llx holds %08x", (unsigned long long)addr[k], *(uint32_t *)GP(addr[k]));
        for (int ph = 0; ph < 2; ph++) {
            watch_phase = ph;
            for (int t = 0; t < 4; t++) {   /* (threads that started since) */
                watch_arm(addr, n, ph ? 1 : 3);
                os_sleep_ns(1000LL * 1000000);
            }
        }
        watch_arm(addr, 0, 0);
        char out[OS_PATH_MAX], done[OS_PATH_MAX];
        snprintf(out, sizeof out, "%s.out", path);
        snprintf(done, sizeof done, "%s.done", path);
        f = fopen(out, "a");
        if (f) {
            for (int i = 0; i < 8192; i++)
                if (watch_keys[i]) {
                    const uint64_t k = watch_keys[i], rip = k & ((1ULL << 58) - 1);
                    uintptr_t base;
                    const int mi = module_of((uintptr_t)rip, &base);
                    fprintf(f, "%llx %s %s+%llx %ld\n", (unsigned long long)addr[(k >> 58) & 3], k >> 62 & 1 ? "w" : "rw",
                            mi < 0 ? "?" : mods[mi].name, (unsigned long long)(rip - base), (long)watch_counts[i]);
                }
            fclose(f);
        }
        unlink(done);
        rename(path, done);
        hos_log("watch: done");
    }
}

/* HOS_DUMP_FILE=<path> (diagnostic): when <path> exists, every guest thread's guest frames (hos_thread_dump) go to the
   log, and <path> is removed: what a stalled game waits in
   without a debugger (ptrace is limited to a process's ancestors here) */
static void dump_thread(void *p) {
    const char *path = (const char *)p;
    void hos_thread_dump(void);
    for (;;) {
        os_sleep_ns(500LL * 1000000);
        if (access(path, F_OK)) continue;
        unlink(path);
        hos_log("dump: asked");
        void rt_slots_dump(void);
        rt_slots_dump();
        hos_thread_dump();
        hos_log_flush();
    }
}

void hos_prof_start(void) {
    const char *dump = getenv("HOS_DUMP_FILE");
    if (dump && *dump) os_thread_release(os_thread_start(dump_thread, (void *)dump, 0, NULL));
    const char *tt = getenv("HOS_THREAD_TIMES");
    if (tt && atoi(tt) > 0) os_thread_release(os_thread_start(thread_times_thread, (void *)(uintptr_t)atoi(tt), 0, NULL));
    const char *watch = getenv("HOS_WATCH");
    if (watch && *watch) os_thread_release(os_thread_start(watch_thread, (void *)watch, 0, NULL));
}
