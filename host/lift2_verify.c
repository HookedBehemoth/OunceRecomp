/* Lift 2's summary verification (milestone 14, nx2/lift2/mixed.py with HOOKSHOT_LIFT2_VERIFY=1): each summarised
   function's generic entry calls lift2_check_enter before its body and lift2_check_exit after it, with its summary
   (sum: inputs, may-write, results, preserved masks over x0-x30 and SP, flags, guest address).

   At entry: an input holding a poison value is a violation (another function's summary said the register was not
   read, or not a result); the entry registers are saved; the registers the summary says are not read are poisoned
   (0xDEAD, the register and the function in the value). At exit: a function claimed not to return returned; a
   preserved register changed; a register claimed not written was written; then the registers claimed not written go
   back to their entry values and the written ones no caller reads are poisoned (0xDEAE). Each violation is logged
   once per function, kind and register, to the game log and to lift2_violations.txt in the working folder. */
#include "hos.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define POISON_IN 0xDEADull
#define POISON_OUT 0xDEAEull
#define RETURNS 1ull

static uint64_t poison(uint64_t tag, int r, uint64_t fn) { return tag << 48 | (uint64_t)r << 40 | (fn & 0xFFFFFFFFFFull); }
static int poisoned(uint64_t v) { return (v >> 48) == POISON_IN || (v >> 48) == POISON_OUT; }

static SRWLOCK lock = SRWLOCK_INIT;
static uint64_t seen[1 << 16];
static unsigned count;

static void violation(const char *kind, uint64_t fn, int r, uint64_t v) {
    const uint64_t key = (fn << 16) ^ ((uint64_t)(uintptr_t)kind << 6) ^ (uint64_t)r ^ 1;
    AcquireSRWLockExclusive(&lock);
    for (unsigned h = (unsigned)(key * 0x9E3779B97F4A7C15ull >> 48), n = 0; n < (1u << 16); h = (h + 1) & 0xFFFF, n++) {
        if (seen[h] == key) { ReleaseSRWLockExclusive(&lock); return; }
        if (!seen[h]) { seen[h] = key; break; }
    }
    count++;
    FILE *f = fopen("lift2_violations.txt", "a");
    if (f) {
        fprintf(f, "f_%08llx %s %s%d value %#llx%s\n", (unsigned long long)fn, kind, r == 31 ? "sp" : "x", r == 31 ? 0 : r,
                (unsigned long long)v, poisoned(v) ? " (poison of f_" : "");
        if (poisoned(v)) fprintf(f, "  ... f_%08llx's %s)\n", (unsigned long long)(v & 0xFFFFFFFFFFull),
                                 (v >> 48) == POISON_IN ? "inputs" : "results");
        fclose(f);
    }
    hos_log("lift2 verify: f_%08llx %s x%d %#llx", (unsigned long long)fn, kind, r, (unsigned long long)v);
    ReleaseSRWLockExclusive(&lock);
}

void lift2_check_enter(uint64_t *x, const uint64_t *sum, uint64_t *frame) {
    const uint64_t in = sum[0], fn = sum[5];
    for (int r = 0; r < 31; r++)
        if (in >> r & 1 && poisoned(x[r])) violation("input poisoned", fn, r, x[r]);
    memcpy(frame, x, 32 * sizeof *x);
    for (int r = 0; r < 29; r++)   /* (not x29, x30: frame and link) */
        if (!(in >> r & 1)) x[r] = poison(POISON_IN, r, fn);
}

void lift2_check_exit(uint64_t *x, const uint64_t *sum, const uint64_t *frame) {
    const uint64_t in = sum[0], may = sum[1], res = sum[2], pres = sum[3], flags = sum[4], fn = sum[5];
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
            if (!(res >> r & 1) && !(pres >> r & 1)) x[r] = poison(POISON_OUT, r, fn);
            continue;
        }
        const uint64_t expect = in >> r & 1 ? frame[r] : poison(POISON_IN, r, fn);
        if (x[r] != expect) violation("written (summary: not)", fn, r, x[r]);
        x[r] = frame[r];
    }
}
