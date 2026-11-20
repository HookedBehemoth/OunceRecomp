/* Pool claims (runtime/nvn_claims.h): what each GPU-visible memory pool the game makes holds, claimed at the engine
   function that makes it. A pool no claim covers is an error in the NVN layer.

   - The render targets' storage: dta_pool_record_create (agl::utl::DynamicTextureAllocator) makes one physical pool
     (flags 0x18c, 687 MB at 1080p) and one virtual pool of its size, and maps the one into the other whole, once. Only
     the targets' textures live there; their images are the back end's and nothing reads the guest bytes. */
#include "hos.h"
#include "patches.h"
#include "nvn_claims.h"

PATCH_FUNC(0x80c49380, word(0xd503237f), body(0x3b0566baade1f62c), aapcs, x0, pools_dta_record, orig) {
    nvn_claim_begin(NVN_CLAIM_TARGETS, "dta_pool_record_create");
    const uint64_t r = guest_orig(orig);
    nvn_claim_end();
    return r;
}
