/* The few zstd calls the reader makes, under one prefix per build of zstd.c (PFX: the standard format or ZBIC). The
 * build makes every other symbol of the pair local, so the two builds and the libzstd that libLLVM loads never meet. */
#define ZSTD_STATIC_LINKING_ONLY
#include <stddef.h>
#include "zstd.h"

#define API __attribute__((visibility("default")))
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define F(n) CAT(PFX, n)

API void *F(dctx_new)(void) { return ZSTD_createDCtx(); }
API void F(dctx_free)(void *d) { ZSTD_freeDCtx(d); }
API size_t F(dctx_reset)(void *d) { return ZSTD_DCtx_reset(d, ZSTD_reset_session_only); }

API size_t F(decompress_stream)(void *d, void *dst, size_t cap, size_t *dpos, const void *src, size_t n, size_t *spos) {
    ZSTD_outBuffer o = {dst, cap, *dpos};
    ZSTD_inBuffer i = {src, n, *spos};
    size_t r = ZSTD_decompressStream(d, &o, &i);
    *dpos = o.pos;
    *spos = i.pos;
    return r;
}

API unsigned F(is_error)(size_t r) { return ZSTD_isError(r); }
API const char *F(error_name)(size_t r) { return ZSTD_getErrorName(r); }

/* 1 when the bytes hold a complete frame header */
API int F(frame_header_ok)(const void *src, size_t n) {
    ZSTD_FrameHeader h;
    return ZSTD_getFrameHeader(&h, src, n) == 0;
}

API size_t F(decompress)(void *dst, size_t cap, const void *src, size_t n) { return ZSTD_decompress(dst, cap, src, n); }

/* for the tests' fixtures */
API size_t F(compress_bound)(size_t n) { return ZSTD_compressBound(n); }
API size_t F(compress)(void *dst, size_t cap, const void *src, size_t n, int level, int checksum) {
    ZSTD_CCtx *c = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, checksum);
    size_t r = ZSTD_compress2(c, dst, cap, src, n);
    ZSTD_freeCCtx(c);
    return r;
}
