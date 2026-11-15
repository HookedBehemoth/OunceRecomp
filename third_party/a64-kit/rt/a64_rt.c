/* a64-kit runtime for lifted code: dispatch, indirect branches, traps, the exclusive monitor, the counter.
   The OS layer (hos/) overrides the weak hooks: a64_svc, a64_on_trap, a64_cntpct_host. */
#define A64_RT_IMPL   /* (the out-of-line exclusive monitor and barrier: rt/a64.h inlines them in lifted code) */
#include "a64.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define A64_WEAK
#else
#define A64_WEAK __attribute__((weak))
#endif

extern const uint32_t a64_fn_count;
extern const uint32_t a64_fn_addr[];
extern const A64Fn a64_fn_ptr[];

/* Indirect calls (C++ virtual calls above all) look their target up here. A direct-mapped cache in front of the
   binary search over every function: each slot is one 64-bit word (address << 32 | the function's offset from a64_lookup), written and read
   whole, so threads racing on a slot see an old entry or a new one, never a mix. */
static uint64_t lookup_cache[1 << 18];
#define A64_NO_FN 0x7FFFFFFF   /* a cached slot's offset when the address has no lifted function */

/* Addresses the OS layer made at run time that are plain host functions (host calls without user data: every NVN proc)
   are answered like lifted functions, so a64_lookup's cache and each call site's slots hold them and a call goes
   straight to the handler. Before, every NVN call (tens of thousands a frame, on the game's recording threads) went
   site_call -> a64_lookup -> a64_call_miss -> its handler. */
A64_WEAK A64Fn a64_lookup_host(uint64_t addr) { (void)addr; return NULL; }

A64Fn a64_lookup(uint64_t addr) {
    if (addr >> 32 || !addr) return NULL;
    uint32_t a = (uint32_t)addr, lo = 0, hi = a64_fn_count;
    uint64_t *slot = &lookup_cache[(a >> 2 ^ a >> 20) & 0x3FFFF];
    const uint64_t e = __atomic_load_n(slot, __ATOMIC_RELAXED);
    const intptr_t anchor = (intptr_t)&a64_lookup;
    if ((uint32_t)(e >> 32) == a) return (int32_t)(uint32_t)e == A64_NO_FN ? NULL : (A64Fn)(anchor + (int32_t)(uint32_t)e);
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (a64_fn_addr[mid] < a) lo = mid + 1;
        else hi = mid;
    }
    if (lo < a64_fn_count && a64_fn_addr[lo] == a) {
        const intptr_t delta = (intptr_t)a64_fn_ptr[lo] - anchor;
        if (delta == (int32_t)delta) __atomic_store_n(slot, (uint64_t)a << 32 | (uint32_t)(int32_t)delta, __ATOMIC_RELAXED);
        return a64_fn_ptr[lo];
    }
    A64Fn h = a64_lookup_host(addr);
    if (h) {
        const intptr_t delta = (intptr_t)h - anchor;
        if (delta == (int32_t)delta && (int32_t)delta != A64_NO_FN)
            __atomic_store_n(slot, (uint64_t)a << 32 | (uint32_t)(int32_t)delta, __ATOMIC_RELAXED);
        return h;
    }
    /* (remembered too: calls into the OS layer's other host calls searched the whole table each time) */
    __atomic_store_n(slot, (uint64_t)a << 32 | (uint32_t)A64_NO_FN, __ATOMIC_RELAXED);
    return NULL;
}

/* Called when a thread cannot continue (trap, missing function). The OS layer decides what that means. */
A64_WEAK void a64_on_trap(Ctx *c, uint64_t pc, uint32_t insn, const char *why) {
    fprintf(stderr, "[a64] %s at %#llx (insn %08x), lr %#llx sp %#llx\n", why, (unsigned long long)pc, insn,
            (unsigned long long)c->x[30], (unsigned long long)c->x[31]);
    fflush(stderr);
    abort();
}

void a64_trap(Ctx *c, uint64_t pc, uint32_t insn) {
    c->pc = pc;
    c->halted = 1;
    a64_on_trap(c, pc, insn, insn ? "trap" : "branch to a non-function");
}

/* Addresses the OS layer made at run time (host calls: e.g. NVN procs handed out by nvnDeviceGetProcAddress) are
   not in the lifted function table; the OS layer claims them here. Returns 1 if it ran the call. */
A64_WEAK int a64_call_miss(Ctx *c, uint64_t target) { (void)c; (void)target; return 0; }
/* (hookshot) every BR that is not a jump table and misses its site's cache asks this first: a longjmp's BR to a
   setjmp return address (host/hos_setjmp.c), which is an entry as well, must unwind the host stack rather than
   call that entry (it does not return when it is one). Targets it takes are never cached at the site. */
A64_WEAK void a64_ijump_hook(Ctx *c, uint64_t target) { (void)c; (void)target; }

/* (hookshot) Code pointers are host function pointers: translated where they originate (docs/lifter.md, "Indirect
   calls and jumps": the relocated slots, written at load from gen/a64_code_slots.c; the lifted code's ADR / ADRP+ADD of
   a function entry; what the host hands out). Guest code addresses are below 4 GB and host code is far above, so a
   target with high bits set is called as it is. A guest address still takes the lookup below. */
#define A64_HOST_FN(t) ((t) >> 32 != 0)

void a64_icall(Ctx *c, uint64_t target) {
    if (A64_HOST_FN(target)) {
        ((A64Fn)(uintptr_t)target)(c);
        return;
    }
    A64Fn f = a64_lookup(target);
    if (!f) {
        if (a64_call_miss(c, target)) return;
        c->pc = target;
        c->halted = 1;
        a64_on_trap(c, target, 0, "indirect call to an address with no function");
        return;
    }
    f(c);
}

void a64_ijump(Ctx *c, uint64_t target) {
    if (A64_HOST_FN(target)) {
        ((A64Fn)(uintptr_t)target)(c);
        return;
    }
    a64_ijump_hook(c, target);
    A64Fn f = a64_lookup(target);
    if (!f) {
        if (a64_call_miss(c, target)) return;
        c->pc = target;
        c->halted = 1;
        a64_on_trap(c, target, 0, "indirect jump to an address with no function");
        return;
    }
    f(c);
}

/* (hookshot) The lifted BLR and BR call a host pointer directly and come here only with a guest address
   (a64_icall_guest / a64_ijump_guest, the site's pc passed): a code pointer whose origin was not translated, a
   setjmp return address (longjmp), a host call with user data, 0. Every call is counted and every site is reported
   once (a64_old_path_site, the host's log); host/code_ptrs.c prints the counts. When they stay zero over long play,
   this path and the lookup's cache can go. */
#define OLD_SITES (1u << 16)
static uint32_t old_site[OLD_SITES];
static volatile uint64_t old_calls, old_sites, guest_received;
A64_WEAK void a64_old_path_site(uint64_t site, uint64_t target, int jump) { (void)site; (void)target; (void)jump; }

static void old_path(uint64_t site, uint64_t target, int jump) {
    __atomic_add_fetch(&old_calls, 1, __ATOMIC_RELAXED);
    const uint32_t s = (uint32_t)site;
    uint32_t i = (s >> 2) * 0x9E3779B1u >> 16;
    for (uint32_t probe = 0; probe < 256; probe++, i = (i + 1) & (OLD_SITES - 1)) {
        uint32_t k = __atomic_load_n(&old_site[i], __ATOMIC_RELAXED);
        if (k == s) return;
        if (!k && __atomic_compare_exchange_n(&old_site[i], &k, s, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            __atomic_add_fetch(&old_sites, 1, __ATOMIC_RELAXED);
            a64_old_path_site(site, target, jump);
            return;
        }
        if (k == s) return;
    }
}
void a64_icall_guest(Ctx *c, uint64_t target, uint64_t site) {
    old_path(site, target, 0);
    a64_icall(c, target);
}
void a64_ijump_guest(Ctx *c, uint64_t target, uint64_t site) {
    old_path(site, target, 1);
    a64_ijump(c, target);
}

/* The host's receiving paths (thread entries, fiber functions, the TLS hooks): the function a code pointer the game
   handed over names; a guest-valued one is looked up and counted */
A64Fn a64_fn_of(uint64_t v) {
    if (A64_HOST_FN(v)) return (A64Fn)(uintptr_t)v;
    __atomic_add_fetch(&guest_received, 1, __ATOMIC_RELAXED);
    return a64_lookup(v);
}
void a64_call_received(Ctx *c, uint64_t v) {
    if (!A64_HOST_FN(v)) __atomic_add_fetch(&guest_received, 1, __ATOMIC_RELAXED);
    a64_icall(c, v);
}
void a64_code_ptr_counts(uint64_t *sites, uint64_t *calls, uint64_t *received) {
    *sites = __atomic_load_n(&old_sites, __ATOMIC_RELAXED);
    *calls = __atomic_load_n(&old_calls, __ATOMIC_RELAXED);
    *received = __atomic_load_n(&guest_received, __ATOMIC_RELAXED);
}

/* The guest address of a code pointer, for logs and the game's own checks (a row, docs/lifter.md): a host function
   pointer of the table maps back to its guest address (a sorted copy of the table, made on first use); others are
   returned as they are */
typedef struct { uint64_t ptr, addr; } ByPtr;
static ByPtr *by_ptr;   /* (the table sorted by host pointer) */
static int by_ptr_cmp(const void *a, const void *b) {
    const uint64_t x = ((const ByPtr *)a)->ptr, y = ((const ByPtr *)b)->ptr;
    return x < y ? -1 : x > y;
}
A64_WEAK uint64_t a64_guest_of_host(uint64_t v) { (void)v; return 0; }   /* (host calls: host/hos_hostcall.c) */
uint64_t a64_guest_of(uint64_t v) {
    if (!A64_HOST_FN(v)) return v;
    ByPtr *t = __atomic_load_n(&by_ptr, __ATOMIC_ACQUIRE);
    if (!t) {
        ByPtr *n = (ByPtr *)malloc(sizeof *n * a64_fn_count);
        if (!n) return v;
        for (uint32_t i = 0; i < a64_fn_count; i++) n[i].ptr = (uint64_t)(uintptr_t)a64_fn_ptr[i], n[i].addr = a64_fn_addr[i];
        qsort(n, a64_fn_count, sizeof *n, by_ptr_cmp);
        ByPtr *was = NULL;
        if (__atomic_compare_exchange_n(&by_ptr, &was, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) t = n;
        else free(n), t = was;
    }
    uint32_t lo = 0, hi = a64_fn_count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) >> 1;
        if (t[mid].ptr < v) lo = mid + 1;
        else hi = mid;
    }
    if (lo < a64_fn_count && t[lo].ptr == v) return t[lo].addr;
    const uint64_t h = a64_guest_of_host(v);
    return h ? h : v;
}

/* A64_SITE_PROFILE=<file> (profiling runs, for the lifter's A64_DEVIRT): call sites remember no targets, so every BLR
   comes through here and counts as (call site, target), the site being the instruction before the return address the
   lifted code put in x30. Every ~5 s the file is rewritten, "site target calls" a line (hex, hex, decimal);
   scripts/devirt.py picks each site's dominant target from it. */
#define SITE_PROF_SLOTS (1u << 20)
static struct { uint64_t key, n; } *site_prof;   /* key = site << 32 | target, open addressing */
static int site_prof_on = -1;
static volatile long site_prof_writing;
static uint64_t site_prof_last;

static uint64_t prof_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void site_prof_write(void) {
    if (__atomic_exchange_n(&site_prof_writing, 1, __ATOMIC_ACQUIRE)) return;
    const uint64_t now = prof_ms();
    if (now - site_prof_last >= 5000) {
        site_prof_last = now;
        FILE *f = fopen(getenv("A64_SITE_PROFILE"), "w");
        if (f) {
            for (uint32_t i = 0; i < SITE_PROF_SLOTS; i++) {
                const uint64_t k = __atomic_load_n(&site_prof[i].key, __ATOMIC_RELAXED);
                if (k) fprintf(f, "%x %x %llu\n", (uint32_t)(k >> 32), (uint32_t)k,
                               (unsigned long long)__atomic_load_n(&site_prof[i].n, __ATOMIC_RELAXED));
            }
            fclose(f);
        }
    }
    __atomic_store_n(&site_prof_writing, 0, __ATOMIC_RELEASE);
}

static void site_prof_count(uint64_t site, uint64_t target) {
    if (site >> 32 || target >> 32) return;
    const uint64_t key = site << 32 | target;
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ULL) >> 44);
    for (uint32_t probe = 0; probe < 64; probe++, i = (i + 1) & (SITE_PROF_SLOTS - 1)) {
        uint64_t k = __atomic_load_n(&site_prof[i].key, __ATOMIC_RELAXED);
        if (!k && __atomic_compare_exchange_n(&site_prof[i].key, &k, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) k = key;
        if (k == key) {
            if ((__atomic_add_fetch(&site_prof[i].n, 1, __ATOMIC_RELAXED) & 0xFFFF) == 0) site_prof_write();
            return;
        }
    }
}

static int site_profiling(void) {
    if (site_prof_on < 0) {
        int on = getenv("A64_SITE_PROFILE") != NULL;
        if (on) {
            site_prof = calloc(SITE_PROF_SLOTS, sizeof *site_prof);
            on = site_prof != NULL;
            site_prof_last = prof_ms();
        }
        site_prof_on = on;
    }
    return site_prof_on;
}

/* A64_ICALL / A64_IJUMP (a64.h) when the site's first slot does not hold the target: its second slot, else looked
   up and remembered in a free slot (only the first two targets: a site calling many would otherwise keep rewriting a
   line other threads read) */
static void site_call(Ctx *c, uint64_t target, uint64_t *site, int call, const char *why) {
    if (site_prof_on && site_profiling()) {   /* (A64_SITE_PROFILE: nothing remembered, every call counted) */
        if (call) {   /* (a BR's x30 is not its site) */
            site_prof_count(c->x[30] - 4, target);
            a64_icall(c, target);
        } else a64_ijump(c, target);
        return;
    }
    const uint64_t e = __atomic_load_n(&site[1], __ATOMIC_RELAXED);
    if (e >> 32 == target && e) {   /* (the site's second target) */
        ((A64Fn)((intptr_t)&a64_lookup + (int32_t)(uint32_t)e))(c);
        return;
    }
    A64Fn f = a64_lookup(target);
    if (!f) {
        if (a64_call_miss(c, target)) return;
        c->pc = target;
        c->halted = 1;
        a64_on_trap(c, target, 0, why);
        return;
    }
    const intptr_t delta = (intptr_t)f - (intptr_t)&a64_lookup;
    if (delta == (int32_t)delta) {   /* (the first free slot of the two; a full site keeps its first two targets) */
        const uint64_t entry = target << 32 | (uint32_t)(int32_t)delta;
        if (!__atomic_load_n(&site[0], __ATOMIC_RELAXED)) __atomic_store_n(&site[0], entry, __ATOMIC_RELAXED);
        else if (!e) __atomic_store_n(&site[1], entry, __ATOMIC_RELAXED);
    }
    f(c);
}
void a64_icall_site(Ctx *c, uint64_t target, uint64_t *site) {
    site_call(c, target, site, 1, "indirect call to an address with no function");
}
void a64_ijump_site(Ctx *c, uint64_t target, uint64_t *site) {
    a64_ijump_hook(c, target);
    site_call(c, target, site, 0, "indirect jump to an address with no function");
}

A64_WEAK void a64_svc(Ctx *c, uint32_t imm) {
    char why[32];
    snprintf(why, sizeof why, "svc #%#x", imm);
    a64_on_trap(c, c->pc, 0xD4000001u | (imm << 5), why);
}

void a64_ret_mismatch(Ctx *c, uint64_t pc) { a64_on_trap(c, pc, 0, "return to an address that is not the caller"); }

void recomp_unhandled(GuestContext *c, uint32_t insn, uint64_t pc) { a64_trap(c, pc, insn); }
void recomp_ic_ivau(GuestContext *c, uint64_t address, int v) { (void)c; (void)address; (void)v; }
void recomp_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* ---- exclusive monitor --------------------------------------------------------------------------------
   LDXR remembers (address, size, value). STXR succeeds only if the address and size match and memory still
   holds that value, by compare-and-swap, so a lock taken by one host thread is respected by every other. */
static uint64_t load_n(uint64_t a, uint32_t size) {
    switch (size) {
    case 1: return __atomic_load_n((uint8_t *)A64_PTR(a), __ATOMIC_SEQ_CST);
    case 2: return __atomic_load_n((uint16_t *)A64_PTR(a), __ATOMIC_SEQ_CST);
    case 4: return __atomic_load_n((uint32_t *)A64_PTR(a), __ATOMIC_SEQ_CST);
    default: return __atomic_load_n((uint64_t *)A64_PTR(a), __ATOMIC_SEQ_CST);
    }
}


static int cas_n(uint64_t a, uint32_t size, uint64_t expect, uint64_t value) {
    switch (size) {
    case 1: { uint8_t e = (uint8_t)expect; return __atomic_compare_exchange_n((uint8_t *)A64_PTR(a), &e, (uint8_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    case 2: { uint16_t e = (uint16_t)expect; return __atomic_compare_exchange_n((uint16_t *)A64_PTR(a), &e, (uint16_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    case 4: { uint32_t e = (uint32_t)expect; return __atomic_compare_exchange_n((uint32_t *)A64_PTR(a), &e, (uint32_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    default: { uint64_t e = expect; return __atomic_compare_exchange_n((uint64_t *)A64_PTR(a), &e, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    }
}

uint64_t recomp_ldxr(GuestContext *c, uint64_t addr, uint32_t size) {
    uint64_t v = load_n(addr, size);
    c->excl_addr = addr; c->excl_size = size; c->excl_lo = v; c->excl_valid = 1;
    return v;
}

uint32_t recomp_stxr(GuestContext *c, uint64_t addr, uint32_t size, uint64_t value) {
    int ok = c->excl_valid && c->excl_addr == addr && c->excl_size == size && cas_n(addr, size, c->excl_lo, value);
    c->excl_valid = 0;
    return ok ? 0 : 1;
}

void recomp_ldxp(GuestContext *c, uint64_t addr, uint32_t size, uint64_t *lo, uint64_t *hi) {
    if (size == 4) {
        uint64_t v = load_n(addr, 8);
        *lo = (uint32_t)v; *hi = v >> 32;
    } else {
        /* 128-bit: two loads. Single-copy atomicity of the pair is not guaranteed here; the STXP below is. */
        *lo = load_n(addr, 8); *hi = load_n(addr + 8, 8);
    }
    c->excl_addr = addr; c->excl_size = 2 * size; c->excl_lo = *lo; c->excl_hi = *hi; c->excl_valid = 1;
}

uint32_t recomp_stxp(GuestContext *c, uint64_t addr, uint32_t size, uint64_t lo, uint64_t hi) {
    int ok = c->excl_valid && c->excl_addr == addr && c->excl_size == 2 * size;
    if (ok && size == 4) {
        ok = cas_n(addr, 8, (c->excl_lo & 0xFFFFFFFFu) | (c->excl_hi << 32), (lo & 0xFFFFFFFFu) | (hi << 32));
    } else if (ok) {
#if defined(__SIZEOF_INT128__) && !defined(__wasm__)
        unsigned __int128 e = ((unsigned __int128)c->excl_hi << 64) | c->excl_lo;
        unsigned __int128 v = ((unsigned __int128)hi << 64) | lo;
        ok = __atomic_compare_exchange_n((unsigned __int128 *)A64_PTR(addr), &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#else
        ok = load_n(addr, 8) == c->excl_lo && load_n(addr + 8, 8) == c->excl_hi;
        if (ok) { recomp_store64(c, addr, lo); recomp_store64(c, addr + 8, hi); }
#endif
    }
    c->excl_valid = 0;
    return ok ? 0 : 1;
}

void recomp_clrex(GuestContext *c) { c->excl_valid = 0; }

/* ---- CNTPCT_EL0: the 19.2 MHz counter ---------------------------------------------------------------- */
A64_WEAK uint64_t a64_cntpct_host(void) {
    static uint64_t base;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    if (!base) base = ns;
    return (ns - base) * 192ULL / 10000ULL;
}

uint64_t recomp_cntpct(GuestContext *c) { (void)c; return a64_cntpct_host(); }
