/* Guest threads: one host thread each, with its own Ctx, TLS block and thread-local region.

   TPIDR_EL0 (TLS): a 4 KB block per thread, TP in its middle. TOTK's main reads its one thread-local at TP+0x10
   (local-exec, after AArch64's 16-byte TCB); the area below TP is zeroed room for whatever the SDK's musl keeps
   there. TPIDRRO_EL0 (thread-local region): 0x200 bytes; the SDK's nn::os keeps the current ThreadType* at
   +0x1F8, so lifted SDK code that reads it inline finds the same thread the HLE knows. */
#include "hos.h"
#include <xmmintrin.h>
#include "direct_crit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include <emmintrin.h>

struct HosThread {
    Ctx ctx;
    uint64_t guest_tt, entry, arg, stack, stack_size, id;
    uint64_t tls_block, tlr;
    int priority, ideal_core;
    uint64_t core_mask;
    uint64_t tls_slots[HOS_TLS_SLOTS];
    char name[48];
    os_thread *handle;
    int started;
    /* the kernel side (hos_svc.c): this thread's Horizon handle and the event it blocks on */
    uint32_t kernel_handle;
    os_event wake;
    /* (HOS_DUMP_FILE) how often it asked for its core number, the guest callers of its first and last asks */
    int64_t core_asks;
    uint64_t core_ask_lr, core_ask_last_lr;
    /* (hookshot) fibers: the context running now (NULL: ctx, the thread's own), the thread's host fiber once it
       switched to a guest fiber */
    Ctx *volatile run_ctx;
    os_fiber *host_fiber;
};

/* The SDK's own mutexes (InternalCriticalSection) tag a lock word with the owner's handle, read from
   ThreadType+0x1B0 through TLR+0x1F8 (sdk+0x13186c). Every thread gets a unique handle there. */
#define TT_HANDLE 0x1B0
static volatile int32_t next_handle = 0x10000;
uint32_t hos_thread_handle(HosThread *t) { return t->kernel_handle; }

/* nnosInitialize gives the main thread the SDK's own ThreadType (it writes TLR+0x1F8): adopt it, so the HLE and the
   SDK agree on which object is the current thread. */
void hos_thread_adopt_guest(HosThread *t) {
    uint64_t tt = G64(t->tlr + 0x1F8);
    if (tt && tt != t->guest_tt) {
        t->guest_tt = tt;
        G32(tt + TT_HANDLE) = t->kernel_handle;
    }
}
int hos_thread_prio(HosThread *t) { return t->priority; }

int hos_trace;
extern const uint64_t hos_thread_start_hook, hos_thread_exit_hook;   /* gen/hos_image.c */
static _Thread_local HosThread *cur;
/* (HOS_DBG=crit: direct_crit.h) what the block that gives the core up waits on, set by the blocking call before it */
static _Thread_local int crit_why = CRIT_BLOCK_OTHER;
static _Thread_local uint64_t crit_key, crit_away_t, crit_away_lr;
static os_rwlock threads_lock = OS_RWLOCK_INIT;
static HosThread *threads[1024];
static int nthreads;
static volatile int64_t next_id = 0x50;

HosThread *hos_self(void) { return cur; }
Ctx *hos_ctx(void) { return cur ? (cur->run_ctx ? cur->run_ctx : &cur->ctx) : NULL; }
uint64_t hos_thread_guest(HosThread *t) { return t->guest_tt; }
uint64_t hos_thread_id(HosThread *t) { return t->id; }
const char *hos_thread_name(HosThread *t) { return t->name; }
uint64_t *hos_tls_slots(HosThread *t) { return t->tls_slots; }
/* The core a thread runs on. Most of TOTK's threads have no ideal core (-2, "use default") and a one-core affinity
   mask, so the mask decides; the engine keys per-core resources (its command memory arenas, for one) on
   GetCurrentCoreNumber, so threads pinned to different cores must report different ones. */
void hos_thread_affinity(HosThread *t, int ideal, uint64_t mask) {
    if (hos_trace) hos_log("SetThreadCoreMask %s: ideal %d mask %#llx (was %d, %#llx)", t->name, ideal,
                           (unsigned long long)mask, t->ideal_core, (unsigned long long)t->core_mask);
    if (ideal >= 0) t->ideal_core = ideal;
    if (mask) t->core_mask = mask;
}
uint64_t hos_thread_mask(HosThread *t) { return t->core_mask; }

/* The cores the game is told about: the game's number, not the host's. A Switch 2 application has six, the game's
   per-core arrays have six entries (indexed by the core number its threads cache at their start, `cmp w0, #6`, core 6 and
   up sharing entry 0), and its threads' masks stay within 0x3f. Guest threads run on all of the host's CPUs whatever
   this says. */
int hos_cores(void) { return 6; }
/* The core number the running thread reads (GetCurrentCoreNumber, svc GetCurrentProcessorNumber; lr: the guest
   caller, counted for HOS_DUMP_FILE). OoT's sead thread entry (0x834a2e50) keeps a one-core thread's number + 1 in
   a TLS slot at its start, and 324 call sites read that slot, falling back to asking (166 of them index a six-entry
   per-core array: `cmp w0, #6`, core 6 and up sharing core 0's entry). So a one-core thread asks once, at its start,
   and uses that number from then on: work/assets/native/cores.md */
int hos_current_core(uint64_t lr) {
    HosThread *t = cur;
    if (!t) return 0;
    if (!t->core_asks++) t->core_ask_lr = lr;
    t->core_ask_last_lr = lr;
    return hos_thread_core(t);
}
int hos_thread_core(HosThread *t) {
    if (!t) return 0;
    const int n = hos_cores();
    const uint64_t m = t->core_mask;
    /* a one-core mask: its core; else the ideal core when the mask has it (or there is no mask); else the mask's lowest
       core, wrapped (a mask past the six cores shares a number). Never a core outside the mask */
    if (m && !(m & (m - 1))) return (int)(__builtin_ctzll(m) % (unsigned)n);
    if (t->ideal_core >= 0 && (!m || (m >> t->ideal_core & 1))) return t->ideal_core % n;
    return m ? (int)(__builtin_ctzll(m) % (unsigned)n) : 0;
}
void hos_thread_stack(HosThread *t, uint64_t *addr, uint64_t *size) { *addr = t->stack; *size = t->stack_size; }
uint64_t hos_thread_tlr(HosThread *t) { return t->tlr; }

int hos_thread_priority(HosThread *t, int set, int value) {
    int old = t->priority;
    if (set) {
        if (hos_trace) hos_log("ChangeThreadPriority %s: %d (was %d)", t->name, value, old);
        t->priority = value;
    }
    return old;
}

static int log_queue(const char *line) { (void)line; return 0; }
static void log_flush(void) {}

void hos_log_flush(void) { log_flush(); }

/* Every line starts "[hos 12.345 p600 <thread>]": seconds since the process started (taken before main) and the present
   the game is at. HOS_LOG_TIME=0 leaves both out. A line cut at the buffer's end ends in "~" */
static int64_t log_t0;
static int log_time = -1;
__attribute__((constructor)) static void log_time_start(void) { log_t0 = os_now_ns(); }

double hos_log_seconds(void) { return (double)(os_now_ns() - log_t0) / 1e9; }

static int log_stamp(char *out, size_t cap) {
    if (log_time < 0) log_time = !getenv("HOS_LOG_TIME") || atoi(getenv("HOS_LOG_TIME")) > 0;
    const char *name = cur ? cur->name : "host";
    if (!log_time) return snprintf(out, cap, "[hos %s] ", name);
    return snprintf(out, cap, "[hos %.3f p%lld %s] ", hos_log_seconds(), (long long)hos_vi_present_number(), name);
}

/* An item of HOS_DBG ("name" or "name:value"), and a log switch (docs/switches.md, "Turn on debug aids with HOS_DBG"): its item or logall */
int hos_dbg_item(const char *item) {
    const char *d = getenv("HOS_DBG");
    const size_t n = strlen(item);
    for (; d && *d; d = strchr(d, ',') ? strchr(d, ',') + 1 : NULL)
        if (!strncmp(d, item, n) && (d[n] == ',' || d[n] == ':' || !d[n])) return 1;
    return 0;
}
int hos_log_switch(const char *item) { return hos_dbg_item(item) || hos_dbg_item("logall"); }

#define LOG_BUF 4096
void hos_log(const char *fmt, ...) {
    char buf[LOG_BUF], line[LOG_BUF + 128];
    va_list ap;
    va_start(ap, fmt);
    const int want = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (want >= (int)sizeof buf) buf[sizeof buf - 2] = '~';
    const int k = log_stamp(line, sizeof line);
    snprintf(line + k, sizeof line - (size_t)k, "%s\n", buf);
    if (log_queue(line)) return;
    fputs(line, stderr);
    fflush(stderr);
}

void hos_fatal(Ctx *c, const char *fmt, ...) {
    char buf[1024], stamp[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!c) c = hos_ctx();
    log_flush();
    log_stamp(stamp, sizeof stamp);
    stamp[strlen(stamp) - 2] = 0;
    fprintf(stderr, "%s] FATAL: %s", stamp, buf);
    if (c) fprintf(stderr, " (lr %#llx sp %#llx)", (unsigned long long)c->x[30], (unsigned long long)c->x[31]);
    fprintf(stderr, "\n");
    fflush(stderr);
    void hos_backtrace(void), hos_hostcall_stats(void);
    hos_backtrace();
    hos_hostcall_stats();
    os_exit(3);
}

void hos_fatal_lr(uint64_t lr, const char *fmt, ...) {
    char buf[1024], stamp[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_flush();
    log_stamp(stamp, sizeof stamp);
    stamp[strlen(stamp) - 2] = 0;
    fprintf(stderr, "%s] FATAL: %s (lr %#llx)\n", stamp, buf, (unsigned long long)lr);
    fflush(stderr);
    void hos_backtrace(void), hos_hostcall_stats(void);
    hos_backtrace();
    hos_hostcall_stats();
    os_exit(3);
}

/* Traps from the lifted code (rt/a64_rt.c): name the thread, then stop. */
void a64_on_trap(Ctx *c, uint64_t pc, uint32_t insn, const char *why) {
    hos_fatal(c, "%s at %#llx (insn %08x)", why, (unsigned long long)pc, insn);
}

static void setup_guest(HosThread *t) {
    t->tls_block = hos_sys_alloc(0x1000, 0x1000);
    t->tlr = hos_sys_alloc(0x200, 0x200);
    G64(t->tlr + 0x1F8) = t->guest_tt;
    t->kernel_handle = (uint32_t)os_atomic_inc(&next_handle);
    G32(t->guest_tt + TT_HANDLE) = t->kernel_handle;
    /* (hookshot) SDK 22's InternalCriticalSection::IsLockedByCurrentThread reads the owner handle from TLR+0x110
       directly (SDK 21: ThreadType+0x1B0 through TLR+0x1F8). Left 0, every free mutex looked held by the caller
       and the first heap lock aborted in pthread_mutex_lock. */
    G32(t->tlr + 0x110) = t->kernel_handle;
    os_event_init(&t->wake, false, false);   /* auto-reset: one signal, one wake-up */
    Ctx *c = &t->ctx;
    memset(c, 0, sizeof *c);
    c->tpidr_el0 = t->tls_block + 0x800;
    c->tpidrro_el0 = t->tlr;
    c->thread = t;
}

static void track(HosThread *t) {
    os_rwlock_lock(&threads_lock);
    if (nthreads < 1024) threads[nthreads++] = t;
    os_rwlock_unlock(&threads_lock);
}

/* The system thread ids of every running guest thread, for the profiler (host/posix/prof.c): read under the lock, as
   a handle may be released once it is let go. */
int hos_thread_tids(uint32_t *out, const char **names, int max) {
    int n = 0;
    os_rwlock_lock_shared(&threads_lock);
    for (int i = 0; i < nthreads && n < max; i++)
        if (threads[i]->handle) { out[n] = os_thread_tid(threads[i]->handle); names[n] = threads[i]->name; n++; }
    os_rwlock_unlock_shared(&threads_lock);
    return n;
}

/* (mkw-recomp) Every guest thread's name and guest registers as of its last call out of lifted code, with its
   frame chain from x29: what each thread of a stalled game waits in (hos_process.c, HOS_STATS_AFTER). */
void hos_thread_dump(void) {
    os_rwlock_lock_shared(&threads_lock);
    for (int i = 0; i < nthreads; i++)   /* (placement: masks and the core number it is told) */
        hos_log("thread %s: priority %d, ideal %d, mask %#llx, core %d; asked its core %lld times (first lr %#llx, last %#llx)",
                threads[i]->name, threads[i]->priority, threads[i]->ideal_core, (unsigned long long)threads[i]->core_mask,
                hos_thread_core(threads[i]), (long long)threads[i]->core_asks, (unsigned long long)threads[i]->core_ask_lr,
                (unsigned long long)threads[i]->core_ask_last_lr);
    for (int i = 0; i < nthreads; i++) {
        const Ctx *c = &threads[i]->ctx;
        char bt[400];
        int n = 0;
        uint64_t fp = c->x[29];
        for (int k = 0; k < 12 && fp && !(fp & 15) && fp < HOS_GUEST_SPACE && n < (int)sizeof bt - 24; k++) {
            os_mem_info mi;
            if (os_mem_query(GP(fp), &mi) || mi.state != OS_MEM_COMMITTED) break;
            n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)G64(fp + 8));
            const uint64_t next = G64(fp);
            if (next <= fp) break;
            fp = next;
        }
        bt[n] = 0;
        hos_log("thread %-24s lr %#llx sp %#llx x0 %#llx; frames%s", threads[i]->name, (unsigned long long)c->x[30],
                (unsigned long long)c->x[31], (unsigned long long)a64_guest_of(c->x[0]), bt);
        /* registers (current at the call out), with the text a register (or the word it points to) leads to */
        for (int r = 0; r < 29; r++) {
            uint64_t v = c->x[r], s = 0;
            os_mem_info mi;
            for (int hop = 0; hop < 2 && v > 0x100000 && v < HOS_GUEST_SPACE - 64; hop++) {
                if (os_mem_query(GP(v), &mi) || mi.state != OS_MEM_COMMITTED) break;
                const uint8_t *p = (const uint8_t *)GP(v);
                int k = 0;
                while (k < 48 && p[k] >= 0x20 && p[k] < 0x7f) k++;
                if (k >= 6 && (p[k] == 0 || k == 48)) { s = v; break; }
                v = G64(v);
            }
            if (s) hos_log("thread %s:   x%d %#llx -> \"%.48s\"", threads[i]->name, r, (unsigned long long)c->x[r],
                           (const char *)GP(s));
        }
    }
    os_rwlock_unlock_shared(&threads_lock);
}

/* HOS_FP_LOG=1 (debugging, nn_vi.c at presents 1, 60 and 600): the presenting thread's guest FPCR and the host's MXCSR,
   which host/hos_fp.c keeps equal to it (FZ: 0x9FC0 plus sticky bits) */
extern void (*hos_fp_log_hook)(unsigned long long fpcr, unsigned from, unsigned to);
static void fp_mode_log(unsigned long long fpcr, unsigned from, unsigned to) {
    static volatile int32_t n;
    if (os_atomic_inc(&n) <= 64) hos_log("fp: fpcr %#llx: mxcsr %#x -> %#x", fpcr, from, to);
}

void hos_thread_fp_log(long long present) {
    HosThread *t = cur;
    const Ctx *c = t ? (t->run_ctx ? t->run_ctx : &t->ctx) : NULL;
    hos_log("fp: present %lld thread %s fpcr %#llx mxcsr %#x (expected %#x)", present, t ? t->name : "(host)",
            c ? (unsigned long long)c->fpcr : 0ULL, _mm_getcsr(), c ? hos_fp_mxcsr(c->fpcr) | (_mm_getcsr() & 0x3Fu) : 0u);
}

/* HOS_FP_REPORT=1 (debugging): the FPCR / FPSR of the busiest game threads, with the thread stats */
int hos_thread_fp_report(char *out, int cap) {
    int len = 0;
    os_rwlock_lock_shared(&threads_lock);
    for (int i = 0; i < nthreads && len < cap - 64; i++) {
        const char *nm = threads[i]->name;
        if (strstr(nm, "Main") || strstr(nm, "Worker") || strstr(nm, "Executor"))
            len += snprintf(out + len, cap - len, " %s fpcr %llx fpsr %llx;", nm,
                            (unsigned long long)threads[i]->ctx.fpcr, (unsigned long long)threads[i]->ctx.fpsr);
    }
    os_rwlock_unlock_shared(&threads_lock);
    return len;
}

HosThread *hos_thread_by_handle(uint32_t h) {
    HosThread *r = NULL;
    os_rwlock_lock_shared(&threads_lock);
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->kernel_handle == h) { r = threads[i]; break; }
    os_rwlock_unlock_shared(&threads_lock);
    return r;
}

/* Block on the thread's wake event (the kernel layer's lock must not be held). */
void hos_thread_wait(HosThread *t, int64_t timeout_ns, int *timed_out) {
    const int away = hos_block_begin(CRIT_BLOCK_THREAD, t);
    *timed_out = os_event_wait(&t->wake, timeout_ns < 0 ? OS_FOREVER : timeout_ns) == OS_ETIMEDOUT;
    if (away) hos_block_end();
}
void hos_thread_wake(HosThread *t) { crit_wake(t); os_event_set(&t->wake); }

HosThread *hos_thread_of(uint64_t guest_tt) {
    HosThread *r = NULL;
    os_rwlock_lock_shared(&threads_lock);
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->guest_tt == guest_tt) { r = threads[i]; break; }
    os_rwlock_unlock_shared(&threads_lock);
    return r;
}

Ctx *hos_boot_main_thread(uint64_t stack_size) {
    if (getenv("HOS_FP_LOG")) hos_fp_log_hook = fp_mode_log;
    HosThread *t = (HosThread *)calloc(1, sizeof *t);
    t->guest_tt = hos_sys_alloc(0x400, 0x10);      /* a ThreadType for the main thread; the SDK never made one */
    t->stack_size = stack_size;
    t->stack = hos_sys_alloc(stack_size, 0x1000);
    t->id = os_atomic_inc(&next_id);
    t->priority = 44;                              /* main thread priority in TOTK's NPDM range */
    t->ideal_core = 0;
    t->started = 1;
    snprintf(t->name, sizeof t->name, "MainThread");
    t->handle = os_thread_current();   /* (kept: the profiler's) */
    os_thread_retain(t->handle);
    os_thread_set_name(t->handle, "MainThread");   /* (HOS_SAMPLE finds threads by it) */
    hos_thread_pin(t->handle, t->name);
    setup_guest(t);
    t->ctx.x[31] = (t->stack + stack_size) & ~15ULL;
    cur = t;
    track(t);
    return &t->ctx;
}

static void thread_main(void *p) {
    HosThread *t = (HosThread *)p;
    cur = t;
    Ctx *c = &t->ctx;
    c->x[0] = t->arg;
    c->x[30] = 0;
    c->x[31] = (t->stack + t->stack_size) & ~15ULL;
    A64Fn f = a64_fn_of(t->entry);
    if (!f) hos_fatal(c, "thread %s: entry %#llx is not a function", t->name, (unsigned long long)a64_guest_of(t->entry));
    a64_fpcr_sync(c);   /* (MXCSR from the guest FPCR: a new host thread may inherit its creator's) */
    if (hos_trace) hos_log("thread start: entry %#llx arg %#llx", (unsigned long long)a64_guest_of(t->entry), (unsigned long long)t->arg);
    /* What the SDK's own thread entry does before the thread function (see build.py thread_hooks): nnmusl's start
       hook gives the thread its TLS (thread pointer with the DTV at TP+8, every module's TLS block) and leaves the
       thread pointer at TLR+0x1F0. Without it every thread would share one DTV, and so every thread_local. */
    uint64_t hook = hos_thread_start_hook ? G64(hos_thread_start_hook) : 0;
    if (hook) {
        c->x[0] = t->guest_tt;
        a64_call_received(c, hook);
        if (G64(t->tlr + 0x1F0)) c->tpidr_el0 = G64(t->tlr + 0x1F0);
        if (hos_trace || getenv("HOS_TLS_TRACE"))
            hos_log("thread %s: TLS hook gave TP %#llx, DTV %#llx; ideal core %d mask %#llx priority %d", t->name,
                    (unsigned long long)c->tpidr_el0, (unsigned long long)G64(c->tpidr_el0 + 8), t->ideal_core,
                    (unsigned long long)t->core_mask, t->priority);
        c->x[31] = (t->stack + t->stack_size) & ~15ULL;
    }
    c->x[0] = t->arg;
    c->x[30] = 0;
    f(c);
    hook = hos_thread_exit_hook ? G64(hos_thread_exit_hook) : 0;
    if (hook) {
        c->x[0] = t->guest_tt;
        a64_call_received(c, hook);
    }
    if (hos_trace) hos_log("thread exit");
}

HosThread *hos_thread_create(uint64_t guest_tt, uint64_t entry, uint64_t arg, uint64_t stack, uint64_t stack_size,
                             int priority, int core) {
    HosThread *t = (HosThread *)calloc(1, sizeof *t);
    t->guest_tt = guest_tt;
    t->entry = entry;
    t->arg = arg;
    t->stack = stack;
    t->stack_size = stack_size;
    t->priority = priority;
    t->ideal_core = core;
    t->id = os_atomic_inc(&next_id);
    snprintf(t->name, sizeof t->name, "Thread_%llx", (unsigned long long)entry);
    setup_guest(t);   /* FPCR starts at 0, as the kernel does it; TOTK sets FZ itself (nnMain+0x474, thread entries) */
    track(t);
    return t;
}

void hos_thread_start(HosThread *t) {
    if (t->started) return;
    t->started = 1;
    /* Host stack: lifted code nests a C call per guest call, so give it plenty. */
    /* (named as it starts: the OS name too, for profilers and HOS_SAMPLE) */
    t->handle = os_thread_start(thread_main, t, 64 << 20, t->name);
    if (!t->handle) hos_fatal(NULL, "thread start failed for %s", t->name);
    hos_thread_pin(t->handle, t->name);
}

void hos_thread_join(HosThread *t) {
    if (!t->handle) return;
    const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)7);
    /* (EvilOOT, 11-07: a freeze after Load, one thread joining another for good: a join of over 10 s says who joins whom, once) */
    if (os_thread_join(t->handle, 10000000000LL) == OS_ETIMEDOUT) {
        HosThread *self = hos_self();
        hos_log("thread %s: joining %s for 10 s", self ? hos_thread_name(self) : "?", t->name);
        os_thread_join(t->handle, OS_FOREVER);
    }
    if (away) hos_block_end();
}

void hos_thread_destroy(HosThread *t) {
    os_rwlock_lock(&threads_lock);
    for (int i = 0; i < nthreads; i++)
        if (threads[i] == t) { threads[i] = threads[--nthreads]; break; }
    os_rwlock_unlock(&threads_lock);
    if (t->handle) os_thread_release(t->handle);
    free(t);
}

void hos_thread_set_name(HosThread *t, const char *name) {
    snprintf(t->name, sizeof t->name, "%s", name);
    if (t->handle) {
        os_thread_set_name(t->handle, t->name);
        hos_thread_pin(t->handle, t->name);
    }
}

/* HOS_PIN=<name part>=<hex mask>,...[,*=<hex mask>] (experiment): host CPU affinity by thread name (game threads as
   they are named or started, host threads as they register); "*" for every other named thread. The first matching
   entry wins. (2026-10-20: the castle's MainThread and ModuleSystem workers each on a physical core of their own, the
   rest elsewhere, was slower than Windows' own scheduling: 63-68 against 69-71 fps.) */
/* HOS_PRIO=<name part>=<level>,... (experiment, same matching): host thread priority, -2 (lowest) to 2 (highest).
   (10-20: MainThread, ModuleSystem workers and BaseProc executors above normal: no change at the castle) */
typedef struct { char pat[40]; long long value; } NameSpec;
static int parse_name_specs(const char *e, NameSpec *out, int max, int base) {
    int n = 0;
    while (e && *e && n < max) {
        const char *eq = strchr(e, '='), *end = strchr(e, ',');
        if (!end) end = e + strlen(e);
        if (!eq || eq > end) break;
        snprintf(out[n].pat, sizeof out[n].pat, "%.*s", (int)(eq - e), e);
        out[n++].value = strtoll(eq + 1, NULL, base);
        e = *end ? end + 1 : end;
    }
    return n;
}
static const NameSpec *match_name_spec(const NameSpec *s, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(s[i].pat, "*") ? strstr(name, s[i].pat) != NULL : 1) return &s[i];
    return NULL;
}
/* The last physical core (its logical processors' mask), kept for the back end's replay thread: the game's threads run on the others. The replay is on the frame's critical path
   (MKW waits for the last frame's GPU work before it submits the next), and on a core shared with game threads
   it ran ~1.7x slower than alone. Only on machines with 8+ logical processors; HOS_RESERVE_CORE=0: none.
   (hookshot) On a CPU with cores of more than one kind, the last core of the fastest kind: the last core of all is an
   efficiency core on Intel's hybrid CPUs (an i7-12700K's logical processors 16-19), the slowest place for the replay
   (an i7-13700K's Kokiri Forest: 48.7 fps with it there, 60 on a performance core). EfficiencyClass is higher the faster
   the core, and the same for every core of a CPU of one kind (the last core, as before). Only with 4+ cores of the
   fastest kind (8+ logical processors always have them on a CPU of one kind): a laptop's 2 performance cores and 8
   efficiency ones (i5-1235U) would keep one of the two from the game; then none is kept. The calling thread's
   processor group's cores: the masks the affinity calls take are its group's. Windows only: Linux keeps the last core,
   as before. */
/* (the choice: the cores' logical processor masks and EfficiencyClass, n of them; 0 for none. tests/test_reserved_core.py) */
static uint64_t pick_reserved_core(const uint64_t *masks, const unsigned char *classes, int n) {
    uint64_t last = 0;
    int best = -1, fast = 0, logical = 0;
    for (int i = 0; i < n; i++) {
        if (!masks[i]) continue;
        logical += __builtin_popcountll(masks[i]);
        if ((int)classes[i] > best) {
            best = classes[i], last = masks[i], fast = 1;
        } else if ((int)classes[i] == best) {
            fast++;
            if (masks[i] > last) last = masks[i];
        }
    }
    if (logical < 8 || fast < 4) return 0;
    return last;
}
uint64_t hos_reserved_cpus(void) {
    static uint64_t mask = ~0ull;
    if (mask != ~0ull) return mask;
    mask = 0;
    if (getenv("HOS_RESERVE_CORE") && !atoi(getenv("HOS_RESERVE_CORE"))) return mask;
    uint64_t cores[64];
    const int n = os_cpu_cores(cores, 64);
    int logical = 0;
    uint64_t last = 0;
    for (int i = 0; i < n && i < 64; i++) {
        logical += __builtin_popcountll(cores[i]);
        if (cores[i] > last) last = cores[i];
    }
    if (logical >= 8) mask = last;
    return mask;
}

void hos_thread_pin(os_thread *h, const char *name) {
    const uint64_t reserved = hos_reserved_cpus();
    if (reserved) {
        const uint64_t proc = os_process_affinity();
        if (proc & ~reserved) os_thread_set_affinity(h, proc & ~reserved);
    }
    static NameSpec pins[24], prios[24];
    static int npins = -1, nprios;
    static os_mutex lk = OS_MUTEX_INIT;
    os_mutex_lock(&lk);
    if (npins < 0) {
        npins = parse_name_specs(getenv("HOS_PIN"), pins, 24, 16);
        nprios = parse_name_specs(getenv("HOS_PRIO"), prios, 24, 10);
    }
    os_mutex_unlock(&lk);
    const NameSpec *p = match_name_spec(pins, npins, name);
    if (p && p->value) os_thread_set_affinity(h, (uint64_t)p->value);
    if ((p = match_name_spec(prios, nprios, name)) && p->value >= -2 && p->value <= 2)
        os_thread_set_priority(h, (int)p->value);   /* (OS_PRIO_LOWEST -2 .. OS_PRIO_HIGHEST 2) */
    else if (strstr(name, "OptimizeChange") && !getenv("HOS_OCB_NORMAL")) {
        /* (OoT's OptimizeChangeBank (guest priority 27, a background thread) walks every placed object of the
           area by name, again and again (Kokiri: ~344 names, ~250k lookups a second). On the console it only gets the
           time nothing more important wants; at the host's normal priority it took the renderer's and the driver's
           cores and the frame waited (Kokiri, RX 6600 + 5600G: 17-20 fps; lowest: 21-28). HOS_OCB_NORMAL=1: as before) */
        os_thread_set_priority(h, OS_PRIO_LOWEST);
    }
}

/* ---- time ------------------------------------------------------------------------------------------------ */
uint64_t hos_ticks(void) { return recomp_cntpct(NULL); }

int64_t hos_now_ns(void) { return os_now_ns(); }

/* HOS_SLEEP_SPIN=1 (comparison): a sleep under a millisecond yields until its time, as the Windows build had to (its
   timers are ~0.5 ms coarse), instead of a timed sleep (os.h's: no rounding, a 1 ns timer slack) */
static int sleep_spin(void) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_SLEEP_SPIN") && atoi(getenv("HOS_SLEEP_SPIN")) != 0;
    return on;
}
void hos_sleep_ns(int64_t ns) {
    if (ns <= 0) {   /* (the three yield variants: every guest thread has a host thread of its own) */
        os_yield();
        return;
    }
    const int away = hos_block_begin(CRIT_BLOCK_SLEEP, (const void *)(uintptr_t)ns);
    if (ns < 1000000 && sleep_spin()) {
        const int64_t until = os_now_ns() + ns;
        while (os_now_ns() < until) os_yield();
    } else {
        os_sleep_ns(ns);
    }
    if (away) hos_block_end();
}

/* ---- nn::os threads -------------------------------------------------------------------------------------- */
static HosThread *thread_arg(Ctx *c, uint64_t tt) {
    HosThread *t = hos_thread_of(tt);
    if (!t) hos_fatal(c, "unknown nn::os::ThreadType %#llx", (unsigned long long)tt);
    return t;
}

/* nn::os::CreateThread(ThreadType*, void (*)(void*), void* arg, void* stack, unsigned long size, int prio) */
HLE(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi) {
    HOS_TRACE_CALL("CreateThread");
    hos_thread_create(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (int)ARG(5), -2);
    RET(0);
}

/* nn::os::CreateThread(ThreadType*, void (*)(void*), void* arg, void* stack, unsigned long size, int prio,
   int idealCore) (hookshot: OoT's Porter thread pool uses this overload; left to the guest SDK it reached
   ThreadManagerImplByHorizon::MapAliasStack, which needs svcMapMemory, and aborted) */
HLE(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii) {
    HOS_TRACE_CALL("CreateThread");
    hos_thread_create(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (int)ARG(5), (int)ARG(6));
    RET(0);
}

HLE(_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE) {
    HOS_TRACE_CALL("StartThread");
    hos_thread_start(thread_arg(c, ARG(0)));
}

HLE(_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE) {
    HOS_TRACE_CALL("WaitThread");
    hos_thread_join(thread_arg(c, ARG(0)));
}

HLE(_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE) {
    HOS_TRACE_CALL("DestroyThread");
    HosThread *t = thread_arg(c, ARG(0));
    hos_thread_join(t);
    hos_thread_destroy(t);
}

HLE(_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc) {
    HosThread *t = thread_arg(c, ARG(0));
    hos_thread_set_name(t, ARG(1) ? (const char *)GP(ARG(1)) : "");
    if (hos_trace) hos_log("SetThreadName %s", t->name);
}

HLE(_ZN2nn2os16GetCurrentThreadEv) { RET(cur->guest_tt); }
HLE(_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE) { RET(thread_arg(c, ARG(0))->id); }
HLE(_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE) { RET((uint32_t)hos_thread_priority(thread_arg(c, ARG(0)), 0, 0)); }
HLE(_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi) {
    RET((uint32_t)hos_thread_priority(thread_arg(c, ARG(0)), 1, (int)ARG(1)));
}

/* nn::os::SetThreadCoreMask(ThreadType*, int idealCore, unsigned long mask) */
HLE(_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim) {
    HosThread *t = thread_arg(c, ARG(0));
    hos_thread_affinity(t, (int)ARG(1), ARG(2));
}

HLE(_ZN2nn2os20GetCurrentCoreNumberEv) { RET((uint32_t)hos_current_core(c->x[30])); }
HLE(_ZN2nn2os26GetThreadAvailableCoreMaskEv) { RET((1u << hos_cores()) - 1); }   /* cores 0-2: what an application gets */

HLE(_ZN2nn2os19GetCurrentStackInfoEPmS1_) {
    if (ARG(0)) G64(ARG(0)) = cur->stack;
    if (ARG(1)) G64(ARG(1)) = cur->stack_size;
}

HLE(_ZN2nn2os11YieldThreadEv) { os_yield(); }
/* HOS_SLEEP_LOG=1 (diagnostic): guest sleeps by caller (LR) every 5 s: count, asked, slept */
void hos_sleep_logged(int64_t ns, uint64_t lr) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_SLEEP_LOG") != NULL;
    if (!on) { hos_sleep_ns(ns); return; }
    const int64_t t0 = hos_now_ns();
    hos_sleep_ns(ns);
    const int64_t took = hos_now_ns() - t0;
    static os_mutex lk = OS_MUTEX_INIT;
    static struct { uint64_t lr; int64_t n, asked, slept; } tab[64];
    static int64_t last;
    os_mutex_lock(&lk);
    int i = 0;
    while (i < 64 && tab[i].lr && tab[i].lr != lr) i++;
    if (i < 64) {
        tab[i].lr = lr;
        tab[i].n++;
        tab[i].asked += ns > 0 ? ns : 0;
        tab[i].slept += took;
    }
    const int64_t now = hos_now_ns();
    if (now - last > 5000000000LL) {
        last = now;
        for (int k = 0; k < 64 && tab[k].lr; k++)
            if (tab[k].n)
                hos_log("sleep: lr %#llx: %lld calls, asked %.2f ms, slept %.2f ms (%.1f us each)",
                        (unsigned long long)tab[k].lr, (long long)tab[k].n, tab[k].asked / 1e6, tab[k].slept / 1e6,
                        tab[k].slept / 1e3 / tab[k].n);
        memset(tab, 0, sizeof tab);
    }
    os_mutex_unlock(&lk);
}

/* (hookshot) OoT's present waits for its frame's GPU work by polling an NVN event, sleeping ~1.6 ms between looks
   (f_805902c0 frame_present_submit at 0x80590500: `while (nvnEventGetValue(e) != 1) SleepThread(...)`, ~3.5 sleeps a frame, 2 ms each on Windows): the
   frame noticed the GPU's signal late, ~7 ms of the 25 ms frame asleep on its critical path. The GPU writes the event
   into guest memory itself, so the sleep looks at it between yields and ends once it is
   1, at the latest when the asked time is up. Only where the poll is nvnEventGetValue. HOS_PRESENT_POLL=1 (an
   experiment: no fps change measured, and it keeps a core busy while it looks) */
static int present_poll(int64_t ns, uint64_t x22, uint64_t x23, uint64_t x24) {
    static int on = -1;
    static uint64_t fn_ok, fn_bad;
    if (on < 0) on = getenv("HOS_PRESENT_POLL") && atoi(getenv("HOS_PRESENT_POLL")) != 0;
    if (!on || ns <= 0 || ns > 20000000) return 0;
    const uint64_t fp = G64(x24);
    if (fp != fn_ok) {
        if (fp == fn_bad) return 0;
        const char *hos_hostcall_name(uint64_t addr);
        const char *n = hos_hostcall_name(fp);
        if (!n || strcmp(n, "nvnEventGetValue")) { fn_bad = fp; return 0; }
        fn_ok = fp;
        hos_log("sleep: OoT's present poll looks at its event between yields (HOS_PRESENT_POLL)");
    }
    int hos_nvn_event_value(uint64_t guest_event, uint32_t *v);
    const uint64_t ev = x23 + (uint64_t)G32(x22 + 0x164) * 64;
    uint32_t v = 0;
    if (!hos_nvn_event_value(ev, &v)) return 0;
    const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)8);
    const int64_t end = hos_now_ns() + ns;
    while (v != 1 && hos_now_ns() < end) {
        os_yield();
        for (int i = 0; i < 64; i++) _mm_pause();
        hos_nvn_event_value(ev, &v);
    }
    if (away) hos_block_end();
    return 1;
}
/* (an At row in place of f_805902c0's SleepThread call at 0x8059051c, the present's event poll: x0 the time, x22-x24
   the poll loop's registers: x24 the address of the poll function, x23 + 64 * u32 [x22 + 0x164] its event) */
PATCH_AT(0x8059051c, word(0x94db4a31), instead, none, hos_oot_present_sleep, as(x0, ns), x22, x23, x24) {
    if (present_poll(ns, x22, x23, x24)) return;
    static int sleep_log = -1;
    if (sleep_log < 0) sleep_log = getenv("HOS_SLEEP_LOG") != NULL;
    if (sleep_log) {   /* (hookshot, diagnostic: OoT's present poll) */
        static int logged;
        if (logged++ < 3) {
            const char *hos_hostcall_name(uint64_t addr);
            const uint64_t fp = G64(x24), sync = x23 + (uint64_t)G32(x22 + 0x164) * 64;
            const char *n = hos_hostcall_name(fp);
            hos_log("sleep: OoT's present poll: fn %#llx (%s), arg %#llx, sleep %lld ns", (unsigned long long)a64_guest_of(fp),
                    n ? n : "guest code", (unsigned long long)sync, (long long)ns);
        }
    }
    hos_sleep_logged(ns, 0x80590520);
}
/* (hookshot, OoT) The calc jobs' runner f_800998a0 (label job_calc_runner: main and ModuleSystemWorker1-5 in calc stage C,
   docs/direct/frame-loop.md) waits for its sub-stages by SleepThread(20 us) and looking again, at three
   sites, ~30 times a frame on each of its six threads: the five others wait while one thread builds the occlusion grid
   (~0.5 ms) and while the last shape items finish. A sleep under a millisecond was yield-spun to its time (hos_sleep_ns), so
   the six threads stayed on the CPU for the whole phase: 4 of its 10 ms of CPU a frame (market, uncapped). Here it is
   a timed sleep with a 1 us timer slack (the thread's slack is set once; Linux's default 50 us would stretch each
   look): the phase keeps its wall time and uses ~3.9 ms less CPU a frame. Linux only
   (Windows' timers are ~0.5 ms coarse). HOS_CALC_NAP=0: spun as before. The three SleepThread calls (bl at 0x8009a474,
   0x8009afec, 0x8009b230) are replaced by At rows (PATCH_AT ... instead, below) calling hos_oot_calc_sleep: SleepThread
   itself is the same for every caller */
static int calc_poll_nap(int64_t ns) {
    static int on = -1;
    if (on < 0) on = !(getenv("HOS_CALC_NAP") && atoi(getenv("HOS_CALC_NAP")) == 0);
    if (!on || ns <= 0 || ns >= 1000000) return 0;
    const int away = hos_block_begin(CRIT_BLOCK_SLEEP, (const void *)(uintptr_t)ns);
    os_sleep_ns(ns);   /* (os.h: the thread's timer slack is 1 ns) */
    if (away) hos_block_end();
    return 1;
}

/* (At rows in place of f_800998a0 job_calc_runner's three SleepThread calls: x0 the time; lr the call's return address,
   for HOS_SLEEP_LOG) */
PATCH_AT(0x8009a474, word(0x94ef225b), instead, none, hos_oot_calc_sleep, as(x0, ns), as(k(0x8009a478), lr)) {
    if (!calc_poll_nap(ns)) hos_sleep_logged(ns, lr);
}
PATCH_AT_SITE(0x8009afec, word(0x94ef1f7d), instead, none, hos_oot_calc_sleep, x0, k(0x8009aff0));
PATCH_AT_SITE(0x8009b230, word(0x94ef1eec), instead, none, hos_oot_calc_sleep, x0, k(0x8009b234));

HLE(_ZN2nn2os11SleepThreadENS_8TimeSpanE) { hos_sleep_logged((int64_t)ARG(0), c->x[30]); }

/* ---- TLS slots --------------------------------------------------------------------------------------------- */
static volatile int32_t slot_used[HOS_TLS_SLOTS];

/* nn::os::AllocateTlsSlot(TlsSlot*, void (*)(unsigned long)) -> Result */
HLE(_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE) {
    for (int i = 0; i < HOS_TLS_SLOTS; i++) {
        if (!os_atomic_cas(&slot_used[i], 0, 1)) {
            G32(ARG(0)) = (uint32_t)i;
            RET(0);
            return;
        }
    }
    RET(0x2C01);   /* os::ResultOutOfResource */
}

HLE(_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE) { if ((uint32_t)ARG(0) < HOS_TLS_SLOTS) slot_used[(uint32_t)ARG(0)] = 0; }
HLE(_ZN2nn2os11GetTlsValueENS0_7TlsSlotE) { RET((uint32_t)ARG(0) < HOS_TLS_SLOTS ? cur->tls_slots[(uint32_t)ARG(0)] : 0); }
HLE(_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm) { if ((uint32_t)ARG(0) < HOS_TLS_SLOTS) cur->tls_slots[(uint32_t)ARG(0)] = ARG(1); }

/* ---- ticks ------------------------------------------------------------------------------------------------- */
volatile uint64_t hos_gfx_module;
/* HOS_TICK_LOG=1 (diagnostic): who reads the clock, by caller (LR), every 5 s */
static void tick_log(uint64_t lr) {
    static os_mutex lk = OS_MUTEX_INIT; static uint64_t lrs[512]; static uint32_t counts[512]; static int64_t next;
    os_mutex_lock(&lk);
    for (int i = (int)((lr >> 2) * 2654435761u >> 8) & 511, n = 0; n < 512; n++, i = (i + 1) & 511)
        if (!lrs[i] || lrs[i] == lr) { lrs[i] = lr; counts[i]++; break; }
    const int64_t now = hos_now_ns();
    if (now > next) {
        next = now + 5000000000LL;
        char line[1500]; int n = 0;
        for (int k = 0; k < 40 && n < 1400; k++) {
            int best = -1;
            for (int i = 0; i < 512; i++) if (counts[i] && (best < 0 || counts[i] > counts[best])) best = i;
            if (best < 0) break;
            n += snprintf(line + n, sizeof line - n, " %llx:%u", (unsigned long long)lrs[best], counts[best]);
            counts[best] = 0;
        }
        memset(counts, 0, sizeof counts);
        hos_log("tick readers in 5 s:%s", line);
    }
    os_mutex_unlock(&lk);
}
/* (an At row in place of f_8348c680 frame_body's GetSystemTick call at 0x8348c76c, its frame-end clock read: x19 the
   graphics module, for nn_vi.c's real-time step) the tick, as GetSystemTick */
PATCH_AT(0x8348c76c, word(0x941f5619), instead, x0, hos_oot_frame_end_tick, as(x19, module)) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_TICK_LOG") != NULL;
    if (on) tick_log(0x8348c770);
    hos_gfx_module = module;
    return hos_ticks();
}
HLE(_ZN2nn2os13GetSystemTickEv) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_TICK_LOG") != NULL;
    if (on) tick_log(c->x[30]);
    RET(hos_ticks());
}
HLE(_ZN2nn2os22GetSystemTickFrequencyEv) { RET(HOS_TICK_HZ); }
/* (hookshot) At the counter's own rate, HOS_TICK_HZ (31.25 MHz: 32 ns a tick), the rate GetSystemTick counts (the Switch
   1's 19.2 MHz made a tick span 1.63x too long: OoT's present slept 1.63 ms for its 1 ms poll step, 0x80590500) */
HLE(_ZN2nn2os13ConvertToTickENS_8TimeSpanE) {
    int64_t ns = (int64_t)ARG(0);
    RET(ns / (int64_t)(1000000000ULL / HOS_TICK_HZ));
}
HLE(_ZN2nn2os17ConvertToTimeSpanENS0_4TickE) {
    int64_t t = (int64_t)ARG(0);
    RET(t * (int64_t)(1000000000ULL / HOS_TICK_HZ));
}

/* ---- fibers (hookshot: nn::async's FiberTaskCoordinator, on OoT's nn.npln threads) ----------------------------
   A guest fiber is a host fiber (os.h) with its own guest context: the lifted frames on its host stack keep using that
   context wherever it runs, so a fiber may resume on another thread. Switching in, it takes the running thread's
   identity (TLS registers, host thread object); the thread's own context stays in HosThread.ctx. The fiber function
   returns the FiberType to switch to next (nn::os semantics). */
typedef struct HosFiber {
    Ctx ctx;   /* (first: a context's fiber is found from its address) */
    uint64_t ft, fn, arg, stack, size;
    os_fiber *host;
    int started, done;
    struct HosFiber *next;
} HosFiber;
static os_rwlock fibers_lock = OS_RWLOCK_INIT;
static HosFiber *fibers;

static HosFiber *fiber_of_ft(uint64_t ft) {
    os_rwlock_lock_shared(&fibers_lock);
    HosFiber *f = fibers;
    while (f && f->ft != ft) f = f->next;
    os_rwlock_unlock_shared(&fibers_lock);
    return f;
}
static HosFiber *fiber_of_ctx(Ctx *c) {
    os_rwlock_lock_shared(&fibers_lock);
    HosFiber *f = fibers;
    while (f && &f->ctx != c) f = f->next;
    os_rwlock_unlock_shared(&fibers_lock);
    return f;
}
static void fiber_switch(Ctx *from, HosFiber *to);

static void fiber_main(void *p) {
    HosFiber *f = (HosFiber *)p;
    Ctx *c = &f->ctx;
    c->x[0] = f->arg;
    c->x[30] = 0;
    c->x[31] = (f->stack + f->size) & ~15ULL;
    a64_fpcr_sync(c);   /* (the FPCR it inherited at its first switch; the host fiber starts from another MXCSR) */
    a64_call_received(c, f->fn);   /* FiberType* (*)(void*) */
    const uint64_t next = c->x[0];
    f->done = 1;
    HosFiber *to = next ? fiber_of_ft(next) : NULL;   /* (NULL: back to the thread's own context, as nn::os does) */
    if (next && !to) hos_fatal(c, "fiber %#llx returned %#llx: not an initialized fiber", (unsigned long long)f->ft,
                               (unsigned long long)next);
    fiber_switch(c, to);
    hos_fatal(c, "fiber %#llx: switched to after it returned", (unsigned long long)f->ft);
}

/* from: the context calling (the thread's own or a fiber's); to: NULL for the thread's own context */
static void fiber_switch(Ctx *from, HosFiber *to) {
    HosThread *t = cur;
    if (!t->host_fiber) {
        t->host_fiber = os_fiber_from_thread();
        if (!t->host_fiber) hos_fatal(from, "the thread's host fiber could not be made");
    }
    Ctx *dst = to ? &to->ctx : &t->ctx;
    if (dst == from) return;
    if (to) {
        if (to->done) hos_fatal(from, "SwitchToFiber(%#llx): that fiber has returned", (unsigned long long)to->ft);
        if (!to->started) {   /* the thread's state it inherits: FPCR, then the identity below */
            to->started = 1;
            to->ctx.fpcr = from->fpcr;
            to->ctx.fpsr = from->fpsr;
        }
    }
    /* the running thread's identity, whoever ran this context last */
    dst->tpidr_el0 = t->ctx.tpidr_el0;
    dst->tpidrro_el0 = t->ctx.tpidrro_el0;
    dst->thread = t->ctx.thread;
    t->run_ctx = to ? &to->ctx : NULL;
    os_fiber_switch(to ? to->host : t->host_fiber);
    /* back in `from`, maybe on another thread: take that one's identity (fiber_switch's caller set it already) */
    a64_fpcr_sync(from);   /* (and its FP mode: the other context may have changed MXCSR; not every platform's fiber
                              switch keeps it) */
}

/* InitializeFiber(FiberType*, FiberType* (*)(void*), void* arg, void* stack, unsigned long size, int flags) */
HLE(_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi,
    clobbers("deletes a host fiber (os_fiber_delete), which may be the running one")) {
    HosFiber *f = fiber_of_ft(ARG(0));
    if (!f) {
        f = (HosFiber *)calloc(1, sizeof *f);
        f->ft = ARG(0);
        os_rwlock_lock(&fibers_lock);
        f->next = fibers, fibers = f;
        os_rwlock_unlock(&fibers_lock);
    } else if (f->host) {
        os_fiber_delete(f->host);
    }
    memset(&f->ctx, 0, sizeof f->ctx);
    f->fn = ARG(1), f->arg = ARG(2), f->stack = ARG(3), f->size = ARG(4);
    f->started = f->done = 0;
    f->host = os_fiber_create(64 << 20, fiber_main, f);
    if (!f->host) hos_fatal(c, "the host fiber could not be made");
    static volatile int32_t logged;
    if (os_atomic_inc(&logged) <= 8 || hos_trace)
        hos_log("fiber %#llx: function %#llx arg %#llx stack %#llx+%#llx flags %d", (unsigned long long)f->ft,
                (unsigned long long)a64_guest_of(f->fn), (unsigned long long)f->arg, (unsigned long long)f->stack,
                (unsigned long long)f->size, (int)ARG(5));
}

HLE(_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE,
    clobbers("deletes a host fiber (os_fiber_delete), which may be the running one")) {
    HosFiber *f = fiber_of_ft(ARG(0));
    if (f && f->host && &f->ctx != c) { os_fiber_delete(f->host); f->host = NULL; f->done = 1; }
}

HLE(_ZN2nn2os15GetCurrentFiberEv) {
    HosFiber *f = c == &cur->ctx ? NULL : fiber_of_ctx(c);
    RET(f ? f->ft : 0);
}

/* SwitchToFiber(FiberType*): NULL goes back to the thread's own context */
HLE(_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE,
    clobbers("context switch: the call returns when another context switches back (fiber_switch)")) {
    HosFiber *to = NULL;
    if (ARG(0) && !(to = fiber_of_ft(ARG(0))))
        hos_fatal(c, "SwitchToFiber(%#llx): not an initialized fiber", (unsigned long long)ARG(0));
    fiber_switch(c, to);
}

/* Every thread's kernel handle (the tag its SDK mutexes hold), for hos_kernel_dump. */
void hos_thread_log_handles(void) {
    os_rwlock_lock_shared(&threads_lock);
    /* (lr: the return address of the thread's last call, x0 its first argument: in a wait, which game code waits on
       what; the web has no stack walk) */
    for (int i = 0; i < nthreads; i++)
        hos_log("kernel:   handle %#x = %s: lr %#llx x0 %#llx x19 %#llx x20 %#llx x21 %#llx x22 %#llx",
                threads[i]->kernel_handle, threads[i]->name, (unsigned long long)threads[i]->ctx.x[30],
                (unsigned long long)threads[i]->ctx.x[0], (unsigned long long)threads[i]->ctx.x[19],
                (unsigned long long)threads[i]->ctx.x[20], (unsigned long long)threads[i]->ctx.x[21],
                (unsigned long long)threads[i]->ctx.x[22]);
    os_rwlock_unlock_shared(&threads_lock);
}

/* ---- no scheduler ---------------------------------------------------------------------------------------------
   Every guest thread runs on its own host thread whenever the host schedules it; nothing in the host keeps two guest
   threads from running at once. On the console threads of one priority on one core run cooperatively (the kernel
   rotates a priority's queue only at its preemption priorities, 59 and 63, and none of the game's threads sits there)
   and a better thread preempts at any instruction. The one structure found that relies on one thread per core number is
   the actor job system's per-core rings (pushed and popped without a lock); host/oot_probes.c asserts that each ring has
   one writer (hos_oot_ring_write). */

/* A block (HOS_DBG=crit: direct_crit.h): its start, what it waits on and its guest caller; 1 if recorded, and then
   hos_block_end records the span */
int hos_block_begin(int why, const void *key) {
    HosThread *t = cur;
    if (!crit_rec || !t) return 0;
    crit_emit_flush();
    crit_why = why, crit_key = (uint64_t)(uintptr_t)key, crit_away_t = crit_tsc();
    crit_away_lr = (t->run_ctx ? t->run_ctx : &t->ctx)->x[30];
    return 1;
}
void hos_block_end(void) {
    if (crit_away_t) crit_span(crit_why, crit_away_t, crit_tsc(), crit_key, crit_away_lr);
    crit_why = CRIT_BLOCK_OTHER, crit_away_t = 0;
}
/* os_cond_wait for guest threads (the block recorded for HOS_DBG=crit) */
int hos_cv_sleep(os_cond *cv, os_mutex *lk, int64_t timeout_ns) {
    const int rec = hos_block_begin(CRIT_BLOCK_CV, cv);
    const int r = os_cond_wait(cv, lk, timeout_ns);
    if (rec) hos_block_end();
    return r;
}

/* the running guest thread's value in a TLS slot (what nn::os::GetTlsValue answers it) */
uint64_t hos_tls_value(uint32_t slot) { return cur && slot < HOS_TLS_SLOTS ? cur->tls_slots[slot] : 0; }
