/* Pool claims (runtime/nvn_claims.h): what each GPU-visible memory pool the game makes holds, claimed at the engine
   function that makes it. A pool no claim covers is an error in the NVN layer.

   - The render targets' storage: dta_init (agl::utl::DynamicTextureAllocator) makes one physical pool (flags 0x18c,
     200 MB) and one virtual pool of its size, and maps the one into the other whole, once. Only the targets' textures
     live there; their images are the back end's and nothing reads the guest bytes. */
#include "hos.h"
#include "patches.h"
#include "nvn_claims.h"

PATCH_FUNC(0x80737cac, word(0xd503237f), body(0x5141fda80a86a2e4), aapcs, x0, pools_dta_init, orig) {
    nvn_claim_begin(NVN_CLAIM_TARGETS, "dta_init");
    const uint64_t r = guest_orig(orig);
    nvn_claim_end();
    return r;
}
