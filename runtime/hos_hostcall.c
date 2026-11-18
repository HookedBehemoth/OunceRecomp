/* Host calls: guest-callable addresses made at run time, each bound to a host function.

   The lifted code reaches them through indirect calls (blr / br): rt/a64_rt.c asks a64_call_miss for any address
   that is not a lifted function. Each host call is a 16-byte slot in HOS_HOSTCALL_BASE.. (no memory behind it).
   Used for the API that hands out function pointers, NVN: every proc nvnDeviceGetProcAddress returns is one (the Proc
   kind). No patch hands the game one: a patch of an indirect call is an instead row on its BLR / BR (runtime/patch.h). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

#define HOS_HOSTCALL_BASE 0x06000000ULL
#define HOS_HOSTCALL_MAX 8192

typedef void (*HostFn)(Ctx *c, void *user);
typedef struct { HostFn fn; void *user; char name[64]; volatile int32_t calls; } HostCall;
static HostCall calls[HOS_HOSTCALL_MAX];
static volatile int32_t ncalls;
static os_mutex lock = OS_MUTEX_INIT;

/* HOS_HOSTCALL_STATS, tracing or HOS_HOSTCALL_SLOW=1: every host call through a64_call_miss (it counts there) */
static int slow_calls(void) {
    static int slow = -1;
    if (slow < 0) slow = getenv("HOS_HOSTCALL_STATS") != NULL || hos_trace || getenv("HOS_HOSTCALL_SLOW") != NULL;
    return slow;
}

/* What the game is handed for a host call named `name` (made on first request, the same after): the host function
   itself when it takes no user data (a code pointer is a host function pointer, docs/lifter-output.md "Indirect calls and
   jumps"; such handlers do not read their user argument), else its guest address in HOS_HOSTCALL_BASE.., which the
   lifted code's guest-address path takes to a64_call_miss. */
uint64_t hos_hostcall(const char *name, HostFn fn, void *user) {
    os_mutex_lock(&lock);
    int32_t i;
    for (i = 0; i < ncalls && strcmp(calls[i].name, name); i++) {}
    if (i == ncalls) {
        if (ncalls == HOS_HOSTCALL_MAX) hos_fatal(NULL, "out of host calls");
        ncalls++;
        calls[i].fn = fn;
        calls[i].user = user;
        snprintf(calls[i].name, sizeof calls[i].name, "%s", name);
    }
    const HostCall *h = &calls[i];
    os_mutex_unlock(&lock);
#ifndef __EMSCRIPTEN__   /* (wasm checks an indirect call's type: a HostFn has two parameters) */
    if (!h->user && !slow_calls()) return (uint64_t)(uintptr_t)h->fn;
#endif
    return HOS_HOSTCALL_BASE + 16ULL * i;
}

/* the host call an address names, by its guest address or its host function (no user data): its index, or -1 */
static int32_t call_of(uint64_t addr) {
    if (addr >> 32) {
        for (int32_t i = 0; i < ncalls; i++)
            if ((uint64_t)(uintptr_t)calls[i].fn == addr && !calls[i].user) return i;
        return -1;
    }
    const uint64_t i = (addr - HOS_HOSTCALL_BASE) / 16;
    return addr >= HOS_HOSTCALL_BASE && i < (uint64_t)ncalls ? (int32_t)i : -1;
}

/* (mkw-recomp) the host function and user data behind a host call's address: 0 when it is not one */
uint64_t hos_hostcall_target(uint64_t addr, HostFn *fn, void **user) {
    const int32_t i = call_of(addr);
    if (i < 0) return 0;
    *fn = calls[i].fn, *user = calls[i].user;
    return addr;
}

const char *hos_hostcall_name(uint64_t addr) {
    const int32_t i = call_of(addr);
    return i < 0 ? NULL : calls[i].name;
}

/* (rt/a64_rt.c a64_guest_of) a host call's guest address, for logs */
uint64_t a64_guest_of_host(uint64_t v) {
    const int32_t i = call_of(v);
    return i < 0 ? 0 : HOS_HOSTCALL_BASE + 16ULL * (uint64_t)i;
}

/* rt/a64_rt.c a64_lookup: a host call without user data as a plain function, called as fn(c) (its user argument, which
   such handlers do not read, is then whatever the register holds). Not with HOS_HOSTCALL_STATS or tracing (they count
   in a64_call_miss) or HOS_HOSTCALL_SLOW=1. Not on the web: wasm checks an indirect call's type, and a HostFn
   (two parameters) called as an A64Fn (one) traps with "function signature mismatch". */
A64Fn a64_lookup_host(uint64_t addr) {
#ifdef __EMSCRIPTEN__
    (void)addr;
    return NULL;
#endif
    if (addr < HOS_HOSTCALL_BASE || (addr - HOS_HOSTCALL_BASE) % 16) return NULL;
    const uint64_t i = (addr - HOS_HOSTCALL_BASE) / 16;
    if (i >= (uint64_t)ncalls || calls[i].user) return NULL;
    return slow_calls() ? NULL : (A64Fn)(void *)calls[i].fn;
}

/* The last host calls, for HOS_HOSTCALL_STATS: what a stalled thread was doing. */
#define RING 256
typedef struct { uint32_t call; uint32_t thread; uint64_t x[4], lr; } Recent;
static Recent ring[RING];
static volatile int32_t ring_pos;

void hos_setjmp_return(Ctx *c, uint64_t target);   /* hos_setjmp.c: a longjmp's BR to a setjmp site */
int a64_call_miss(Ctx *c, uint64_t target) {
    hos_setjmp_return(c, target);   /* (hookshot; does not return when it is one) */
    if (!target) {
        /* (hookshot) a callback table entry zeroed at run time: OoT's UpdateTextBoxTextureThread reaches its text release trampoline
           with a null function pointer at the same cutscene point every session. 0 is never a real function; returning 1 turns the
           tail call into a plain return. chrispository, Hookshot PR 55 */
        static volatile int32_t logged;
        if (os_atomic_inc(&logged) <= 8)
            hos_log("skipped an indirect call to 0 (lr %#llx, thread %s)", (unsigned long long)c->x[30], hos_thread_name(hos_self()));
        return 1;
    }
    if (target < HOS_HOSTCALL_BASE || (target - HOS_HOSTCALL_BASE) % 16) return 0;
    uint64_t i = (target - HOS_HOSTCALL_BASE) / 16;
    if (i >= (uint64_t)ncalls) return 0;
    HostCall *h = &calls[i];
    /* (the ring and the counts only for HOS_HOSTCALL_STATS / tracing: every NVN call took two interlocked increments
       on lines all game threads share) */
    static int stats = -1;
    if (stats < 0) stats = getenv("HOS_HOSTCALL_STATS") != NULL || hos_trace;
    if (!stats) {
        h->fn(c, h->user);
        return 1;
    }
    Recent *r = &ring[(os_atomic_inc(&ring_pos) - 1) & (RING - 1)];
    r->call = (uint32_t)i;
    r->thread = os_tid();
    memcpy(r->x, c->x, sizeof r->x);
    r->lr = c->x[30];
    int32_t n = os_atomic_inc(&h->calls);
    if (hos_trace && n <= 3) hos_log("hostcall %s(%#llx, %#llx, %#llx, %#llx)", h->name, (unsigned long long)c->x[0],
                                     (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    h->fn(c, h->user);
    return 1;
}

/* An OS-facing import the host neither replaces (HLE) nor answers (PATCH_ANSWER, runtime/nn_answers.c): the generated stub
   (gen/a64_stubs.c, ounce/lift/src/gen.rs gen_stubs) stops the game here, naming the function; the caller is the lr. A
   direct call of the stub is lifted as a noreturn call (ounce/lift "stop") */
__attribute__((noreturn)) void hos_import_stop(Ctx *c, const char *sym, const char *name);
void hos_import_stop(Ctx *c, const char *sym, const char *name) {
    hos_fatal(c, "the game called %s (%s), which the host neither implements nor answers", name, sym);
    __builtin_unreachable();   /* (hos_fatal ends the process) */
}

/* Call guest code from inside a host function (an HLE handler), with up to four arguments, on the current guest
   stack. The guest callee keeps the AArch64 callee-saved registers; LR is restored here. Returns x0. */
uint64_t hos_call_guest(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t lr = c->x[30];
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    c->x[30] = 0;
    a64_icall(c, fn);
    c->x[30] = lr;
    return c->x[0];
}

/* Guest code from an At row or a replaced function (guest_call), transparent to the lifted caller: every register (x0-x30,
   SP, NZCV, V0-V31) as it was afterwards, so the lifted code keeps its values as SSA values across it (runtime/hos.h). */
static uint64_t call_restoring(Ctx *c, uint64_t sp, uint64_t fn, const uint64_t *a, int n) {
    uint64_t x[32], v[32][2];
    uint8_t f[4] = {c->n, c->z, c->c, c->v};
    memcpy(x, c->x, sizeof x);
    memcpy(v, c->vreg, sizeof v);
    memcpy(c->x, a, n * sizeof *a);
    c->x[31] = sp, c->x[30] = 0;
    a64_icall(c, fn);
    const uint64_t r = c->x[0];
    memcpy(c->x, x, sizeof x);
    memcpy(c->vreg, v, sizeof v);
    c->n = f[0], c->z = f[1], c->c = f[2], c->v = f[3];
    return r;
}
uint64_t guest_call(GuestCall *g, uint64_t sp, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    const uint64_t a[5] = {a0, a1, a2, a3, a4};
    return call_restoring((Ctx *)g, sp, fn, a, 5);
}
uint64_t guest_call8(GuestCall *g, uint64_t sp, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                     uint64_t a5, uint64_t a6, uint64_t a7) {
    const uint64_t a[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    return call_restoring((Ctx *)g, sp, fn, a, 8);
}

/* HOS_HOSTCALL_STATS=1: at exit, how often each host call ran (the NVN surface a game really uses). */
void hos_hostcall_stats(void) {
    if (!getenv("HOS_HOSTCALL_STATS")) return;
    fprintf(stderr, "[hos] host calls:\n");
    for (int32_t i = 0; i < ncalls; i++) fprintf(stderr, "  %10ld  %s\n", calls[i].calls, calls[i].name);
    int32_t end = ring_pos, start = end > RING ? end - RING : 0;
    fprintf(stderr, "[hos] last %ld host calls (oldest first): thread, call(x0, x1, x2, x3) lr\n", end - start);
    for (int32_t k = start; k < end; k++) {
        Recent *r = &ring[k & (RING - 1)];
        fprintf(stderr, "  %6u %s(%#llx, %#llx, %#llx, %#llx) lr %#llx\n", r->thread, calls[r->call].name,
                (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2],
                (unsigned long long)r->x[3], (unsigned long long)r->lr);
    }
    fflush(stderr);
}
