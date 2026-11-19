/* Frame-rate and resolution patches of Ocarina of Time 1.1.1: the lifter's At probes, each declared
   here by its PATCH_AT (runtime/patch.h, one row each: a guest instruction, the registers passed here, the register given the result). Plain values and
   guest addresses in, a value out; no guest context. Each is a runtime switch where it can be, so the game's own path
   stays reachable.

   - Dynamic resolution (Lichii's built-in "Max Resolution" patch, main+0x805BD0): the engine's config flag at +0x94 picks
     the path that follows the GPU-time controller; while the resolution is fixed (hos_fixed_resolution) it takes the
     other path, the 3D at full scale. The controller made a new set of render targets at each size, ~11 images a frame
     at times, and held the GPU near its budget.
   - The fixed path's scale (hos_fixed_scale_bits: the Insert menu's Resolution) reaching the 3D, in the scene's update
     f_831f3200. It recomputes the 3D's scale (+0xe5c = scale * +0xe58 / +0xe50) only while +0xe2c bit 3 is set. The
     dynamic path sets it on a level change; the fixed path's flag 1 calls f_831f41d0, whose bit 3 the apply block clears
     in the same frame, before any recompute. So the fixed path kept the dynamic path's last scale (0.75 docked: 1536x864
     at every setting). Flag 1 now also asks for the recompute, and on the fixed path the flags +0xe20 bits 3/4 (a
     hardcoded 0.75 / 0.5 in place of the scale) are not looked at.
   - Native scene resolution (oscarrc91's PR #578 and i900gaming's PR #329): the graphics configuration has a scale table
     apart from the GPU-time controller, (1.0, 0.8, 2/3) for each axis in two profiles, and the game runs in mode 1, so
     even the fixed path's 100% sized the scene at 0.8 of the picture. With hos_native_scene (HOS_SCENE_NATIVE=1) mode 1's
     0.8 becomes 1.0 and the mode compares unequal once, so the game's own notification remakes the targets. With
     hos_fix_res (HOS_FIX_RES=1) a cached scale that differs from the one asked for sets the scale-dirty bit (8).
   - The frame's real time in place of the nominal rate, for a Frame rate cap above 60 (hos_frame_seconds_bits; 0, as at
     a cap of 60: the game's own divide). The engine's two step divides "base / nominal" (f_80706be0: the frame's step,
     base 60.0 or 30.0, added to the game clock; f_83b6ea90: the global step) become base * seconds, both or neither (one
     timed and the other stepping 1/60 s would run the game's two clocks apart). The engine's own rate is never above 60
     and its controller puts 60 back over a higher one, so above 60 fps every frame stepped 1/60 s and the game ran
     fps / 60 times too fast. hos_step_probe: f_80706be0's x19, which tells the host the hook is in this build and gives
     it the game clock for its log (HOS_FPS_LOG=1).
   - A movie's frame count above a 60 cap (issue 1742): the movie player counts its updates (f_83b7c710, +760 of the
     player), a cutscene places its narration, rumble and end by that count, and the movie itself follows the clock; the
     count grows by the frames' time instead (hos_movie_frames, runtime/nn_vi.c).
   - Output size (runtime/nn_applet.c hos_output_size: the Insert menu's Output resolution, 2560 x 1440 as shipped): nnMain
     (f_81222ce0) puts the display size in the framework's create arg (the window textures); f_810a7b70 sets up the
     display-size object from a config whose docked size list (+8, first 2560 x 1440) is the frame's base; f_80ea2f30
     takes the 3D's largest size, ResolutionParam's BaseResolution (or 1920 x 1080 as constants), 3/4 of 2560 x 1440, above
     which the game takes a smaller upscale network. Each follows the output.
   - The render targets' GPU pool: 0x13100000 bytes (the graphics config's +1464, read in f_836ce9d0, which takes its
     storage from the "Graphics" heap). Sized for 2560 x 1440: a 3840x2160 3D ran out of it and the next target came back
     null (crash in f_8005ed00, the pool's allocator). Scaled with the output (hos_rt_pool_size); the Graphics heap, made
     by f_83b7b340 (0x34900000 bytes, then 0x348ffe20 kept), grows by as much, its use counts as the shipped one against
     the module's budget (f_83b7b340 aborts below zero), and f_80b72340's heap of what the Graphics heap can still give
     keeps its shipped size. */
#include "hos.h"
#include "os.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern volatile int hos_fixed_resolution, hos_dynres_runs, hos_native_scene, hos_fix_res;
extern volatile uint32_t hos_fixed_scale_bits, hos_frame_seconds_bits;
extern volatile uint64_t hos_res_module, hos_step_probe;
uint32_t hos_output_size(int height);
uint64_t hos_rt_pool_size(uint64_t game);
uint32_t hos_movie_frames(uint64_t player);

/* The guest objects these patches touch, the fields they use at their guest offsets (both sides little-endian LP64 with
   the same alignments). The lifted code reaches the same memory through other types and the host's C is built with
   strict aliasing (ounce/builder host_fl, unlike the lifted code's -fno-strict-aliasing): the structs are may_alias. */
#define GUEST(type, addr) ((type *)A64_PTR(addr))

/* scene_resolution_update's object (f_831f3200: the scene's update; runtime/nn_vi.c logs its scale) */
typedef struct __attribute__((may_alias)) {
    uint8_t _0[0xe20];
    uint64_t flags;        /* +0xe20: bits 3/4, a hardcoded 0.75 / 0.5 in place of the scale */
    uint8_t _1[0xe2c - 0xe28];
    uint32_t apply;        /* +0xe2c: bit 3, recompute the scale */
    uint8_t _2[0xe50 - 0xe30];
    float against;         /* +0xe50: the size the scale is taken against */
    float _e54;
    float reference;       /* +0xe58 */
    float scale;           /* +0xe5c: the 3D's scale, = requested * +0xe58 / +0xe50 */
} OotSceneResolution;
_Static_assert(offsetof(OotSceneResolution, flags) == 0xe20, "flags");
_Static_assert(offsetof(OotSceneResolution, apply) == 0xe2c, "apply");
_Static_assert(offsetof(OotSceneResolution, against) == 0xe50, "against");
_Static_assert(offsetof(OotSceneResolution, reference) == 0xe58, "reference");
_Static_assert(offsetof(OotSceneResolution, scale) == 0xe5c, "scale");

/* display_scale_mode_select's configuration (f_806702e0): a scale table per profile, (x, y) for modes 0..2 */
typedef struct __attribute__((may_alias)) { float mode[3][2]; } OotScaleTable;
typedef struct __attribute__((may_alias)) {
    uint8_t _0[724];
    OotScaleTable profile0;   /* +724 */
    uint8_t _1[1244 - 748];
    OotScaleTable profile1;   /* +1244 */
} OotScaleConfig;
_Static_assert(offsetof(OotScaleConfig, profile0) == 724 && offsetof(OotScaleConfig, profile1) == 724 + 520, "profiles");

/* display_size_setup's config (f_810a7b70, x1): the docked size list's first entry */
typedef struct __attribute__((may_alias)) {
    uint8_t _0[8];
    uint32_t docked_width, docked_height;   /* +8, +12: 2560 x 1440 as shipped */
} OotDisplaySizeConfig;
_Static_assert(offsetof(OotDisplaySizeConfig, docked_width) == 8 && offsetof(OotDisplaySizeConfig, docked_height) == 12, "size");

/* the graphics configuration dta_pool_config_read reads (f_836ce9d0, x19) */
typedef struct __attribute__((may_alias)) {
    uint8_t _0[1464];
    uint64_t rt_pool_size;   /* +1464: 0x13100000 as shipped */
} OotGraphicsConfig;
_Static_assert(offsetof(OotGraphicsConfig, rt_pool_size) == 1464, "rt_pool_size");

typedef uint64_t __attribute__((may_alias)) GuestU64;

static float fixed_scale(void) { float f; const uint32_t u = hos_fixed_scale_bits; memcpy(&f, &u, 4); return f; }

/* ---- dynamic resolution and the fixed scale */

/* f_80825aa0, after the config's flags load (bit 0: the dynamic path): bit 0 clear while the resolution is fixed */
PATCH_AT(0x80825bcc, word(0xb9409508), after, w8, hos_oot_dynres_flags, as(w8, flags)) {   /* ldr w8, [x8, #148] */
    if (hos_fixed_resolution) return flags & ~1u;
    if (flags & 1) hos_dynres_runs++;   /* (the dynamic path's frames: runtime/nn_vi.c waits for 30 before fixing it) */
    return flags;
}

/* f_80825aa0's fixed path: its scale (mov w9, #1.0: the float's bits in a W register), 1.0 as shipped */
PATCH_AT(0x80826420, word(0x52a7f009), after, w9, hos_oot_fixed_scale) { return hos_fixed_scale_bits; }

/* f_831f3200 (module: x20), w8 the bits about to be or-ed into +0xe2c: bit 3 (recompute) too while +0xe5c is not the
   chosen scale (only then: a recompute every frame remade the targets) */
PATCH_AT(0x831f3efc, word(0xb94e2e89), after, w8, hos_oot_scale_recompute, as(x20, module), as(w8, bits)) {   /* ldr w9, [x20, #3628] */
    if (!hos_fixed_resolution) return bits;
    const OotSceneResolution *m = GUEST(OotSceneResolution, module);
    const float want = fixed_scale();
    const float d = m->scale - (m->against != 0 ? want * m->reference / m->against : want);
    return d > 1e-4f || d < -1e-4f ? bits | 8 : bits;
}

/* f_831f3200, its flags +0xe20 just loaded: bits 3/4 (0.75 / 0.5 in place of the scale) not looked at while fixed */
PATCH_AT(0x831f3f08, word(0xf9471288), after, x8, hos_oot_scale_flags, as(x20, module), as(x8, flags)) {   /* ldr x8, [x20, #3616] */
    hos_res_module = module;   /* (runtime/nn_vi.c logs the 3D's scale from it) */
    return hos_fixed_resolution ? flags & ~0x18ULL : flags;
}

/* ---- native scene (HOS_SCENE_NATIVE, HOS_FIX_RES) */

/* f_806702e0 (config: x0), w14 the current mode: mode 1's base scale 0.8 -> 1.0 in a stock table, and the mode made to
   compare unequal once (0xff), so the game's own refresh runs; the same after a window resize (the game then sizes its
   frame again, makes its targets at that size and sets the window's crop to it) */
static volatile int frame_refresh;   /* (game_frame_size: the game's frame to be sized again) */
static void hud_rescale(void);
static void hud_follow(void);
PATCH_AT(0x80670308, word(0x3955280e), after, w14, hos_oot_scene_mode, as(x0, config), as(w14, mode)) {   /* ldrb w14, [x0, #1354] */
    if (__atomic_exchange_n(&frame_refresh, 0, __ATOMIC_ACQ_REL)) {   /* (on the game's thread, in the frame it sizes again) */
        mode = 0xff;
        hud_rescale();
    }
    hud_follow();
    if (!hos_native_scene) return mode;
    OotScaleConfig *c = GUEST(OotScaleConfig, config);
    OotScaleTable *const tables[2] = {&c->profile0, &c->profile1};
    for (unsigned profile = 0; profile < 2; ++profile) {
        OotScaleTable *t = tables[profile];
        if (t->mode[0][0] == 1.0f && t->mode[0][1] == 1.0f && t->mode[1][0] == 0.8f && t->mode[1][1] == 0.8f &&
            t->mode[2][0] == 2.0f / 3 && t->mode[2][1] == 2.0f / 3) {
            t->mode[1][0] = t->mode[1][1] = 1.0f;
            mode = 0xff;
            hos_log("native-scene: profile %u mode 1 base scale 0.8 -> 1.0", profile);
        }
    }
    return mode;
}

/* f_831f3200's head (module: x0): the fixed scale marked dirty when the cached one differs (HOS_FIX_RES) */
PATCH_AT(0x831f3200, word(0x39788008), after, none, hos_oot_scene_check, as(x0, module)) {   /* ldrb w8, [x0, #3616] */
    if (!hos_fixed_resolution || !hos_fix_res) return;
    OotSceneResolution *m = GUEST(OotSceneResolution, module);
    const float req = fixed_scale();
    if (m->against > 0.0f && m->reference > 0.0f && req > 0.0f && req <= 2.0f && m->scale != req * m->reference / m->against)
        m->apply |= 8u;
}

/* ---- frame seconds, movie frames */

/* after fdiv: the quotient, or base * the frame's seconds while the host gives them */
PATCH_AT(0x83b6eaf8, word(0x1e201820), after, s0, hos_oot_frame_step, as(s1, base), as(s0, quotient)) {   /* fdiv s0, s1, s0 */
    const uint32_t s = hos_frame_seconds_bits;
    float secs;
    memcpy(&secs, &s, 4);
    return s ? base * secs : quotient;
}

/* f_80706be0's divide, which also tells the host its x19 (the module with the game clock) */
PATCH_AT(0x80706cc4, word(0x1e201828), after, s8, hos_oot_frame_step_probe, as(s1, base), as(s8, quotient), as(x19, module)) {   /* fdiv s8, s1, s0 */
    hos_step_probe = module;
    return hos_oot_frame_step(base, quotient);
}

/* f_83b7c710: the movie player's count after its add of one: that one is the frames' time instead */
PATCH_AT(0x83b7c7c8, word(0x11000508), after, w8, hos_oot_movie_count, as(w8, count), as(x20, player)) { return count - 1 + hos_movie_frames(player); }

/* f_80b1e140, a text pane's letter animation, on the edge past its hidden / no-text checks: the time (s0) that moves
   the letters' clock and the pauses' timer, times the Insert menu's Text speed (hos_text_scale, 1.0 as shipped; the
   auto-advance timer before keeps the game's time) */
PATCH_AT(0x80b1e1bc, word(0x37201528), before, s0, hos_oot_text_time, as(s0, time)) {   /* tbnz w8, #4 */
    extern volatile float hos_text_scale;
    const float k = hos_text_scale;
    return k != 1.0f ? time * k : time;
}

/* ---- output size */

/* the frame's width (0) or height (1): the window's when the frame follows it (game_frame_size), else the widescreen
   picture's (runtime/hos_main.c), else the output's */
static volatile uint64_t frame_size;   /* (width | height << 32; 0: the frame does not follow the window) */
static uint32_t picture_size(int height) {
    extern volatile uint64_t hos_wide_out;
    const uint64_t f = frame_size, w = f ? f : hos_wide_out;
    return w ? (uint32_t)(height ? w >> 32 : w) : hos_output_size(height);
}

/* nnMain's display size for the framework's create arg (width | height << 32, the pair the game stores at +20) */
PATCH_AT(0x81222ee4, word(0xf2c0b408), after, x8, hos_oot_output_size) { return hos_output_size(0) | (uint64_t)hos_output_size(1) << 32; }

/* f_810a7b70 (config: x20): the docked size list's first entry, 2560 x 1440, is the output size */
PATCH_AT(0x810a7b8c, word(0xaa0103f4), after, none, hos_oot_output_config, as(x20, config)) {   /* mov x20, x1 */
    OotDisplaySizeConfig *c = GUEST(OotDisplaySizeConfig, config);
    extern volatile uint64_t hos_wide_out;
    const uint64_t pic = hos_wide_out;
    const uint32_t pw = pic ? (uint32_t)pic : hos_output_size(0), ph = pic ? (uint32_t)(pic >> 32) : hos_output_size(1);
    if (pw != 2560 && c->docked_width == 2560) c->docked_width = pw, c->docked_height = ph;
}

/* f_80ea2f30: BaseResolution's width (height 0) or height (1) scaled with the output */
PATCH_AT(0x80ea2ffc, word(0x29406418), after, w24, hos_oot_base_size, as(w24, v), as(k(0), height)) {   /* ldp w24, w25, [x0] */
    if (picture_size(0) == 2560 && picture_size(1) == 1440) return v;
    return height ? v * picture_size(1) / 1440 : v * picture_size(0) / 2560;
}
PATCH_AT_SITE(0x80ea2ffc, word(0x29406418), after, w25, hos_oot_base_size, w25, k(1));   /* ldp w24, w25, [x0] */

/* f_80ea2f30: the constants 1920.0 / 1080.0, 3/4 of the output (mov w8, #<float>: the float's bits in a W register) */
PATCH_AT(0x80ea303c, word(0x52a89e08), after, w8, hos_oot_base_size_bits, as(k(0), height)) {   /* mov w8, #1920.0 */
    const float f = (float)(picture_size((int)height) * 3 / 4);
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
PATCH_AT_SITE(0x80ea3044, word(0x52a890e8), after, w8, hos_oot_base_size_bits, k(1));   /* mov w8, #1080.0 */

/* ---- the render targets' pool */

/* the pool as shipped, for the scene's pixels at a 3D scale above 1.0 at boot (games/oot/patches/ndsr.c renders larger
   than the output then): a 3D scale of 2.0 at 2560 x 1440 left the game's allocator out of room in Kokiri Forest */
float hos_res_scale_at_boot(void);
static uint64_t pool_shipped(void) {
    const float s = hos_res_scale_at_boot();
    return s > 1.0f ? ((uint64_t)(0x13100000ULL * s * s) + 0xfffff) & ~0xfffffULL : 0x13100000ULL;
}
static uint64_t pool_growth(void) { return hos_rt_pool_size(pool_shipped()) - 0x13100000ULL; }

/* (game_hooks.h) the console's memory holds the pool as the output scales it; the part a 3D scale above 1.0 adds
   comes on top (a 3D scale of 2.0 at a 2560 x 1440 frame: the Graphics heap did not fit the game's memory) */
uint64_t game_memory_extra(void) { return hos_rt_pool_size(pool_shipped()) - hos_rt_pool_size(0x13100000ULL); }

/* f_836ce9d0 (config: x19): the pool's size as shipped, scaled */
PATCH_AT(0x836cea54, word(0x39587668), after, none, hos_oot_rt_pool_config, as(x19, config)) {   /* ldrb w8, [x19, #1565] */
    OotGraphicsConfig *c = GUEST(OotGraphicsConfig, config);
    if (c->rt_pool_size == 0x13100000ULL) c->rt_pool_size = hos_rt_pool_size(pool_shipped());
}

/* f_83b7b340: the Graphics heap's size (x0 before its BL) grown by the pool's growth */
PATCH_AT(0x83b7b438, word(0xaa1703e2), after, x0, hos_oot_rt_heap_size, as(x0, size)) {   /* mov x2, x23 (x0: the heap's size) */
    return size == 0x34900000ULL || size == 0x348ffe20ULL ? size + pool_growth() : size;
}

/* f_83b7b340: a module heap's use (x0 after its vtable +120) against its budget, budgets[index] (x21, x22): the Graphics
   heap's counts as the shipped one */
PATCH_AT(0x83b7b4dc, word(0xd63f0100), after, x0, hos_oot_rt_heap_use, as(x0, use), as(x21, budgets), as(x22, index)) {   /* blr x8 (returns the use) */
    const uint64_t budget = GUEST(GuestU64, budgets)[index], g = pool_growth();
    return (budget == 0x34900000ULL || budget == 0x348ffe20ULL) && use > g ? use - g : use;
}

/* f_80b72340: the heap of what the Graphics heap can still give (x0 before its BL) keeps its shipped size */
PATCH_AT(0x80b723a4, word(0x52800024), after, x0, hos_oot_rt_rest, as(x0, size)) {   /* mov w4, #1 (x0: the size) */
    const uint64_t g = pool_growth();
    return size > g ? size - g : size;
}

/* ---- the player's update, for HOS_BENCH's "in play" (runtime/bench.c hos_bench_player): Link updated, no map loading
   and no event playing. Read by the game's own structures, no guest call */

typedef struct __attribute__((may_alias)) { uint64_t vtable; uint8_t _0[616]; uint64_t components; } OotPlayerActor;
_Static_assert(offsetof(OotPlayerActor, components) == 624, "components");
typedef struct __attribute__((may_alias)) { uint8_t _0[800]; uint64_t movement; } OotPlayerComponents;
_Static_assert(offsetof(OotPlayerComponents, movement) == 800, "movement");
typedef struct __attribute__((may_alias)) { uint8_t _0[24]; uint64_t actor; } OotPlayerMovement;   /* (Link's actor at movement - 0x1d50) */
_Static_assert(offsetof(OotPlayerMovement, actor) == 24, "actor");
typedef struct __attribute__((may_alias)) { uint64_t vtable; uint8_t _0[8]; uint64_t resource; } OotLifeActor;
_Static_assert(offsetof(OotLifeActor, resource) == 16, "resource");
typedef struct __attribute__((may_alias)) { uint8_t _0[88]; char name[8]; } OotActorResource;   /* "Player" for Link */
_Static_assert(offsetof(OotActorResource, name) == 88, "name");
typedef struct __attribute__((may_alias)) {   /* GameDataMgr: its bools, as f_8022d9c0 (gamedata_get_bool) reads them */
    uint8_t _0[16];
    uint32_t bool_count;   /* +16 */
    uint8_t _1[4];
    uint64_t bools;        /* +24: entries of 24 bytes, the value at +22 */
    uint64_t table;        /* +32: (name hash, index) pairs, open addressing */
    uint8_t _2[4];
    uint32_t table_size;   /* +44 */
} OotGameDataMgr;
_Static_assert(offsetof(OotGameDataMgr, bool_count) == 16 && offsetof(OotGameDataMgr, bools) == 24 &&
               offsetof(OotGameDataMgr, table) == 32 && offsetof(OotGameDataMgr, table_size) == 44, "GameDataMgr");
typedef struct __attribute__((may_alias)) { uint32_t hash, index; } OotGameDataSlot;
typedef struct __attribute__((may_alias)) { uint8_t _0[22]; uint8_t value; uint8_t _1; } OotGameDataBool;
_Static_assert(sizeof(OotGameDataBool) == 24, "bool entry");
typedef struct __attribute__((may_alias)) { uint8_t _0[104]; uint64_t playing; } OotEventMgr;   /* +104: an event plays */
_Static_assert(offsetof(OotEventMgr, playing) == 104, "playing");

enum { OOT_GAMEDATA_SLOT = 0x84c9e180, OOT_EVENTMGR_SLOT = 0x84c9e090 };
#define OOT_SEQUENCE_IS_LOADING_BANC 0xef2b60c2u   /* (MurMur3 of the bool's name) */

static uint64_t singleton(uint64_t slot) {
    const uint64_t holder = *GUEST(uint64_t, slot);
    return holder && hos_guest_range(holder, 8) ? *GUEST(uint64_t, holder) : 0;
}

/* a GameData bool by its name's hash: -1 when not found */
static int gamedata_bool(uint32_t hash) {
    const uint64_t m = singleton(OOT_GAMEDATA_SLOT);
    if (!m || !hos_guest_range(m, sizeof(OotGameDataMgr))) return -1;
    const OotGameDataMgr *g = GUEST(OotGameDataMgr, m);
    const uint32_t size = g->table_size;
    if (!g->table || !size || !hos_guest_range(g->table, size * 8ULL)) return -1;
    const OotGameDataSlot *t = GUEST(OotGameDataSlot, g->table);
    for (uint32_t probe = 0, k = hash % size; probe < size; probe++, k = (k + 1) % size) {
        if (!t[k].hash) return -1;
        if (t[k].hash != hash) continue;
        if (t[k].index >= g->bool_count || !hos_guest_range(g->bools + t[k].index * 24ULL, 24)) return -1;
        return GUEST(OotGameDataBool, g->bools + t[k].index * 24ULL)->value & 1;
    }
    return -1;
}

/* f_82c415e0, the player actor's movement step (x19 the actor): once a frame Link is updated */
PATCH_AT(0x82c41728, word(0xf9000bea), after, none, hos_oot_player_update, as(x19, actor)) {   /* str x10, [sp, #16] */
    if (!hos_bench_on()) return;
    void hos_bench_player(int loading, int event_playing);
    if (!hos_guest_range(actor, sizeof(OotPlayerActor)) || GUEST(OotPlayerActor, actor)->vtable != 0x84b45e18ULL) return;
    const uint64_t comp = GUEST(OotPlayerActor, actor)->components;
    if (!hos_guest_range(comp, sizeof(OotPlayerComponents))) return;
    const uint64_t mv = GUEST(OotPlayerComponents, comp)->movement;
    if (mv < 0x1d50 || !hos_guest_range(mv, sizeof(OotPlayerMovement)) || GUEST(OotPlayerMovement, mv)->actor != actor) return;
    const uint64_t link = mv - 0x1d50;
    if (!hos_guest_range(link, sizeof(OotLifeActor)) || GUEST(OotLifeActor, link)->vtable != 0x84b6c460ULL) return;
    const uint64_t res = GUEST(OotLifeActor, link)->resource;   /* (other actors share the vtable: Link's resource is "Player") */
    if (!res || !hos_guest_range(res, sizeof(OotActorResource)) || memcmp(GUEST(OotActorResource, res)->name, "Player", 7)) return;
    const uint64_t ev = singleton(OOT_EVENTMGR_SLOT);
    const int event = ev && hos_guest_range(ev, sizeof(OotEventMgr)) && GUEST(OotEventMgr, ev)->playing;
    hos_bench_player(gamedata_bool(OOT_SEQUENCE_IS_LOADING_BANC) == 1, event);
}

/* ---- widescreen and ultrawide. The game draws a 2560x1440 picture whatever the screen and renders its scene at
   1920x1080; its cameras use a 16:9 ratio. With these rows (PATCH_AT declarations, runtime/patch.h) the picture takes the window's shape
   (the height stays the game's, the width grows on an ultrawide screen and shrinks on a 16:10 or 4:3 one), the main
   camera takes the picture's ratio, the HUD and menus keep their 16:9 shape in the middle of a wider picture (a narrower
   one: its whole width), pre-rendered movies play in that middle 16:9, and the picture a save keeps shows that middle
   16:9 (a narrower picture: its middle band, at the texture's shape). runtime/hos_main.c sets the globals from the window's
   shape, all 0 for a 16:9 window: the game as shipped.
   The sites come from NX2navi's recomp/patches-v111.txt, where they are credited to Ivan0xFF (MIT, "The matrix-update,
   UI and movie sites: Copyright (c) 2026 Ivan0xFF"); the picture and scene size sites are the same constants that file
   names; the save picture's site is the patches' own (issue 952). */

extern volatile uint64_t hos_wide_out, hos_wide_scene;
extern volatile float hos_wide_ratio, hos_wide_margin, hos_wide_squeeze;

/* f_806702e0's frame size, the scale mode's size times its scale and clamped (w8, w9 before the stp to its argument of
   f_810a7f40): the window's pixels while the frame follows the window */
PATCH_AT(0x806705f8, word(0x1a89b2c9), after, none, hos_oot_frame_window, io(w8, w9)) {   /* csel w9, w22, w9, lt */
    const uint64_t f = frame_size;
    if (f) io->w8 = (uint32_t)f, io->w9 = (uint32_t)(f >> 32);
}

/* f_80ea2f30 (display_base_resolution), the scale mode's size times its scale (w23 width, w21 height): the frame's
   while it follows the window. The 3D's scale is taken against the height (+0xe50), so a frame of another size than the
   mode's made the 3D scale with its height: a short wide frame rendered its 3D far below its pixels */
PATCH_AT(0x80ea2f94, word(0x1e380017), after, w23, hos_oot_base_frame, as(w23, v), as(k(0), height)) {   /* fcvtzs w23, s0 */
    const uint64_t f = frame_size;
    return f ? (uint32_t)(height ? f >> 32 : f) : v;
}
PATCH_AT_SITE(0x80ea2fa4, word(0x1e380015), after, w21, hos_oot_base_frame, w21, k(1));   /* fcvtzs w21, s0 */

/* f_8114c5e0, the frame's largest size by mode, from a table of 16:9 sizes ({2560, 1440} docked): as wide as the
   picture's shape at that height, so a wider picture's frame is not cut to 16:9's width */
PATCH_AT(0x8114c604, word(0xf8687928), after, x8, hos_oot_frame_max, as(x8, size)) {   /* ldr x8, [x9, x8, lsl #3] */
    const uint32_t w = (uint32_t)size, h = (uint32_t)(size >> 32), pw = picture_size(0), ph = picture_size(1);
    if (!ph || (uint64_t)pw * h <= (uint64_t)w * ph) return size;
    return (uint64_t)h << 32 | (uint32_t)((uint64_t)h * pw / ph);
}

/* runtime/hos_main.c (start), runtime/media/display_sdl.c (a settled resize): the game's frame is
   now w x h (the window's pixels, at most 3840 x 2160's count, at most the picture's size): the main camera, HUD and
   movies take its shape, and the game sizes its frame and targets again */
void game_frame_size(uint32_t w, uint32_t h, int start) {
    extern volatile uint64_t hos_wide_out;
    const uint64_t pic = hos_wide_out;
    if (!pic || w < 64 || h < 64 || (!start && !frame_size)) return;   /* (a frame that does not follow the window) */
    const double r = (double)w / h, game = 16.0 / 9.0;
    if ((double)w * h > 3840.0 * 2160.0) h = (uint32_t)sqrt(3840.0 * 2160.0 / r), w = (uint32_t)(h * r);
    if (w > (uint32_t)pic) w = (uint32_t)pic, h = (uint32_t)(w / r);
    if (h > (uint32_t)(pic >> 32)) h = (uint32_t)(pic >> 32), w = (uint32_t)(h * r);
    w &= ~7u, h &= ~7u;
    const float ratio = (float)w / (float)h;
    hos_wide_ratio = ratio;
    hos_wide_squeeze = (float)(ratio > game ? game / ratio : 1.0);   /* (a narrower frame: the HUD its whole width) */
    hos_wide_margin = (1.0f - hos_wide_squeeze) / 2.0f;
    frame_size = (uint64_t)h << 32 | w;
    __atomic_store_n(&frame_refresh, 1, __ATOMIC_RELEASE);
    hos_log("frame: %ux%u, camera ratio %.4f, HUD and movies in the middle %.1f%%", w, h, (double)ratio, hos_wide_squeeze * 100.0);
}

/* the picture's {2560, 1440} (width | height << 32 in x8, just built by mov + movk): the window's shape at the game's height */
PATCH_AT(0x81222ee4, word(0xf2c0b408), after, x8, hos_oot_wide_out, as(x8, size)) { const uint64_t s = hos_wide_out; return s ? s : size; }
PATCH_AT_SITE(0x832139f8, word(0xf2c0b408), after, x8, hos_oot_wide_out, x8);   /* likewise, the display settings */
/* the scene the upscaler takes in, {1920, 1080}, likewise */
PATCH_AT(0x83213a04, word(0xf2c08708), after, x8, hos_oot_wide_scene, as(x8, size)) { const uint64_t s = hos_wide_scene; return s ? s : size; }

/* f_804e48c0 (camera_view_loop), the main camera's ratio just loaded, which it copies to its projection's aspect
   ([[x19 + 0x18] + 0x10] + 0xb0, read by the projection's matrix update and culling): when it is 16:9 (within 0.01), the
   picture's. Only this camera is widened; other perspective projections keep their 16:9 */
PATCH_AT(0x804e5334, word(0xbd414260), after, s0, hos_oot_wide_ratio, as(s0, ratio)) {   /* ldr s0, [x19, #0x140] */
    const float w = hos_wide_ratio, d = ratio - 16.0f / 9.0f;
    return w > 0.0f && d <= 0.01f && d >= -0.01f ? w : ratio;
}

/* the movie layer's draw: its viewport in its frame (x29 - 40), min x at +8 and max x at +16 */
typedef struct __attribute__((may_alias)) {
    uint8_t _0[8];
    float min_x;   /* +8 (x29 - 32) */
    uint8_t _1[4];
    float max_x;   /* +16 (x29 - 24) */
} OotMovieViewport;
_Static_assert(offsetof(OotMovieViewport, min_x) == 8 && offsetof(OotMovieViewport, max_x) == 16, "viewport");

/* movies in the middle 16:9 of a wider picture: the viewport narrowed by the margin on each side */
PATCH_AT(0x83b7c758, word(0xa941a269), after, none, hos_oot_wide_movie, as(x29, frame)) {   /* ldp x9, x8, [x19, #0x18] */
    const float m = hos_wide_margin;
    if (!(m > 0.0f)) return;
    OotMovieViewport *v = GUEST(OotMovieViewport, frame - 40);
    const float in = (v->max_x - v->min_x) * m;
    v->min_x += in, v->max_x -= in;
}

/* the 2D canvas setup's projection rows, just loaded (ldp q1, q4, [x2]; the canvas in x0, its screen 0x1b8 before it):
   the UI's full-screen orthographic projection (a 3840x2160 grid: q1 lane 0 2/3840, q4 lane 1 2/2160) gets its first
   row squeezed on a frame wider than 16:9 and its second on a narrower one, so the HUD keeps its 16:9 shape in the
   middle of any frame; the screens whose UI keeps the whole picture (the item-wheel and pause / system menu
   backgrounds redraw a capture of it; the event and loading fades cover it) are left alone, by their screen's vtable */
#define OOT_MAIN 0x80020000ull
static const uint64_t oot_keep_vtables[] = {OOT_MAIN + 0x4bed098, OOT_MAIN + 0x4be4fb0, OOT_MAIN + 0x4be61c8,
                                            OOT_MAIN + 0x4be7b30};
static volatile int64_t hud_n[4];   /* (canvas setups seen, squeezed, kept whole by their screen, canvases rescaled) */
static int hud_squeezed(uint64_t canvas, const void *q1, const void *q4) {   /* the UI's full-screen projection, not kept whole */
    float m00, m11;
    memcpy(&m00, q1, 4);
    memcpy(&m11, (const uint8_t *)q4 + 4, 4);
    if (m00 != 2.0f / 3840.0f || m11 != 2.0f / 2160.0f || !hos_guest_range(canvas - 0x1b8, 8)) return 0;
    const uint64_t vt = *(const uint64_t *)GP(canvas - 0x1b8);
    for (size_t k = 0; k < sizeof oot_keep_vtables / sizeof *oot_keep_vtables; k++)
        if (vt == oot_keep_vtables[k]) return 0;
    return 1;
}
static void scale_row(void *q, float k) {
    float row[4];
    memcpy(row, q, 16);
    for (int i = 0; i < 4; i++) row[i] *= k;
    memcpy(q, row, 16);
}
static float hud_x(void) { const float s = hos_wide_squeeze; return s > 0.0f && s < 1.0f ? s : 1.0f; }
static float hud_y(void) { const float r = hos_wide_ratio, game = 16.0f / 9.0f; return r > 0.0f && r < game ? r / game : 1.0f; }

/* The canvases set up once with the squeeze of their time (the setup runs when a screen is made, not every frame):
   their matrices are scaled again in the frame the game sizes its frame again. A canvas is known by its address and its screen's
   vtable; one whose screen is gone (another vtable there) is dropped */
enum { HUD_CANVASES = 256 };
static struct { uint64_t canvas, vt; float x, y; } hud_canvas[HUD_CANVASES];
static os_mutex hud_lock = OS_MUTEX_INIT;
static void hud_note(uint64_t canvas, int axis, float k) {
    const uint64_t vt = *(const uint64_t *)GP(canvas - 0x1b8);
    os_mutex_lock(&hud_lock);
    int free_slot = -1, i = 0;
    for (; i < HUD_CANVASES; i++) {
        if (hud_canvas[i].canvas == canvas) break;
        if (!hud_canvas[i].canvas && free_slot < 0) free_slot = i;
    }
    if (i == HUD_CANVASES && free_slot >= 0) i = free_slot, hud_canvas[i].canvas = canvas, hud_canvas[i].x = hud_canvas[i].y = 1.0f;
    if (i < HUD_CANVASES) {
        hud_canvas[i].vt = vt;
        if (axis) hud_canvas[i].y = k; else hud_canvas[i].x = k;
    }
    os_mutex_unlock(&hud_lock);
}
/* (hos_oot_scene_mode, when a resize sizes the frame again) each noted canvas's matrices (+8, and +72 its copy with the y row negated: 4x4 stored by
   columns, the x row at floats 0, 4, 8, 12, the y row at 1, 5, 9, 13) scaled from the squeeze they have to the frame's */
static void hud_reanchor(void);
static void hud_rescale(void) {
    const float x = hud_x(), y = hud_y();
    os_mutex_lock(&hud_lock);
    for (int i = 0; i < HUD_CANVASES; i++) {
        const uint64_t c = hud_canvas[i].canvas;
        if (!c) continue;
        if (!hos_guest_range(c - 0x1b8, 0x1b8 + 136) || *(const uint64_t *)GP(c - 0x1b8) != hud_canvas[i].vt) {
            hud_canvas[i].canvas = 0;
            continue;
        }
        const float kx = x / hud_canvas[i].x, ky = y / hud_canvas[i].y;
        for (int block = 0; block < 2; block++) {
            float *m = (float *)GP(c + 8 + block * 64);
            for (int col = 0; col < 4; col++) m[col * 4] *= kx, m[col * 4 + 1] *= ky;
        }
        hud_canvas[i].x = x, hud_canvas[i].y = y;
        __atomic_fetch_add(&hud_n[3], 1, __ATOMIC_RELAXED);
    }
    hud_reanchor();
    os_mutex_unlock(&hud_lock);
}

/* the projection's first row (q1): squeezed on a frame wider than 16:9 */
PATCH_AT(0x80794368, word(0xad401041), after, q1, hos_oot_wide_hud, as(x0, canvas), q1, q4) {
    __atomic_fetch_add(&hud_n[0], 1, __ATOMIC_RELAXED);
    if (!hud_squeezed(canvas, &q1, &q4)) { __atomic_fetch_add(&hud_n[2], 1, __ATOMIC_RELAXED); return q1; }
    __atomic_fetch_add(&hud_n[1], 1, __ATOMIC_RELAXED);
    const float k = hud_x();
    hud_note(canvas, 0, k);
    if (k != 1.0f) scale_row(&q1, k);
    return q1;
}
/* its second row (q4): squeezed on a frame narrower than 16:9, so the HUD keeps its shape in a band across the middle */
PATCH_AT(0x80794368, word(0xad401041), after, q4, hos_oot_tall_hud, as(x0, canvas), q1, q4) {
    float probe[4];
    memcpy(probe, &q1, 16);
    probe[0] = 2.0f / 3840.0f;   /* (the check sees the game's first row whichever row runs first) */
    if (!hud_squeezed(canvas, probe, &q4)) return q4;
    const float k = hud_y();
    hud_note(canvas, 1, k);
    if (k != 1.0f) scale_row(&q4, k);
    return q4;
}
/* The HUD's corners at the window's edges. The layouts (UI/Layout2Archive, Nintendo.Ui) have no anchors: each pane sits
   at a fixed offset from the middle of the 3840 x 2160 grid, and the HUD's corner groups are nodes placed at the grid's
   corners. In a canvas wider or taller than 16:9 those nodes are moved out by the extra half width or height, so
   their panes keep their size and sit at the window's edges. The pane builder (f_81549620) copies a pane's translate
   from its record; that copy gets the offset, and a resize moves the panes by the change (hud_rescale). The minimap's
   nodes are moved through their parents, which the game's code neither reads nor sets (it places the map's icons from
   the map and pattern nodes below them, and an icon offset there was carried into the pause menu's map): the minimap's
   art and edges by N_MiniMap_00, the icons, one tree for the minimap and the pause menu's map, by N_Common_00, each
   frame while the minimap is shown and the pause menu's map is not */
typedef struct __attribute__((may_alias)) {
    uint8_t _0[0x18];
    uint64_t name;    /* +0x18: the layout's name ("PlayerStatus_00") */
    uint64_t names;   /* +0x20: its panes' names, by index */
    uint8_t _1[0x38 - 0x28];
    uint32_t count;   /* +0x38: its panes */
} OotLayoutRes;
typedef struct __attribute__((may_alias)) {
    uint32_t changed, watched;   /* +0, +4: the changes seen, the changes looked for (bit 6: the translate) */
    uint8_t _0[0x18 - 8];
    uint64_t layout;             /* +0x18 */
    uint8_t _1[0x6c - 0x20];
    float x, y;                  /* +0x6c, +0x70: the translate */
    uint8_t _2[0xa8 - 0x74];
    uint32_t flags;              /* +0xa8: bit 0, shown */
} OotPane;
typedef struct __attribute__((may_alias)) {
    uint8_t _0[0xf8];
    uint64_t res;     /* +0xf8: its OotLayoutRes */
    uint64_t panes;   /* +0x100: its panes, 0xb0 bytes each */
} OotLayout;
_Static_assert(offsetof(OotLayoutRes, names) == 0x20 && offsetof(OotLayoutRes, count) == 0x38 && offsetof(OotPane, x) == 0x6c &&
               offsetof(OotPane, flags) == 0xa8 && offsetof(OotLayout, panes) == 0x100, "ui");
enum { OOT_PANE_SIZE = 0xb0 };

/* (layout, pane, side: x -1 left / +1 right, y -1 bottom / +1 top; shown, hidden, either may be none: the pane is moved
   only while the layout's pane shown is shown and its pane hidden is not) */
static const struct { const char *layout, *pane; signed char x, y; const char *shown, *hidden; } hud_anchors[] = {
    {"PlayerStatus_00", "N_ScaleL_00", -1, 1},               /* hearts, magic, the d-pad guide */
    {"PlayerStatus_00", "N_ScaleR_00", 1, 1},                /* the item buttons */
    {"PlayerStatus_00", "N_SubItemBaseFishing_00", 1, 1},
    {"PlayerStatus_00", "N_VRS_00", 1, 1},
    {"MapMenu_00", "N_MiniMap_00", 1, -1, 0, "N_LocalMap_00"},      /* the minimap, its edges */
    {"MapMenu_00", "N_MiniMapCap_00", 1, -1, 0, "N_LocalMap_00"},   /* (its edges' captures) */
    {"MapMenu_00", "N_EventEndIn_01", 1, -1, 0, "N_LocalMap_00"},   /* its frame and floor */
    {"MapMenu_00", "N_VRS_00", 1, -1},
    {"MapMenu_00", "N_Common_00", 1, -1, "N_MiniMap_00", "N_LocalMap_00"},   /* the map's icons, on the minimap */
    {"SaveLoadIcon_00", "N_InOut_00", -1, -1},
};
enum { HUD_PANES = 256 };
static struct {
    uint64_t pane, layout, res;
    uint32_t index;
    int anchor;     /* (its hud_anchors row) */
    float dx, dy;   /* the offset it has */
    uint64_t shown, hidden;   /* (its gate panes) */
} hud_pane[HUD_PANES];
static float hud_dx(void) { return 1920.0f / hud_x() - 1920.0f; }
static float hud_dy(void) { return 1080.0f / hud_y() - 1080.0f; }
static const char *guest_str(uint64_t a) { return a && hos_guest_range(a, 64) ? (const char *)GP(a) : ""; }

/* (its lock held) an anchored pane, or NULL when its layout is gone (its pane array, record or index not the noted
   ones): then dropped */
static OotPane *hud_pane_live(int i) {
    const uint64_t p = hud_pane[i].pane;
    if (!p) return NULL;
    const OotLayout *l = hos_guest_range(hud_pane[i].layout, sizeof(OotLayout)) ? GUEST(OotLayout, hud_pane[i].layout) : NULL;
    if (!hos_guest_range(p, OOT_PANE_SIZE) || !l || GUEST(OotPane, p)->layout != hud_pane[i].layout ||
        l->res != hud_pane[i].res || l->panes + (uint64_t)hud_pane[i].index * OOT_PANE_SIZE != p) {
        hud_pane[i].pane = 0;
        return NULL;
    }
    return GUEST(OotPane, p);
}

/* the pane builder (pane x19, its layout's record x27, its index x21), the translate in d0 about to be stored */
PATCH_AT(0x81549918, word(0xfc06c260), before, q0, hos_oot_hud_anchor, as(x19, pane), as(x27, res), as(x21, index), as(q0, t)) {   /* stur d0, [x19, #0x6c] */
    const OotLayoutRes *r = GUEST(OotLayoutRes, res);
    const char *layout = guest_str(r->name);
    for (int k = 0; k < (int)(sizeof hud_anchors / sizeof *hud_anchors); k++) {
        if (strcmp(layout, hud_anchors[k].layout)) continue;
        if (!hos_guest_range(r->names + 8 * (uint32_t)index, 8)) return t;
        if (strcmp(guest_str(*(const uint64_t *)GP(r->names + 8 * (uint32_t)index)), hud_anchors[k].pane)) continue;
        float xy[2];
        memcpy(xy, &t, 8);
        os_mutex_lock(&hud_lock);
        const int gated = hud_anchors[k].shown || hud_anchors[k].hidden;
        const float dx = gated ? 0.0f : hud_dx() * hud_anchors[k].x, dy = gated ? 0.0f : hud_dy() * hud_anchors[k].y;
        int slot = -1;   /* (its own, or a free one: a pane whose layout is gone frees its own) */
        for (int i = 0; i < HUD_PANES && slot < 0; i++)
            if (hud_pane[i].pane == pane || !hud_pane_live(i)) slot = i;
        if (slot >= 0) {
            const uint64_t l = GUEST(OotPane, pane)->layout;
            hud_pane[slot].pane = pane, hud_pane[slot].layout = l, hud_pane[slot].index = (uint32_t)index;
            hud_pane[slot].res = hos_guest_range(l, sizeof(OotLayout)) ? GUEST(OotLayout, l)->res : 0;
            hud_pane[slot].anchor = k, hud_pane[slot].dx = dx, hud_pane[slot].dy = dy;
            hud_pane[slot].shown = hud_pane[slot].hidden = 0;
            for (uint32_t j = 0; gated && j < r->count && hos_guest_range(r->names + 8 * j, 8); j++) {
                const char *name = guest_str(*(const uint64_t *)GP(r->names + 8 * j));
                const uint64_t at = GUEST(OotLayout, l)->panes + (uint64_t)j * OOT_PANE_SIZE;
                if (hud_anchors[k].shown && !strcmp(name, hud_anchors[k].shown)) hud_pane[slot].shown = at;
                if (hud_anchors[k].hidden && !strcmp(name, hud_anchors[k].hidden)) hud_pane[slot].hidden = at;
            }
            xy[0] += dx, xy[1] += dy;
        }
        os_mutex_unlock(&hud_lock);
        memcpy(&t, xy, 8);
        return t;
    }
    return t;
}

/* (its lock held) a pane moved to the frame's offset (a gated one: to none while its gates say no) */
static int hud_shown(uint64_t p) { return hos_guest_range(p, OOT_PANE_SIZE) && (GUEST(OotPane, p)->flags & 1); }
static void hud_pane_place(int i, OotPane *pane) {
    const int k = hud_pane[i].anchor;
    float nx = hud_dx() * hud_anchors[k].x, ny = hud_dy() * hud_anchors[k].y;
    if ((hud_anchors[k].shown && !hud_shown(hud_pane[i].shown)) || (hud_anchors[k].hidden && hud_shown(hud_pane[i].hidden)))
        nx = ny = 0.0f;
    if (nx == hud_pane[i].dx && ny == hud_pane[i].dy) return;
    pane->x += nx - hud_pane[i].dx, pane->y += ny - hud_pane[i].dy;
    pane->changed |= pane->watched & 0x40;
    hud_pane[i].dx = nx, hud_pane[i].dy = ny;
}

/* (hud_rescale, its lock held) every anchored pane given the new offset */
static void hud_reanchor(void) {
    for (int i = 0; i < HUD_PANES; i++) {
        OotPane *pane = hud_pane_live(i);
        if (pane) hud_pane_place(i, pane);
    }
}

/* (each frame, on the game's thread) the gated panes placed by their gates */
static void hud_follow(void) {
    os_mutex_lock(&hud_lock);
    for (int i = 0; i < HUD_PANES; i++) {
        if (!hud_pane[i].pane || !(hud_anchors[hud_pane[i].anchor].shown || hud_anchors[hud_pane[i].anchor].hidden)) continue;
        OotPane *pane = hud_pane_live(i);
        if (pane) hud_pane_place(i, pane);
    }
    os_mutex_unlock(&hud_lock);
}

void hos_oot_hud_log(void) {
    hos_log("widescreen HUD: canvas setups %lld, squeezed %lld, kept whole %lld, rescaled at resizes %lld (since the start)",
            (long long)hud_n[0], (long long)hud_n[1], (long long)hud_n[2], (long long)hud_n[3]);
}

/* The save's picture (issue 952): the pause menu draws the whole picture into a 543 x 306 texture, the save's JPEG,
   through the game's viewport-and-scissor helper (f_802a46e0: SetScissor, then SetViewport(x, y, w, h) in w1-w4, called
   by blr x8 after ldr x8, [x8]). Wider than 16:9 the whole picture was squeezed into it: that one viewport is made wider
   than the texture by the picture's ratio over 16:9, centred, so the middle 16:9 fills the texture (the scissor keeps
   the texture's), as a 16:9 window would show. Narrower than 16:9 (4:3, 16:10) it is made taller instead, by 16:9 over
   the picture's ratio: the picture's middle band fills the texture at its own shape, neither squeezed nor with bars. */
/* (io: w1-w4, SetViewport's x, y, width, height) */
PATCH_AT(0x802a4824, word(0xf9400108), after, none, hos_oot_wide_save_picture, io(w1, w2, w3, w4)) {
    const float r = hos_wide_ratio;
    if (!(r > 0.0f) || (uint32_t)io->w1 != 0 || (uint32_t)io->w2 != 0 || (uint32_t)io->w3 != 543 || (uint32_t)io->w4 != 306) return;
    if (r > 16.0f / 9.0f) {
        const int w = (int)(543.0f * r / (16.0f / 9.0f) + 0.5f);
        io->w1 = (uint32_t)((543 - w) / 2), io->w3 = (uint32_t)w;
    } else {
        const int h = (int)(306.0f * (16.0f / 9.0f) / r + 0.5f);
        io->w2 = (uint32_t)((306 - h) / 2), io->w4 = (uint32_t)h;
    }
}

/* ---- occlusion off: cLohan's community patch (zortos293/Lichii#641), as Fritchii ships it (Frizti/Fritchii 1a46ba972,
   "Occlusion Off", ldrb w8, [x0] -> mov w8, wzr). The game decides at six places in f_811cc790 whether to make its
   z-prepass occlusion queries and its samples-passed queries, each by a byte it loads; that byte reads 0, so the game
   makes no occlusion queries and draws every candidate object */

/* the byte that decides whether the game makes its occlusion queries, just loaded: 0 with OUNCE_OCCLUSION_OFF=1 */
PATCH_AT(0x811cce68, word(0x39400008), after, w8, hos_oot_occlusion_flag, as(w8, flag)) {
    extern volatile int hos_occlusion_off;
    return hos_occlusion_off ? 0 : flag;
}
PATCH_AT_SITE(0x811d828c, word(0x39400008), after, w8, hos_oot_occlusion_flag, w8);
PATCH_AT_SITE(0x811d82a4, word(0x39400008), after, w8, hos_oot_occlusion_flag, w8);
PATCH_AT_SITE(0x811ccec8, word(0x39400008), after, w8, hos_oot_occlusion_flag, w8);
PATCH_AT_SITE(0x811d82dc, word(0x39400008), after, w8, hos_oot_occlusion_flag, w8);
PATCH_AT_SITE(0x811d82f4, word(0x39400008), after, w8, hos_oot_occlusion_flag, w8);

/* ---- the actor singleton's per-core rings (singleton at 0x84c9e890; ring k at +0x20 + k * 0x30: buffer +0x10,
   capacity +0x18, head +0x1c, count +0x20). The game pushes and pops them without a lock or atomics, indexed by
   GetCurrentCoreNumber, so they are safe only while one thread per core number ever writes each ring. Every push and
   pop site (its PATCH_AT_SITE declarations) calls hos_oot_ring_write with the ring's address. The first
   writer of a ring is its owner (logged: "ring owner: ring N is A's"); a different thread writing it is logged once
   per (ring, thread): "ring owner: ring N pushed by A and B (pc P)". Always on: on the owner's writes, a guest load
   of the singleton, a relaxed add and a compare. */
enum { OOT_RING_SLOT = 0x84c9e890, OOT_RINGS = 7 };
static struct {
    HosThread *volatile owner;
    volatile int64_t pushes, pops;
    HosThread *volatile others[4];
    char pad[64 - 7 * 8];
} rings[OOT_RINGS];
static volatile int64_t ring_conflicts;

PATCH_AT(0x8004c820, word(0xb9002128), after, none, hos_oot_ring_write, as(x9, elem), as(k(0), pop), as(k(0x8004c820), pc)) {
    const uint64_t obj = singleton(OOT_RING_SLOT);
    const uint64_t k = (elem - obj - 0x20) / 0x30;
    if (!obj || elem < obj + 0x20 || k >= OOT_RINGS) return;
    HosThread *self = hos_self();
    __atomic_fetch_add(pop ? &rings[k].pops : &rings[k].pushes, 1, __ATOMIC_RELAXED);
    HosThread *owner = __atomic_load_n(&rings[k].owner, __ATOMIC_RELAXED);
    if (owner == self) return;
    if (!owner && __atomic_compare_exchange_n(&rings[k].owner, &owner, self, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        hos_log("ring owner: ring %d is %s's (pc %#x)", (int)k, hos_thread_name(self), (unsigned)pc);
        return;
    }
    for (int i = 0; i < 4; i++) {   /* (once per (ring, thread)) */
        HosThread *seen = __atomic_load_n(&rings[k].others[i], __ATOMIC_RELAXED);
        if (seen == self) return;
        if (!seen && __atomic_compare_exchange_n(&rings[k].others[i], &seen, self, 0, __ATOMIC_RELAXED,
                                                 __ATOMIC_RELAXED))
            break;
        if (seen == self) return;
    }
    __atomic_fetch_add(&ring_conflicts, 1, __ATOMIC_RELAXED);
    hos_log("ring owner: ring %d %s by %s and %s (pc %#x)", (int)k, pop ? "popped" : "pushed",
            hos_thread_name(owner), hos_thread_name(self), (unsigned)pc);
}
PATCH_AT_SITE(0x800b0b1c, word(0xb9002128), after, none, hos_oot_ring_write, x9, k(0), k(0x800b0b1c));
PATCH_AT_SITE(0x800b4228, word(0xb900212b), after, none, hos_oot_ring_write, x9, k(0), k(0x800b4228));
PATCH_AT_SITE(0x800b7630, word(0xb9002128), after, none, hos_oot_ring_write, x9, k(0), k(0x800b7630));
PATCH_AT_SITE(0x8073a17c, word(0xb900212b), after, none, hos_oot_ring_write, x9, k(0), k(0x8073a17c));
PATCH_AT_SITE(0x83af6328, word(0xb9002128), after, none, hos_oot_ring_write, x9, k(0), k(0x83af6328));
PATCH_AT_SITE(0x83af5bb4, word(0xb9002128), after, none, hos_oot_ring_write, x9, k(0), k(0x83af5bb4));
PATCH_AT_SITE(0x8004c988, word(0xb9002109), after, none, hos_oot_ring_write, x8, k(1), k(0x8004c988));
PATCH_AT_SITE(0x800b0ca0, word(0xb9002109), after, none, hos_oot_ring_write, x8, k(1), k(0x800b0ca0));
PATCH_AT_SITE(0x800b4468, word(0xb900210a), after, none, hos_oot_ring_write, x8, k(1), k(0x800b4468));
PATCH_AT_SITE(0x800b787c, word(0xb900210a), after, none, hos_oot_ring_write, x8, k(1), k(0x800b787c));
PATCH_AT_SITE(0x8073c9d0, word(0xb900210a), after, none, hos_oot_ring_write, x8, k(1), k(0x8073c9d0));
PATCH_AT_SITE(0x83af6724, word(0xb900210a), after, none, hos_oot_ring_write, x8, k(1), k(0x83af6724));
PATCH_AT_SITE(0x83af5d9c, word(0xb900210a), after, none, hos_oot_ring_write, x8, k(1), k(0x83af5d9c));

/* (a bench run's end: who owns each ring, and its pushes and pops per present so far) */
void game_bench_report(void) {
    extern volatile int64_t hos_present_count;   /* (runtime/media/display_sdl.c) */
    const double frames = hos_present_count > 0 ? (double)hos_present_count : 1.0;
    char line[1024];
    int n = snprintf(line, sizeof line, "rings: second writers %lld;", (long long)ring_conflicts);
    for (int k = 0; k < OOT_RINGS && n < (int)sizeof line - 96; k++)
        n += snprintf(line + n, sizeof line - n, " %d %s %.1f+%.1f/frame;", k,
                      rings[k].owner ? hos_thread_name(rings[k].owner) : "-", rings[k].pushes / frames,
                      rings[k].pops / frames);
    hos_log("%s", line);
}

