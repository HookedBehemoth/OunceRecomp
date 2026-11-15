/* a64-kit guest model: what lifted AArch64 code and the host runtime see of a guest thread: the context
   (`GuestContext`), guest memory (`A64_PTR`, `recomp_load*` / `recomp_store*`) and the runtime's
   entry points (a64_rt.c). Hookshot's copy is generated from this by nx2/rtgen.py (the 64-bit guest window). */
#ifndef A64_H
#define A64_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct GuestContext {
    uint64_t x[32];             /* x[31] is SP */
    uint64_t pc;                /* only written where the lifter stops a thread (traps, unhandled) */
    uint8_t n, z, c, v;         /* NZCV, one byte each */
    int halted;
    uint64_t vreg[32][2];       /* Q0-Q31 as two 64-bit halves */
    uint64_t tpidr_el0;         /* the guest's own thread pointer (C++ TLS) */
    uint64_t tpidrro_el0;       /* Horizon's thread-local region, set by the OS layer */
    uint64_t fpcr, fpsr;
    /* Exclusive monitor (LDXR/STXR): the address, size and value LDXR saw. STXR compare-and-swaps against
       that value, so it works with real host threads (dynarmic's model). */
    uint64_t excl_addr, excl_lo, excl_hi;
    uint32_t excl_size;
    int excl_valid;
    void *thread;               /* the OS layer's thread object */
} GuestContext;
typedef GuestContext Ctx;

/* A64_NOESC on every context parameter lifted code can call (the lifted functions, A64Fn, the runtime's and the OS
   layer's entry points): none of them keeps the pointer past the call. Without it each call "captures" the context, and
   LLVM must assume any guest load or store (an address made from an integer) might touch it: after every guest store
   it reloaded guest registers, and it kept every store to them. It also needs the lifted code built with a higher
   -capture-tracking-max-uses-to-explore (build.py): past 100 uses of the pointer LLVM stops looking and assumes
   it escaped, and a large lifted function uses it thousands of times. (Its effect on one of TOTK's hot functions:
   context loads 539 -> 278, stores 1068 -> 893, instructions -6%.) -DA64_NOESC= turns it off. */
#ifndef A64_NOESC
#if defined(__clang__)
#define A64_NOESC __attribute__((noescape))
#else
#define A64_NOESC
#endif
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define A64_INLINE static __forceinline
#else
#define A64_INLINE static inline __attribute__((always_inline))
#endif

/* Guest memory: the 4 GB guest window starts at a constant host address, so the base folds into every access
   instead of being reloaded from a global (with -fno-strict-aliasing any guest byte store could have changed
   that global). Natively the window is reserved at A64_HOST_BASE (hos_main.c); with A64_MEM_ZERO (web builds)
   guest address = wasm address, which needs -fno-delete-null-pointer-checks because guest address 0 is real. */
#ifdef A64_MEM_ZERO
#define A64_MEM ((uint8_t *)0)
#else
#ifndef A64_HOST_BASE
#define A64_HOST_BASE 0x100000000000ULL   /* 16 TB: far from anything Windows or glibc map by default */
#endif
#define A64_MEM ((uint8_t *)A64_HOST_BASE)
#endif
#define A64_PTR(a) (A64_MEM + (uint32_t)(a))

A64_INLINE uint64_t recomp_load8(GuestContext *c, uint64_t a) { (void)c; return *(const uint8_t *)A64_PTR(a); }
A64_INLINE uint64_t recomp_load16(GuestContext *c, uint64_t a) { uint16_t v; (void)c; memcpy(&v, A64_PTR(a), 2); return v; }
A64_INLINE uint64_t recomp_load32(GuestContext *c, uint64_t a) { uint32_t v; (void)c; memcpy(&v, A64_PTR(a), 4); return v; }
A64_INLINE uint64_t recomp_load64(GuestContext *c, uint64_t a) { uint64_t v; (void)c; memcpy(&v, A64_PTR(a), 8); return v; }
A64_INLINE void recomp_store8(GuestContext *c, uint64_t a, uint64_t v) { (void)c; *(uint8_t *)A64_PTR(a) = (uint8_t)v; }
A64_INLINE void recomp_store16(GuestContext *c, uint64_t a, uint64_t v) { uint16_t t = (uint16_t)v; (void)c; memcpy(A64_PTR(a), &t, 2); }
A64_INLINE void recomp_store32(GuestContext *c, uint64_t a, uint64_t v) { uint32_t t = (uint32_t)v; (void)c; memcpy(A64_PTR(a), &t, 4); }
A64_INLINE void recomp_store64(GuestContext *c, uint64_t a, uint64_t v) { (void)c; memcpy(A64_PTR(a), &v, 8); }
A64_INLINE void recomp_ldp32(GuestContext *c, uint64_t a, uint64_t *lo, uint64_t *hi) { *lo = recomp_load32(c, a); *hi = recomp_load32(c, a + 4); }
A64_INLINE void recomp_ldp64(GuestContext *c, uint64_t a, uint64_t *lo, uint64_t *hi) { *lo = recomp_load64(c, a); *hi = recomp_load64(c, a + 8); }
A64_INLINE void recomp_stp32(GuestContext *c, uint64_t a, uint64_t v0, uint64_t v1) { recomp_store32(c, a, v0); recomp_store32(c, a + 4, v1); }
A64_INLINE void recomp_stp64(GuestContext *c, uint64_t a, uint64_t v0, uint64_t v1) { recomp_store64(c, a, v0); recomp_store64(c, a + 8, v1); }

/* Everything is lifted at its final guest address, so the translator's module base is a constant 0. */
#define g_module_base ((uint64_t)0)

/* ---- control flow, as ppc-kit spells it -------------------------------------------------------------- */
#if defined(__wasm__) && !defined(__wasm_tail_call__)
#define TAILCALL(f) do { f(c); return; } while (0)
#else
#define TAILCALL(f) __attribute__((musttail)) return f(c)
#endif

/* ---- the lifter's runtime (rt/a64_rt.c) ------------------------------------------------------------- */
typedef void (*A64Fn)(Ctx *A64_NOESC);
A64Fn a64_lookup(uint64_t addr);                    /* function at a guest address, or NULL */
void a64_icall(Ctx *A64_NOESC c, uint64_t target);            /* BLR: indirect call */
/* (hookshot) setjmp in lifted code: a call of a function that calls __setjmp is wrapped in a host __builtin_setjmp
   on a slot (pushed per thread with that function's setjmp return addresses, popped after the call); the guest
   longjmp's final BR to one of them comes back to the newest such slot through the host's a64_call_miss
   (host/hos_setjmp.c), and the caller runs the function's code from that address (recomp_sj_target) */
void **recomp_sj_push(Ctx *A64_NOESC c, uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3);
uint64_t recomp_sj_target(void **slot);
void recomp_sj_pop(void **slot);
void hos_sj_call(Ctx *A64_NOESC c, void (*f)(Ctx *), uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3);   /* (hookshot: lift 2's form) */
void a64_ijump(Ctx *A64_NOESC c, uint64_t target);            /* BR that is not a jump table: indirect tail call */
/* The lifted BLR / BR: a cache per call site, filled by its first target (the guest address in the top 32 bits, the
   function as an offset from a64_lookup in the low 32). A site calling that target again reads no shared table and
   has an indirect branch of its own to predict; other targets go through a64_icall_site / a64_ijump_site, which try
   the site's second slot (its second target) and else look them up as a64_icall does. A64_NO_SITE_CACHE: the plain
   calls. */
void a64_icall_site(Ctx *A64_NOESC c, uint64_t target, uint64_t *site);
void a64_ijump_site(Ctx *A64_NOESC c, uint64_t target, uint64_t *site);
#ifdef A64_NO_SITE_CACHE
#define A64_ICALL(c, t) a64_icall(c, t)
#define A64_IJUMP(c, t) a64_ijump(c, t)
#else
#define A64_SITE_CALL(c, t, slow) do { \
    static uint64_t a64_site_[2]; \
    const uint64_t a64_site_t_ = (t), a64_site_e_ = __atomic_load_n(&a64_site_[0], __ATOMIC_RELAXED); \
    if (__builtin_expect(a64_site_e_ >> 32 == a64_site_t_ && a64_site_e_ != 0, 1)) \
        ((A64Fn)((intptr_t)&a64_lookup + (int32_t)(uint32_t)a64_site_e_))(c); \
    else slow(c, a64_site_t_, a64_site_); \
} while (0)   /* (its names must not be ones the lifted code passes in, such as _t) */
#define A64_ICALL(c, t) A64_SITE_CALL(c, t, a64_icall_site)
#define A64_IJUMP(c, t) A64_SITE_CALL(c, t, a64_ijump_site)
#endif
void a64_svc(Ctx *A64_NOESC c, uint32_t imm);                 /* SVC #imm (sdk only) */
void a64_trap(Ctx *A64_NOESC c, uint64_t pc, uint32_t insn);  /* BRK, HLT, UDF and words that are not code */
void a64_ret_mismatch(Ctx *A64_NOESC c, uint64_t pc);         /* RET to something other than the caller (debug builds) */
/* (hookshot) the guest's FP mode on the host (host/hos_fp.c): MXCSR follows FPCR; lift 2's MRS/MSR FPCR/FPSR */
unsigned hos_fp_mxcsr(uint64_t fpcr);
void a64_fpcr_sync(Ctx *A64_NOESC c);                          /* MXCSR = c->fpcr */
/* (hookshot) the counter (a64_rt.c), also for host/hos_thread.c */
uint64_t recomp_cntpct(GuestContext *c);
uint64_t a64_fpcr_read(Ctx *A64_NOESC c);
void a64_fpcr_write(Ctx *A64_NOESC c, uint64_t v);
uint64_t a64_fpsr_read(Ctx *A64_NOESC c);
void a64_fpsr_write(Ctx *A64_NOESC c, uint64_t v);

#endif
