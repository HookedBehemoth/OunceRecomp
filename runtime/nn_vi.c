/* nn::vi: the default display (SDR), one layer, and the display's vsync event, signaled by a host thread. A variable
   refresh is reported while the frame rate cap is above 60. Frames are paced at the
   native NVN layer's window acquire (the end of this file); the game's own frame timing is its patches'
   (game_hooks.h game_frame_timing). */
#include "hos.h"
#include "game_hooks.h"
#include "os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hos_system_event_init(uint64_t key, int autoclear);   /* nn_os_sync.c */
void hos_system_event_signal(uint64_t key);

static uint64_t display, layer;
static uint64_t vsync_events[8];
static volatile int32_t nvsync;
static os_thread *vsync_thread;

static int fps_max(void);   /* (below: frame_rate) */
static void vsync_main(void *p) {
    (void)p;
    int64_t next = hos_now_ns() + 1000000000LL / fps_max();
    for (;;) {
        const int64_t now = hos_now_ns(), period = 1000000000LL / fps_max();   /* (the cap changes while the game runs) */
        if (next > now) hos_sleep_ns(next - now);
        next += period;
        if (next < hos_now_ns()) next = hos_now_ns() + period;   /* (fell behind: no burst of vsyncs) */
        for (int32_t i = 0; i < nvsync; i++) hos_system_event_signal(vsync_events[i]);
    }
}

/* nn::vi::Initialize() */
HLE(_ZN2nn2vi10InitializeEv) {}

/* nn::vi::OpenDefaultDisplay(nn::vi::Display**) -> Result */
HLE(_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE) {
    if (!display) display = hos_sys_alloc(0x100, 16);
    G64(ARG(0)) = display;
    RET(0);
}

/* nn::vi::CreateLayer(nn::vi::Layer**, nn::vi::Display*) -> Result */
HLE(_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE) {
    if (!layer) layer = hos_sys_alloc(0x100, 16);
    G64(ARG(0)) = layer;
    RET(0);
}

/* nn::vi::GetDisplayVsyncEvent(nn::os::SystemEventType*, nn::vi::Display*) -> Result */
HLE(_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE) {
    hos_system_event_init(ARG(0), 1);
    {   /* (runtime) under a lock: two threads checked the count at once and wrote past the table (the thread's start too) */
        static os_mutex vsync_lock = OS_MUTEX_INIT;
        os_mutex_lock(&vsync_lock);
        if (nvsync < (int32_t)(sizeof vsync_events / sizeof vsync_events[0])) {
            vsync_events[nvsync] = ARG(0);
            os_atomic_inc(&nvsync);
        }
        if (!vsync_thread) vsync_thread = os_thread_start(vsync_main, NULL, 0, "vi vsync");
        os_mutex_unlock(&vsync_lock);
    }
    RET(0);
}

/* nn::vi::QueryDisplayHdrStatus(bool* on, HdrOutputFormat*, unsigned short* max nits, Display const*) -> Result: SDR, as a
   desktop window is */
HLE(_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE) {
    *(uint8_t *)GP(ARG(0)) = 0;
    if (ARG(1)) G32(ARG(1)) = 0;
    if (ARG(2)) *(uint16_t *)GP(ARG(2)) = 0;
    RET(0);
}
/* nn::vi::QueryDisplayHdrPaperWhite(unsigned short*, Display const*) -> Result */
HLE(_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE) {
    *(uint16_t *)GP(ARG(0)) = 203;
    RET(0);
}
/* nn::vi::QueryDisplayVrrSupported(bool*, Display const*) -> Result: a variable refresh while the frames are paced
   freely (a Frame rate cap above 60), so a game that has a variable-rate path takes it; the fixed 60 Hz display
   otherwise, as before */
static int fps_max(void);
HLE(_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE) {
    *(uint8_t *)GP(ARG(0)) = fps_max() > 60;
    RET(0);
}
HLE(_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb) { RET(0); }
/* nn::vi::SetLayerExtendedSwapInterval(Layer*, int, int fps) / ClearLayerExtendedSwapInterval(Layer*): the frame
   rate the game asks for under a variable refresh (DKB: (layer, 1, its controller's rate)), which the pacing then
   holds (frame_period); cleared, the present interval again */
static volatile int layer_fps;
HLE(_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii) {
    const int fps = (int)ARG(2), n = (int)ARG(1);
    layer_fps = n >= 1 && fps >= 1 ? fps / n : 0;
    RET(0);
}
HLE(_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE) {
    layer_fps = 0;
    RET(0);
}

/* nn::vi::GetNativeWindow(void**, nn::vi::Layer*) -> Result: a window object (NVN builds its window on it; the
   native NVN layer only keeps the pointer) */
HLE(_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE) {
    static uint64_t window;
    if (!window) window = hos_sys_alloc(0x100, 16);
    G64(ARG(0)) = window;
    hos_log("vi: GetNativeWindow -> %#llx", (unsigned long long)window);
    RET(0);
}

/* ---- frame pacing for the native NVN layer's window (nvn_native.c) ----
   The game acquires a window texture before each frame and presents it after; on the console the acquire blocks
   while every texture waits for the display. Here the acquire waits for the frame's display time: the present
   interval's vsyncs (1: 60 fps, 2: 30) after the last present, so the game runs at the display's rate.
   no_pacing: as fast as it can. */
static volatile int present_interval = 1;
int hos_display_refresh_mhz(void);   /* (runtime/media/display_sdl.c: the window's display, 0 headless or not known) */
/* (runtime) The frame rate cap, the frame_rate setting, read at each use (the game's Options screen changes it while the game
   runs): 30, 60 (the console's), 120, or vsync: the window's display's refresh rate (60 headless or not known), presented in
   step with it (the renderer's FIFO present). The display pacing (pace_rate) and the vsync event follow it; above 60 a
   variable refresh is reported and the game's patches make its limiter and time step follow (game_frame_timing) */
static int fps_max(void) {
    static const int caps[] = {30, 60, 120};
    const int i = (int)setting_int(SET_FRAME_RATE);
    if (i < 3) return caps[i];
    const int hz = (hos_display_refresh_mhz() + 500) / 1000;
    return hz >= 30 ? (hz > 240 ? 240 : hz) : 60;
}
int hos_fps_max(void) { return fps_max(); }
/* the rate frames are paced to: the cap, or the window's display when it refreshes slower (read at each present: the window
   may move to another display) */
static int pace_rate(void) {
    const int cap = fps_max();
    if (cap <= 60) return cap;
    const int hz = (hos_display_refresh_mhz() + 500) / 1000;
    return hz > 60 && hz < cap ? hz : cap;
}
int hos_pace_rate(void) { return pace_rate(); }
/* a frame's display time: the game's extended swap rate (at most the pacing's), else the present interval's vsyncs */
static int64_t frame_period(void) {
    const int rate = pace_rate(), want = layer_fps;
    if (want > 0) return 1000000000LL / (want < rate ? want : rate);
    return 1000000000LL / rate * present_interval;
}
static int64_t next_frame_at, window_start, frames_in_window;
static volatile int64_t presents;
long long hos_vi_present_number(void) { return __atomic_load_n(&presents, __ATOMIC_RELAXED); }
int hos_vi_native_interval(Ctx *c, int requested) {
    (void)c;
    /* (runtime) OoT's frame-rate controller asks for 2, 3 or 4 vsyncs a frame whenever one runs long (480 changes a minute
       in Kokiri): paced to them, frames snapped to 33, 50 and 67 ms, the stutter, and the frame rate sat far under what the
       PC managed. With game time following the clock (realtime_step), a frame is shown as soon as it is ready, at the cap at
       most; the game still reads back its own interval. A benchmark steps a 60th a frame: paced to the game's interval */
    const int clamped = requested < 1 ? 1 : requested > 4 ? 4 : requested;
    present_interval = hos_bench_on() ? clamped : 1;
    return requested;
}
/* (runtime) The window's textures as the console's display queue has them: each present is shown at its display
   time, the later of its present and one interval after the frame shown before it, and a texture is free again once
   the frame presented after it is shown. The acquire of a frame waits for its texture: with `active` textures, the
   texture of the frame `active` back, freed when the frame after that one is shown. With three (OoT) a frame slower
   than the display waits for nothing and a fast one is held to the display's rate, two frames queued at most;
   waiting one interval after the last present instead (two textures' behaviour) holds every frame over 16.7 ms to
   the next vsync step (gameplay at 27 ms frames, 5 ms of them in that sleep) */
static int64_t shown_at[16];
static volatile int64_t shown_count;
void hos_vi_native_acquire(int active) {
    static int no_pacing = -1;
    if (no_pacing < 0) no_pacing = setting_bool(SET_NO_PACING) || hos_bench_on();   /* (bench: unpaced) */
    if (no_pacing || !next_frame_at) return;
    int64_t until = next_frame_at;
    if (active >= 2) {
        const int64_t n = shown_count, frame = n - active + 1;   /* (the frame whose showing frees this texture) */
        until = frame >= 0 && frame < n ? shown_at[frame & 15] : 0;
    }
    const int64_t now = hos_now_ns();
    if (until > now) hos_sleep_ns(until - now);
}
/* present_trace=<csv>: native game present-call returns, not GPU completion or display times.
   Uses the existing frame ID/clock; a forced stop can lose the last second of buffered present records. */
static void present_trace(int64_t frame, int64_t now) {
    static FILE *file;
    static char buffer[64 * 1024];
    static int looked;
    static int64_t last, flushed;
    if (!looked) {
        const char *path = setting_str(SET_PRESENT_TRACE);
        looked = 1;
        if (*path) {
            file = fopen(path, "w");
            if (!file) hos_log("vi: could not open present trace %s", path);
        }
        if (file) {
            setvbuf(file, buffer, _IOFBF, sizeof buffer);
            fputs("frame,return_ns,interval_ns\n", file);
            flushed = now;
        }
    }
    if (!file) return;
    fprintf(file, "%lld,%lld,%lld\n", (long long)frame, (long long)now, (long long)(frame > 1 ? now - last : 0));
    last = now;
    if (now - flushed >= 1000000000LL) {
        fflush(file);
        flushed = now;
    }
}
void hos_vi_native_presented(void) {
    static long long scan_n;
    ++scan_n;
    const int64_t period = frame_period(), now = hos_now_ns();
    game_frame_timing(now);   /* (the game's patches: its step and limiter) */
    if (hos_bench_on()) hos_bench_present(now);
    {   /* log.fp (diagnostic): the guest FPCR and the host MXCSR at presents 1, 60 and 600 */
        static int on = -1;
        if (on < 0) on = setting_bool(SET_LOG_FP);
        if (on && (scan_n == 1 || scan_n == 60 || scan_n == 600)) hos_thread_fp_log(scan_n);
    }
    /* (behind by more than a frame: the next one is paced from now, with no burst to catch up) */
    next_frame_at = next_frame_at && now - next_frame_at < period ? next_frame_at + period : now + period;
    {   /* (the display queue: this frame is shown one interval after the last, or now when the display waits) */
        const int64_t n = shown_count, last = n ? shown_at[(n - 1) & 15] : 0;
        shown_at[n & 15] = last + period > now ? last + period : now;
        shown_count = n + 1;
    }
    const int64_t n = os_atomic_inc(&presents);
    if (n % 1800 == 0) {
        void hos_code_ptr_report(void);
        hos_code_ptr_report();   /* code_ptrs.c */
    }
    present_trace(n, now);
    if (!window_start) window_start = now;
    frames_in_window++;
    /* (present-to-present intervals of the window, in buckets: which frames miss their vsync) */
    static int64_t last_present;
    static int hist[6];
    if (last_present) {
        const int64_t d = now - last_present;
        hist[d < 15500000 ? 0 : d < 17800000 ? 1 : d < 22000000 ? 2 : d < 30000000 ? 3 : d < 40000000 ? 4 : 5]++;
    }
    last_present = now;
    if (now - window_start >= 5000000000LL) {
        static int on = -1;
        if (on < 0) on = hos_log_switch(SET_DBG_LOGVI);
        if (on)
            hos_log("vi: %lld frames presented (%.1f fps over the last %.1f s, interval %d)", (long long)n,
                    frames_in_window * 1e9 / (double)(now - window_start), (now - window_start) / 1e9, present_interval);
        hos_log("vi: present intervals: <15.5 ms %d, 15.5-17.8 %d, 17.8-22 %d, 22-30 %d, 30-40 %d, over 40 %d", hist[0],
                hist[1], hist[2], hist[3], hist[4], hist[5]);
        memset(hist, 0, sizeof hist);
        window_start = now, frames_in_window = 0;
    }
}
