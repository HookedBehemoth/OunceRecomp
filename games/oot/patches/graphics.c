/* Game patch: the graphics settings the game itself draws by (renderer/api/settings.h): the sun's shadow resolution
   (shadow_resolution), the local lights' (local_shadow_resolution, applied in rt_slots.c), the field of view, bloom and
   depth of field. Each is read from the store where the game uses it, so a change from the Options screen applies at
   the next frame; a shadow resolution above the boot one waits for the next start (below). The defaults are the game's
   own picture.

   The sun shadow: its three cascades are one array target, "shadow_map_depth" (pass_sun_shadow_alloc), as large as the
   shadow object's size. That size comes from nine sizes that pass_sun_shadow_quality_init writes into the game's
   shadow quality object (+0x78, halfwords: 1000 1400 1600 1600 2000 2200 3072 3072 3072); the docked picture at
   2560x1440 takes the fourth, 1600 (the scene config's DepthShadowTexWidth, 1024, is not what is drawn). The first six
   are the game's sizes times the scale, rounded up to 8 and kept within 256..8192; the last three are not the cascades'
   (changing them changed no target) and stay. The array is transient, so a new size is the next frame's target.

   The field of view: camera_view_loop copies the main camera's vertical angle (+0x13c) to its projection (+0xa0) and
   takes its half's sine, cosine and tangent. The angle just loaded is widened so that the tangent of its half is
   scaled. Culling reads the same projection, so what is drawn follows. Only the main camera goes through there.

   Bloom and depth of field: the view's stage of each (pass_bloom_stage, pass_dof_stage) is skipped when its enable
   byte reads 0, the game's own path for a stage that is off. Each stage blends its result into the scene's colour
   itself, so without it the frame is the scene as lit. */
#include "patches.h"
#include "hos.h"
#include <math.h>
#include <stdint.h>

/* The two shadow resolutions' scales (50%|100%|200%|400%). The render-target pool is sized at boot for the boot choice
   (oot_shadow_pool_extra), so a larger choice waits for the next start, as render_scale does: the scale used is the
   chosen one up to the boot one. The game's allocator has no way out of a full pool: at a 1920x1080 output (512 MB)
   with both at 400%, dta_alloc read through a null block for the sun's array (lr 0x80669534) and the game stopped. */
static const float resolution_scales[] = {0.5f, 1.0f, 2.0f, 4.0f};
static int resolution_boot(setting_id id) {
    static int boot[2] = {-1, -1};
    int *b = &boot[id == SET_LOCAL_SHADOW_RESOLUTION];
    if (*b < 0) *b = (int)setting_int(id);
    return *b;
}
static int resolution_choice(setting_id id) {
    const int64_t k = setting_int(id), boot = resolution_boot(id);
    if (k < 0 || k > 3) hos_fatal(NULL, "graphics: setting %d has no choice %lld", (int)id, (long long)k);
    return k < boot ? (int)k : boot;
}
static float resolution_scale(setting_id id) { return resolution_scales[resolution_choice(id)]; }

/* (oot_probes.c, at the pool's configuration) the bytes the boot choices add to the game's render-target pool over the
   game's own shadows: the sun's array at the table's largest size (2200, three D16 layers), and four local maps of D32F
   (the most seen at once, Kokiri Forest: three), the game's largest 2432 against its scaled class (at most 4096) */
uint64_t oot_shadow_pool_extra(void) {
    const float sun = resolution_scales[resolution_boot(SET_SHADOW_RESOLUTION)];
    const float local = resolution_scales[resolution_boot(SET_LOCAL_SHADOW_RESOLUTION)];
    uint64_t extra = 0;
    if (sun > 1.0f) {
        uint64_t w = (uint64_t)(2200 * sun);
        w = w > 8192 ? 8192 : w;
        extra += (w * w - 2200ull * 2200) * 2 * 3;
    }
    if (local > 1.0f) {
        uint64_t w = (uint64_t)(2432 * local);
        w = w > 4096 ? 4096 : w;
        extra += (w * w - 2432ull * 2432) * 4 * 4;
    }
    return (extra + 0xfffff) & ~0xfffffull;
}

/* ---- the sun shadow */
#define SHADOW_SIZES 6   /* (of the table's nine: those the cascades take) */
static uint64_t shadow_quality;   /* (the game's shadow quality object, once pass_sun_shadow_quality_init made it) */
static uint16_t shadow_base[SHADOW_SIZES];
static int shadow_applied = -1;   /* (the choice the table holds) */

static void shadow_table_apply(int choice) {
    uint16_t *t = (uint16_t *)GP(shadow_quality + 0x78);
    const float s = resolution_scale(SET_SHADOW_RESOLUTION);
    for (int i = 0; i < SHADOW_SIZES; i++) {
        float v = (float)shadow_base[i] * s;
        v = v < 256.0f ? 256.0f : v > 8192.0f ? 8192.0f : v;
        t[i] = (uint16_t)(((uint32_t)v + 7) & ~7u);
    }
    shadow_applied = choice;
    hos_log("graphics: sun shadow sizes %u %u %u %u %u %u (x%.2f)", t[0], t[1], t[2], t[3], t[4], t[5], (double)s);
}

/* pass_sun_shadow_quality_init: its size table just written (x19 the object), before its floats */
PATCH_AT(0x80ea2508, word(0xd2a83408), before, none, oot_sun_shadow_sizes, as(x19, q)) {
    const uint16_t *t = (const uint16_t *)GP(q + 0x78);
    for (int i = 0; i < SHADOW_SIZES; i++) shadow_base[i] = t[i];
    shadow_quality = q;
    shadow_table_apply(resolution_choice(SET_SHADOW_RESOLUTION));
}
/* pass_sun_shadow_alloc's entry (the cascades' array of this frame): a changed choice reaches the table (the game reads
   it once a frame on the same thread, so the next frame's array has the new size) */
PATCH_AT(0x80669460, word(0xd503237f), before, none, oot_sun_shadow_follow) {
    const int choice = resolution_choice(SET_SHADOW_RESOLUTION);
    if (shadow_quality && choice != shadow_applied) shadow_table_apply(choice);
}

/* ---- the local lights' shadows (rt_slots.c's size classes take the scale) */
float oot_local_shadow_scale(void) { return resolution_scale(SET_LOCAL_SHADOW_RESOLUTION); }

/* ---- the field of view */
/* camera_view_loop: the main camera's vertical angle, just loaded (ldr s0, [x19, #0x13c]) */
PATCH_AT(0x804e52f8, word(0xbd413e60), after, s0, oot_fov_widen, as(s0, fovy)) {
    static const float scale[] = {0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f};
    const int64_t k = setting_int(SET_FIELD_OF_VIEW);
    if (k < 0 || k > 6) hos_fatal(NULL, "graphics: field_of_view has no choice %lld", (long long)k);
    if (k == 2) return fovy;
    const float wide = 2.0f * atanf(tanf(fovy * 0.5f) * scale[k]);
    return wide < 3.0f ? wide : 3.0f;
}

/* ---- bloom and depth of field */
/* pass_bloom_stage: the bloom stage's enable byte just loaded (ldrb w11, [x0, #0x808]) */
PATCH_AT(0x8027e6dc, word(0x3960200b), after, w11, oot_bloom_gate, as(w11, on)) { return setting_bool(SET_BLOOM) ? on : 0; }
/* pass_dof_stage: the depth of field stage's enable byte just loaded (ldrb w8, [x0, x8], x8 = 0x10e0) */
PATCH_AT(0x8027f578, word(0x38686808), after, w8, oot_dof_gate, as(w8, on)) { return setting_bool(SET_DEPTH_OF_FIELD) ? on : 0; }
