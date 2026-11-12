/* (native-vk M4g) Game patch, direct mode only: textures the game loads with no data over another texture's texels.

   agl::TextureData loadImage_ (f_80798690, hooked in direct_assets.c) copies a texture's bytes into its slot in the texture
   heap ([td+0x18]+0x10). Some textures are loaded with no data (src 0): their names are in no BNTX of the romfs (Kokiri:
   141, Link's hair, cap and face maps among them, `Player_Child_Hairs_Alb` and the rest). Each lands on the slot of the load
   just before it of the same size, with data: `Player_Child_Hairs_Alb` on `Mt_Hairs_Player_Adult_Alb`'s, the child's
   bake textures on the adult's, `Rock_Pot_A_Rgh` on `Rock_Pot_A_02_Rgh`'s (HOS_DBG=nodata, runs m4g_nd_f / m4g_nd_d: 141
   of 141 that have a load before them at their slot). The game shares identical texels that way, and on the console, as
   in the frozen back end, the texture reads them from memory (nonzero there, and no CPU writes them after; m4g_nd_f).
   The direct back end never reads texture storage, so it is told: the slot's content is noted at each load with data
   (and where compaction moves a texture: f_807982f0's new base, its texels' place now); a load with no data over a noted
   slot of the same size is handed over as HSD_ASSET_SAME, the texture whose load it shares. A load with no data over a
   slot nothing was noted at stays as before (a plain image, named by the back end).
   A log line for the first few, and every 100th. With RX_BACKEND unset (or HOS_DBG=noassets) nothing here runs. */
#include "hos.h"
#include "direct.h"
#include <string.h>
#include <windows.h>

#define NSLOT (1 << 15)   /* (open addressing by slot address; Kokiri holds ~6000 live heap textures) */
static struct { uint64_t slot; const void *tex; uint32_t size; } map[NSLOT];
static SRWLOCK lock = SRWLOCK_INIT;
static volatile LONG64 n_noted, n_same, n_none, n_size, n_full;

static uint32_t at(uint64_t slot) { return (uint32_t)((slot >> 6) * 2654435761u) & (NSLOT - 1); }

/* the texture at that slot now (a load with data, or a texture moved there) */
void direct_tex_dedup_note(uint64_t slot, const void *tex, uint32_t size) {
    if (!slot || !tex) return;
    AcquireSRWLockExclusive(&lock);
    uint32_t i = at(slot), n = 0;
    for (; n < NSLOT && map[i].slot && map[i].slot != slot; n++) i = (i + 1) & (NSLOT - 1);
    if (n < NSLOT) map[i].slot = slot, map[i].tex = tex, map[i].size = size, InterlockedIncrement64(&n_noted);
    else InterlockedIncrement64(&n_full);
    ReleaseSRWLockExclusive(&lock);
}

/* a load with no data at that slot: the texture whose content it has (k->path HSD_ASSET_SAME), 1; none noted, 0 */
int direct_tex_dedup_nodata(uint64_t slot, uint32_t size, hsd_asset_key *k) {
    const void *src = NULL;
    uint32_t src_size = 0;
    AcquireSRWLockShared(&lock);
    for (uint32_t i = at(slot), n = 0; slot && n < NSLOT && map[i].slot; n++, i = (i + 1) & (NSLOT - 1))
        if (map[i].slot == slot) {
            src = map[i].tex, src_size = map[i].size;
            break;
        }
    ReleaseSRWLockShared(&lock);
    if (!src || src == k->tex) return InterlockedIncrement64(&n_none), 0;
    if (src_size != size) return InterlockedIncrement64(&n_size), 0;
    k->path = HSD_ASSET_SAME;
    k->same_as = src;
    const LONG64 c = InterlockedIncrement64(&n_same);
    if (c <= 4 || c % 100 == 0)
        hos_log("assets: texture dedup patch: '%.*s' (no data, %u bytes) has the texels loaded before it at slot %#llx (%lld so far; "
                "none noted %lld, another size %lld; slots noted %lld, %lld not kept)", k->name ? (int)k->name_len : 0,
                k->name ? k->name : "", size, (unsigned long long)slot, (long long)c, (long long)n_none, (long long)n_size,
                (long long)n_noted, (long long)n_full);
    /* (the alias holds that content too: a load with no data over it later takes the same) */
    return 1;
}
