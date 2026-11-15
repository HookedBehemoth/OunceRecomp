#include "hos.h"
#include <stdlib.h>
#include <string.h>

#if defined(RX_WEB_MT) && RX_WEB_MT
void rx_wait_writebacks_reading(const void *p, uint64_t n);   /* gpu/api/rx_nvn.h */
#define WAIT_WRITEBACKS(p, n) rx_wait_writebacks_reading(p, n)
#else
#define WAIT_WRITEBACKS(p, n) ((void)0)
#endif

PATCH_HOOK_SYM(memcpy) {
    const uint64_t n = a->x[2];
    if (!hos_guest_range(a->x[0], n) || !hos_guest_range(a->x[1], n)) return 0;   /* (hookshot: past the guest window, the game's own copy) */
    if (n) {
        WAIT_WRITEBACKS(GP(a->x[1]), n);   /* (TOTK copies some rendered maps with the CPU) */
        memmove(GP(a->x[0]), GP(a->x[1]), (size_t)n);
    }
    return 1;
}


PATCH_HOOK_SYM(memmove) {
    const uint64_t n = a->x[2];
    if (!hos_guest_range(a->x[0], n) || !hos_guest_range(a->x[1], n)) return 0;   /* (hookshot: past the guest window, the game's own copy) */
    if (n) {
        WAIT_WRITEBACKS(GP(a->x[1]), n);
        memmove(GP(a->x[0]), GP(a->x[1]), (size_t)n);
    }
    return 1;
}

PATCH_HOOK_SYM(memset) {
    const uint64_t n = a->x[2];
    if (!hos_guest_range(a->x[0], n)) return 0;   /* (hookshot: past the guest window, the game's own copy) */
    if (n) {
        memset(GP(a->x[0]), (int)(uint8_t)a->x[1], (size_t)n);
    }
    return 1;
}

PATCH_HOOK_SYM(memcmp) {
    const uint8_t *p = (const uint8_t *)GP(a->x[0]), *q = (const uint8_t *)GP(a->x[1]);
    const size_t n = (size_t)a->x[2];
    int r = 0;
    if (n && memcmp(p, q, n)) {
        size_t i = 0;
        while (p[i] == q[i]) i++;
        r = (int)p[i] - (int)q[i];
    }
    return hook_return(h, (uint64_t)(int64_t)r);
}

PATCH_HOOK_SYM(strlen) {
    return hook_return(h, strlen((const char *)GP(a->x[0])));
}

PATCH_HOOK_SYM(strcmp) {
    const uint8_t *p = (const uint8_t *)GP(a->x[0]), *q = (const uint8_t *)GP(a->x[1]);
    int r = 0;
    if (strcmp((const char *)p, (const char *)q)) {
        size_t i = 0;
        while (p[i] && p[i] == q[i]) i++;
        r = (int)p[i] - (int)q[i];
    }
    return hook_return(h, (uint64_t)(int64_t)r);
}
