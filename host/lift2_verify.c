/* The lifter's summary verification (nx2/lift2/game.py with HOOKSHOT_LIFT_VERIFY=1): each summarised
   function's entry calls lift2_check_enter before its body and lift2_check_exit after it, with its summary
   (sum: inputs, may-write, results, preserved masks over x0-x30 and SP, flags, guest address, must-write mask).

   At entry: the entry registers are saved; the registers the summary says are not read are poisoned
   (0xDEAD, the register and the function in the value). At exit: a function claimed not to return returned; a
   preserved register changed; a register claimed not written was written; then the registers claimed not written go
   back to their entry values and the written ones no caller reads are poisoned (0xDEAE). Each violation is logged
   once per function, kind and register, to the game log and to lift2_violations.txt in the working folder.

   A field whose write-through store the lifter omitted (nx2/lift2/build_ir.py omit_stores: no reader reads it at a
   direct call) is filled before the call with 0xDEAF, the register and the call's guest address. At exit a result
   the function claims to write on every path still holding it is a violation naming that site (kind "result not
   written, store omitted at a call"). An input holding it at entry is only counted, as the other poisons are not
   reported there: inputs over-approximate (an indirect jump's target is assumed to read the AAPCS64's arguments, not
   x29 and x30, which a callee saves; host code calls guest functions with fewer arguments than their summaries
   list), so it may never be read; a real read shows as a wrong value or a crash at an address 0xdeaf... that names
   the site.

   A direct call of an HLE'd function declared clean (nx2/lift2/host_effects.py) goes through lift2_host_enter and
   lift2_host_exit: x19-x29 and SP on its return must equal their values at the call (kind "changed by the host"). */
#ifdef LIFT2_VERIFY_STANDALONE   /* (tests/test_lift2_verify.py: the checks alone, without the host) */
#include <pthread.h>
typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define AcquireSRWLockExclusive pthread_mutex_lock
#define ReleaseSRWLockExclusive pthread_mutex_unlock
#define hos_log(...) ((void)0)
#else
#include "hos.h"
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define POISON_IN 0xDEADull
#define POISON_OUT 0xDEAEull
#define POISON_SITE 0xDEAFull   /* (build_ir.SITE_POISON) */
#define RETURNS 1ull

static uint64_t poison(uint64_t tag, int r, uint64_t fn) { return tag << 48 | (uint64_t)r << 40 | (fn & 0xFFFFFFFFFFull); }
static int poisoned(uint64_t v) { return (v >> 48) == POISON_IN || (v >> 48) == POISON_OUT; }

static SRWLOCK lock = SRWLOCK_INIT;
static uint64_t seen[1 << 20];
static unsigned count;

static void violation(const char *kind, uint64_t fn, int r, uint64_t v) {
    const uint64_t key = (fn << 16) ^ ((uint64_t)(uintptr_t)kind << 6) ^ (uint64_t)r ^ 1;
    AcquireSRWLockExclusive(&lock);
    unsigned n = 0;
    for (unsigned h = (unsigned)(key * 0x9E3779B97F4A7C15ull >> 44); n < 64; h = (h + 1) & 0xFFFFF, n++) {
        if (seen[h] == key) { ReleaseSRWLockExclusive(&lock); return; }
        if (!seen[h]) { seen[h] = key; break; }
    }
    if (n == 64 || count >= 100000) { ReleaseSRWLockExclusive(&lock); return; }   /* (full: the run reports no more) */
    count++;
    FILE *f = fopen("lift2_violations.txt", "a");
    if (f) {
        fprintf(f, "f_%08llx %s %s%d value %#llx%s\n", (unsigned long long)fn, kind, r == 31 ? "sp" : "x", r == 31 ? 0 : r,
                (unsigned long long)v, poisoned(v) ? " (poison of f_" : "");
        if (poisoned(v)) fprintf(f, "  ... f_%08llx's %s)\n", (unsigned long long)(v & 0xFFFFFFFFFFull),
                                 (v >> 48) == POISON_IN ? "inputs" : "results");
        if ((v >> 48) == POISON_SITE) fprintf(f, "  (the store omitted at the call at %#llx)\n", (unsigned long long)(v & 0xFFFFFFFFFFull));
        fclose(f);
    }
    hos_log("lift2 verify: f_%08llx %s x%d %#llx", (unsigned long long)fn, kind, r, (unsigned long long)v);
    ReleaseSRWLockExclusive(&lock);
}

/* (how often the poisons are met: entries with a site-poisoned register, exits that poison a register; logged at
   every power of two from 1024, so a run shows that both were exercised) */
static volatile uint64_t n_site, n_insite, n_out;
static void tally(volatile uint64_t *n, const char *what) {
    const uint64_t k = __atomic_add_fetch(n, 1, __ATOMIC_RELAXED);
    if (k >= 1024 && !(k & (k - 1))) hos_log("lift2 verify: %llu %s", (unsigned long long)k, what);
}

void lift2_check_enter(uint64_t *x, const uint64_t *sum, uint64_t *frame) {
    const uint64_t in = sum[0], fn = sum[5];
    /* (an input holding poison is not reported: inputs are an over-approximation (a BLR or host call reads x0-x8 by
       the AAPCS64), so a poisoned input may never be read; a real read of poison shows as a wrong value or a crash
       at an address 0xdead.../0xdeae... that names the function) */
    memcpy(frame, x, 32 * sizeof *x);
    int site = 0, insite = 0;
    for (int r = 0; r < 31; r++) {
        site |= (x[r] >> 48) == POISON_SITE;
        insite |= in >> r & 1 && (x[r] >> 48) == POISON_SITE;
    }
    if (site) tally(&n_site, "entries with a register holding the site poison (0xDEAF)");
    if (insite) tally(&n_insite, "entries with an input holding the site poison (not a violation: inputs over-approximate)");
    for (int r = 0; r < 29; r++)   /* (not x29, x30: frame and link) */
        if (!(in >> r & 1)) x[r] = poison(POISON_IN, r, fn);
}

void lift2_check_exit(uint64_t *x, const uint64_t *sum, const uint64_t *frame) {
    const uint64_t in = sum[0], may = sum[1], res = sum[2], pres = sum[3], flags = sum[4], fn = sum[5], must = sum[6];
    int out = 0;
    if (!(flags & RETURNS)) violation("returned (summary: noreturn)", fn, 0, 0);
    for (int r = 0; r < 32; r++) {   /* (a preserved register not read was poisoned at entry: it must still be) */
        if (!(pres >> r & 1)) continue;
        const uint64_t expect = r >= 29 || in >> r & 1 ? frame[r] : poison(POISON_IN, r, fn);
        if (x[r] != expect) violation("not preserved", fn, r, x[r]);
        x[r] = frame[r];
    }
    for (int r = 0; r < 29; r++) {
        if (pres >> r & 1) continue;
        if (may >> r & 1) {
            if (must >> r & 1 && res >> r & 1 && (frame[r] >> 48) == POISON_SITE &&
                x[r] == (in >> r & 1 ? frame[r] : poison(POISON_IN, r, fn)))
                violation("result not written, store omitted at a call", fn, r, frame[r]);
            if (!(res >> r & 1) && !(pres >> r & 1)) x[r] = poison(POISON_OUT, r, fn), out = 1;
            continue;
        }
        const uint64_t expect = in >> r & 1 ? frame[r] : poison(POISON_IN, r, fn);
        if (x[r] != expect) violation("written (summary: not)", fn, r, x[r]);
        x[r] = frame[r];
    }
    if (out) tally(&n_out, "exits poisoning a written register no caller reads (0xDEAE)");
}

void lift2_host_enter(const uint64_t *x, uint64_t *frame) { memcpy(frame, x, 32 * sizeof *x); }

void lift2_host_exit(const uint64_t *x, const uint64_t *frame, uint64_t fn) {
    for (int r = 19; r < 32; r++)
        if (r != 30 && x[r] != frame[r]) violation("changed by the host", fn, r, x[r]);
}
