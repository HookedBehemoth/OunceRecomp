/* (hookshot) The guest's FP mode on the host: MXCSR follows the guest FPCR, and the FP routines the lifted code calls.

   The lifted code (docs/lifter-output.md, "FP and NEON") computes FP natively, so the host's MXCSR is the guest's FPCR on every
   thread while guest code runs: FPCR.FZ is MXCSR.DAZ|FTZ, RMode is RC, every exception stays masked. It is set where
   FPCR changes or a guest context starts running on a host thread:
   - MSR FPCR: the lifted code calls a64_fpcr_write;
   - thread start (hos_thread.c thread_main) and both sides of a fiber switch (fiber_main, fiber_switch): a host
     thread or fiber does not reliably start from, or keep, the MXCSR its guest context needs.

   FPSR: the lifted code keeps no cumulative flags per instruction; MRS FPSR (a64_fpsr_read) is the context's FPSR with MXCSR's
   sticky bits folded in, MSR FPSR (a64_fpsr_write) clears them.

   FRECPE and FRSQRTE are inline in the lifted code (nx2/lift2/fp_estimate.py); they read FZ and RMode from the
   context's FPCR, which this file keeps MXCSR equal to. */
#ifndef HOS_FP_TEST
#include "a64.h"
#endif
#include <stdint.h>
#include <xmmintrin.h>

/* MXCSR for a guest FPCR: FZ -> DAZ (bit 6) | FTZ (bit 15); RMode RN/RP/RM/RZ -> RC 0/2/1/3; exceptions masked */
unsigned hos_fp_mxcsr(uint64_t fpcr) {
    unsigned rm = (unsigned)(fpcr >> 22) & 3, m = 0x1F80u;
    if (fpcr >> 24 & 1) m |= 0x8040u;
    return m | (rm == 1 ? 2u : rm == 2 ? 1u : rm) << 13;
}

/* MXCSR = the context's FPCR, keeping the sticky exception bits */
/* HOS_FP_LOG=1 (diagnostic): hos_thread.c sets this to log changes of a thread's FP mode */
void (*hos_fp_log_hook)(unsigned long long fpcr, unsigned from, unsigned to);

void a64_fpcr_sync(Ctx *A64_NOESC c) {
    const unsigned m = hos_fp_mxcsr(c->fpcr);
    if (hos_fp_log_hook && (_mm_getcsr() & ~0x3Fu) != m) hos_fp_log_hook(c->fpcr, _mm_getcsr(), m);
    _mm_setcsr(m | (_mm_getcsr() & 0x3Fu));
}

uint64_t a64_fpcr_read(Ctx *A64_NOESC c) { return c->fpcr & 0xFFFFFFFFu; }

/* (bits 32 and up of c->fpcr are the host's; MSR keeps them) */
void a64_fpcr_write(Ctx *A64_NOESC c, uint64_t v) {
    c->fpcr = (v & 0xFFFFFFFFu) | (c->fpcr & ~(uint64_t)0xFFFFFFFFu);
    a64_fpcr_sync(c);
}

/* FPSR = the context's | MXCSR's sticky bits: IE -> IOC, ZE -> DZC, OE -> OFC, UE -> UFC, PE -> IXC, and DE -> IDC
   under FZ only (without FZ ARM has no input denormal flag for what x86 flags as DE) */
uint64_t a64_fpsr_read(Ctx *A64_NOESC c) {
    unsigned m = _mm_getcsr();
    uint64_t f = c->fpsr;
    f |= (m & 1u ? 1u : 0) | (m & 4u ? 2u : 0) | (m & 8u ? 4u : 0) | (m & 16u ? 8u : 0) | (m & 32u ? 16u : 0);
    if ((m & 2u) && (c->fpcr >> 24 & 1)) f |= 128u;
    return f;
}

void a64_fpsr_write(Ctx *A64_NOESC c, uint64_t v) {
    c->fpsr = v;
    _mm_setcsr(_mm_getcsr() & ~0x3Fu);
}
