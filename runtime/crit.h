/* HOS_DBG=crit[:<first present>[:<presents>]] (measurement only, direct mode): a trace of what every guest
   thread does over a window of presents, for the frame's critical path. Each guest thread's
   blocks (hos_block_begin/end: what it waited on, its guest caller), the wakes of
   the objects it waits on, and every call into the direct back end (the record emits merged into bursts) are spans
   with TSC times. At the window's end they go to crit.bin in the working directory (the run folder), the thread names to
   crit_threads.txt; scripts/crit_path.py reads them. With HOS_DBG unset, or without "crit", nothing is recorded and the
   back end's calls are not wrapped. */
#ifndef OUNCE_DIRECT_CRIT_H
#define OUNCE_DIRECT_CRIT_H
#include <stdint.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
static inline uint64_t crit_tsc(void) { return __rdtsc(); }
#else
static inline uint64_t crit_tsc(void) { return 0; }
#endif

enum {
    CRIT_BLOCK_CV = 0,      /* a = the condition variable slept on, b = guest lr */
    CRIT_BLOCK_THREAD = 1,  /* hos_thread_wait (the kernel's arbiter, mutexes): a = the thread, b = guest lr */
    CRIT_BLOCK_SLEEP = 2,   /* a = ns asked, b = guest lr */
    CRIT_BLOCK_OTHER = 3,   /* a = the host caller of hos_block_begin, b = guest lr */
    CRIT_VCWAIT = 4,        /* (none any more: the waits for a virtual core, which are gone) */
    CRIT_WAKE = 5,          /* instant: a = the object woken (cv, thread or vc_cv) */
    CRIT_BE_EMIT = 10,      /* a burst of hsd_emit: a = tsc inside the calls, b = calls */
    CRIT_BE_BEGIN = 11, CRIT_BE_END = 12, CRIT_BE_CALL = 13, CRIT_BE_SUBMIT = 14, CRIT_BE_PRESENT = 15,
    CRIT_BE_EVSIG = 16, CRIT_BE_WAIT = 17, CRIT_BE_OTHER = 18, CRIT_BE_RELEASED = 19,
    CRIT_PRESENT = 20,      /* instant at rx_nvn_present's entry: a = present number */
};

extern volatile int crit_rec;   /* recording now */
int crit_wanted(void);          /* HOS_DBG names crit (read once) */
void crit_span(int kind, uint64_t t0, uint64_t t1, uint64_t a, uint64_t b);
static inline void crit_wake(const void *key) {
    if (crit_rec) { const uint64_t t = crit_tsc(); crit_span(CRIT_WAKE, t, t, (uint64_t)(uintptr_t)key, 0); }
}
void crit_present(void);        /* rx_nvn_present: the window's start and end */
void crit_emit_flush(void);     /* this thread's open emit burst out */
#endif
