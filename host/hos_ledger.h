/* Main memory by owner, the host's part: the host's larger heap users count their bytes here (pools count what they hold
   from the heap, in use or free). hos_posix_mem_ledger (hos_mem.c) reports it. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>

enum {
    HL_NVN_LISTS,   /* (nvn_list.c: the recordings' blocks, live or cached) */
    HL_NVN_CMD,     /* (nvn_native.c: the command buffers' recording memory, in use or pooled) */
    HL_TILES,       /* (direct_assets.c: terrain tiles kept until their image takes them) */
    HL_VIDEO,       /* (nn_video.c: the decoder's image pools and pictures waiting) */
    HL_N
};
/* (weak: a test that builds one of these files alone still links) */
__attribute__((weak)) int64_t hos_ledger[HL_N];
#define hos_msize(p) ((int64_t)malloc_usable_size(p))
static inline void hos_ledger_add(int k, int64_t bytes) { __atomic_fetch_add(&hos_ledger[k], bytes, __ATOMIC_RELAXED); }
/* a heap block just taken (NULL: nothing) and one about to be freed, by the heap's own size for it */
static inline void *hos_ledger_took(int k, void *p) { if (p) hos_ledger_add(k, hos_msize(p)); return p; }
static inline void hos_ledger_frees(int k, void *p) { if (p) hos_ledger_add(k, -hos_msize(p)); }
