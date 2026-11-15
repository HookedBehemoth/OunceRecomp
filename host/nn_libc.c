#include "hos.h"
#include <stdlib.h>
#include <string.h>

#if defined(RX_WEB_MT) && RX_WEB_MT
void rx_wait_writebacks_reading(const void *p, uint64_t n);   /* gpu/api/rx_nvn.h */
#define WAIT_WRITEBACKS(p, n) rx_wait_writebacks_reading(p, n)
#else
#define WAIT_WRITEBACKS(p, n) ((void)0)
#endif

/* (hookshot) a range past the guest window is the game's own copy (orig): the host's would read or write host memory */
PATCH_FUNC_SYM(memcpy, aapcs, x0, hos_memcpy, orig, as(x0, dst), as(x1, src), as(x2, n), x3, x4, x5, x6, x7, x8) {
    if (!hos_guest_range(dst, n) || !hos_guest_range(src, n)) return guest_orig(orig, dst, src, n, x3, x4, x5, x6, x7, x8);
    if (n) {
        WAIT_WRITEBACKS(GP(src), n);   /* (TOTK copies some rendered maps with the CPU) */
        memmove(GP(dst), GP(src), (size_t)n);
    }
    return dst;
}

PATCH_FUNC_SYM(memmove, aapcs, x0, hos_memmove, orig, as(x0, dst), as(x1, src), as(x2, n), x3, x4, x5, x6, x7) {
    if (!hos_guest_range(dst, n) || !hos_guest_range(src, n)) return guest_orig(orig, dst, src, n, x3, x4, x5, x6, x7);
    if (n) {
        WAIT_WRITEBACKS(GP(src), n);
        memmove(GP(dst), GP(src), (size_t)n);
    }
    return dst;
}

PATCH_FUNC_SYM(memset, aapcs, x0, hos_memset, orig, as(x0, dst), as(x1, c), as(x2, n)) {
    if (!hos_guest_range(dst, n)) return guest_orig(orig, dst, c, n);
    if (n) memset(GP(dst), (int)(uint8_t)c, (size_t)n);
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
