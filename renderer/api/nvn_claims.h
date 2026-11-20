/* Pool claims (docs/direct/pool-claims.md): a game patch names what the memory pools an engine function makes hold,
   around that function's body. The back end serves only claimed pools; nvnMemoryPoolInitialize with no claim is an
   error (runtime/nvn_native.c). A claim covers every pool the claiming thread makes between begin and end; claims
   nest, and the outermost one holds (the most specific caller of a generic allocator decides). */
#pragma once

enum {
    NVN_CLAIM_NONE = 0,
    NVN_CLAIM_TEXTURES,   /* texture storage: CPU memory only; texels come from the store, uploads and the GPU's writes */
    NVN_CLAIM_TARGETS,    /* render-target storage: no memory behind its GPU range (virtual and physical pools too) */
    NVN_CLAIM_HOST,       /* read by the host only: shader code, command memory */
    NVN_CLAIM_VRAM,       /* GPU-only memory the back end owns */
    NVN_CLAIM_UPLOAD,     /* CPU writes the GPU reads, handed over by patches that report them */
    NVN_CLAIM_READBACK,   /* GPU writes the CPU reads, copied back when their submit completes */
    NVN_CLAIM_LAYER0,     /* (temporary, until its pools move) guest memory imported as it is */
    NVN_CLAIM_KINDS
};

/* by: a fixed string naming the patch, for the log */
void nvn_claim_begin(int kind, const char *by);
void nvn_claim_end(void);
