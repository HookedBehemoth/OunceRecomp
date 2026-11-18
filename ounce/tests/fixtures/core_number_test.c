
#include <stdint.h>
#include <stdio.h>
typedef struct HosThread { int ideal_core; uint64_t core_mask; } HosThread;
@CORES@
@CORE@
static int fails;
static void check(int ideal, uint64_t mask, int want) {
    HosThread t = {ideal, mask};
    const int got = hos_thread_core(&t);
    if (got != want) { printf("FAIL: ideal %d mask %#llx: core %d, want %d\n", ideal, (unsigned long long)mask, got, want); fails++; }
    if (mask && (mask & 0x3f) && !(mask >> got & 1)) { printf("FAIL: ideal %d mask %#llx: core %d outside the mask\n", ideal, (unsigned long long)mask, got); fails++; }
}
int main(void) {
    if (hos_cores() != 6) { printf("FAIL: %d cores\n", hos_cores()); fails++; }
    check(-2, 0x2, 1);     /* one core: its core */
    check(3, 0x2, 1);      /* one core: its core, whatever the ideal says */
    check(1, 0x6, 1);      /* the ideal core, in the mask */
    check(0, 0x6, 1);      /* an ideal outside the mask: the mask's lowest */
    check(-2, 0x3f, 0);    /* no ideal: the mask's lowest */
    check(-2, 0x38, 3);
    check(-2, 0, 0);       /* no mask, no ideal: core 0 */
    check(2, 0, 2);        /* no mask: the ideal */
    check(-2, 0x80, 1);    /* a core past the six wraps */
    puts(fails ? "FAILED" : "PASS: core numbers from masks and ideal cores, never outside the mask, six cores");
    return fails != 0;
}
