/* (M6f) Game patch, direct mode only: textures the game loads with no data, its UnifyTex aliases.

   agl::TextureData loadImage_ (f_80798690, hooked in direct_assets.c) copies a texture's bytes into its slot in the texture
   heap ([td+0x18]+0x10). A texture loaded with no data (src 0; Kokiri: Link's `Player_Child_Hairs_Alb` and 140 more) is an
   alias of System/Gfx/UnifyTex.Product.100.byml (5,186 names, 333 targets): f_807974f0 gives it a TextureData of its own
   that shares its target's slot and copies its target's BRTI, appends it to the target's alias chain (td+0x188 prev,
   td+0x190 next; the head, the target, has prev 0) and initializes its NVN texture over the target's storage
   (docs/direct/texture-streaming.md). On the console, as in the frozen back end, it reads its target's texels.

   The direct back end never reads texture storage, so it is told: the alias's target is read from the alias itself, the
   head of its chain, and handed over as HSD_ASSET_SAME with that texture's NVN object (the target is live and registered
   while it has aliases). M4g guessed the target as "the last load with data at the same slot"; walking in Hyrule Field
   that guess named textures the game had finalized and initialized again for other loads (M6f: 19 aliases as plain
   images, ASTC placeholders among them). A load with no data that is no alias (no chain) stays
   as before: a plain image, named by the back end. A log line for the first few, and every 100th. With HOS_DBG=noassets nothing here runs. */
#include "hos.h"
#include "direct.h"
#include <stdio.h>
#include <string.h>
#include "os.h"

static volatile int64_t n_alias, n_none, n_bad;

/* a load with no data, td its TextureData: its target's NVN texture as an HSD_ASSET_SAME key (1); no alias chain (0) */
int direct_tex_alias(uint64_t td, hsd_asset_key *k) {
    if (!hos_guest_range(td + 0x188, 0x10)) return os_atomic_inc(&n_bad), 0;
    uint64_t node = td + 0x188, head = 0;
    for (int n = 0; n < 65536; n++) {   /* (prev links back to the head, whose prev is 0) */
        const uint64_t prev = G64(node);
        if (!prev) { head = node; break; }
        if (!hos_guest_range(prev, 0x10)) return os_atomic_inc(&n_bad), 0;
        node = prev;
    }
    if (!head) return os_atomic_inc(&n_bad), 0;
    const uint64_t target = head - 0x188;
    if (target == td) return os_atomic_inc(&n_none), 0;   /* (no chain: not an alias) */
    const uint64_t gfx = hos_guest_range(target + 0xd8, 8) ? G64(target + 0xd8) : 0;
    if (!gfx || !hos_guest_range(gfx + 0x10, 0x40)) return os_atomic_inc(&n_bad), 0;
    k->path = HSD_ASSET_SAME;
    k->same_as = GP(gfx + 0x10);
    const int64_t c = os_atomic_inc(&n_alias);
    if (c <= 4 || c % 100 == 0) {
        char tn[64] = "";
        const uint64_t np = target + 0xf8;
        if (hos_guest_range(np, 2)) {
            const uint16_t l = *(const uint16_t *)GP(np);
            if (hos_guest_range(np + 2, l)) snprintf(tn, sizeof tn, "%.*s", (int)l, (const char *)GP(np + 2));
        }
        hos_log("assets: alias patch: '%.*s' (no data) is an alias of '%s' (td %#llx): its texels (%lld so far; not aliases %lld, "
                "chains not readable %lld)", k->name ? (int)k->name_len : 0, k->name ? k->name : "", tn, (unsigned long long)target,
                (long long)c, (long long)n_none, (long long)n_bad);
    }
    return 1;
}
