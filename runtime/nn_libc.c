#include "hos.h"
#include <stdlib.h>
#include <string.h>

#if defined(RX_WEB_MT) && RX_WEB_MT
void rx_wait_writebacks_reading(const void *p, uint64_t n);   /* gpu/api/rx_nvn.h */
#define WAIT_WRITEBACKS(p, n) rx_wait_writebacks_reading(p, n)
#else
#define WAIT_WRITEBACKS(p, n) ((void)0)
#endif

/* (hookshot) a range outside the guest window is a fatal stop: the game's own copy could not reach it either (the
   lifted code's addresses are the window's), and the host's would read or write host memory */
static void *range_of(const char *fn, uint64_t a, uint64_t n, uint64_t lr) {
    void *p = hos_guest_range(a, n);
    if (!p) hos_fatal(NULL, "%s: %#llx+%#llx is outside the guest window (lr %#llx)", fn, (unsigned long long)a,
                      (unsigned long long)n, (unsigned long long)lr);
    return p;
}

PATCH_FUNC_SYM(memcpy, aapcs, x0, hos_memcpy, as(x0, dst), as(x1, src), as(x2, n), as(x30, lr)) {
    if (!n) return dst;
    void *d = range_of("memcpy", dst, n, lr), *s = range_of("memcpy", src, n, lr);
    WAIT_WRITEBACKS(s, n);   /* (TOTK copies some rendered maps with the CPU) */
    memmove(d, s, (size_t)n);
    return dst;
}

PATCH_FUNC_SYM(memmove, aapcs, x0, hos_memmove, as(x0, dst), as(x1, src), as(x2, n), as(x30, lr)) {
    if (!n) return dst;
    void *d = range_of("memmove", dst, n, lr), *s = range_of("memmove", src, n, lr);
    WAIT_WRITEBACKS(s, n);
    memmove(d, s, (size_t)n);
    return dst;
}

PATCH_FUNC_SYM(memset, aapcs, x0, hos_memset, as(x0, dst), as(x1, c), as(x2, n), as(x30, lr)) {
    if (n) memset(range_of("memset", dst, n, lr), (int)(uint8_t)c, (size_t)n);
    return dst;
}

PATCH_FUNC_SYM(memcmp, aapcs, x0, hos_memcmp, as(x0, a), as(x1, b), as(x2, n)) {
    const uint8_t *p = (const uint8_t *)GP(a), *q = (const uint8_t *)GP(b);
    int r = 0;
    if (n && memcmp(p, q, (size_t)n)) {
        size_t i = 0;
        while (p[i] == q[i]) i++;
        r = (int)p[i] - (int)q[i];
    }
    return (uint64_t)(int64_t)r;
}

PATCH_FUNC_SYM(strlen, aapcs, x0, hos_strlen, as(x0, s)) { return strlen((const char *)GP(s)); }

PATCH_FUNC_SYM(strcmp, aapcs, x0, hos_strcmp, as(x0, a), as(x1, b)) {
    const uint8_t *p = (const uint8_t *)GP(a), *q = (const uint8_t *)GP(b);
    int r = 0;
    if (strcmp((const char *)p, (const char *)q)) {
        size_t i = 0;
        while (p[i] && p[i] == q[i]) i++;
        r = (int)p[i] - (int)q[i];
    }
    return (uint64_t)(int64_t)r;
}
