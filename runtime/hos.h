/* Horizon OS layer (HLE of the Nintendo SDK at the nn:: boundary) for a64-kit games.

   An import or SDK function the game calls is a C function `void imp_<mangled name>(Ctx *c)` (HLE(...), runtime/patch.h):
   arguments in c->x[0..7], the result in c->x[0]. A symbol with such a definition is hooked both where the game
   imports it and at the sdk's own definition, so lifted SDK code reaching it lands here too (nx_image.py).
   Everything without a definition gets a generated stub that logs its first call and returns 0.

   Guest address map (mkw-recomp: the 64-bit window of a 39-bit process, reserved whole and committed as used):
     0x01000000 - 0x07000000      system region: our own guest objects (main stack, TLS blocks, thread regions)
     0x80000000 - image end       rtld, main, subsdk0, subsdk1, sdk in Eden's layout (hos_image_base, the lift's)
     0x1000000000 - ...           heap (SetMemoryHeapSize), up from 64 GB
     ... - 0x8000000000           address regions (AllocateAddressRegion), down from 512 GB */
#ifndef HOS_H
#define HOS_H
#include "a64.h"
#include "os.h"
#include <stdarg.h>

/* rt/a64_rt.c: code pointers are host function pointers (docs/lifter-output.md, "Indirect calls and jumps"). What the game
   hands the host (thread entries, fiber functions, the TLS hooks) goes through these: a64_fn_of the function a code
   pointer names, a64_call_received a call of it (a guest-valued one is looked up and counted); a64_guest_of the guest
   address of a code pointer, for logs */
A64Fn a64_fn_of(uint64_t v);
void a64_call_received(Ctx *c, uint64_t v);
uint64_t a64_guest_of(uint64_t v);
#define CODE_PTR(f) ((uint64_t)(uintptr_t)(f))   /* (a lifted function, f_<address>, as the game sees it) */
/* At rows and replaced functions (PATCH_AT, PATCH_FUNC, runtime/patch.h): host functions of plain values; a 16-byte vector
   register ("qN") is a hos_q. One declared to call guest code gets a GuestCall first: guest_call runs guest code at fn
   with the arguments, SP sp and LR 0, and puts every register back as it was (the row or function writes none); its
   x0 */
typedef uint64_t hos_q __attribute__((vector_size(16)));
typedef struct GuestCall GuestCall;
uint64_t guest_call(GuestCall *g, uint64_t sp, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
/* (the same with eight arguments, x0-x7) */
uint64_t guest_call8(GuestCall *g, uint64_t sp, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                     uint64_t a5, uint64_t a6, uint64_t a7);
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)A64_PTR(a))           /* host pointer of a guest address */
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))

#define HOS_SYSTEM_BASE 0x01000000ULL
#define HOS_SYSTEM_END 0x07000000ULL
#define HOS_HEAP_BASE 0x1000000000ULL
#define HOS_GUEST_SPACE (1ULL << 39)
extern const uint64_t hos_image_base, hos_image_end;   /* (gen/hos_image.c) */
extern const uint64_t hos_program_id;                  /* (gen/hos_image.c: main.npdm's) */
extern const char hos_main_build_id[];                 /* (gen/hos_image.c: main's NSO build id, hex) */
#define HOS_GAME_TITLE "Hookshot"
void hos_commit(uint64_t addr, uint64_t size);         /* (hos_process.c) guest pages made usable by host APIs */
#ifdef A64_MEM_ZERO
extern uint64_t hos_space_end;   /* web: the guest window ends where the host's own data starts (hos_main.c) */
#define HOS_SPACE_END hos_space_end
#else
#define HOS_SPACE_END 0x8000000000ULL
#endif
/* (hookshot) host pointer of the n guest bytes at a, NULL unless all of them lie in the guest window. For addresses
   and sizes host code takes from guest memory or arguments before it reads or writes there: GP alone wraps around
   to any host address. */
static inline void *hos_guest_range(uint64_t a, uint64_t n) {
    return a < (uint64_t)HOS_SPACE_END && n <= (uint64_t)HOS_SPACE_END - a ? GP(a) : (void *)0;
}
#define HOS_TICK_HZ 31250000ULL   /* (mkw-recomp: the Switch 2 counter; the SDK checks it at init) */

/* ---- logging: HOS_TRACE=1 logs every HLE call that uses HOS_TRACE_CALL */
extern int hos_trace;
void hos_log(const char *fmt, ...);
void hos_log_flush(void);
long long hos_vi_present_number(void);   /* (nn_vi.c) */
int hos_dbg_item(const char *item);
int hos_log_switch(const char *item);
/* HOS_BENCH (runtime/bench.c): the frames to measure, 0 when off; the game's file reads, its player updates, the presents */
int hos_bench_on(void);
void hos_bench_fs_read(void);
void hos_bench_player(int loading, int event_playing);
void hos_bench_present(int64_t now);
#define HOS_TRACE_CALL(name) do { if (hos_trace) hos_log("%s(%#llx, %#llx, %#llx, %#llx) lr %#llx", name, \
    (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], \
    (unsigned long long)c->x[3], (unsigned long long)c->x[30]); } while (0)
void hos_fatal(Ctx *c, const char *fmt, ...);
/* (the same without a context: the guest address it reports, an SVC's or a caller's) */
void hos_fatal_lr(uint64_t lr, const char *fmt, ...) __attribute__((noreturn));

/* ---- guest memory (hos_mem.c) */
uint64_t hos_sys_alloc(uint64_t size, uint64_t align);     /* zeroed, from the system region, never freed */
uint64_t hos_region_alloc(uint64_t size, uint64_t align);  /* address space above the heap */
void hos_region_free(uint64_t addr);
void hos_mem_init(uint64_t image_end);
extern uint64_t hos_heap_size, hos_heap_used, hos_total_memory, hos_program_size;

/* ---- threads (hos_thread.c) */
typedef struct HosThread HosThread;
HosThread *hos_self(void);
Ctx *hos_ctx(void);
uint64_t hos_thread_guest(HosThread *t);                   /* its nn::os::ThreadType in guest memory */
uint64_t hos_thread_id(HosThread *t);
HosThread *hos_thread_of(uint64_t guest_thread_type);      /* NULL if unknown */
HosThread *hos_thread_create(uint64_t guest_tt, uint64_t entry, uint64_t arg, uint64_t stack, uint64_t stack_size,
                             int priority, int core);
void hos_thread_start(HosThread *t);
void hos_thread_join(HosThread *t);
void hos_thread_destroy(HosThread *t);
void hos_thread_set_name(HosThread *t, const char *name);
void hos_thread_pin(os_thread *h, const char *name);   /* HOS_PIN: host CPU affinity by thread name */
const char *hos_thread_name(HosThread *t);
void hos_thread_affinity(HosThread *t, int ideal, uint64_t mask);
uint64_t hos_thread_mask(HosThread *t);
int hos_thread_priority(HosThread *t, int set, int value);
int hos_thread_core(HosThread *t);
int hos_cores(void);
/* (HOS_DBG=crit: direct_crit.h) a guest thread's block in a host call: hos_block_begin names it (1 if recorded), and
   hos_block_end after it records the span. No scheduler: guest threads run freely on their host threads (hos_thread.c) */
int hos_block_begin(int why, const void *key);
void hos_block_end(void);
void hos_thread_fp_log(long long present);   /* (hos_thread.c) HOS_FP_LOG=1: FPCR and MXCSR of the presenting thread */
int hos_current_core(uint64_t lr);   /* (hos_thread.c) GetCurrentCoreNumber for the running thread */
int hos_cv_sleep(os_cond *cv, os_mutex *lk, int64_t timeout_ns);   /* os_cond_wait for guest threads: 0 or OS_ETIMEDOUT */
void hos_thread_stack(HosThread *t, uint64_t *addr, uint64_t *size);
uint64_t hos_thread_tlr(HosThread *t);   /* its TPIDRRO_EL0: the thread local region */
uint64_t *hos_tls_slots(HosThread *t);                     /* nn::os TLS slot values */
#define HOS_TLS_SLOTS 32
Ctx *hos_boot_main_thread(uint64_t stack_size);           /* the calling host thread becomes the guest main thread */
/* the kernel side: Horizon handles and blocking for hos_svc.c's arbitration */
uint32_t hos_thread_handle(HosThread *t);
HosThread *hos_thread_by_handle(uint32_t h);
int hos_thread_prio(HosThread *t);
void hos_thread_wait(HosThread *t, int64_t timeout_ns, int *timed_out);   /* < 0: forever */
void hos_thread_wake(HosThread *t);

/* ---- for host-side devices: guest events (nn_os_sync.c) */
void hos_event_signal(uint64_t guest_event);

/* ---- videos (movie_mf.c): a video file read as compressed samples or as decoded NV12 frames */
typedef struct MovieReader MovieReader;
MovieReader *movie_reader_open(const char *host_path, int decoded, int *width, int *height, int64_t *duration_us);
/* compressed: the next sample into dst; its size, 0 at the end, -1 on an error or if it does not fit */
int movie_reader_sample(MovieReader *r, void *dst, int cap, int64_t *time_us);
/* decoded: the next frame as NV12 (width x height luma rows of `width` bytes, then the interleaved chroma rows);
   1, or 0 at the end */
int movie_reader_frame(MovieReader *r, uint8_t *dst, int64_t *time_us);
void movie_reader_close(MovieReader *r);
uint32_t movie_reader_codec(MovieReader *r);   /* the stream's FourCC: 'VP90', 'VP80', 'H264', ... */

/* ---- time */
uint64_t hos_ticks(void);
void hos_sleep_ns(int64_t ns);
int64_t hos_now_ns(void);
int64_t hos_spin_ns(void);   /* how long a sync wait spins before it sleeps: HOS_SPIN_US, default 5 us (nn_os_sync.c) */
#include "patch.h"   /* (the patch declarations: PATCH_AT, PATCH_FUNC, HLE, NV, ...) */
#endif
