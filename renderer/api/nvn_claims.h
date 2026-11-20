/* Pool claims (docs/direct/pool-claims.md): a game patch names what the memory pools an engine function makes hold,
   around that function's body. The back end serves only claimed pools; nvnMemoryPoolInitialize with no claim is an
   error (runtime/nvn_native.c). A claim covers every pool the claiming thread makes between begin and end. Claims nest:
   a kind a patch names holds over any rule, the innermost kind holds over the kinds around it (a maker's exception
   inside its caller's claim), and between rules the outermost holds (a generic allocator's rule is its callers' default). */
#pragma once
#include <stdint.h>

enum {
    NVN_CLAIM_NONE = 0,
    NVN_CLAIM_TEXTURES,   /* texture storage: CPU memory only; texels come from the store, uploads and the GPU's writes */
    NVN_CLAIM_TARGETS,    /* render-target storage: no memory behind its GPU range (virtual and physical pools too) */
    NVN_CLAIM_HOST,       /* read by the host only: shader code, command memory */
    NVN_CLAIM_VRAM,       /* GPU-only memory the back end owns */
    NVN_CLAIM_UPLOAD,     /* CPU writes the GPU reads, handed over by patches that report them */
    NVN_CLAIM_READBACK,   /* GPU writes the CPU reads, copied back when their submit completes */
    NVN_CLAIM_LOAD_ONCE,  /* CPU data written before the GPU first uses it, then never again: each page copied at its first use */
    NVN_CLAIM_LAYER0,     /* (temporary, until its pools move) guest memory imported as it is */
    NVN_CLAIM_KINDS
};

/* by: a fixed string naming the patch, for the log */
void nvn_claim_begin(int kind, const char *by);
/* the same, with the kind the patch's rule gives each pool by its NVNmemoryPoolFlags and size (a generic allocator whose
   pools differ by access) */
typedef int (*nvn_claim_rule)(uint32_t flags, uint64_t size);
void nvn_claim_begin_rule(nvn_claim_rule rule, const char *by);
void nvn_claim_end(void);
/* The CPU wrote [addr, addr + size) of a claimed pool where no bind hands it to the GPU: a range GPU work writes (a
   counter the reader zeroes), or data only shaders reach. A writer patch reports it after the write; the next submission
   copies it in, and a copy out of the GPU's older value does not land over it */
void nvn_claim_write(uint64_t addr, uint64_t size);
