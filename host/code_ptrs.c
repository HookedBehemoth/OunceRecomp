/* Code pointers are host function pointers (docs/lifter.md, "Indirect calls and jumps"): this file reports what still
   takes the guest-address path. Each site is logged once (rt/a64_rt.c calls a64_old_path_site on a site's first
   guest-valued call), and hos_code_ptr_report logs the totals (every 1800 presents, host/nn_vi.c, and at a bench's
   exit, host/bench.c). When a long play session's line shows 0 sites and 0 received, the guest path can go. */
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

/* After the image is in the guest window: the reverse map a64_guest_of uses is made now, not first in a crash
   handler */
void hos_code_ptrs_init(void) {
    (void)a64_guest_of(1ull << 40);
}
