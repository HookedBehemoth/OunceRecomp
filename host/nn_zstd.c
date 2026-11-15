/* The game's zstd decoder (zstd 1.4.x, lifted) replaced by the host's (third_party/zstd, 1.5.7, compiled into
   this file). The loading screen spent ~22% of its CPU in the lifted decoder (f_801102e0 the sequence decode), on the
   loaders' critical path (Superluminal, 2026-11-10); work/assets/zstd.md maps the game's side.

   - f_8010b380, ZSTD_decompressMultiFrame(dctx, dst, dstCap, src, srcSize, dict, dictSize, ddict): a whole file that
     fits one read chunk (the ARes loaders, f_80108da0, and two other callers). The guest DCtx is left alone: a host
     DCtx per thread does the work; the result is returned as zstd's own size_t (the error codes are the same numbers).
   - f_80115c80(job, chunk, chunkSize, ddict): a ParallelDecompThread's step through a larger file, one frame into one
     buffer, chunk by chunk. job: +0 state (1 start, 2 decoding, 3 failed, 4 done), +8 the output cursor, +16 the room
     left there (s32), +20 the content still to come (u32), +24 the guest DCtx's workspace, +48 the DCtx. A host DCtx per
     job streams it; the guest DCtx is never touched again once a job starts here. Returns (32-bit) as the guest's:
     the content still to come; -1 failed (or entered failed), -2 the output too small, -3 entered done.

   Dictionaries: the guest DDict (0x6860 bytes, made by reference: +8 its content, +0x10 the size) gets a host DDict by
   reference to the same guest bytes, made once per (DDict, content, size). */
#define ZSTD_DISABLE_ASM 1
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_MULTITHREAD 0
#define ZSTD_STATIC_LINKING_ONLY 1
#define DEBUGLEVEL 0
#include "../third_party/zstd/lib/common/debug.c"
#include "../third_party/zstd/lib/common/entropy_common.c"
#include "../third_party/zstd/lib/common/error_private.c"
#include "../third_party/zstd/lib/common/fse_decompress.c"
#include "../third_party/zstd/lib/common/xxhash.c"
#include "../third_party/zstd/lib/common/zstd_common.c"
#include "../third_party/zstd/lib/decompress/huf_decompress.c"
#include "../third_party/zstd/lib/decompress/zstd_ddict.c"
#include "../third_party/zstd/lib/decompress/zstd_decompress.c"
#include "../third_party/zstd/lib/decompress/zstd_decompress_block.c"

#include "hos.h"
#include <pthread.h>
#include <stdlib.h>

/* ---- host DDicts for guest DDicts ---- */
typedef struct { uint64_t guest, content, size; ZSTD_DDict *d; } DictEntry;
static DictEntry dicts[64];
static int ndicts;
static pthread_mutex_t dict_mu = PTHREAD_MUTEX_INITIALIZER;

static ZSTD_DDict *host_ddict(uint64_t guest) {
    if (!guest) return NULL;
    const uint64_t content = G64(guest + 8), size = G64(guest + 0x10);
    if (!hos_guest_range(content, size)) return NULL;
    pthread_mutex_lock(&dict_mu);
    ZSTD_DDict *d = NULL;
    for (int i = 0; i < ndicts && !d; i++)
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size) d = dicts[i].d;
    if (!d) {
        d = ZSTD_createDDict_byReference(GP(content), (size_t)size);
        if (d) {
            if (ndicts == 64) { ZSTD_freeDDict(dicts[0].d); memmove(dicts, dicts + 1, 63 * sizeof *dicts); ndicts--; }
            dicts[ndicts++] = (DictEntry){guest, content, size, d};
            hos_log("zstd: dictionary %u (%llu bytes) for guest DDict %#llx", ZSTD_getDictID_fromDDict(d),
                    (unsigned long long)size, (unsigned long long)guest);
        }
    }
    pthread_mutex_unlock(&dict_mu);
    return d;
}

static _Thread_local ZSTD_DCtx *tl_dctx;
static int64_t st_files, st_bytes, st_streams;

/* ZSTD_decompressMultiFrame: a whole file at once */
ENTRY_HOOK(0x8010b380) {
    const uint64_t dst = a->x[1], cap = a->x[2], src = a->x[3], n = a->x[4], dict = a->x[5], dict_size = a->x[6], ddict = a->x[7];
    if (!hos_guest_range(dst, cap) || !hos_guest_range(src, n) || (dict && !hos_guest_range(dict, dict_size))) return 0;
    if (!tl_dctx && !(tl_dctx = ZSTD_createDCtx())) return 0;
    size_t r;
    if (ddict) {
        ZSTD_DDict *d = host_ddict(ddict);
        if (!d) return 0;
        r = ZSTD_decompress_usingDDict(tl_dctx, GP(dst), (size_t)cap, GP(src), (size_t)n, d);
    } else {
        r = ZSTD_decompress_usingDict(tl_dctx, GP(dst), (size_t)cap, GP(src), (size_t)n, dict ? GP(dict) : NULL, (size_t)dict_size);
    }
    if (!ZSTD_isError(r)) {
        __atomic_fetch_add(&st_bytes, (int64_t)r, __ATOMIC_RELAXED);
        if (__atomic_add_fetch(&st_files, 1, __ATOMIC_RELAXED) % 2000 == 0)
            hos_log("zstd: native: %lld files, %lld streams, %.1f MB out", (long long)st_files, (long long)st_streams,
                    st_bytes / 1048576.0);
    } else {
        static int told;
        if (told++ < 8) hos_log("zstd: a file failed: %s (src %#llx+%llu, dst %llu)", ZSTD_getErrorName(r),
                                (unsigned long long)src, (unsigned long long)n, (unsigned long long)cap);
    }
    return hook_return(h, r);
}

/* ---- ParallelDecompThread's streaming jobs ---- */
typedef struct { uint64_t job; ZSTD_DCtx *d; } Job;
static Job jobs[64];
static int njobs;
static pthread_mutex_t job_mu = PTHREAD_MUTEX_INITIALIZER;

static ZSTD_DCtx *job_dctx(uint64_t job) {
    pthread_mutex_lock(&job_mu);
    ZSTD_DCtx *d = NULL;
    for (int i = 0; i < njobs && !d; i++)
        if (jobs[i].job == job) d = jobs[i].d;
    if (!d && njobs < 64 && (d = ZSTD_createDCtx())) jobs[njobs++] = (Job){job, d};
    pthread_mutex_unlock(&job_mu);
    return d;
}

#define J8(o) (*(uint8_t *)GP(job + (o)))
#define J32(o) (*(uint32_t *)GP(job + (o)))
#define J64(o) (*(uint64_t *)GP(job + (o)))

ENTRY_HOOK(0x80115c80) {
    const uint64_t job = a->x[0], chunk = a->x[1], ddict = a->x[3];
    const uint32_t n = (uint32_t)a->x[2];
    if (!hos_guest_range(job, 64) || !hos_guest_range(chunk, n)) return 0;
    const uint8_t state = J8(0);
    if (state == 3) return hook_return(h, 0xffffffffu);
    if (state == 4) return hook_return(h, 0xfffffffdu);
    ZSTD_DCtx *d = job_dctx(job);
    if (!d) return 0;
    if (state == 1) {   /* (the frame's header is in the first chunk) */
        const unsigned long long size = ZSTD_getFrameContentSize(GP(chunk), n);
        if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
            J8(0) = 3;
            return hook_return(h, 0xffffffffu);
        }
        ZSTD_DCtx_reset(d, ZSTD_reset_session_only);
        ZSTD_DCtx_refDDict(d, host_ddict(ddict));
        J32(20) = (uint32_t)size;
        J8(0) = 2;
        __atomic_fetch_add(&st_streams, 1, __ATOMIC_RELAXED);
    }
    const uint64_t out = J64(8);
    const int32_t room = (int32_t)J32(16);
    if (room < 0 || !hos_guest_range(out, (uint64_t)room)) return 0;
    ZSTD_inBuffer in = {GP(chunk), n, 0};
    ZSTD_outBuffer o = {GP(out), (size_t)room, 0};
    size_t r = 1;
    while (in.pos < in.size && r != 0) {
        r = ZSTD_decompressStream(d, &o, &in);
        if (ZSTD_isError(r)) break;
        if (o.pos == o.size && in.pos < in.size && r != 0) { r = ERROR(dstSize_tooSmall); break; }
    }
    if (o.pos) {
        __atomic_fetch_add(&st_bytes, (int64_t)o.pos, __ATOMIC_RELAXED);
    }
    J64(8) = out + o.pos;
    J32(16) = (uint32_t)(room - (int32_t)o.pos);
    J32(20) = J32(20) - (uint32_t)o.pos;
    if (ZSTD_isError(r)) {
        J8(0) = 3;
        static int told;
        if (told++ < 8) hos_log("zstd: a stream failed: %s", ZSTD_getErrorName(r));
        return hook_return(h, ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall ? 0xfffffffeu : 0xffffffffu);
    }
    if (r == 0) J8(0) = 4;   /* (the frame is whole) */
    return hook_return(h, J32(20));
}

