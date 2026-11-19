/* os.h on Windows (Win32, the UCRT). Depends on nothing of the runtime's: kernel32, the WaitOnAddress words
   (synchronization), psapi and winmm.

   What differs from os_linux.c:
   - the futex words sleep with WaitOnAddress, which has no wake bitsets: the reader/writer lock wakes every sleeper
     of the word and each looks again (the same states as Linux's, only the wake is wider), and a timed wait's
     deadline is in whole milliseconds, rounded up (the process asks for 1 ms timer ticks at start);
   - sleeps take a high-resolution waitable timer of the thread's own, so a 100 us sleep is not a 1 ms one;
   - faults come through a vectored handler (os_fault_add's handlers); the crash report (os_fault_set_last) is the
     unhandled-exception filter, so an exception a library raises and catches itself (SEH) never reaches it;
   - paths are UTF-8 with '/' (Win32 takes either separator) and converted to UTF-16 at each call. */
#if defined(_WIN32)   /* (sources.txt lists both systems' files: each compiles on its own) */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "os.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <io.h>
#include <locale.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "synchronization.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

__declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int ms);

static int err_of(DWORD e) {
    switch (e) {
    case ERROR_SUCCESS: return OS_OK;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_NETPATH:
    case ERROR_DIRECTORY: return OS_ENOENT;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return OS_EEXIST;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_WRITE_PROTECT:
        return OS_EACCES;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT: return OS_ENOMEM;
    case ERROR_TIMEOUT: case WAIT_TIMEOUT: return OS_ETIMEDOUT;
    case ERROR_DIR_NOT_EMPTY: return OS_ENOTEMPTY;
    case ERROR_INVALID_PARAMETER: return OS_EINVAL;
    default: return OS_EFAIL;
    }
}
static int last_err(void) { return err_of(GetLastError()); }

/* ---- UTF-8 <-> UTF-16 ---- */
/* the path as UTF-16 into out (cap characters): false when it does not fit or is not UTF-8 */
static bool wide(const char *s, wchar_t *out, int cap) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap);
    return n > 0;
}
/* UTF-16 as UTF-8 with '\' made '/': the length, 0 when it does not fit */
static int narrow(const wchar_t *s, char *out, size_t cap) {
    if (!cap) return 0;
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, (int)(cap > 0x7fffffff ? 0x7fffffff : cap), NULL, NULL);
    if (n <= 0) { out[0] = 0; return 0; }
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
    return n - 1;
}
#define WPATH(var, path) wchar_t var[OS_PATH_MAX]; if (!wide((path), var, OS_PATH_MAX))

/* ---- time ---- */
static int64_t qpc_freq;
static void time_init(void) {
    if (qpc_freq) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpc_freq = f.QuadPart;
}
int64_t os_now_ns(void) {
    if (!qpc_freq) time_init();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    /* (split, so the multiplication does not overflow: the counter runs at 10 MHz on current systems) */
    const int64_t s = c.QuadPart / qpc_freq, r = c.QuadPart % qpc_freq;
    return s * 1000000000LL + r * 1000000000LL / qpc_freq;
}
/* 1 ms scheduler ticks for the process (timed waits and Sleep end on a tick), and the counter's frequency, before main */
__attribute__((constructor)) static void precise_timers(void) {
    time_init();
    timeBeginPeriod(1);
}
static _Thread_local HANDLE sleep_timer;
void os_sleep_ns(int64_t ns) {
    if (ns <= 0) { SwitchToThread(); return; }
    if (!sleep_timer) {
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!sleep_timer) sleep_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due;
    due.QuadPart = -(ns + 99) / 100;   /* (relative, in 100 ns) */
    if (sleep_timer && SetWaitableTimerEx(sleep_timer, &due, 0, NULL, NULL, NULL, 0)) {
        WaitForSingleObject(sleep_timer, INFINITE);
        return;
    }
    Sleep((DWORD)((ns + 999999) / 1000000));
}
void os_yield(void) { SwitchToThread(); }
/* (os_crt_windows.h's clock_gettime, for the host's C that calls it) */
int os_crt_clock_gettime(int clock, struct timespec *ts) {
    (void)clock;
    const int64_t ns = os_now_ns();
    ts->tv_sec = (time_t)(ns / 1000000000LL), ts->tv_nsec = (long)(ns % 1000000000LL);
    return 0;
}

#define os_pause() _mm_pause()
#include <immintrin.h>

/* ---- futex words: WaitOnAddress ---- */
static int64_t deadline_of(int64_t timeout_ns) { return timeout_ns < 0 ? -1 : os_now_ns() + timeout_ns; }
static int past(int64_t deadline) { return deadline >= 0 && os_now_ns() >= deadline; }
/* sleeps while *w == expected, until the monotonic deadline (< 0: none): 0 or OS_ETIMEDOUT */
static int wait_until(volatile uint32_t *w, uint32_t expected, int64_t deadline) {
    DWORD ms = INFINITE;
    if (deadline >= 0) {
        const int64_t left = deadline - os_now_ns();
        if (left <= 0) return OS_ETIMEDOUT;
        ms = (DWORD)((left + 999999) / 1000000);
    }
    if (WaitOnAddress((volatile void *)w, &expected, sizeof expected, ms)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? OS_ETIMEDOUT : 0;
}
static void wake_one(volatile uint32_t *w) { WakeByAddressSingle((void *)w); }
static void wake_all(volatile uint32_t *w) { WakeByAddressAll((void *)w); }

/* ---- mutex: Drepper's three states, as os_linux.c ---- */
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
void os_mutex_unlock(os_mutex *m) {
    if (os_atomic_fetch_add(&m->state, (uint32_t)-1) != 1) {
        os_atomic_store(&m->state, 0u);
        wake_one(&m->state);
    }
}

/* ---- reader/writer lock: os_linux.c's states. WaitOnAddress has no bitsets, so every wake is of all sleepers of
   the word (readers and writers alike), and each looks at the state again ---- */
#define RW_READERS 0x0000ffffu
#define RW_WW1 0x00010000u
#define RW_WWAIT 0x1fff0000u
#define RW_WRITER 0x40000000u
#define RW_RSLEEP 0x80000000u
void os_rwlock_lock_shared(os_rwlock *l) {
    int spins = SPINS;
    for (;;) {
        uint32_t s = os_atomic_load(&l->state);
        if (!(s & (RW_WRITER | RW_WWAIT))) {
            if (os_atomic_cas(&l->state, s, s + 1) == s) return;
            continue;
        }
        if (spins-- > 0) { os_pause(); continue; }
        if (!(s & RW_RSLEEP)) {
            if (os_atomic_cas(&l->state, s, s | RW_RSLEEP) != s) continue;
            s |= RW_RSLEEP;
        }
        wait_until(&l->state, s, -1);
    }
}
void os_rwlock_unlock_shared(os_rwlock *l) {
    const uint32_t s = os_atomic_fetch_add(&l->state, (uint32_t)-1);
    if ((s & RW_READERS) == 1 && (s & RW_WWAIT)) wake_all(&l->state);   /* (the last reader out: a writer's turn) */
}
void os_rwlock_lock(os_rwlock *l) {
    uint32_t s = os_atomic_load(&l->state);
    if (!(s & (RW_WRITER | RW_READERS)) && os_atomic_cas(&l->state, s, s | RW_WRITER) == s) return;
    os_atomic_add(&l->state, RW_WW1);
    int spins = SPINS;
    for (;;) {
        s = os_atomic_load(&l->state);
        if (!(s & (RW_WRITER | RW_READERS))) {
            if (os_atomic_cas(&l->state, s, (s - RW_WW1) | RW_WRITER) == s) return;
            continue;
        }
        if (spins-- > 0) { os_pause(); continue; }
        wait_until(&l->state, s, -1);
    }
}
void os_rwlock_unlock(os_rwlock *l) {
    uint32_t s = os_atomic_load(&l->state);
    for (;;) {
        const uint32_t to = s & RW_WWAIT ? s & ~RW_WRITER : s & ~(RW_WRITER | RW_RSLEEP);
        const uint32_t was = os_atomic_cas(&l->state, s, to);
        if (was == s) break;
        s = was;
    }
    if (s & (RW_WWAIT | RW_RSLEEP)) wake_all(&l->state);
}

/* ---- condition variable, once, event: os_linux.c's ---- */
int os_cond_wait(os_cond *c, os_mutex *m, int64_t timeout_ns) {
    os_atomic_inc(&c->waiters);
    const uint32_t seq = os_atomic_load(&c->seq);
    os_mutex_unlock(m);
    const int r = wait_until(&c->seq, seq, deadline_of(timeout_ns));
    os_mutex_lock(m);
    os_atomic_dec(&c->waiters);
    return r;
}
void os_cond_signal(os_cond *c) {
    if (!os_atomic_load(&c->waiters)) return;
    os_atomic_inc(&c->seq);
    wake_one(&c->seq);
}
void os_cond_broadcast(os_cond *c) {
    if (!os_atomic_load(&c->waiters)) return;
    os_atomic_inc(&c->seq);
    wake_all(&c->seq);
}
void os_once_run(os_once *o, void (*fn)(void *arg), void *arg) {
    uint32_t s = os_atomic_load(&o->state);
    if (s == 3) return;
    if (s == 0 && os_atomic_cas(&o->state, 0u, 1u) == 0) {
        fn(arg);
        if (os_atomic_xchg(&o->state, 3u) == 2) wake_all(&o->state);
        return;
    }
    while ((s = os_atomic_load(&o->state)) != 3) {
        if (s == 1 && os_atomic_cas(&o->state, 1u, 2u) != 1) continue;
        wait_until(&o->state, 2, -1);
    }
}
void os_event_init(os_event *e, bool manual, bool set) {
    e->manual = manual;
    e->waiters = 0;
    os_atomic_store(&e->state, set ? 1u : 0u);
}
void os_event_set(os_event *e) {
    os_atomic_store(&e->state, 1u);
    if (os_atomic_load(&e->waiters)) e->manual ? wake_all(&e->state) : wake_one(&e->state);
}
static bool os_event_try(os_event *e) {
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

/* ---- threads ---- */
struct os_thread {
    HANDLE th;   /* (a real handle, also for a thread this file did not start) */
    volatile uint32_t tid;
    os_event done;
    volatile int refs;
    int priority;
    os_thread_fn fn;
    void *arg;
    char name[64];
};
static _Thread_local os_thread *self_thread;
uint32_t os_tid(void) { return (uint32_t)GetCurrentThreadId(); }
static int64_t filetime_ns(FILETIME f) { return (int64_t)(((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime) * 100); }
int64_t os_self_cpu_ns(void) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return filetime_ns(k) + filetime_ns(u);
}
void os_thread_retain(os_thread *t) { os_atomic_inc(&t->refs); }
void os_thread_release(os_thread *t) {
    if (!t || os_atomic_dec(&t->refs) != 0) return;
    if (t->th) CloseHandle(t->th);
    free(t);
}
/* (room on the stack for the fault handlers when a thread overflows its stack) */
static void stack_guarantee(void) {
    ULONG g = 64 << 10;
    SetThreadStackGuarantee(&g);
}
static void set_name_of(HANDLE th, const char *name) {
    wchar_t w[64];
    if (wide(name, w, 64)) SetThreadDescription(th, w);
}
os_thread *os_thread_current(void) {
    if (!self_thread) {
        stack_guarantee();
        os_thread *t = (os_thread *)calloc(1, sizeof *t);
        os_event_init(&t->done, true, false);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->th, 0, FALSE, DUPLICATE_SAME_ACCESS);
        t->tid = os_tid();
        t->refs = 1;
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
static DWORD WINAPI thread_entry(LPVOID p) {
    os_thread *t = (os_thread *)p;
    self_thread = t;
    t->tid = os_tid();
    stack_guarantee();
    if (t->name[0]) set_name_of(GetCurrentThread(), t->name);
    t->fn(t->arg);
    thread_finish();
    return 0;
}
os_thread *os_thread_start(os_thread_fn fn, void *arg, size_t stack, const char *name) {
    os_thread *t = (os_thread *)calloc(1, sizeof *t);
    if (!t) return NULL;
    os_event_init(&t->done, true, false);
    t->fn = fn, t->arg = arg, t->refs = 2;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    const size_t size = stack ? (stack + 0xFFFF) & ~(size_t)0xFFFF : (size_t)8 << 20;
    DWORD id;
    t->th = CreateThread(NULL, size, thread_entry, t, STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
    if (!t->th) { free(t); return NULL; }
    return t;
}
int os_thread_join(os_thread *t, int64_t timeout_ns) { return os_event_wait(&t->done, timeout_ns); }
uint32_t os_thread_tid(os_thread *t) {
    while (!os_atomic_load(&t->tid) && !os_event_try(&t->done)) SwitchToThread();
    return t->tid;
}
void os_thread_set_name(os_thread *t, const char *name) {
    snprintf(t->name, sizeof t->name, "%s", name);
    if (t->th) set_name_of(t->th, name);
}
void os_thread_name(os_thread *t, char *out, size_t cap) {
    if (!cap) return;
    snprintf(out, cap, "%s", t->name);
    PWSTR w = NULL;
    if (!t->name[0] && t->th && SUCCEEDED(GetThreadDescription(t->th, &w)) && w) {
        narrow(w, out, cap);
        LocalFree(w);
    }
}
bool os_thread_set_priority(os_thread *t, int prio) {
    const int p = prio <= OS_PRIO_IDLE ? THREAD_PRIORITY_IDLE : prio == OS_PRIO_LOWEST ? THREAD_PRIORITY_LOWEST
                : prio == OS_PRIO_BELOW ? THREAD_PRIORITY_BELOW_NORMAL : prio == OS_PRIO_NORMAL ? THREAD_PRIORITY_NORMAL
                : prio == OS_PRIO_ABOVE ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_HIGHEST;
    if (!t->th || !SetThreadPriority(t->th, p)) return false;
    t->priority = prio;
    return true;
}
int os_thread_priority(os_thread *t) { return t->priority; }
uint64_t os_thread_set_affinity(os_thread *t, uint64_t mask) {
    if (!mask || !t->th) return 0;
    const DWORD_PTR old = SetThreadAffinityMask(t->th, (DWORD_PTR)mask);
    return old ? (uint64_t)old : 0;
}
void os_thread_exit(void) {
    thread_finish();
    ExitThread(0);
}
void os_exit(int code) {
    fflush(NULL);
    /* (no DLL detach either: the Vulkan driver's threads would be waited for) */
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    for (;;) {}
}

int os_threads_cpu(os_threads_cpu_fn fn, void *ctx) {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 e;
    e.dwSize = sizeof e;
    int n = 0;
    for (BOOL more = Thread32First(snap, &e); more; more = Thread32Next(snap, &e)) {
        if (e.th32OwnerProcessID != pid) continue;
        const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.th32ThreadID);
        if (!h) continue;   /* (it has ended) */
        FILETIME c, x, k, u;
        char name[64] = "";
        PWSTR w = NULL;
        if (SUCCEEDED(GetThreadDescription(h, &w)) && w) { narrow(w, name, sizeof name); LocalFree(w); }
        if (GetThreadTimes(h, &c, &x, &k, &u)) {
            fn(ctx, (uint32_t)e.th32ThreadID, name, filetime_ns(k) + filetime_ns(u));
            n++;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return n;
}

/* ---- processors: the first processor group's (the affinity calls take a group's mask) ---- */
int os_cpu_count(void) {
    const DWORD n = GetActiveProcessorCount(0);
    return n ? (int)n : 1;
}
uint64_t os_process_affinity(void) {
    DWORD_PTR proc = 0, sys = 0;
    return GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) ? (uint64_t)proc : 0;
}
int os_cpu_cores(uint64_t *masks, int cap) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (!len) return 0;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf || !GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        free(buf);
        return 0;
    }
    int n = 0;
    for (DWORD off = 0; off < len;) {
        const PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        if (p->Relationship == RelationProcessorCore && p->Processor.GroupMask[0].Group == 0) {
            if (n < cap) masks[n] = (uint64_t)p->Processor.GroupMask[0].Mask;
            n++;
        }
        off += p->Size;
    }
    free(buf);
    return n;
}

/* ---- fibers: Win32's ---- */
struct os_fiber {
    void *fiber;
    void *arg;
    os_fiber_fn fn;
    bool thread;   /* (the thread's own context, made by ConvertThreadToFiber) */
};
static _Thread_local os_fiber *cur_fiber;
os_fiber *os_fiber_from_thread(void) {
    if (cur_fiber) return cur_fiber;
    os_fiber *f = (os_fiber *)calloc(1, sizeof *f);
    f->fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    f->thread = true;
    cur_fiber = f;
    return f;
}
static void WINAPI fiber_start(void *p) {
    os_fiber *f = (os_fiber *)p;
    f->fn(f->arg);
    os_thread_exit();
}
os_fiber *os_fiber_create(size_t stack, os_fiber_fn fn, void *arg) {
    size_t size = stack < ((size_t)1 << 20) ? (size_t)1 << 20 : stack;
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    os_fiber *f = (os_fiber *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->arg = arg, f->fn = fn;
    f->fiber = CreateFiberEx(64 << 10, size, FIBER_FLAG_FLOAT_SWITCH, fiber_start, f);
    if (!f->fiber) { free(f); return NULL; }
    return f;
}
void os_fiber_switch(os_fiber *to) {
    os_fiber *from = cur_fiber;
    if (!from || from == to) return;
    cur_fiber = to;
    SwitchToFiber(to->fiber);
}
void os_fiber_delete(os_fiber *f) {
    if (!f) return;
    if (f == cur_fiber) os_thread_exit();
    if (f->fiber && !f->thread) DeleteFiber(f->fiber);
    free(f);
}

/* ---- memory ---- */
size_t os_page_size(void) { return 4096; }
static DWORD prot_of(int p) {
    if (p & OS_PROT_X) return p & OS_PROT_W ? PAGE_EXECUTE_READWRITE : p & OS_PROT_R ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (p & OS_PROT_W) return PAGE_READWRITE;
    if (p & OS_PROT_R) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
int os_mem_protect(void *addr, size_t size, int prot) {
    const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)4095, hi = ((uintptr_t)addr + size + 4095) & ~(uintptr_t)4095;
    const DWORD p = prot_of(prot);
    /* (piece by piece: a range may mix committed and reserved pages, and VirtualProtect takes one kind) */
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) return last_err();
        uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (end > hi) end = hi;
        if (mi.State == MEM_FREE) return OS_EINVAL;
        if (mi.State == MEM_RESERVE) {
            /* (committed even as NONE: a reserved page is one os_guest_windows.c's first-touch handler would commit, so a
               guard such as the guest's null region must be committed inaccessible to keep faulting) */
            if (!VirtualAlloc((void *)a, end - a, MEM_COMMIT, p)) return last_err();
        } else {
            DWORD old;
            if (!VirtualProtect((void *)a, end - a, p, &old)) return last_err();
        }
        a = end;
    }
    return 0;
}
void os_mem_use_get(os_mem_use *out) {
    memset(out, 0, sizeof *out);
    PROCESS_MEMORY_COUNTERS_EX c;
    c.cb = sizeof c;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&c, sizeof c)) return;
    out->resident = c.WorkingSetSize;
    out->private_bytes = c.WorkingSetSize;   /* (the working set is not split by kind without a page walk) */
}
int os_mem_available(uint64_t *ram, uint64_t *swap) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return last_err();
    *ram = m.ullTotalPhys;
    *swap = m.ullTotalPageFile > m.ullTotalPhys ? m.ullTotalPageFile - m.ullTotalPhys : 0;   /* (the page file) */
    return 0;
}
int os_mem_resident(const void *addr, size_t size, uint8_t *out) {
    const size_t pages = (size + 4095) / 4096;
    enum { BATCH = 1024 };
    PSAPI_WORKING_SET_EX_INFORMATION info[BATCH];
    for (size_t i = 0; i < pages; i += BATCH) {
        const size_t n = pages - i < BATCH ? pages - i : BATCH;
        for (size_t k = 0; k < n; k++) info[k].VirtualAddress = (char *)addr + (i + k) * 4096;
        if (!QueryWorkingSetEx(GetCurrentProcess(), info, (DWORD)(n * sizeof info[0]))) return last_err();
        for (size_t k = 0; k < n; k++) out[i + k] = (uint8_t)(info[k].VirtualAttributes.Valid ? 1 : 0);
    }
    return 0;
}

/* ---- faults: a vectored handler for the handlers, the unhandled-exception filter for the last one ---- */
typedef struct { int n; os_fault_fn fn[16]; } handler_list;
static handler_list *volatile handlers;
static os_fault_fn volatile last_handler;
static os_mutex handlers_lock = OS_MUTEX_INIT;
/* the fault of an exception record, false when it is not a hardware fault (a C++ throw, a debugger's message) */
static bool fault_of(const EXCEPTION_POINTERS *x, os_fault *f) {
    memset(f, 0, sizeof *f);
    const EXCEPTION_RECORD *r = x->ExceptionRecord;
    switch (r->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_GUARD_PAGE: case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        f->kind = OS_FAULT_ACCESS;
        if (r->NumberParameters >= 2 && r->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
            f->write = r->ExceptionInformation[0] == 1;
            f->addr = (uintptr_t)r->ExceptionInformation[1];
        }
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION: f->kind = OS_FAULT_ILLEGAL; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW: f->kind = OS_FAULT_DIVIDE; break;
    case EXCEPTION_BREAKPOINT: case EXCEPTION_SINGLE_STEP: f->kind = OS_FAULT_TRAP; break;
    default: return false;
    }
    f->pc = (uintptr_t)x->ContextRecord->Rip;
    return true;
}
static LONG CALLBACK on_vectored(EXCEPTION_POINTERS *x) {
    os_fault f;
    if (!fault_of(x, &f)) return EXCEPTION_CONTINUE_SEARCH;
    const handler_list *h = __atomic_load_n(&handlers, __ATOMIC_ACQUIRE);
    for (int i = 0; h && i < h->n; i++)
        if (h->fn[i](&f) == OS_FAULT_RESUME) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
static LPTOP_LEVEL_EXCEPTION_FILTER previous_filter;
static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *x) {
    os_fault f;
    const os_fault_fn last = __atomic_load_n(&last_handler, __ATOMIC_ACQUIRE);
    if (fault_of(x, &f) && last && last(&f) == OS_FAULT_RESUME) return EXCEPTION_CONTINUE_EXECUTION;
    return previous_filter ? previous_filter(x) : EXCEPTION_CONTINUE_SEARCH;
}
static void install_handlers(void) {
    static int done;
    if (done) return;
    done = 1;
    stack_guarantee();
    AddVectoredExceptionHandler(1, on_vectored);
    previous_filter = SetUnhandledExceptionFilter(on_unhandled);
}
void os_fault_add(os_fault_fn fn, bool first) {
    os_mutex_lock(&handlers_lock);
    install_handlers();
    const handler_list *old = handlers;
    handler_list *l = (handler_list *)calloc(1, sizeof *l);
    if (l && (!old || old->n < 16)) {
        if (first) l->fn[l->n++] = fn;
        for (int i = 0; old && i < old->n; i++) l->fn[l->n++] = old->fn[i];
        if (!first) l->fn[l->n++] = fn;
        __atomic_store_n(&handlers, l, __ATOMIC_RELEASE);
    } else {
        free(l);
    }
    os_mutex_unlock(&handlers_lock);
}
void os_fault_set_last(os_fault_fn fn) {
    os_mutex_lock(&handlers_lock);
    install_handlers();
    __atomic_store_n(&last_handler, fn, __ATOMIC_RELEASE);
    os_mutex_unlock(&handlers_lock);
}

/* ---- modules ---- */
os_lib *os_lib_open(const char *path) {
    if (!path) return NULL;
    WPATH(w, path) return NULL;
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';   /* (LoadLibrary takes '\' only) */
    /* (a path: its own folder is searched for the libraries it needs, as on Linux with $ORIGIN) */
    const bool has_dir = wcschr(w, L'\\') != NULL;
    HMODULE m = LoadLibraryExW(w, NULL, has_dir ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
    return (os_lib *)m;
}
void *os_lib_sym(os_lib *l, const char *name) { return l ? (void *)GetProcAddress((HMODULE)l, name) : NULL; }
void *os_sym(const char *name) {
    void *p = (void *)GetProcAddress(GetModuleHandleW(NULL), name);
    if (p) return p;
    HMODULE mods[512];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return NULL;
    for (DWORD i = 0; i < need / sizeof mods[0] && i < 512; i++)
        if ((p = (void *)GetProcAddress(mods[i], name))) return p;
    return NULL;
}
int os_exe_path(char *out, size_t cap) {
    if (!cap) return 0;
    wchar_t w[OS_PATH_MAX];
    const DWORD n = GetModuleFileNameW(NULL, w, OS_PATH_MAX);
    if (!n || n >= OS_PATH_MAX) { out[0] = 0; return 0; }
    return narrow(w, out, cap);
}
uintptr_t os_exe_base(void) { return (uintptr_t)GetModuleHandleW(NULL); }
int os_backtrace(void **pcs, int cap) { return cap > 0 ? (int)RtlCaptureStackBackTrace(0, (DWORD)cap, pcs, NULL) : 0; }
uintptr_t os_module_of(const void *addr, char *out, size_t cap) {
    if (cap) out[0] = 0;
    HMODULE m = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m) || !m)
        return 0;
    if (cap) {
        wchar_t w[OS_PATH_MAX];
        const DWORD n = GetModuleFileNameW(m, w, OS_PATH_MAX);
        if (n && n < OS_PATH_MAX) narrow(w, out, cap);
    }
    return (uintptr_t)m;
}

/* ---- files ---- */
struct os_file { HANDLE h; };
os_file *os_file_open(const char *path, int flags, int *err) {
    WPATH(w, path) { if (err) *err = OS_ENOENT; return NULL; }
    const DWORD access = (flags & OS_READ ? GENERIC_READ : 0) | (flags & OS_WRITE ? GENERIC_WRITE : 0);
    DWORD disp = OPEN_EXISTING;
    if (flags & OS_EXCL) disp = CREATE_NEW;
    else if ((flags & OS_CREATE) && (flags & OS_TRUNC)) disp = CREATE_ALWAYS;
    else if (flags & OS_CREATE) disp = OPEN_ALWAYS;
    else if (flags & OS_TRUNC) disp = TRUNCATE_EXISTING;
    const HANDLE h = CreateFileW(w, access ? access : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (err) {
            /* (a folder: CreateFile refuses it with access denied, as Linux's open of a folder for writing) */
            const DWORD a = GetFileAttributesW(w);
            *err = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? OS_EACCES : err_of(e);
        }
        return NULL;
    }
    os_file *f = (os_file *)malloc(sizeof *f);
    f->h = h;
    if (err) *err = 0;
    return f;
}
int64_t os_file_read_at(os_file *f, void *buf, size_t n, uint64_t at) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED o;
        memset(&o, 0, sizeof o);
        const uint64_t pos = at + done;
        o.Offset = (DWORD)pos, o.OffsetHigh = (DWORD)(pos >> 32);
        const DWORD want = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done);
        DWORD got = 0;
        if (!ReadFile(f->h, (char *)buf + done, want, &got, &o)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? (int64_t)done : last_err();
        }
        if (!got) break;
        done += got;
    }
    return (int64_t)done;
}
int64_t os_file_write_at(os_file *f, const void *buf, size_t n, uint64_t at) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED o;
        memset(&o, 0, sizeof o);
        const uint64_t pos = at + done;
        o.Offset = (DWORD)pos, o.OffsetHigh = (DWORD)(pos >> 32);
        const DWORD want = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done);
        DWORD put = 0;
        if (!WriteFile(f->h, (const char *)buf + done, want, &put, &o)) return last_err();
        if (!put) return OS_EFAIL;
        done += put;
    }
    return (int64_t)done;
}
int64_t os_file_size(os_file *f) {
    LARGE_INTEGER s;
    return GetFileSizeEx(f->h, &s) ? (int64_t)s.QuadPart : last_err();
}
int os_file_resize(os_file *f, uint64_t size) {
    FILE_END_OF_FILE_INFO i;
    i.EndOfFile.QuadPart = (LONGLONG)size;
    return SetFileInformationByHandle(f->h, FileEndOfFileInfo, &i, sizeof i) ? 0 : last_err();
}
int os_file_flush(os_file *f) { return FlushFileBuffers(f->h) || GetLastError() == ERROR_INVALID_HANDLE ? 0 : last_err(); }
void os_file_close(os_file *f) {
    if (!f) return;
    CloseHandle(f->h);
    free(f);
}
bool os_file_trylock(os_file *f) {
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    return LockFileEx(f->h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &o) != 0;
}
void os_file_random(os_file *f) { (void)f; }   /* (a hint only: Windows takes it at the open, FILE_FLAG_RANDOM_ACCESS) */
const void *os_file_map_read(os_file *f, size_t *size) {
    *size = 0;
    const int64_t n = os_file_size(f);
    if (n <= 0) return NULL;
    const HANDLE m = CreateFileMappingW(f->h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m) return NULL;
    void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);   /* (the view keeps the mapping) */
    if (!p) return NULL;
    *size = (size_t)n;
    return p;
}
int os_path_kind(const char *path) {
    WPATH(w, path) return OS_PATH_NONE;
    const DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return OS_PATH_NONE;
    int k = a & FILE_ATTRIBUTE_DIRECTORY ? OS_PATH_DIR : OS_PATH_FILE;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
        /* (followed: a link to nothing is nothing, as stat's answer on Linux) */
        const HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h == INVALID_HANDLE_VALUE) return OS_PATH_NONE;
        BY_HANDLE_FILE_INFORMATION bi;
        if (GetFileInformationByHandle(h, &bi)) k = bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? OS_PATH_DIR : OS_PATH_FILE;
        CloseHandle(h);
        k |= OS_PATH_LINK;
    }
    return k;
}
int os_mkdir(const char *path) {
    WPATH(w, path) return OS_ENOENT;
    return CreateDirectoryW(w, NULL) ? 0 : last_err();
}
int os_rmdir(const char *path) {
    WPATH(w, path) return OS_ENOENT;
    return RemoveDirectoryW(w) ? 0 : last_err();
}
int os_remove(const char *path) {
    WPATH(w, path) return OS_ENOENT;
    return DeleteFileW(w) ? 0 : last_err();
}
int os_rename(const char *from, const char *to, bool replace) {
    WPATH(a, from) return OS_ENOENT;
    WPATH(b, to) return OS_ENOENT;
    return MoveFileExW(a, b, replace ? MOVEFILE_REPLACE_EXISTING : 0) ? 0 : last_err();
}
int os_chdir(const char *path) {
    WPATH(w, path) return OS_ENOENT;
    return SetCurrentDirectoryW(w) ? 0 : last_err();
}
struct os_dir { HANDLE h; WIN32_FIND_DATAW fd; bool first; };
os_dir *os_dir_open(const char *path, int *err) {
    char pat[OS_PATH_MAX];
    size_t n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) n--;
    if (n + 3 > sizeof pat) { if (err) *err = OS_EINVAL; return NULL; }
    memcpy(pat, path, n);
    memcpy(pat + n, "/*", 3);
    WPATH(w, pat) { if (err) *err = OS_ENOENT; return NULL; }
    os_dir *d = (os_dir *)calloc(1, sizeof *d);
    d->h = FindFirstFileExW(w, FindExInfoBasic, &d->fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (d->h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        free(d);
        if (err) *err = e == ERROR_DIRECTORY ? OS_EACCES : err_of(e);
        return NULL;
    }
    d->first = true;
    if (err) *err = 0;
    return d;
}
bool os_dir_next(os_dir *d, os_dirent *e) {
    for (;;) {
        if (!d->first && !FindNextFileW(d->h, &d->fd)) return false;
        d->first = false;
        const wchar_t *n = d->fd.cFileName;
        if (!wcscmp(n, L".") || !wcscmp(n, L"..")) continue;
        memset(e, 0, sizeof *e);
        narrow(n, e->name, sizeof e->name);
        const DWORD a = d->fd.dwFileAttributes;
        e->kind = a & FILE_ATTRIBUTE_DIRECTORY ? OS_PATH_DIR : OS_PATH_FILE;
        if (!(a & FILE_ATTRIBUTE_DIRECTORY)) e->size = (uint64_t)d->fd.nFileSizeHigh << 32 | d->fd.nFileSizeLow;
        if ((a & FILE_ATTRIBUTE_REPARSE_POINT) && (d->fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || d->fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT))
            e->kind |= OS_PATH_LINK;
        return true;
    }
}
void os_dir_close(os_dir *d) {
    if (!d) return;
    FindClose(d->h);
    free(d);
}
bool os_stderr_shown(void) {
    const HANDLE h = (HANDLE)_get_osfhandle(2);
    if (h == INVALID_HANDLE_VALUE || !h) return false;
    const DWORD t = GetFileType(h);
    if (t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE) return true;
    DWORD mode;
    return t == FILE_TYPE_CHAR && GetConsoleMode(h, &mode);   /* (a console is; NUL is a character device too) */
}
char **os_args(int *argc, char **argv) {
    (void)argv;
    setlocale(LC_CTYPE, ".UTF8");   /* (the UCRT's narrow file names, fopen's, as UTF-8) */
    SetConsoleOutputCP(CP_UTF8);
    int n = 0;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return argv;
    char **out = (char **)calloc((size_t)n + 1, sizeof *out);
    for (int i = 0; i < n; i++) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        out[i] = (char *)malloc(len > 0 ? (size_t)len : 1);
        if (len <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out[i], len, NULL, NULL)) out[i][0] = 0;
    }
    LocalFree(w);
    *argc = n;
    return out;
}
int os_data_home(char *out, size_t cap) {
    if (!cap) return 0;
    out[0] = 0;
    PWSTR w = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &w)) || !w) return 0;
    const int n = narrow(w, out, cap);
    CoTaskMemFree(w);
    return n;
}
#endif
