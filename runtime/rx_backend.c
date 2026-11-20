/* The direct back end's loader and the rx_* calls the host makes (renderer/api/vk_api.h): ounce_renderer.so beside the
   executable is loaded at rx_load and its device made at rx_start, which the boot calls before any guest code runs
   (runtime/media/display_sdl.c hos_display_start), so every call reaches a started back end. A library that does not load,
   lacks an entry or does not start ends the game. */
#include "hos.h"
#include "game_hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

#include "vk_api.h"
#include "renderer.h"
#include "crit.h"

/* what the back end is given */
uint8_t *hos_guest_base(void);
uint64_t hos_guest_size(void);
uint8_t *hos_gpu_ptr(uint64_t va, uint64_t *avail);
uint64_t nvn_gpu_guest(uint64_t gpu);
void nvn_texture_site(uint64_t out[2]);
int nvn_gpu_read(uint64_t gpu, void *dst, uint64_t n);
int nvn_gpu_write(uint64_t gpu, const void *src, uint64_t n);
void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);
int nvn_depth_minus_w(void);
int hos_gpu_pools(uint64_t (*out)[2], int max);
uint32_t hos_gpu_maps_generation(void);
uint64_t hos_reserved_cpus(void);   /* hos_thread.c */
extern float hos_aspect_ratio;      /* hos_main.c: the guest camera aspect applied at startup, or 0 */
uint32_t hos_pad_raw(void);         /* nn_hid.c: the gamepads and the controls, for the Insert menu */
int hos_binding(int set, int button, int slot);
int hos_mic_count(void);            /* nn_audio_in.c: the microphones, for the Insert menu */
const char *hos_mic_name(int index);
void hos_mic_select(int mode, const char *name, int index);
void hos_bind(int set, int button, int slot, int code);

static void host_log(const char *line) { hos_log("%s", line); }
static void host_free_pages(uint32_t *pages) { free(pages); }

/* (the renderer's own guest memory: the address space above the heap, as the game's own regions, committed and
   zeroed; the region allocator never hands it to the game) */
static uint64_t host_guest_alloc(uint64_t size) {
    const uint64_t a = hos_region_alloc(size, 0x10000);
    if (a) memset(hos_guest_base() + a, 0, size);
    return a;
}

void hos_watch_guest_writes(const uint8_t *data, uint64_t size);   /* nvn_native.c */
static void host_watch_guest_writes(uint64_t guest, uint64_t size) { hos_watch_guest_writes(hos_guest_base() + guest, size); }
static float host_aspect_ratio(void) { return hos_aspect_ratio; }
int hos_mouse_pointer(MkwVkPointer *out);   /* display.c, posix/display_sdl.c */
int hos_sdl_gyro_status(void);   /* pad_sdl.c */
extern const char *hos_romfs_dir;   /* nn_fs.c, set by hos_main.c */
static const char *host_romfs_dir(void) { return hos_romfs_dir; }

#define host_take_key_press NULL
/* (the renderer's settings: renderer/vk_settings.cpp) */
static int64_t host_setting_int(int id) { return setting_int((setting_id)id); }
static double host_setting_float(int id) { return setting_float((setting_id)id); }
static const char *host_setting_str(int id) { return setting_str((setting_id)id); }
static int host_setting_given(int id) { return setting_given((setting_id)id); }

uint64_t poolcensus_begin(void);   /* poolcensus.c */
void poolcensus_use(uint64_t seq, uint64_t lo, uint64_t size, int use, int writes);
void poolcensus_phase(uint64_t seq, int phase);
static const MkwVkHost vk_host = {
    MKWVK_API_VERSION, host_log, hos_guest_base, hos_guest_size, hos_gpu_ptr, nvn_gpu_guest, nvn_gpu_read,
    nvn_gpu_write, host_free_pages, hos_ticks, hos_now_ns, nvn_tex_layout,
    nvn_tex_level_offset, nvn_fmt_bpb, nvn_tex_level_tiles, nvn_tex_linear_stride, nvn_depth_minus_w, hos_gpu_pools,
    hos_gpu_maps_generation, hos_reserved_cpus, host_guest_alloc,
    host_watch_guest_writes, hos_pad_raw, hos_binding, hos_bind, host_aspect_ratio, host_take_key_press,
    hos_mouse_pointer, hos_mic_count, hos_mic_name, hos_mic_select, hos_sdl_gyro_status, hos_log_flush, game_prompts_face,
    host_romfs_dir, host_setting_int, host_setting_float, host_setting_str, host_setting_given, nvn_texture_site,
    poolcensus_begin, poolcensus_use, poolcensus_phase,
};

static const MkwVkRenderer *vk;   /* the back end, once started */

/* The direct back end, ounce_renderer.so: the version-14 table for the object layer, sync and present, and the list calls the
   host finds by name (runtime/renderer.h). Loaded at rx_load, before any guest code runs; every entry is required. */
static gpu_fns renderer_fns;
static os_lib *renderer_module;
static const char kDirectLib[] = OS_LIB("ounce_renderer.so", "ounce_renderer.dll");
static mkwvk_init_fn renderer_init;           /* (mkwvk_init: the back end's table, for the host's) */
void rx_load(void) {
    char path[OS_PATH_MAX];
    const int n = os_exe_path(path, sizeof path);
    char *slash = n ? strrchr(path, '/') : NULL;
    if (!slash || (size_t)(slash + 1 - path) + sizeof kDirectLib > sizeof path) hos_fatal(NULL, "rx: no path for %s", kDirectLib);
    memcpy(slash + 1, kDirectLib, sizeof kDirectLib);
    renderer_module = os_lib_open(path);
    if (!renderer_module) hos_fatal(NULL, "rx: %s did not load", path);
#define DIRECT_FN(field, name)                                                                         \
    if (!(*(void **)&renderer_fns.field = os_lib_sym(renderer_module, name)))                            \
        hos_fatal(NULL, "rx: %s has no %s", kDirectLib, name);
    DIRECT_FN(list_begin, "gpu_list_begin")
    DIRECT_FN(emit, "gpu_emit")
    DIRECT_FN(list_call, "gpu_list_call")
    DIRECT_FN(list_end, "gpu_list_end")
    DIRECT_FN(list_released, "gpu_list_released")
    DIRECT_FN(submit, "gpu_submit")
    DIRECT_FN(queue_wait_fence, "gpu_queue_wait_fence")
    DIRECT_FN(present_queue, "gpu_present_queue")
    DIRECT_FN(assets_open, "gpu_assets_open")
    DIRECT_FN(asset_load, "gpu_asset_load")
    DIRECT_FN(asset_rebind, "gpu_asset_rebind")
    DIRECT_FN(asset_pool, "gpu_asset_pool")
    DIRECT_FN(texels, "gpu_texels")
    DIRECT_FN(pass_known, "gpu_pass_known")
    DIRECT_FN(pass_inline, "gpu_pass_inline")
    DIRECT_FN(upscale, "gpu_upscale")
    DIRECT_FN(shots, "gpu_shots")
    DIRECT_FN(transient_pool, "gpu_transient_pool")
    DIRECT_FN(image_need, "gpu_image_need")
    DIRECT_FN(transient_slot, "gpu_transient_slot")
    DIRECT_FN(target_given, "gpu_target_given")
    DIRECT_FN(host_event, "gpu_host_event")
    DIRECT_FN(texture_cached, "gpu_texture_cached")
    DIRECT_FN(vram_free, "gpu_vram_free")
    DIRECT_FN(measure, "gpu_measure")
    DIRECT_FN(claim_write, "gpu_claim_write")
    DIRECT_FN(claim_fill, "gpu_claim_fill")
    DIRECT_FN(claim_read, "gpu_claim_read")
    DIRECT_FN(claim_made, "gpu_claim_made")
#undef DIRECT_FN
    if (!(renderer_init = (mkwvk_init_fn)os_lib_sym(renderer_module, "mkwvk_init"))) hos_fatal(NULL, "rx: %s has no mkwvk_init", kDirectLib);
    hos_log("rx: the direct back end: %s", path);
    {   /* (debug.crit= the calls timed, crit.c) */
        void crit_wrap(gpu_fns *f);
        if (crit_wanted()) crit_wrap(&renderer_fns);
        void bench_wrap(gpu_fns *f);   /* (bench: the CPU time in the back end's calls, bench.c) */
        if (hos_bench_on()) bench_wrap(&renderer_fns);
    }
}
const gpu_fns *rx_direct(void) { return &renderer_fns; }

/* (rx_load first) the back end's device made and its table checked whole: a library of another API version, or one
   whose table lacks an entry, ends the game here */
int rx_start(void *hwnd) {
    if (*setting_str(SET_SHOTS)) rx_direct()->shots(setting_str(SET_SHOTS), (int)setting_int(SET_SHOT_EVERY));   /* (the presented frames as PNGs) */
    const MkwVkRenderer *r = renderer_init(&vk_host);
    if (!r) hos_fatal(NULL, "rx: the direct back end (%s) made no device", kDirectLib);
    if (r->version != MKWVK_API_VERSION)
        hos_fatal(NULL, "rx: %s is of API version %u, not %u", kDirectLib, r->version, (unsigned)MKWVK_API_VERSION);
#define NEED(f) \
    if (!r->f) hos_fatal(NULL, "rx: %s's table has no %s", kDirectLib, #f);
    NEED(start) NEED(resize) NEED(describe) NEED(stats) NEED(nvn_fence) NEED(nvn_pushed) NEED(nvn_replayed) NEED(nvn_wait)
    NEED(wait_writebacks_reading) NEED(nvn_finish) NEED(nvn_present) NEED(nvn_register_texture) NEED(nvn_register_sampler)
    NEED(nvn_set_texture_pool_size) NEED(nvn_texture_released) NEED(nvn_program_released) NEED(nvn_read_texels)
    NEED(nvn_write_texels) NEED(nvn_invalidate) NEED(nvn_pool_created) NEED(nvn_texture_created) NEED(nvn_event_signal)
    NEED(nvn_fence_q) NEED(nvn_program_set) NEED(nvn_pool_native)
#undef NEED
    int w = 0, h = 0;   /* (no window: the offscreen image takes window_size, as the window would) */
    if (!hwnd && sscanf(setting_str(SET_WINDOW_SIZE), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) r->resize(w, h);
    if (!r->start(hwnd)) hos_fatal(NULL, "rx: the direct back end (%s) did not start", kDirectLib);
    vk = r;
    return 1;
}
/* The game's --precompile: the renderer makes the shader pack's programs into the caches beside the executable, with no
   window and no game; "@@shaders <done> <of> <translated> <kept> <failed>" lines on standard output for the builder */
static void precompile_progress(int done, int of, int made, int kept, int failed) {
    printf("@@shaders %d %d %d %d %d\n", done, of, made, kept, failed);
    fflush(stdout);
}
int rx_precompile(void) {
    rx_load();
    int (*pre)(const MkwVkHost *, void (*)(int, int, int, int, int)) =
        (int (*)(const MkwVkHost *, void (*)(int, int, int, int, int)))os_lib_sym(renderer_module, "mkwvk_precompile");
    if (!pre) hos_fatal(NULL, "rx: %s has no mkwvk_precompile", kDirectLib);
    return pre(&vk_host, precompile_progress);
}
void rx_resize(int width, int height) { vk->resize(width, height); }
const char *rx_describe(void) { return vk->describe(); }
void rx_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines) {
    *draws = *skipped = *pipelines = 0;
    vk->stats(draws, skipped, pipelines);
}
uint64_t rx_nvn_fence(void) { return vk->nvn_fence(); }
uint64_t rx_nvn_fence_q(uint64_t queue) { return vk->nvn_fence_q(queue); }
uint64_t rx_nvn_pushed(void) { return vk->nvn_pushed(); }
uint64_t rx_nvn_replayed(void) { return vk->nvn_replayed(); }
int rx_nvn_wait(uint64_t fence, uint64_t timeout_ns) {
    if (crit_rec) {   /* (debug.crit) */
        const uint64_t t0 = crit_tsc();
        const int r = vk->nvn_wait(fence, timeout_ns);
        crit_span(CRIT_BE_WAIT, t0, crit_tsc(), timeout_ns, (uint64_t)r);
        return r;
    }
    return vk->nvn_wait(fence, timeout_ns);
}
void rx_wait_writebacks_reading(const void *p, uint64_t n) {
    vk->wait_writebacks_reading(p, n);
}
void rx_nvn_finish(void) { vk->nvn_finish(); }
void rx_nvn_present(const nvn_tex *t, const int crop[4]) {
    if (crit_wanted()) {   /* (debug.crit) */
        crit_present();
        if (crit_rec) {
            crit_emit_flush();
            const uint64_t t0 = crit_tsc();
            vk->nvn_present(t, crop);
            crit_span(CRIT_BE_PRESENT, t0, crit_tsc(), 0, 0);
            return;
        }
    }
    vk->nvn_present(t, crop);
}
void rx_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v) {
    vk->nvn_register_texture(id, t, v);
}
void rx_nvn_register_sampler(int id, const nvn_samp_par *p) {
    vk->nvn_register_sampler(id, p);
}
void rx_nvn_set_texture_pool_size(int n) {
    vk->nvn_set_texture_pool_size(n);
}
void rx_nvn_texture_released(const nvn_tex *t) {
    vk->nvn_texture_released(t);
}
void rx_nvn_texture_created(const nvn_tex *t) {
    vk->nvn_texture_created(t);
}
void rx_nvn_program_released(const nvn_prog *p) { vk->nvn_program_released(p); }
void rx_nvn_program_set(const nvn_prog *p) { vk->nvn_program_set(p); }
/* a pool made or finalized, with its flags, its claim and the claiming patch (renderer/vk_claims.cpp) */
void rx_nvn_pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint32_t claim, const char *by, int on) {
    vk->nvn_pool_native(gpu, size, flags, claim, by, on);
}
void rx_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                        size_t row, size_t img) {
    vk->nvn_read_texels(t, level, layer0, r, dst, row, img);
}
void rx_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src,
                         size_t row, size_t img) {
    vk->nvn_write_texels(t, level, layer0, r, src, row, img);
}
void rx_nvn_invalidate(uint64_t gpu, uint64_t size) {
    vk->nvn_invalidate(gpu, size);
}
/* (nvn_native.c: the CPU set an event's value) in order with the submits, for the GPU's waits on it */
void rx_nvn_event_signal(uint64_t gpu, uint32_t value) {
    if (crit_rec) {   /* (debug.crit) */
        crit_emit_flush();
        const uint64_t t0 = crit_tsc();
        vk->nvn_event_signal(gpu, value);
        crit_span(CRIT_BE_EVSIG, t0, crit_tsc(), gpu, value);
        return;
    }
    vk->nvn_event_signal(gpu, value);
}
void rx_nvn_pool_created(uint64_t gpu, uint64_t size) {
    vk->nvn_pool_created(gpu, size);
}