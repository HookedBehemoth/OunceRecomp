/* Frame-rate and resolution patches of Ocarina of Time 1.1.1: the host side of the lifter's At probes (nx2/lift2/probes.py
   AT, one row each: a guest instruction, the registers passed here, the register given the result). Plain values and
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
     count grows by the frames' time instead (hos_movie_frames, host/nn_vi.c).
   - Output size (host/nn_applet.c hos_output_size: the Insert menu's Output resolution, 2560 x 1440 as shipped): nnMain
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
#include <stddef.h>
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
   strict aliasing (nx2/build.py host_fl, unlike the lifted code's -fno-strict-aliasing): the structs are may_alias. */
#define GUEST(type, addr) ((type *)A64_PTR(addr))

/* scene_resolution_update's object (f_831f3200: the scene's update; host/nn_vi.c logs its scale) */
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
uint32_t hos_oot_dynres_flags(uint32_t flags) {
    if (hos_fixed_resolution) return flags & ~1u;
    if (flags & 1) hos_dynres_runs++;   /* (the dynamic path's frames: host/nn_vi.c waits for 30 before fixing it) */
    return flags;
}

/* f_80825aa0's fixed path: its scale (mov w9, #1.0: the float's bits in a W register), 1.0 as shipped */
uint32_t hos_oot_fixed_scale(void) { return hos_fixed_scale_bits; }

/* f_831f3200 (module: x20), w8 the bits about to be or-ed into +0xe2c: bit 3 (recompute) too while +0xe5c is not the
   chosen scale (only then: a recompute every frame remade the targets) */
uint32_t hos_oot_scale_recompute(uint64_t module, uint32_t bits) {
    if (!hos_fixed_resolution) return bits;
    const OotSceneResolution *m = GUEST(OotSceneResolution, module);
    const float want = fixed_scale();
    const float d = m->scale - (m->against != 0 ? want * m->reference / m->against : want);
    return d > 1e-4f || d < -1e-4f ? bits | 8 : bits;
}

/* f_831f3200, its flags +0xe20 just loaded: bits 3/4 (0.75 / 0.5 in place of the scale) not looked at while fixed */
uint64_t hos_oot_scale_flags(uint64_t module, uint64_t flags) {
    hos_res_module = module;   /* (host/nn_vi.c logs the 3D's scale from it) */
    return hos_fixed_resolution ? flags & ~0x18ULL : flags;
}

/* ---- native scene (HOS_SCENE_NATIVE, HOS_FIX_RES) */

/* f_806702e0 (config: x0), w14 the current mode: mode 1's base scale 0.8 -> 1.0 in a stock table, and the mode made to
   compare unequal once (0xff), so the game's own refresh runs */
uint32_t hos_oot_scene_mode(uint64_t config, uint32_t mode) {
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
void hos_oot_scene_check(uint64_t module) {
    if (!hos_fixed_resolution || !hos_fix_res) return;
    OotSceneResolution *m = GUEST(OotSceneResolution, module);
    const float req = fixed_scale();
    if (m->against > 0.0f && m->reference > 0.0f && req > 0.0f && req <= 2.0f && m->scale != req * m->reference / m->against)
        m->apply |= 8u;
}

/* ---- frame seconds, movie frames */

/* after fdiv: the quotient, or base * the frame's seconds while the host gives them */
float hos_oot_frame_step(float base, float quotient) {
    const uint32_t s = hos_frame_seconds_bits;
    float secs;
    memcpy(&secs, &s, 4);
    return s ? base * secs : quotient;
}

/* f_80706be0's divide, which also tells the host its x19 (the module with the game clock) */
float hos_oot_frame_step_probe(float base, float quotient, uint64_t module) {
    hos_step_probe = module;
    return hos_oot_frame_step(base, quotient);
}

/* f_83b7c710: the movie player's count after its add of one: that one is the frames' time instead */
uint32_t hos_oot_movie_count(uint32_t count, uint64_t player) { return count - 1 + hos_movie_frames(player); }

/* f_80b1e140, a text pane's letter animation, on the edge past its hidden / no-text checks: the time (s0) that moves
   the letters' clock and the pauses' timer, times the Insert menu's Text speed (hos_text_scale, 1.0 as shipped; the
   auto-advance timer before keeps the game's time) */
float hos_oot_text_time(float time) {
    extern volatile float hos_text_scale;
    const float k = hos_text_scale;
    return k != 1.0f ? time * k : time;
}

/* ---- output size */

/* nnMain's display size for the framework's create arg (width | height << 32, the pair the game stores at +20) */
uint64_t hos_oot_output_size(void) { return hos_output_size(0) | (uint64_t)hos_output_size(1) << 32; }

/* f_810a7b70 (config: x20): the docked size list's first entry, 2560 x 1440, is the output size */
void hos_oot_output_config(uint64_t config) {
    OotDisplaySizeConfig *c = GUEST(OotDisplaySizeConfig, config);
    if (hos_output_size(0) != 2560 && c->docked_width == 2560) c->docked_width = hos_output_size(0), c->docked_height = hos_output_size(1);
}

/* f_80ea2f30: BaseResolution's width (height 0) or height (1) scaled with the output */
uint32_t hos_oot_base_size(uint32_t v, uint32_t height) {
    if (hos_output_size(0) == 2560) return v;
    return height ? v * hos_output_size(1) / 1440 : v * hos_output_size(0) / 2560;
}

/* f_80ea2f30: the constants 1920.0 / 1080.0, 3/4 of the output (mov w8, #<float>: the float's bits in a W register) */
uint32_t hos_oot_base_size_bits(uint32_t height) {
    const float f = (float)(hos_output_size((int)height) * 3 / 4);
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ---- the render targets' pool */

static uint64_t pool_growth(void) { return hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL; }

/* f_836ce9d0 (config: x19): the pool's size as shipped, scaled */
void hos_oot_rt_pool_config(uint64_t config) {
    OotGraphicsConfig *c = GUEST(OotGraphicsConfig, config);
    if (c->rt_pool_size == 0x13100000ULL) c->rt_pool_size = hos_rt_pool_size(0x13100000ULL);
}

/* f_83b7b340: the Graphics heap's size (x0 before its BL) grown by the pool's growth */
uint64_t hos_oot_rt_heap_size(uint64_t size) {
    return size == 0x34900000ULL || size == 0x348ffe20ULL ? size + pool_growth() : size;
}

/* f_83b7b340: a module heap's use (x0 after its vtable +120) against its budget, budgets[index] (x21, x22): the Graphics
   heap's counts as the shipped one */
uint64_t hos_oot_rt_heap_use(uint64_t use, uint64_t budgets, uint64_t index) {
    const uint64_t budget = GUEST(GuestU64, budgets)[index], g = pool_growth();
    return (budget == 0x34900000ULL || budget == 0x348ffe20ULL) && use > g ? use - g : use;
}

/* f_80b72340: the heap of what the Graphics heap can still give (x0 before its BL) keeps its shipped size */
uint64_t hos_oot_rt_rest(uint64_t size) {
    const uint64_t g = pool_growth();
    return size > g ? size - g : size;
}
