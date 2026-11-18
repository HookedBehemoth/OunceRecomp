/* Code pointers are host function pointers (docs/lifter-output.md, "Indirect calls and jumps"): this file reports what still
   takes the guest-address path. Each site is logged once (rt/a64_rt.c calls a64_old_path_site on a site's first
   guest-valued call), and hos_code_ptr_report logs the totals (every 1800 presents, runtime/nn_vi.c, and at a bench's
   exit, runtime/bench.c). When a long play session's line shows 0 sites and 0 received, the guest path can go. */
#include "hos.h"

void a64_code_ptr_counts(uint64_t *sites, uint64_t *calls, uint64_t *received);   /* rt/a64_rt.c */

void a64_old_path_site(uint64_t site, uint64_t target, int jump) {
    hos_log("code pointers: a guest address at %s %#llx: %#llx (thread %s)", jump ? "the BR" : "the BLR",
            (unsigned long long)site, (unsigned long long)target, hos_thread_name(hos_self()));
}

void hos_code_ptr_report(void) {
    uint64_t sites, calls, received;
    a64_code_ptr_counts(&sites, &calls, &received);
    hos_log("code pointers: %llu sites and %llu calls took the guest-address path; %llu guest-valued code addresses "
            "reached the host", (unsigned long long)sites, (unsigned long long)calls, (unsigned long long)received);
}

extern const uint32_t a64_code_slot_count, a64_code_slot[], a64_code_slot_fn[];   /* gen/a64_code_slots.c */
extern const uint32_t a64_fn_count, a64_fn_addr[];
extern const A64Fn a64_fn_ptr[];

/* After the image is in the guest window: every relocated slot holding a function (gen/a64_code_slots.c, from the
   lift's relocations) gets that function's host pointer; the reverse map a64_guest_of uses is made now, not first in a
   crash handler */
void hos_code_ptrs_init(void) {
    for (uint32_t i = 0; i < a64_code_slot_count; i++) {
        uint64_t *slot = (uint64_t *)(A64_MEM + a64_code_slot[i]);
        const uint32_t k = a64_code_slot_fn[i];
        if (k >= a64_fn_count || *slot != a64_fn_addr[k])
            hos_fatal(NULL, "code pointers: the slot at %#x holds %#llx, the lift's relocation %#x: image and lift differ",
                      a64_code_slot[i], (unsigned long long)*slot, k < a64_fn_count ? a64_fn_addr[k] : 0);
        *slot = (uint64_t)(uintptr_t)a64_fn_ptr[k];
    }
    hos_log("boot: code pointers: %u relocated slots hold host function pointers", a64_code_slot_count);
    (void)a64_guest_of(1ull << 40);
}

/* (an At row: the PATCH_AT below) a code pointer the game's own code compares with guest addresses: its guest
   address. __nnmusl_init_dso's range check of nnMain (0x850565b0); counted, so a run shows the row ran */
static volatile uint64_t guest_values;
PATCH_AT(0x850565b0, word(0xf947158c), after, x12, hos_code_guest_value, as(x12, v)) {   /* ldr x12, [x12, #0xe28] */
    const uint64_t n = __atomic_add_fetch(&guest_values, 1, __ATOMIC_RELAXED);
    const uint64_t g = a64_guest_of(v);
    if (n <= 4) hos_log("code pointers: a guest check reads %#llx as %#llx (row %llu)", (unsigned long long)v,
                        (unsigned long long)g, (unsigned long long)n);
    return g;
}
