/* Hookshot's OS layer: everything the host runtime asks of the operating system, under names of its own. The host's
   C files reach the system through this header (no <pthread.h>, <sys/mman.h> or <dlfcn.h> of their own; the few
   Linux-only details, such as the guest memory's memfd, stay where they are used); its implementation is os_linux.c,
   which depends on nothing of Hookshot's.

   Conventions:
   - timeouts are nanoseconds, OS_FOREVER (any negative value) waits without end;
   - a call that can fail for a reason the caller tells apart returns 0 or a negative OS_E* code;
   - the locks, condition variables, events and semaphores are plain structs, valid when zeroed (or set to their
     _INIT), never allocated by this layer. They are futex words: a wake makes a system call only when a thread sleeps
     on the word (each counts its sleepers), and an uncontended lock or unlock is one atomic instruction;
   - paths are UTF-8 and may use '\' as the separator (it is taken as '/'). */
#ifndef HOOKSHOT_OS_H
#define HOOKSHOT_OS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define OS_PATH_MAX 4096
#define OS_FOREVER ((int64_t)-1)

enum {
    OS_OK = 0,
    OS_EFAIL = -1,       /* (anything not below) */
    OS_ENOENT = -2,      /* no such file or folder (or a part of the path is missing) */
    OS_EEXIST = -3,      /* it exists already */
    OS_EACCES = -4,      /* not allowed (or a folder where a file was asked for) */
    OS_ENOMEM = -5,
    OS_ETIMEDOUT = -6,
    OS_ENOTEMPTY = -7,   /* a folder still holding something */
    OS_EINVAL = -8,
};

/* ---- atomics: GCC/Clang builtins, sequentially consistent, on any integer or pointer width ---------------------- */
#define os_atomic_load(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define os_atomic_store(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define os_atomic_inc(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)               /* the new value */
#define os_atomic_dec(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)               /* the new value */
#define os_atomic_add(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)          /* the new value */
#define os_atomic_fetch_add(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)    /* the old value */
#define os_atomic_fetch_or(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)      /* the old value */
#define os_atomic_fetch_and(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)    /* the old value */
#define os_atomic_xchg(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)        /* the old value */
/* *p = desired if *p == expected; the value *p had (== expected: it was swapped) */
#define os_atomic_cas(p, expected, desired) __sync_val_compare_and_swap((p), (expected), (desired))
#define os_fence() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define os_compiler_fence() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#if defined(__x86_64__) || defined(__i386__)
#define os_pause() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define os_pause() __asm__ __volatile__("yield")
#else
#define os_pause() ((void)0)
#endif

/* ---- time ---------------------------------------------------------------------------------------------------------- */
int64_t os_now_ns(void);                    /* monotonic */
int64_t os_wall_ns(void);                   /* real time: nanoseconds since 1970-01-01 UTC */
void os_sleep_ns(int64_t ns);               /* at least ns (to within the kernel's wake-up latency: no rounding to ms) */
void os_sleep_until_ns(int64_t deadline);   /* until os_now_ns() >= deadline */
void os_yield(void);                        /* gives the core to another ready thread, if there is one */

/* ---- waiting on a word ----------------------------------------------------------------------------------------------- */
/* sleeps while *w == expected, at most timeout_ns: 0 (woken, or *w was not expected: look again) or OS_ETIMEDOUT.
   Spurious returns happen: callers loop on their own condition. */
int os_futex_wait(volatile uint32_t *w, uint32_t expected, int64_t timeout_ns);
#define OS_WAKE_ALL 0x7fffffff
void os_futex_wake(volatile uint32_t *w, int n);   /* wakes up to n sleepers on w */

/* ---- locks ----------------------------------------------------------------------------------------------------------- */
/* a mutex: not recursive, owner unchecked. state 0 free, 1 held, 2 held with (possible) sleepers */
typedef struct os_mutex { volatile uint32_t state; } os_mutex;
#define OS_MUTEX_INIT {0}
void os_mutex_lock(os_mutex *m);
bool os_mutex_trylock(os_mutex *m);
void os_mutex_unlock(os_mutex *m);

/* a reader/writer lock: any number of readers or one writer. Readers are let in while no writer holds it (a waiting
   writer does not hold them back) */
typedef struct os_rwlock { volatile uint32_t state; } os_rwlock;
#define OS_RWLOCK_INIT {0}
void os_rwlock_lock(os_rwlock *l);           /* exclusive */
bool os_rwlock_trylock(os_rwlock *l);
void os_rwlock_unlock(os_rwlock *l);
void os_rwlock_lock_shared(os_rwlock *l);
bool os_rwlock_trylock_shared(os_rwlock *l);
void os_rwlock_unlock_shared(os_rwlock *l);

/* a condition variable, with an os_mutex or an os_rwlock held exclusively. Wake-ups may be spurious: wait in a loop
   on the condition. A signal or broadcast with no thread waiting is a load and nothing else. */
typedef struct os_cond { volatile uint32_t seq, waiters; } os_cond;
#define OS_COND_INIT {0, 0}
int os_cond_wait(os_cond *c, os_mutex *m, int64_t timeout_ns);       /* 0 or OS_ETIMEDOUT; m held again on return */
int os_cond_wait_rw(os_cond *c, os_rwlock *l, int64_t timeout_ns);   /* (l held exclusively) */
void os_cond_signal(os_cond *c);
void os_cond_broadcast(os_cond *c);

/* an event: set or not. An auto-reset event lets one waiter through per set (the wait takes it); a manual one stays set
   until reset and lets every waiter through */
typedef struct os_event { volatile uint32_t state, waiters; uint32_t manual; } os_event;
void os_event_init(os_event *e, bool manual, bool set);
void os_event_set(os_event *e);
void os_event_reset(os_event *e);
int os_event_wait(os_event *e, int64_t timeout_ns);   /* 0 (set; auto-reset: taken) or OS_ETIMEDOUT */
bool os_event_try(os_event *e);                       /* os_event_wait without waiting */

/* a counting semaphore */
typedef struct os_sema { volatile uint32_t count, waiters; } os_sema;
void os_sema_init(os_sema *s, uint32_t count);
void os_sema_post(os_sema *s, uint32_t n);
int os_sema_wait(os_sema *s, int64_t timeout_ns);   /* 0 (one taken) or OS_ETIMEDOUT */

/* ---- threads --------------------------------------------------------------------------------------------------------- */
typedef struct os_thread os_thread;
typedef void (*os_thread_fn)(void *arg);
/* a new thread running fn(arg), with a stack of that size (0: 8 MB) and that name: its handle, NULL when it could not
   be made. The handle stays valid until os_thread_release, also after the thread ends. */
os_thread *os_thread_start(os_thread_fn fn, void *arg, size_t stack, const char *name);
os_thread *os_thread_current(void);   /* the calling thread's (any thread's, also one this layer did not start) */
void os_thread_retain(os_thread *t);   /* one more reference (os_thread_current's handle is borrowed: retain to keep it) */
void os_thread_release(os_thread *t);
int os_thread_join(os_thread *t, int64_t timeout_ns);   /* 0 (it has ended) or OS_ETIMEDOUT */
uint32_t os_thread_tid(os_thread *t);   /* the system's thread id */
uint32_t os_tid(void);                  /* the calling thread's */
void os_thread_set_name(os_thread *t, const char *name);
void os_thread_name(os_thread *t, char *out, size_t cap);
enum { OS_PRIO_IDLE = -3, OS_PRIO_LOWEST = -2, OS_PRIO_BELOW = -1, OS_PRIO_NORMAL = 0, OS_PRIO_ABOVE = 1, OS_PRIO_HIGHEST = 2 };
void os_thread_set_priority(os_thread *t, int prio);   /* (Linux: only lowered, with nice values) */
int os_thread_priority(os_thread *t);                  /* the one last set */
uint64_t os_thread_set_affinity(os_thread *t, uint64_t mask);   /* the mask it had; 0 when refused */
int64_t os_thread_cpu_ns(os_thread *t);                /* CPU time used; -1 when unknown (it has ended) */
__attribute__((noreturn)) void os_thread_exit(void);
__attribute__((noreturn)) void os_exit(int code);      /* at once: stdio flushed, no exit handlers, no destructors */
uint32_t os_pid(void);

/* ---- processors ------------------------------------------------------------------------------------------------------ */
int os_cpu_count(void);                      /* logical processors online */
uint64_t os_process_affinity(void);          /* the processors the process may use (as it started), one bit each */
int os_cpu_cores(uint64_t *masks, int cap);  /* one mask per physical core (its SMT siblings): how many cores */

/* ---- fibers: contexts switched by hand, each on a stack of its own --------------------------------------------------- */
typedef struct os_fiber os_fiber;
typedef void (*os_fiber_fn)(void *arg);   /* (returning from it ends the thread) */
os_fiber *os_fiber_from_thread(void);     /* the calling thread's own context, as a fiber to switch back to */
os_fiber *os_fiber_create(size_t stack, os_fiber_fn fn, void *arg);
void os_fiber_switch(os_fiber *to);
void os_fiber_delete(os_fiber *f);        /* (the running fiber: ends the thread) */
os_fiber *os_fiber_current(void);

/* ---- memory ---------------------------------------------------------------------------------------------------------- */
enum { OS_PROT_NONE = 0, OS_PROT_R = 1, OS_PROT_W = 2, OS_PROT_X = 4, OS_PROT_RW = 3, OS_PROT_RX = 5, OS_PROT_RWX = 7 };
size_t os_page_size(void);
/* address space at addr (NULL: anywhere; else exactly there, failing when taken), committed with prot or, for
   OS_PROT_NONE, only reserved. Pages are given on first touch either way. NULL on failure. */
void *os_mem_map(void *addr, size_t size, int prot);
int os_mem_unmap(void *addr, size_t size);
int os_mem_protect(void *addr, size_t size, int prot);   /* (commits reserved pages, with a prot other than NONE) */
int os_mem_discard(void *addr, size_t size);             /* the pages' contents dropped: they read as zero again */
/* which pages of [addr, addr + size) are in memory (addr page-aligned): out[i] & 1 for the i-th; 0 or < 0 */
int os_mem_resident(const void *addr, size_t size, uint8_t *out);
enum { OS_MEM_FREE = 0, OS_MEM_RESERVED = 1, OS_MEM_COMMITTED = 2 };
typedef struct { uintptr_t base, size; int state, prot; } os_mem_info;   /* (size: from base to the region's end) */
int os_mem_query(const void *addr, os_mem_info *out);
/* a region the caller made and keeps (the guest window): os_mem_query answers for it without asking the kernel */
void os_mem_declare(void *base, size_t size, int prot);
typedef struct { uint64_t resident, peak_resident, private_bytes; } os_mem_usage;
int os_mem_usage_get(os_mem_usage *out);

/* ---- faults: access violations, illegal instructions, division by zero, traps ---------------------------------------- */
enum { OS_FAULT_ACCESS = 1, OS_FAULT_ILLEGAL, OS_FAULT_DIVIDE, OS_FAULT_TRAP };
typedef struct { int kind, write; uintptr_t addr, pc; } os_fault;   /* (addr, write: for OS_FAULT_ACCESS) */
enum { OS_FAULT_PASS = 0, OS_FAULT_RESUME = 1 };   /* a handler's answer: not mine / handled, run on */
typedef int (*os_fault_fn)(const os_fault *f);
void os_fault_add(os_fault_fn fn, bool first);   /* asked in order, first ones first */
void os_fault_remove(os_fault_fn fn);
void os_fault_set_last(os_fault_fn fn);          /* asked when no handler resumed (a crash report) */

/* ---- modules --------------------------------------------------------------------------------------------------------- */
typedef struct os_lib os_lib;
os_lib *os_lib_open(const char *path);   /* a shared library (by path, or a bare name the loader searches); NULL */
void *os_lib_sym(os_lib *l, const char *name);
void os_lib_close(os_lib *l);
void *os_sym(const char *name);          /* a symbol of the executable or anything loaded (NULL: none) */
int os_exe_path(char *out, size_t cap);  /* the executable's full path: its length, 0 on failure */
/* the base address of the module holding addr (0: none), and its path into out when given */
uintptr_t os_module_of(const void *addr, char *out, size_t cap);
uintptr_t os_exe_base(void);

/* ---- files and folders ----------------------------------------------------------------------------------------------- */
typedef struct os_file os_file;
enum { OS_READ = 1, OS_WRITE = 2, OS_CREATE = 4, OS_EXCL = 8, OS_TRUNC = 16 };
os_file *os_file_open(const char *path, int flags, int *err);   /* (a folder is refused: OS_EACCES) */
int64_t os_file_read_at(os_file *f, void *buf, size_t n, uint64_t at);         /* bytes read (0: end), or < 0 */
int64_t os_file_write_at(os_file *f, const void *buf, size_t n, uint64_t at);  /* bytes written (all), or < 0 */
int64_t os_file_size(os_file *f);
int os_file_resize(os_file *f, uint64_t size);
int os_file_flush(os_file *f);
void os_file_close(os_file *f);
enum { OS_PATH_NONE = 0, OS_PATH_FILE = 1, OS_PATH_DIR = 2, OS_PATH_LINK = 4 };   /* (LINK: added to FILE or DIR) */
int os_path_kind(const char *path);   /* what is there (links followed; OS_PATH_LINK when the path itself is one) */
int64_t os_path_size(const char *path);   /* a file's size, < 0 when there is none */
int os_mkdir(const char *path);       /* 0, OS_EEXIST, OS_ENOENT (the parent is missing), ... */
int os_rmdir(const char *path);
int os_remove(const char *path);
int os_rename(const char *from, const char *to, bool replace);
int os_copy_file(const char *from, const char *to, bool replace);
int os_chdir(const char *path);
typedef struct os_dir os_dir;
typedef struct { char name[256]; int kind; uint64_t size; } os_dirent;   /* (kind: as os_path_kind's) */
os_dir *os_dir_open(const char *path, int *err);
bool os_dir_next(os_dir *d, os_dirent *e);   /* the next entry, "." and ".." left out; false at the end */
void os_dir_close(os_dir *d);
bool os_stderr_shown(void);   /* stderr goes somewhere a person sees (a terminal, a file or a pipe; not /dev/null) */

/* ---- dialogs ----------------------------------------------------------------------------------------------------- */
void os_message_box(const char *title, const char *text);   /* on stderr, and in a dialog when SDL is loaded */

#ifdef __cplusplus
}
#endif
#endif
