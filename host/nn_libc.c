#include "hos.h"
#include <stdlib.h>
#include <string.h>

static int guest_libc(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_GUEST_LIBC") != NULL;
    return v;
}

#if defined(RX_WEB_MT) && RX_WEB_MT
void rx_wait_writebacks_reading(const void *p, uint64_t n);   /* gpu/render/rx_nvn.h */
#define WAIT_WRITEBACKS(p, n) rx_wait_writebacks_reading(p, n)
#else
#define WAIT_WRITEBACKS(p, n) ((void)0)
#endif

ENTRY_HOOK_SYM(memcpy) {
    if (guest_libc()) return 0;
    const uint64_t n = ARG(2);
    if (!hos_guest_range(ARG(0), n) || !hos_guest_range(ARG(1), n)) return 0;   /* (hookshot: past the guest window, the game's own copy) */
    if (n) {
        WAIT_WRITEBACKS(GP(ARG(1)), n);   /* (TOTK copies some rendered maps with the CPU) */
        memmove(GP(ARG(0)), GP(ARG(1)), (size_t)n);
        HOS_WRITTEN(GP(ARG(0)), (size_t)n);
    }
    return 1;
}

/* (native-vk) the sdk's own copies, which the game calls by name past memcpy: __memcpy_aarch64 (sdk f_8575cdc0) was
   90% of the game's loads and stores of GPU pool memory and 2.8 s of the loading screen's CPU, lifted */
ENTRY_HOOK_SYM(__memcpy_aarch64) { return entry_sym_memcpy(c); }
ENTRY_HOOK_SYM(__memcpy_aarch64_simd) { return entry_sym_memcpy(c); }

ENTRY_HOOK_SYM(memmove) {
    if (guest_libc()) return 0;
    const uint64_t n = ARG(2);
    if (!hos_guest_range(ARG(0), n) || !hos_guest_range(ARG(1), n)) return 0;   /* (hookshot: past the guest window, the game's own copy) */
    if (n) {
        WAIT_WRITEBACKS(GP(ARG(1)), n);
        memmove(GP(ARG(0)), GP(ARG(1)), (size_t)n);
        HOS_WRITTEN(GP(ARG(0)), (size_t)n);
    }
    return 1;
}

ENTRY_HOOK_SYM(memset) {
    if (guest_libc()) return 0;
    const uint64_t n = ARG(2);
    if (!hos_guest_range(ARG(0), n)) return 0;   /* (hookshot: past the guest window, the game's own copy) */
    if (n) {
        memset(GP(ARG(0)), (int)(uint8_t)ARG(1), (size_t)n);
        HOS_WRITTEN(GP(ARG(0)), (size_t)n);
    }
    return 1;
}

ENTRY_HOOK_SYM(memcmp) {
    if (guest_libc()) return 0;
    const uint8_t *a = (const uint8_t *)GP(ARG(0)), *b = (const uint8_t *)GP(ARG(1));
    const size_t n = (size_t)ARG(2);
    int r = 0;
    if (n && memcmp(a, b, n)) {
        size_t i = 0;
        while (a[i] == b[i]) i++;
        r = (int)a[i] - (int)b[i];
    }
    RET((uint64_t)(int64_t)r);
    return 1;
}

ENTRY_HOOK_SYM(strlen) {
    if (guest_libc()) return 0;
    RET(strlen((const char *)GP(ARG(0))));
    return 1;
}

ENTRY_HOOK_SYM(strcmp) {
    if (guest_libc()) return 0;
    const uint8_t *a = (const uint8_t *)GP(ARG(0)), *b = (const uint8_t *)GP(ARG(1));
    int r = 0;
    if (strcmp((const char *)a, (const char *)b)) {
        size_t i = 0;
        while (a[i] && a[i] == b[i]) i++;
        r = (int)a[i] - (int)b[i];
    }
    RET((uint64_t)(int64_t)r);
    return 1;
}
