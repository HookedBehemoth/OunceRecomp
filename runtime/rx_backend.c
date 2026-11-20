/* The direct back end's loader and the rx_* calls the host makes (renderer/api/vk_api.h): ounce_renderer.so beside the executable
   is loaded at the first call (NVN records before the window's thread starts the renderer), its device made at rx_start.
   A library that does not load or start ends the game. Calls made before rx_start that the back end needs are kept and given
   to it at the start, in order. */
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

static const MkwVkHost vk_host = {
    MKWVK_API_VERSION, host_log, hos_guest_base, hos_guest_size, hos_gpu_ptr, nvn_gpu_guest, nvn_gpu_read,
    nvn_gpu_write, host_free_pages, hos_ticks, hos_now_ns, nvn_tex_layout,
    nvn_tex_level_offset, nvn_fmt_bpb, nvn_tex_level_tiles, nvn_tex_linear_stride, nvn_depth_minus_w, hos_gpu_pools,
    hos_gpu_maps_generation, hos_reserved_cpus, host_guest_alloc,
    host_watch_guest_writes, hos_pad_raw, hos_binding, hos_bind, host_aspect_ratio, host_take_key_press,
    hos_mouse_pointer, hos_mic_count, hos_mic_name, hos_mic_select, hos_sdl_gyro_status, hos_log_flush, game_prompts_face,
    host_romfs_dir, host_setting_int, host_setting_float, host_setting_str, host_setting_given, nvn_texture_site,
};

static const MkwVkRenderer *vk;   /* the back end, once started */

/* The game registers its first textures and samplers before the window's thread starts the renderer (NVN is up
   first): those calls are kept here and given to the renderer rx_start chose, in order. Without that the Vulkan back
   end never knew the first ~0x60 texture slots, and lit geometry sampled them as its black dummy.
   The texels the game writes then (WriteTexels) are kept too, with a copy of their data: without a renderer they went
   nowhere, and a texture filled so at boot kept zeros in its storage. OoT's
   4x4 white placeholder is one: bound in place of the ground fog's depth, it read 0 and hid the ground fog at some camera
   angles. That happened when the window's thread started the renderer after the game had begun (windowed, not headless). */
typedef struct {
    int kind;   /* 0 texture registration, 1 sampler registration, 2 texture pool size, 3 texture finalized, 4 texture
                   made, 5 texels written */
    int id;
    nvn_tex t;
    nvn_view v;
    int has_view;
    nvn_samp_par p;
    /* (5: WriteTexels) */
    unsigned level, layer0;
    NVNcopyRegion r;
    uint64_t row, img;
    uint8_t *data;
} early_call;
static early_call *early;
static size_t n_early, cap_early;
static volatile int32_t started;   /* rx_start chose (the calls go straight to the renderer from then on) */
static os_mutex early_lock = OS_MUTEX_INIT;

static void forward(const early_call *c);
/* Keeps a call made before rx_start; 0 when the renderer is chosen already (the caller forwards it) */
static int keep_early(const early_call *c) {
    if (started) return 0;
    os_mutex_lock(&early_lock);
    if (started) {
        os_mutex_unlock(&early_lock);
        return 0;
    }
    if (n_early == cap_early) {
        cap_early = cap_early ? cap_early * 2 : 1024;
        early = (early_call *)realloc(early, cap_early * sizeof *early);
    }
    early[n_early++] = *c;
    os_mutex_unlock(&early_lock);
    return 1;
}
/* rx_start: the kept calls given to the renderer chosen, then the gate opened */
static void flush_early(void) {
    os_mutex_lock(&early_lock);
    size_t texels = 0;
    for (size_t i = 0; i < n_early; i++) {
        forward(&early[i]);
        if (early[i].kind == 5) texels++, free(early[i].data);
    }
    if (n_early)
        hos_log("rx: %zu texture and sampler calls made before the renderer started (%zu texel writes), given to it", n_early,
                texels);
    free(early);
    early = NULL, n_early = cap_early = 0;
    os_atomic_store(&started, 1);
    os_mutex_unlock(&early_lock);
}

/* The direct back end, ounce_renderer.so: the version-14 table for the object layer, sync and present, and the list calls the
   host finds by name (runtime/renderer.h). Read once, at the first call. */
static gpu_fns renderer_fns;
static volatile int32_t renderer_state;   /* 0 not loaded yet, 2 loaded */
static os_mutex renderer_lock = OS_MUTEX_INIT;
static os_lib *renderer_module;
static const char kDirectLib[] = OS_LIB("ounce_renderer.so", "ounce_renderer.dll");
const gpu_fns *rx_direct(void) {
    if (renderer_state) return &renderer_fns;
    os_mutex_lock(&renderer_lock);
    if (!renderer_state) {
        char path[OS_PATH_MAX];
        const int n = os_exe_path(path, sizeof path);
        char *slash = n ? strrchr(path, '/') : NULL;
        if (!slash || (size_t)(slash + 1 - path) + sizeof kDirectLib > sizeof path) hos_fatal(NULL, "rx: no path for %s", kDirectLib);
        memcpy(slash + 1, kDirectLib, sizeof kDirectLib);
        renderer_module = os_lib_open(path);
        if (!renderer_module) hos_fatal(NULL, "rx: %s did not load", path);
        #define DIRECT_FN(field, name)                                                                     \
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
        DIRECT_FN(texture_data_pool, "gpu_texture_data_pool")
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
#undef DIRECT_FN
        hos_log("rx: the direct back end: %s", path);
        {   /* (debug.crit= the calls timed, crit.c) */
            void crit_wrap(gpu_fns *f);
            if (crit_wanted()) crit_wrap(&renderer_fns);
            void bench_wrap(gpu_fns *f);   /* (bench: the CPU time in the back end's calls, bench.c) */
            if (hos_bench_on()) bench_wrap(&renderer_fns);
        }
        os_atomic_store(&renderer_state, 2);
    }
    os_mutex_unlock(&renderer_lock);
    return &renderer_fns;
}

int rx_start(void *hwnd) {
    /* (loaded here too: the window's thread can reach this before the game's first call loads the library) */
    const gpu_fns *d = rx_direct();
    if (*setting_str(SET_SHOTS)) d->shots(setting_str(SET_SHOTS), (int)setting_int(SET_SHOT_EVERY));   /* (the presented frames as PNGs) */
    mkwvk_init_fn init = (mkwvk_init_fn)os_lib_sym(renderer_module, "mkwvk_init");
    const MkwVkRenderer *r = init ? init(&vk_host) : NULL;
    int w = 0, h = 0;   /* (no window: the offscreen image takes window_size, as the window would) */
    if (r && !hwnd && sscanf(setting_str(SET_WINDOW_SIZE), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64)
        r->resize(w, h);
    if (!r || r->version != MKWVK_API_VERSION || !r->start(hwnd)) hos_fatal(NULL, "rx: the direct back end (%s) did not start", kDirectLib);
    vk = r;
    flush_early();
    return 1;
}
/* The game's --precompile: the renderer makes the shader pack's programs into the caches beside the executable, with no
   window and no game; "@@shaders <done> <of> <translated> <kept> <failed>" lines on standard output for the builder */
static void precompile_progress(int done, int of, int made, int kept, int failed) {
    printf("@@shaders %d %d %d %d %d\n", done, of, made, kept, failed);
    fflush(stdout);
}
int rx_precompile(void) {
    rx_direct();
    int (*pre)(const MkwVkHost *, void (*)(int, int, int, int, int)) =
        (int (*)(const MkwVkHost *, void (*)(int, int, int, int, int)))os_lib_sym(renderer_module, "mkwvk_precompile");
    if (!pre) hos_fatal(NULL, "rx: %s has no mkwvk_precompile", kDirectLib);
    return pre(&vk_host, precompile_progress);
}
/* (runtime) the window is being closed: a back end still preparing its shaders stops and keeps what it made first (a few
   seconds at most). One without the export (an older one) has nothing to keep */
void rx_close_asked(void) {
    if (!renderer_module) return;
    void (*close_asked)(void) = (void (*)(void))os_lib_sym(renderer_module, "mkwvk_close_asked");
    if (close_asked) close_asked();
}
void rx_resize(int width, int height) { if (vk) vk->resize(width, height); }
const char *rx_describe(void) { return vk ? vk->describe() : "none"; }
void rx_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines) {
    *draws = *skipped = *pipelines = 0;
    if (vk) vk->stats(draws, skipped, pipelines);
}
void rx_nvn_submit(const uint64_t *handles, int n) { if (vk) vk->nvn_submit(handles, n); }
void rx_nvn_submit_q(const uint64_t *handles, int n, uint64_t queue) {
    if (vk && vk->nvn_submit_q) vk->nvn_submit_q(handles, n, queue);
    else rx_nvn_submit(handles, n);
}
uint64_t rx_nvn_fence(void) { return vk ? vk->nvn_fence() : 0; }
uint64_t rx_nvn_fence_q(uint64_t queue) { return vk && vk->nvn_fence_q ? vk->nvn_fence_q(queue) : rx_nvn_fence(); }
uint64_t rx_nvn_pushed(void) { return vk ? vk->nvn_pushed() : 0; }
uint64_t rx_nvn_replayed(void) { return vk ? vk->nvn_replayed() : 0; }
int rx_nvn_wait(uint64_t fence, uint64_t timeout_ns) {
    if (crit_rec && vk) {   /* (debug.crit) */
        const uint64_t t0 = crit_tsc();
        const int r = vk->nvn_wait(fence, timeout_ns);
        crit_span(CRIT_BE_WAIT, t0, crit_tsc(), timeout_ns, (uint64_t)r);
        return r;
    }
    return vk ? vk->nvn_wait(fence, timeout_ns) : 1;
}
void rx_wait_writebacks_reading(const void *p, uint64_t n) {
    if (vk) vk->wait_writebacks_reading(p, n);
}
void rx_nvn_finish(void) { if (vk) vk->nvn_finish(); }
void rx_nvn_present(const nvn_tex *t, const int crop[4]) {
    if (crit_wanted()) {   /* (debug.crit) */
        crit_present();
        if (crit_rec && vk) {
            crit_emit_flush();
            const uint64_t t0 = crit_tsc();
            vk->nvn_present(t, crop);
            crit_span(CRIT_BE_PRESENT, t0, crit_tsc(), 0, 0);
            return;
        }
    }
    if (vk) vk->nvn_present(t, crop);
}
static void forward(const early_call *c) {
    switch (c->kind) {
    case 0: {
        const nvn_view *v = c->has_view ? &c->v : NULL;
        if (vk) vk->nvn_register_texture(c->id, &c->t, v);
        break;
    }
    case 1: if (vk) vk->nvn_register_sampler(c->id, &c->p); break;
    case 2: if (vk) vk->nvn_set_texture_pool_size(c->id); break;
    case 3: if (vk) vk->nvn_texture_released(&c->t); break;
    case 4: if (vk && vk->nvn_texture_created) vk->nvn_texture_created(&c->t); break;
    case 5:
        if (vk) vk->nvn_write_texels(&c->t, c->level, c->layer0, &c->r, c->data, c->row, c->img);
        break;
    }
}
void rx_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v) {
    if (!started && t) {
        early_call c = {0, id};
        c.t = *t;
        if (v) c.v = *v, c.has_view = 1;
        if (keep_early(&c)) return;
    }
    if (vk) vk->nvn_register_texture(id, t, v);
}
void rx_nvn_register_sampler(int id, const nvn_samp_par *p) {
    if (!started && p) {
        early_call c = {1, id};
        c.p = *p;
        if (keep_early(&c)) return;
    }
    if (vk) vk->nvn_register_sampler(id, p);
}
void rx_nvn_set_texture_pool_size(int n) {
    if (!started) {
        early_call c = {2, n};
        if (keep_early(&c)) return;
    }
    if (vk) vk->nvn_set_texture_pool_size(n);
}
void rx_nvn_texture_released(const nvn_tex *t) {
    if (!started && t) {
        early_call c = {3, 0};
        c.t = *t;
        if (keep_early(&c)) return;
    }
    if (vk) vk->nvn_texture_released(t);
}
void rx_nvn_texture_created(const nvn_tex *t) {
    if (!started && t) {
        early_call c = {4, 0};
        c.t = *t;
        if (keep_early(&c)) return;
    }
    if (vk && vk->nvn_texture_created) vk->nvn_texture_created(t);
}
void rx_nvn_program_released(const nvn_prog *p) { if (vk) vk->nvn_program_released(p); }
void rx_nvn_program_set(const nvn_prog *p) { if (vk && vk->nvn_program_set) vk->nvn_program_set(p); }
/* (native-vk layer 0) a pool made or finalized, with its flags and creation key (gpu/legacy/vk/vk_pools.cpp) */
void rx_nvn_pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint64_t key, int on) {
    if (vk && vk->nvn_pool_native) vk->nvn_pool_native(gpu, size, flags, key, on);
}
void rx_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                        size_t row, size_t img) {
    if (vk) vk->nvn_read_texels(t, level, layer0, r, dst, row, img);
}
void rx_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src,
                         size_t row, size_t img) {
    if (!started && t && r && src) {   /* (kept with a copy of the data: the caller's buffer is its own again on return) */
        int bw = 1, bh = 1;
        const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        if (bpb > 0 && bw > 0 && bh > 0 && r->width > 0 && r->height > 0) {
            const uint64_t wb = (uint64_t)((r->width + bw - 1) / bw) * (uint64_t)bpb, hb = (uint64_t)((r->height + bh - 1) / bh);
            const uint64_t depth = r->depth > 0 ? (uint64_t)r->depth : 1;
            /* (the bytes the copy reads: the last row of the last slice ends the data, whatever the strides) */
            const uint64_t n = (depth - 1) * img + (hb - 1) * row + wb;
            early_call c = {5, 0};
            c.t = *t, c.level = level, c.layer0 = layer0, c.r = *r, c.row = row, c.img = img;
            c.data = (uint8_t *)malloc(n);
            if (c.data) {
                memcpy(c.data, src, n);
                if (keep_early(&c)) return;
                free(c.data);
            }
        }
    }
    if (vk) vk->nvn_write_texels(t, level, layer0, r, src, row, img);
}
void rx_nvn_invalidate(uint64_t gpu, uint64_t size) {
    if (vk) vk->nvn_invalidate(gpu, size);
}
int rx_guest_imported(uint64_t lo, uint64_t hi) {
    /* (it takes the back end's pool lock, which the replay holds at times: a block for debug.crit) */
    const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)13);
    const int r = vk && vk->guest_imported ? vk->guest_imported(lo, hi) : 0;
    if (away) hos_block_end();
    return r;
}
/* (nvn_native.c: the CPU set an event's value) in order with the submits, for the GPU's waits on it */
void rx_nvn_event_signal(uint64_t gpu, uint32_t value) {
    if (crit_rec && vk && vk->nvn_event_signal) {   /* (debug.crit) */
        crit_emit_flush();
        const uint64_t t0 = crit_tsc();
        vk->nvn_event_signal(gpu, value);
        crit_span(CRIT_BE_EVSIG, t0, crit_tsc(), gpu, value);
        return;
    }
    if (vk && vk->nvn_event_signal) vk->nvn_event_signal(gpu, value);
}
void rx_nvn_pool_created(uint64_t gpu, uint64_t size) {
    if (vk && vk->nvn_pool_created) vk->nvn_pool_created(gpu, size);
}
