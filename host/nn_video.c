/* nn::codec's hardware video decoders (VP8/VP9 here) and their image pools, on FFmpeg's libavcodec (the static
   libraries in Eglantine's package cache: work/eg/.cache/cpm/ffmpeg). MKW's movie library (subsdk0, movie::) plays
   its WebM movies through them: the cup openings, the courses' billboards, the emblems.

   How the library uses them (subsdk0, movie::detail::VideoDecoderBuffer): each picture buffer is an image pool of
   one image, sized by MakeDecodeImagePreference + CalculateDecoderImagePoolBuffer, registered with the decoder by
   index (RegisterXDecoderBuffer). It submits each compressed frame (SubmitXDecoderInputBuffer), takes finished
   pictures by buffer index (AcquireXDecoderOutputBuffer, GetXDecoderOutputBufferProperties), reads them back to
   its own memory as RGBA or NV12 (ReadDecoderOutputImageRgba / Nv12) and gives the buffer back (Release).

   Here the decoder's own thread (worker_main) decodes the submissions a few pictures ahead; a decoded picture waits
   in a queue until an Acquire puts it in the first free registered buffer (host memory: the pool's guest memory is
   not used), or fills a buffer handed out before it (its fence). The decoder takes as much input as a hardware one has room for (MAX_IN_FLIGHT).
   The video service itself (nn::gsv) is not emulated: nn::codec::video::Initialize does nothing. */
#include "hos.h"
#include "hos_ledger.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef _WIN32
#include <shellapi.h>
#endif
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#define RESULT_NO_OUTPUT 0x2a1ca   /* (a failure: "no picture yet"; the library polls again) */
#define POOL_MAGIC 0x4c4f4f50u     /* 'POOL' */
#define DEC_MAGIC 0x43454456u      /* 'VDEC' */
#define PREF_MAGIC 0x46455250u     /* 'PREF' */
#define MAX_SLOTS 16
#define MAX_QUEUE 8
/* SubmitXDecoderInputBuffer's "input queue full" (the movie library turns it into a retry: the frame stays with the game
   until the submission-ready event is signalled) */
#define RESULT_INPUT_FULL 0x1c266f
/* What the decoder holds: submissions not decoded plus pictures not acquired. A hardware decoder takes no more input
   than it has room for; MAX_QUEUE, so the queue never drops a picture */
#define MAX_IN_FLIGHT MAX_QUEUE

static int video_log(void) {   /* HOS_VIDEO_LOG=1: submissions and acquisitions */
    static int on = -1;
    if (on < 0) on = getenv("HOS_VIDEO_LOG") != NULL;
    return on;
}

void hos_system_event_init(uint64_t key, int autoclear);   /* nn_os_sync.c */
void hos_system_event_signal(uint64_t key);

/* an image pool: its pictures, w x h, as RGBA and as NV12 (both kept: the reader asks for either) */
typedef struct {
    int w, h, format, count;
    uint8_t *rgba, *nv12;
    uint64_t mem, mem_size;   /* (hookshot) the pool's guest memory: where the hardware decoder writes its pictures */
    int rgba_stale;           /* (hookshot) its NV12 picture not converted to RGBA yet (pool_rgba, when it is asked for) */
    int read_by_api;          /* (hookshot) read through ReadDecoderOutputImage*: no copy into its memory (dec_acquire) */
    struct SwsContext *sws;   /* (that conversion's) */
} Pool;
/* the preference MakeDecodeImagePreference returns (the caller's buffer is larger: only these fields are ours) */
typedef struct { uint32_t magic; uint16_t w, h; uint32_t format, tiling; } Pref;

/* (rgba NULL: a picture decoded as NV12 is kept as that alone; its pool makes the RGBA if a reader wants it) */
typedef struct { uint8_t *rgba, *nv12; int w, h; int64_t pts; int last; } Picture;
/* a submission waiting to be decoded (pkt NULL: the stream's end) */
typedef struct Pending { AVPacket *pkt; struct Pending *next; } Pending;
#ifdef _WIN32
/* (hookshot, issue 380) once, when Windows has no AV1 decoder: OoT's movies are AV1, and Windows decodes it only with
   Microsoft's free "AV1 Video Extension" (preinstalled on most PCs, not all). Yes opens its Store page */
static DWORD WINAPI av1_notice(LPVOID unused) {
    (void)unused;
    if (MessageBoxA(NULL, "The game's movies need Microsoft's free \"AV1 Video Extension\", which this PC doesn't have, so they "
                          "show black (the sound plays).\n\nOpen its page in the Microsoft Store now? Install it, then start the game again.",
                    "Hookshot", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES)
        ShellExecuteA(NULL, "open", "ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V", NULL, NULL, SW_SHOWNORMAL);
    return 0;
}
#endif
/* (hookshot) host/mf_av1.c: AV1 on Media Foundation, as the static FFmpeg has no AV1 decoder */
typedef void (*mf_av1_picture_fn)(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts);
void *mf_av1_open(int w, int h);
void mf_av1_decode(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user);
void mf_av1_flush(void *m);
void mf_av1_close(void *m);

typedef struct {
    AVCodecContext *ctx;
    void *mf;                        /* (hookshot) an AV1 decoder on Media Foundation instead of ctx */
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws;
    int sws_w, sws_h, sws_fmt;
    uint64_t slots[MAX_SLOTS];       /* registered pools (guest pointers), by buffer index */
    int busy[MAX_SLOTS];             /* acquired, not released */
    int64_t slot_pts[MAX_SLOTS];
    int slot_last[MAX_SLOTS];        /* the stream's last picture (the submission marked last, all decoded) */
    Picture queue[MAX_QUEUE];
    int qhead, qn;
    int eos;                         /* the last submission is in and decoded: an empty queue is the end */
    Pending *phead, *ptail;          /* submissions not decoded yet, oldest first */
    int npending;                    /* (of them, with data: the stream's end takes no room) */
    uint64_t ready;                  /* the submission-ready event: signalled while the decoder has room */
    int64_t submits, acquires;
    int waiting[MAX_SLOTS];          /* buffers handed out before their pictures, oldest first, and their fences */
    uint64_t waiting_fence[MAX_SLOTS];
    int nwaiting;
    int64_t last_pts;                /* the pts of the last picture handed out */
    int64_t log_submits;             /* (HOS_VIDEO_LOG) submissions still to log, after a flush or the end */
    SRWLOCK lock;
    HANDLE worker;                   /* decodes the submissions (worker_main) */
    CONDITION_VARIABLE wake;         /* (with lock) work for the worker, or its submission done */
    int stop, decoding;
    unsigned gen;                    /* flushes: a submission decoded across one is thrown away */
    Picture got[MAX_QUEUE];          /* (the worker's own) pictures of the submission it decodes */
    int ngot;
} Dec;

/* (hookshot) the host objects of the game's pool and decoder objects, by the guest object's address: kept here, not in
   the guest object (guest memory). Few at a time (a movie's decoder and its pools). */
typedef struct { uint64_t obj; uint32_t magic; void *p; } HostObj;
static HostObj *host_objs;
static size_t nhost_objs, cap_host_objs;
static SRWLOCK host_objs_lock = SRWLOCK_INIT;
static void *host_of(uint64_t obj, uint32_t magic) {
    void *p = NULL;
    if (!obj) return NULL;
    AcquireSRWLockShared(&host_objs_lock);
    for (size_t i = 0; i < nhost_objs; i++)
        if (host_objs[i].obj == obj && host_objs[i].magic == magic) p = host_objs[i].p;
    ReleaseSRWLockShared(&host_objs_lock);
    return p;
}
static void set_host(uint64_t obj, uint32_t magic, void *p) {
    if (!obj) return;
    AcquireSRWLockExclusive(&host_objs_lock);
    size_t i = 0;
    while (i < nhost_objs && host_objs[i].obj != obj) i++;
    if (!p) {
        if (i < nhost_objs) host_objs[i] = host_objs[--nhost_objs];
    } else {
        if (i == nhost_objs) {
            if (nhost_objs == cap_host_objs) {
                cap_host_objs = cap_host_objs ? cap_host_objs * 2 : 16;
                host_objs = (HostObj *)realloc(host_objs, cap_host_objs * sizeof *host_objs);
                if (!host_objs) hos_fatal(NULL, "video: out of memory");
            }
            nhost_objs++;
        }
        host_objs[i] = (HostObj){obj, magic, p};
    }
    ReleaseSRWLockExclusive(&host_objs_lock);
}

/* ---- the service ---- */
HLE(_ZN2nn5codec5video10InitializeEv) {
    static int logged;
    if (!logged++) hos_log("video: nn::codec::video on libavcodec");
    RET(0);
}
HLE(_ZN2nn5codec5video8FinalizeEv) { RET(0); }

/* ---- image pools ---- */
/* MakeDecodeImagePreference(u16 w, u16 h, OutputFormat, TilingMode) -> the preference (returned through x8) */
HLE(_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE) {
    const uint64_t out = c->x[8];
    Pref p = {PREF_MAGIC, (uint16_t)ARG(0), (uint16_t)ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3)};
    if (out) memcpy(GP(out), &p, sizeof p);
    RET(out);
}
/* CalculateDecoderImagePoolBuffer(u64* size, u64* alignment, Preference const&, u32 count) */
HLE(_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj) {
    const Pref *p = (const Pref *)GP(ARG(2));
    const uint64_t one = p->magic == PREF_MAGIC ? (uint64_t)p->w * p->h * 4 : 0x400000;
    if (ARG(0)) G64(ARG(0)) = ((one * (uint32_t)ARG(3)) + 0xFFF) & ~0xFFFull;
    if (ARG(1)) G64(ARG(1)) = 0x1000;
    RET(0);
}
/* InitializeDecoderImagePool(pool, void* memory, size, Preference const&, u32 count) */
HLE(_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj) {
    const Pref *p = (const Pref *)GP(ARG(3));
    Pool *o = (Pool *)calloc(1, sizeof *o);
    if (!o) hos_fatal(NULL, "video: out of memory for an image pool");
    o->w = p->magic == PREF_MAGIC && p->w ? p->w : 1280, o->h = p->magic == PREF_MAGIC && p->h ? p->h : 720;
    o->format = p->magic == PREF_MAGIC ? (int)p->format : 2;
    o->count = (int)ARG(4) > 0 ? (int)ARG(4) : 1;
    o->rgba = (uint8_t *)hos_ledger_took(HL_VIDEO, calloc((size_t)o->count, (size_t)o->w * o->h * 4));
    /* (hookshot, issue 342: an allocation that failed went on as a NULL picture, and the movie thread read address 0) */
    o->nv12 = (uint8_t *)hos_ledger_took(HL_VIDEO, calloc((size_t)o->count, (size_t)o->w * o->h * 3 / 2));
    /* (hookshot) video black, not zeros. A buffer the game takes before the decoder's first picture is shown as it is
       (dec_acquire: "nothing decoded, the buffer again", up to ~100 ms into a movie's start), and NV12 zeros are bright
       green (luma 0 with no chroma): one green frame at each scene change that opens a movie. Luma 16, chroma 128 is
       black; the RGBA opaque black */
    if (o->nv12) {
        for (int i = 0; i < o->count; i++) {
            uint8_t *img = o->nv12 + (size_t)i * o->w * o->h * 3 / 2;
            memset(img, 0x10, (size_t)o->w * o->h);
            memset(img + (size_t)o->w * o->h, 0x80, (size_t)o->w * o->h / 2);
        }
    }
    if (o->rgba)
        for (size_t k = 3; k < (size_t)o->count * o->w * o->h * 4; k += 4) o->rgba[k] = 0xff;
    o->mem = ARG(1), o->mem_size = ARG(2);
    if (video_log())
        hos_log("video: image pool %#llx: %dx%d format %d, %d images, memory %#llx+%#llx", (unsigned long long)ARG(0),
                o->w, o->h, o->format, o->count, (unsigned long long)o->mem, (unsigned long long)o->mem_size);
    set_host(ARG(0), POOL_MAGIC, o);
    RET(0);
}
HLE(_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE) {
    Pool *o = (Pool *)host_of(ARG(0), POOL_MAGIC);
    if (o && o->sws) sws_freeContext(o->sws);
    if (o) hos_ledger_frees(HL_VIDEO, o->rgba), hos_ledger_frees(HL_VIDEO, o->nv12), free(o->rgba), free(o->nv12), free(o);
    set_host(ARG(0), POOL_MAGIC, NULL);
}
/* (hookshot) the pool's picture as RGBA, from its NV12, if it was not made yet. Each 1080p frame of OoT's movies was
   converted on the movie thread, in plain C, though OoT reads only the NV12: nearly half that thread's time, which put the
   picture behind the sound */
static SRWLOCK pool_rgba_lock = SRWLOCK_INIT;
static void pool_rgba(Pool *o) {
    AcquireSRWLockExclusive(&pool_rgba_lock);
    if (o->rgba_stale && o->rgba && o->nv12) {
        if (!o->sws)
            o->sws = sws_getContext(o->w, o->h, AV_PIX_FMT_NV12, o->w, o->h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        const uint8_t *src[4] = {o->nv12, o->nv12 + (size_t)o->w * o->h, NULL, NULL};
        const int src_stride[4] = {o->w, o->w, 0, 0};
        uint8_t *dst[4] = {o->rgba, NULL, NULL, NULL};
        int dst_stride[4] = {o->w * 4, 0, 0, 0};
        if (o->sws) sws_scale(o->sws, src, src_stride, 0, o->h, dst, dst_stride);
        o->rgba_stale = 0;
    }
    ReleaseSRWLockExclusive(&pool_rgba_lock);
}
/* ReadDecoderOutputImageRgba(pool, u32 index, u16 x, u16 y, u16 w, u16 h, void* dst, size_t stride, size_t size) */
HLE(_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm) {
    Pool *o = (Pool *)host_of(ARG(0), POOL_MAGIC);
    const uint32_t i = (uint32_t)ARG(1), x = (uint16_t)ARG(2), y = (uint16_t)ARG(3), w = (uint16_t)ARG(4), h = (uint16_t)ARG(5);
    const uint64_t dst = ARG(6), stride = ARG(7) ? ARG(7) : (uint64_t)w * 4;
    {   /* (hookshot, HOS_VIDEO_LOG) the first reads, with where they go */
        static volatile LONG n;
        if (video_log() && InterlockedIncrement(&n) <= 4)
            hos_log("video: ReadDecoderOutputImageRgba(pool %s, index %u, %u,%u %ux%u, dst %#llx stride %llu)",
                    o ? "ok" : "unknown", i, x, y, w, h, (unsigned long long)dst, (unsigned long long)stride);
    }
    if (!o || !o->rgba || i >= (uint32_t)o->count || !dst || x >= (uint32_t)o->w) { RET(0); return; }
    o->read_by_api = 1;
    if (i == 0 && o->rgba_stale) pool_rgba(o);
    const uint8_t *src = o->rgba + (size_t)i * o->w * o->h * 4;
    const uint32_t cw = x + w <= (uint32_t)o->w ? w : o->w - x;
    for (uint32_t r = 0; r < h && y + r < (uint32_t)o->h; r++) {
        void *row = hos_guest_range(dst + r * stride, (size_t)cw * 4);   /* (hookshot: only into guest memory) */
        if (row) memcpy(row, src + ((size_t)(y + r) * o->w + x) * 4, (size_t)cw * 4);
    }
    HOS_WRITTEN(GP(dst), (size_t)(h ? h - 1 : 0) * stride + (size_t)cw * 4);   /* (the game may give it to the GPU) */
    RET(0);
}
/* ReadDecoderOutputImageNv12(pool, u32 index, u16 x, u16 y, u16 w, u16 h, void* y_dst, size_t y_stride, [stack:]
   size_t y_size, void* uv_dst, size_t uv_stride, size_t uv_size) */
HLE(_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm) {
    Pool *o = (Pool *)host_of(ARG(0), POOL_MAGIC);
    const uint32_t i = (uint32_t)ARG(1), x = (uint16_t)ARG(2), y = (uint16_t)ARG(3), w = (uint16_t)ARG(4), h = (uint16_t)ARG(5);
    const uint64_t ydst = ARG(6), ystride = ARG(7) ? ARG(7) : w;
    const uint64_t uvdst = G64(c->x[31] + 8), uvstride = G64(c->x[31] + 16) ? G64(c->x[31] + 16) : w;
    {   /* (HOS_VIDEO_LOG) each new destination, with the pool and image */
        static uint64_t seen[32];
        static volatile LONG nseen;
        int k;
        for (k = 0; k < nseen && seen[k] != ydst; k++) {}
        if (video_log() && k == nseen && nseen < 32) {
            seen[InterlockedIncrement(&nseen) - 1] = ydst;
            hos_log("video: ReadDecoderOutputImageNv12(pool %#llx %s, index %u, %u,%u %ux%u, y %#llx/%llu, uv %#llx/%llu) "
                    "lr %#llx", (unsigned long long)ARG(0), o ? "ok" : "unknown", i, x, y, w, h, (unsigned long long)ydst,
                    (unsigned long long)ystride, (unsigned long long)uvdst, (unsigned long long)uvstride,
                    (unsigned long long)c->x[30]);
        }
    }
    /* (hookshot) and no NV12 when its allocation failed: o->nv12 + offset was then any host address, copied to the game */
    if (!o || !o->nv12 || i >= (uint32_t)o->count || x >= (uint32_t)o->w) { RET(0); return; }
    o->read_by_api = 1;
    const uint8_t *yp = o->nv12 + (size_t)i * o->w * o->h * 3 / 2, *uvp = yp + (size_t)o->w * o->h;
    const uint32_t cw = x + w <= (uint32_t)o->w ? w : o->w - x;
    for (uint32_t r = 0; ydst && r < h && y + r < (uint32_t)o->h; r++) {
        void *row = hos_guest_range(ydst + r * ystride, cw);   /* (hookshot: only into guest memory) */
        if (row) memcpy(row, yp + (size_t)(y + r) * o->w + x, cw);
    }
    for (uint32_t r = 0; uvdst && r < h / 2 && y / 2 + r < (uint32_t)o->h / 2; r++) {
        void *row = hos_guest_range(uvdst + r * uvstride, cw);
        if (row) memcpy(row, uvp + (size_t)(y / 2 + r) * o->w + (x & ~1u), cw);
    }
    if (ydst) HOS_WRITTEN(GP(ydst), (size_t)(h ? h - 1 : 0) * ystride + cw);
    if (uvdst) HOS_WRITTEN(GP(uvdst), (size_t)(h / 2 ? h / 2 - 1 : 0) * uvstride + cw);
    RET(0);
}

/* ---- decoders ---- */
static void dec_init_sized(Ctx *c, enum AVCodecID id, const char *what, int w, int h);
static DWORD WINAPI worker_main(void *arg);
static void dec_init(Ctx *c, enum AVCodecID id, const char *what) { dec_init_sized(c, id, what, 0, 0); }
static void dec_init_sized(Ctx *c, enum AVCodecID id, const char *what, int w, int h) {
    const AVCodec *codec = NULL;
#ifndef _WIN32   /* (linux) FFmpeg's own AV1 decoder needs a hardware context, and there is no Media Foundation to fall
                    back on: dav1d, else libaom, by name */
    if (id == AV_CODEC_ID_AV1) codec = avcodec_find_decoder_by_name("libdav1d");
    if (!codec && id == AV_CODEC_ID_AV1) codec = avcodec_find_decoder_by_name("libaom-av1");
#endif
    if (!codec) codec = avcodec_find_decoder(id);
    Dec *d = (Dec *)calloc(1, sizeof *d);
    InitializeSRWLock(&d->lock);
    if (codec) {
        d->ctx = avcodec_alloc_context3(codec);
        d->ctx->thread_count = id == AV_CODEC_ID_AV1 ? 4 : 2;   /* (1080p AV1 in software) */
        if (avcodec_open2(d->ctx, codec, NULL) < 0) avcodec_free_context(&d->ctx);
    }
    if (!d->ctx && id == AV_CODEC_ID_AV1) d->mf = mf_av1_open(w, h);
#ifdef _WIN32
    if (!d->ctx && !d->mf && id == AV_CODEC_ID_AV1) {   /* (issue 380: no AV1 decoder in Windows: the movies were black, with sound) */
        static volatile LONG told;
        if (!InterlockedExchange(&told, 1)) {
            hos_log("video: no AV1 decoder: install Microsoft's free AV1 Video Extension for the movies");
            if (!getenv("HOS_HEADLESS") && !getenv("HOOKSHOT_NO_AV1_NOTICE"))
                CreateThread(NULL, 0, av1_notice, NULL, 0, NULL);
        }
    }
#endif
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    InitializeConditionVariable(&d->wake);
    if (d->ctx || d->mf) d->worker = CreateThread(NULL, 0, worker_main, d, 0, NULL);
    set_host(ARG(0), DEC_MAGIC, d);
    static int logged;
    if (logged++ < 4)
        hos_log("video: %s decoder %s (%dx%d)", what, d->ctx ? "ready" : d->mf ? "ready on Media Foundation" :
                "not available in this FFmpeg", w, h);
    RET(0);
}
/* the decoders' submission-ready events (manual-clear system events in guest memory, which is never freed: a finalized
   decoder's goes to the next one) */
static uint64_t spare_events[8];
static int nspare_events;
static void picture_free(Picture *p) {
    hos_ledger_frees(HL_VIDEO, p->rgba), hos_ledger_frees(HL_VIDEO, p->nv12);
    free(p->rgba), free(p->nv12);
    memset(p, 0, sizeof *p);
}
static void pending_free(Dec *d) {
    while (d->phead) {
        Pending *n = d->phead;
        d->phead = n->next;
        av_packet_free(&n->pkt);
        free(n);
    }
    d->ptail = NULL;
    d->npending = 0;
}
static void dec_unwait(Dec *d, int slot);
static void dec_finalize(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (!d) return;
    if (d->worker) {
        AcquireSRWLockExclusive(&d->lock);
        d->stop = 1;
        WakeAllConditionVariable(&d->wake);
        ReleaseSRWLockExclusive(&d->lock);
        WaitForSingleObject(d->worker, INFINITE);
        CloseHandle(d->worker);
    }
    while (d->nwaiting) dec_unwait(d, d->waiting[0]);
    pending_free(d);
    for (int i = 0; i < d->qn; i++) picture_free(&d->queue[(d->qhead + i) % MAX_QUEUE]);
    if (d->sws) sws_freeContext(d->sws);
    avcodec_free_context(&d->ctx);
    mf_av1_close(d->mf);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    if (d->ready) {
        AcquireSRWLockExclusive(&host_objs_lock);
        if (nspare_events < (int)(sizeof spare_events / sizeof spare_events[0])) spare_events[nspare_events++] = d->ready;
        ReleaseSRWLockExclusive(&host_objs_lock);
    }
    free(d);
    set_host(ARG(0), DEC_MAGIC, NULL);
}
/* a decoded frame into the queue, as RGBA and NV12 (the oldest dropped when the reader falls behind) */
static void queue_frame(Dec *d, const AVFrame *f) {
    const int w = f->width, h = f->height;
    if (w <= 0 || h <= 0) return;
    if (f->format != AV_PIX_FMT_NV12 && (!d->sws || d->sws_w != w || d->sws_h != h || d->sws_fmt != f->format)) {
        if (d->sws) sws_freeContext(d->sws);
        d->sws = sws_getContext(w, h, (enum AVPixelFormat)f->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        d->sws_w = w, d->sws_h = h, d->sws_fmt = f->format;
    }
    Picture p = {0};
    p.w = w, p.h = h, p.pts = f->pts;
    const int nv12 = f->format == AV_PIX_FMT_NV12;   /* (hookshot: Media Foundation's pictures, kept as NV12 alone) */
    p.rgba = nv12 ? NULL : (uint8_t *)malloc((size_t)w * h * 4);
    p.nv12 = nv12 ? (uint8_t *)malloc((size_t)w * h * 3 / 2) : (uint8_t *)calloc(1, (size_t)w * h * 3 / 2);
    hos_ledger_took(HL_VIDEO, p.rgba), hos_ledger_took(HL_VIDEO, p.nv12);
    if (!p.nv12 || (!nv12 && !p.rgba)) {   /* (out of memory: this frame is dropped, not written through NULL; issue 342) */
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 4) hos_log("video: no memory for a %dx%d frame: dropped", w, h);
        picture_free(&p);
        return;
    }
    uint8_t *dst[4] = {p.rgba, NULL, NULL, NULL};
    int dst_stride[4] = {w * 4, 0, 0, 0};
    if (d->sws && p.rgba) sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dst, dst_stride);
    if (nv12) {
        for (int y = 0; y < h; y++) memcpy(p.nv12 + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
        for (int y = 0; y < h / 2; y++)
            memcpy(p.nv12 + (size_t)w * h + (size_t)y * w, f->data[1] + (size_t)y * f->linesize[1], (size_t)w);
    } else if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVA420P) {   /* (planar 4:2:0 -> NV12) */
        for (int y = 0; y < h; y++) memcpy(p.nv12 + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
        uint8_t *uv = p.nv12 + (size_t)w * h;
        for (int y = 0; y < h / 2; y++)
            for (int x = 0; x < w / 2; x++) {
                uv[(size_t)y * w + 2 * x] = f->data[1][(size_t)y * f->linesize[1] + x];
                uv[(size_t)y * w + 2 * x + 1] = f->data[2][(size_t)y * f->linesize[2] + x];
            }
    }
    if (d->ngot < MAX_QUEUE) d->got[d->ngot++] = p;
    else picture_free(&p);
}
static void mf_picture(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts) {
    AVFrame f;
    memset(&f, 0, sizeof f);
    f.width = w, f.height = h, f.format = AV_PIX_FMT_NV12, f.pts = pts;
    f.data[0] = (uint8_t *)y, f.data[1] = (uint8_t *)uv;
    f.linesize[0] = f.linesize[1] = stride;
    queue_frame((Dec *)user, &f);
}
static void drain(Dec *d) {
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        queue_frame(d, d->frame);
        av_frame_unref(d->frame);
    }
}
/* one submission through the codec, its pictures into d->got (the worker, d->lock not held) */
static void decode_one(Dec *d, AVPacket *pkt) {
    if (d->mf) {   /* (hookshot) */
        if (pkt) mf_av1_decode(d->mf, pkt->data, (size_t)pkt->size, pkt->pts, mf_picture, d);
        else mf_av1_decode(d->mf, NULL, 0, 0, mf_picture, d);
    } else if (pkt) {
        if (avcodec_send_packet(d->ctx, pkt) == AVERROR(EAGAIN)) {   /* (full: its pictures out first) */
            drain(d);
            avcodec_send_packet(d->ctx, pkt);
        }
        drain(d);
    } else {   /* (the end: the rest out) */
        avcodec_send_packet(d->ctx, NULL);
        drain(d);
        avcodec_flush_buffers(d->ctx);   /* (a stream after it starts again) */
    }
}
/* The decoder's own thread, as the hardware's engine: it decodes the oldest submissions while fewer than DECODE_AHEAD
   pictures wait, with the lock released, so no guest thread runs the codec. A large frame takes several display
   intervals to decode; done on the movie library's input thread, the game's frame waited for that thread. The stream's
   end is taken as soon as it is next, pictures queued or not: the last picture decoded must carry the last flag, as the
   hardware's does (Media Foundation's decoder holds nothing back). The rest of the queue is room for the pictures the
   codec still holds (dav1d's frame delay) */
#define DECODE_AHEAD (MAX_QUEUE / 2)
static void dec_fill(Dec *d);
static void ready_update(Dec *d);
static DWORD WINAPI worker_main(void *arg) {
    Dec *d = (Dec *)arg;
    AcquireSRWLockExclusive(&d->lock);
    for (;;) {
        while (!d->stop && !(d->phead && d->qn < DECODE_AHEAD)) SleepConditionVariableSRW(&d->wake, &d->lock, INFINITE, 0);
        if (d->stop) break;
        Pending *n = d->phead;
        d->phead = n->next;
        if (!d->phead) d->ptail = NULL;
        if (n->pkt) d->npending--;
        const unsigned gen = d->gen;
        d->decoding = 1;
        ReleaseSRWLockExclusive(&d->lock);
        decode_one(d, n->pkt);
        AcquireSRWLockExclusive(&d->lock);
        d->decoding = 0;
        for (int i = 0; i < d->ngot; i++) {
            if (gen != d->gen) { picture_free(&d->got[i]); continue; }
            if (d->qn == MAX_QUEUE) {   /* (not reached: DECODE_AHEAD leaves room) */
                picture_free(&d->queue[d->qhead]);
                d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--;
            }
            d->queue[(d->qhead + d->qn) % MAX_QUEUE] = d->got[i];
            d->qn++;
        }
        d->ngot = 0;
        if (gen == d->gen) {
            d->eos = !n->pkt;
            if (!n->pkt) {   /* (the end: the last picture marked) */
                d->log_submits = 8;   /* (data after the end: a restart, worth seeing) */
                if (d->qn) d->queue[(d->qhead + d->qn - 1) % MAX_QUEUE].last = 1;
            }
        }
        av_packet_free(&n->pkt);
        free(n);
        dec_fill(d);
        ready_update(d);
    }
    ReleaseSRWLockExclusive(&d->lock);
    return 0;
}
static void pending_push(Dec *d, AVPacket *pkt) {
    Pending *n = (Pending *)calloc(1, sizeof *n);
    n->pkt = pkt;
    if (pkt) d->npending++;
    if (d->ptail) d->ptail->next = n;
    else d->phead = n;
    d->ptail = n;
}
/* the submission-ready event follows the room the decoder has (d->lock held). The movie library clears it before each
   submission and, refused one, waits for it */
static int dec_room(const Dec *d) { return d->npending + d->qn < MAX_IN_FLIGHT; }
static void ready_update(Dec *d) {
    if (d->ready && dec_room(d)) hos_system_event_signal(d->ready);
    WakeAllConditionVariable(&d->wake);
}
/* (hookshot) Sync handles. An input handle (SubmitXDecoderInputBuffer's id) is reached at once: the packet is copied.
   An output handle is the hardware's fence for a buffer handed out before its picture is decoded: AcquireXDecoderOutputBuffer
   returns at once (the library aborts on any failure there), and the buffer is filled with the next picture decoded
   (dec_fill), which reaches its fence. A buffer handed out with its picture in it gets 0, reached. The movie library waits for
   an output fence as a sync point event (VideoDecoderEventManager::SetFence: hos_video_sync_defer, from nn_syncpt.c) and
   with TryWaitNativeSync (nn_gs.c); VideoDecoder::Flush takes buffers until one is not reached, the decoder's sign that it
   holds nothing more. Fences are reached in the order they are given out. */
#define SYNC_FENCE_TAG 0x5ec0000000000000ull
#define SYNC_FENCE_MASK 0x0000ffffffffffffull
static SRWLOCK waiter_lock = SRWLOCK_INIT;
static uint64_t fence_given, fence_reached;   /* (waiter_lock) the last fence handed out, the last one reached */
static struct { uint64_t event, fence; } waiters[32];   /* sync point events waiting for a fence */
static int nwaiters;

int hos_video_sync_reached(uint64_t handle) {   /* (nn_gs.c: TryWaitNativeSync, TimedWaitNativeSync) */
    if ((handle & ~SYNC_FENCE_MASK) != SYNC_FENCE_TAG) return 1;
    AcquireSRWLockShared(&waiter_lock);
    const int reached = (handle & SYNC_FENCE_MASK) <= fence_reached;
    ReleaseSRWLockShared(&waiter_lock);
    return reached;
}
static uint64_t fence_give(void) {
    AcquireSRWLockExclusive(&waiter_lock);
    const uint64_t f = ++fence_given;
    ReleaseSRWLockExclusive(&waiter_lock);
    return f;
}
static void fence_reach(uint64_t f) {   /* f and every fence before it; their waiting events signalled */
    uint64_t due[32];
    int n = 0;
    AcquireSRWLockExclusive(&waiter_lock);
    if (f > fence_reached) fence_reached = f;
    for (int i = 0; i < nwaiters;)
        if (waiters[i].fence <= fence_reached) due[n++] = waiters[i].event, waiters[i] = waiters[--nwaiters];
        else i++;
    ReleaseSRWLockExclusive(&waiter_lock);
    for (int i = 0; i < n; i++) hos_system_event_signal(due[i]);
}

/* RequestSyncPointEvent(event, handle): 1 when the event waits for the handle's fence, 0 when it is due now */
int hos_video_sync_defer(uint64_t event, uint64_t handle) {
    if ((handle & ~SYNC_FENCE_MASK) != SYNC_FENCE_TAG) return 0;
    AcquireSRWLockExclusive(&waiter_lock);
    const uint64_t f = handle & SYNC_FENCE_MASK;
    int deferred = f > fence_reached;
    if (deferred) {
        int i = 0;
        while (i < nwaiters && waiters[i].event != event) i++;
        if (i < nwaiters) waiters[i].fence = f;
        else if (nwaiters < (int)(sizeof waiters / sizeof waiters[0])) waiters[nwaiters].event = event, waiters[nwaiters++].fence = f;
        else deferred = 0;   /* (full: signalled now) */
    }
    ReleaseSRWLockExclusive(&waiter_lock);
    return deferred;
}

/* CancelSyncPointEvent(event) */
void hos_video_sync_cancel(uint64_t event) {
    AcquireSRWLockExclusive(&waiter_lock);
    for (int i = 0; i < nwaiters; i++)
        if (waiters[i].event == event) { waiters[i] = waiters[--nwaiters]; break; }
    ReleaseSRWLockExclusive(&waiter_lock);
}

int hos_video_sync_waiting(uint64_t event) {   /* TryWaitSyncPointEvent(event): 1 while it waits for a fence */
    AcquireSRWLockExclusive(&waiter_lock);
    int i = 0;
    while (i < nwaiters && waiters[i].event != event) i++;
    const int waiting = i < nwaiters;
    ReleaseSRWLockExclusive(&waiter_lock);
    return waiting;
}

static volatile LONG64 submissions;
static uint64_t submitted(void) { return (uint64_t)InterlockedIncrement64(&submissions); }   /* an input handle */

/* the picture at the queue's head into buffer slot (d->lock held) */
static void deliver(Dec *d, int slot) {
    Picture *p = &d->queue[d->qhead];
    Pool *o = (Pool *)host_of(d->slots[slot], POOL_MAGIC);
    if (o) {
        /* (the picture at the pool's size: rows and columns past either are left out) */
        const int ok = o->nv12 != NULL && p->nv12 != NULL;   /* (an allocation that failed: nothing copied; issue 342) */
        const int w = !ok ? 0 : p->w < o->w ? p->w : o->w, h = !ok ? 0 : p->h < o->h ? p->h : o->h;
        for (int y = 0; y < h; y++) {
            if (p->rgba && o->rgba) memcpy(o->rgba + (size_t)y * o->w * 4, p->rgba + (size_t)y * p->w * 4, (size_t)w * 4);
            memcpy(o->nv12 + (size_t)y * o->w, p->nv12 + (size_t)y * p->w, (size_t)w);
        }
        for (int y = 0; y < h / 2; y++)
            memcpy(o->nv12 + (size_t)o->w * o->h + (size_t)y * o->w, p->nv12 + (size_t)p->w * p->h + (size_t)y * p->w,
                   (size_t)w);
        o->rgba_stale = !p->rgba;
        /* (hookshot) and into the pool's guest memory, as the hardware decoder leaves it (NV12, pitch = width, chroma right
           after the luma rows; RGBA when the memory has room for it and the pool's format is not NV12's). Not once the
           game reads the pool through ReadDecoderOutputImageNv12 / Rgba, as OoT's movie library does: its pools are
           format 3, so each 1080p picture was made RGBA and copied there for nothing (the movie is the same without it) */
        if (o->mem && o->mem_size && !o->read_by_api) {
            const size_t nv12_size = (size_t)o->w * o->h * 3 / 2, rgba_size = (size_t)o->w * o->h * 4;
            const int rgba = o->format != 0 && o->mem_size >= rgba_size;   /* (VideoDecoderOutputFormat 0: NV12) */
            const size_t need = rgba ? rgba_size : nv12_size;
            void *to = hos_guest_range(o->mem, need);   /* (hookshot: only into guest memory) */
            if (o->mem_size >= need && ok && to && (rgba ? o->rgba != NULL : 1)) {
                if (rgba && o->rgba_stale) pool_rgba(o);
                memcpy(to, rgba ? o->rgba : o->nv12, need);
                HOS_WRITTEN(to, need);
            }
        }
    }
    d->slot_pts[slot] = d->last_pts = p->pts;
    d->slot_last[slot] = p->last;
    if (video_log() && (d->acquires <= 8 || p->last || d->acquires % 300 == 0)) {
        uint64_t sum = 0, n = 0;   /* (the picture's mean luma and RGB, sampled: a black or garbage decode shows) */
        uint64_t rgb[3] = {0, 0, 0};
        for (int y = 0; y < p->h; y += 37)
            for (int x = 0; x < p->w; x += 41, n++) {
                sum += p->nv12[(size_t)y * p->w + x];
                for (int k = 0; k < 3 && p->rgba; k++) rgb[k] += p->rgba[((size_t)y * p->w + x) * 4 + k];
            }
        hos_log("video: acquire %lld: %dx%d pts %lld%s into buffer %d (mean Y %llu, RGB %llu %llu %llu)",
                (long long)d->acquires, p->w, p->h, (long long)p->pts, p->last ? " (last)" : "", slot,
                (unsigned long long)(n ? sum / n : 0), (unsigned long long)(n ? rgb[0] / n : 0),
                (unsigned long long)(n ? rgb[1] / n : 0), (unsigned long long)(n ? rgb[2] / n : 0));
    }
    picture_free(p);
    d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--;
}
/* the buffers handed out before their pictures, filled in order as pictures are decoded; at the stream's end, with nothing
   left, the rest are the end (the last picture's time, marked last). Their fences reached (d->lock held) */
static void dec_fill(Dec *d) {
    while (d->nwaiting) {
        const int slot = d->waiting[0];
        if (d->qn) deliver(d, slot);
        else if (d->eos && !d->phead) d->slot_last[slot] = 1, d->slot_pts[slot] = d->last_pts;
        else break;
        fence_reach(d->waiting_fence[0]);
        d->nwaiting--;
        memmove(d->waiting, d->waiting + 1, d->nwaiting * sizeof d->waiting[0]);
        memmove(d->waiting_fence, d->waiting_fence + 1, d->nwaiting * sizeof d->waiting_fence[0]);
    }
}
/* buffer slot no longer waits for a picture: given back, or the decoder finalized (d->lock held) */
static void dec_unwait(Dec *d, int slot) {
    for (int i = 0; i < d->nwaiting; i++)
        if (d->waiting[i] == slot) {
            fence_reach(d->waiting_fence[i]);
            d->nwaiting--;
            memmove(d->waiting + i, d->waiting + i + 1, (d->nwaiting - i) * sizeof d->waiting[0]);
            memmove(d->waiting_fence + i, d->waiting_fence + i + 1, (d->nwaiting - i) * sizeof d->waiting_fence[0]);
            return;
        }
}

/* SubmitXDecoderInputBuffer(u64* id, decoder, data, size, TimeSpan pts, bool last): kept, decoded when a buffer waits for
   its picture or one is acquired; refused while the decoder holds MAX_IN_FLIGHT, so the reader keeps the movie's pace
   instead of reading the whole file ahead of it */
static void dec_submit(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(1), DEC_MAGIC);
    if (d && (d->ctx || d->mf)) {
        AcquireSRWLockExclusive(&d->lock);
        const int full = ARG(3) && !dec_room(d);
        ReleaseSRWLockExclusive(&d->lock);
        if (full) { RET(RESULT_INPUT_FULL); return; }
    }
    const uint64_t id = submitted();
    if (ARG(0)) G64(ARG(0)) = id;
    if (!d || (!d->ctx && !d->mf)) { RET(0); return; }
    AcquireSRWLockExclusive(&d->lock);
    /* (hookshot) the size is 64-bit: one past what an AVPacket holds was cut to a small packet and copied in whole; and
       the data must lie in guest memory */
    const void *data = ARG(2) && ARG(3) && ARG(3) <= INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE ? hos_guest_range(ARG(2), ARG(3)) : NULL;
    if (ARG(2) && ARG(3) && !data) hos_log("video: submit of %#llx bytes at %#llx left out", (unsigned long long)ARG(3),
                                          (unsigned long long)ARG(2));
    if (data) {
        AVPacket *pkt = av_packet_alloc();
        if (pkt && av_new_packet(pkt, (int)ARG(3)) == 0) {
            memcpy(pkt->data, data, (size_t)ARG(3));
            pkt->pts = (int64_t)ARG(4);
            pending_push(d, pkt);
        } else {
            av_packet_free(&pkt);
        }
    }
    d->submits++;
    if (ARG(5) & 1) pending_push(d, NULL);
    const int logged_one = d->log_submits > 0;
    if (logged_one) d->log_submits--;
    if (video_log() && (d->submits <= 8 || (ARG(5) & 1) || logged_one))
        hos_log("video: submit %lld (%llu bytes, pts %lld%s)", (long long)d->submits, (unsigned long long)ARG(3),
                (long long)ARG(4), (ARG(5) & 1) ? ", last" : "");
    dec_fill(d);
    ready_update(d);
    ReleaseSRWLockExclusive(&d->lock);
    RET(0);
}
static void dec_flush(Ctx *c, const char *what) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (d->ctx || d->mf)) {
        AcquireSRWLockExclusive(&d->lock);
        if (video_log())
            hos_log("video: %s after %lld submits and %lld acquires (%d queued, %s, lr %#llx)", what, (long long)d->submits,
                    (long long)d->acquires, d->qn, d->eos ? "at the end" : d->phead ? "submissions left" : "none left",
                    (unsigned long long)c->x[30]);
        d->log_submits = 8;
        while (d->decoding) SleepConditionVariableSRW(&d->wake, &d->lock, INFINITE, 0);   /* (the codec is the worker's) */
        d->gen++;
        if (d->ctx) avcodec_flush_buffers(d->ctx);
        else mf_av1_flush(d->mf);
        pending_free(d);
        for (int i = 0; i < d->qn; i++) picture_free(&d->queue[(d->qhead + i) % MAX_QUEUE]);
        d->qhead = d->qn = 0;
        d->eos = 0;
        for (int i = 0; i < MAX_SLOTS; i++) d->slot_last[i] = 0;
        ready_update(d);
        ReleaseSRWLockExclusive(&d->lock);
    }
    RET(0);
}
/* RegisterXDecoderBuffer(decoder, u32 index, pool, u32 image) / UnregisterXDecoderBuffer(decoder, u32 index) */
static void dec_register(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (uint32_t)ARG(1) < MAX_SLOTS) d->slots[(uint32_t)ARG(1)] = ARG(2), d->busy[(uint32_t)ARG(1)] = 0;
    RET(0);
}
static void dec_unregister(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (uint32_t)ARG(1) < MAX_SLOTS) d->slots[(uint32_t)ARG(1)] = 0;
    RET(0);
}
/* AcquireXDecoderOutputBuffer(u32* index, NativeSyncHandle*, decoder): a free buffer, with the oldest picture in it, or
   with a fence it waits on for the next picture (dec_fill); after the stream's end, the end */
static void dec_acquire(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(2), DEC_MAGIC);
    if (ARG(1)) G64(ARG(1)) = 0;
    if (!d) { RET(RESULT_NO_OUTPUT); return; }
    AcquireSRWLockExclusive(&d->lock);
    dec_fill(d);
    int slot = -1;
    for (int i = 0; i < MAX_SLOTS && slot < 0; i++)
        if (d->slots[i] && !d->busy[i] && host_of(d->slots[i], POOL_MAGIC)) slot = i;
    if (slot < 0) {
        ready_update(d);
        ReleaseSRWLockExclusive(&d->lock);
        RET(RESULT_NO_OUTPUT);
        return;
    }
    d->acquires++;
    d->busy[slot] = 1;
    if (!d->nwaiting && d->qn) {
        deliver(d, slot);
    } else if (!d->nwaiting && d->eos && !d->phead) {   /* (the end at the last picture's time) */
        d->slot_last[slot] = 1, d->slot_pts[slot] = d->last_pts;
        if (video_log()) hos_log("video: acquire %lld: the end, buffer %d", (long long)d->acquires, slot);
    } else {
        const uint64_t f = fence_give();
        d->waiting[d->nwaiting] = slot, d->waiting_fence[d->nwaiting++] = f;
        if (ARG(1)) G64(ARG(1)) = SYNC_FENCE_TAG | (f & SYNC_FENCE_MASK);
        if (video_log()) hos_log("video: acquire %lld: buffer %d waits for the next picture", (long long)d->acquires, slot);
    }
    ready_update(d);
    ReleaseSRWLockExclusive(&d->lock);
    if (ARG(0)) G32(ARG(0)) = (uint32_t)slot;
    RET(0);
}
/* ReleaseXDecoderOutputBuffer(decoder, u32 index, NativeSyncHandle) */
static void dec_release(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (uint32_t)ARG(1) < MAX_SLOTS) {
        AcquireSRWLockExclusive(&d->lock);
        dec_unwait(d, (int)(uint32_t)ARG(1));
        d->busy[(uint32_t)ARG(1)] = 0;
        ReleaseSRWLockExclusive(&d->lock);
    }
    RET(0);
}
/* GetXDecoderOutputBufferProperties(u16* w, u16* h, u16* display w, u16* display h, TimeSpan* pts, bool* last,
   decoder, u32 index) */
static void dec_properties(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(6), DEC_MAGIC);
    const uint32_t i = (uint32_t)ARG(7);
    const Pool *o = d && i < MAX_SLOTS ? (const Pool *)host_of(d->slots[i], POOL_MAGIC) : NULL;
    const uint16_t w = o ? (uint16_t)o->w : 0, h = o ? (uint16_t)o->h : 0;
    if (ARG(0)) *(uint16_t *)GP(ARG(0)) = w;
    if (ARG(1)) *(uint16_t *)GP(ARG(1)) = h;
    if (ARG(2)) *(uint16_t *)GP(ARG(2)) = w;
    if (ARG(3)) *(uint16_t *)GP(ARG(3)) = h;
    if (ARG(4)) G64(ARG(4)) = d && i < MAX_SLOTS ? (uint64_t)d->slot_pts[i] : 0;
    if (ARG(5)) *(uint8_t *)GP(ARG(5)) = d && i < MAX_SLOTS ? (uint8_t)d->slot_last[i] : 0;
    {   /* (HOS_VIDEO_LOG) the first times the game is told a picture is the last */
        static volatile LONG n;
        if (video_log() && d && i < MAX_SLOTS && d->slot_last[i] && InterlockedIncrement(&n) <= 8)
            hos_log("video: properties of buffer %u: the last picture, pts %lld (lr %#llx)", i, (long long)d->slot_pts[i],
                    (unsigned long long)c->x[30]);
    }
    RET(0);
}
/* GetXDecoderSubmissionReadyEvent(decoder): the decoder's own (dec_room) */
static uint64_t ready_event(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (!d) return 0;
    AcquireSRWLockExclusive(&d->lock);
    if (!d->ready) {
        AcquireSRWLockExclusive(&host_objs_lock);
        d->ready = nspare_events ? spare_events[--nspare_events] : 0;
        ReleaseSRWLockExclusive(&host_objs_lock);
        if (!d->ready) d->ready = hos_sys_alloc(0x40, 16);
        hos_system_event_init(d->ready, 0);
        ready_update(d);
    }
    const uint64_t event = d->ready;
    ReleaseSRWLockExclusive(&d->lock);
    return event;
}

/* VP9 */
HLE(_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP9, "VP9"); }
HLE(_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP9, "VP9"); }
HLE(_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE) { dec_finalize(c); }
HLE(_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj) { dec_register(c); }
HLE(_ZN2nn5codec26UnregisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEj) { dec_unregister(c); }
HLE(_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE) { RET(0); }
HLE(_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE) { dec_flush(c, "Stop"); }
HLE(_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE) { dec_flush(c, "Flush"); }
HLE(_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPNS0_14Vp9DecoderTypeE) { RET(ready_event(c)); }
HLE(_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb) { dec_submit(c); }
HLE(_ZN2nn5codec36SubmitEncryptedVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb) {
    const uint64_t id = submitted();
    if (ARG(0)) G64(ARG(0)) = id;
    RET(0);
}
HLE(_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE) { dec_acquire(c); }
HLE(_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE) { dec_release(c); }
HLE(_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj) { dec_properties(c); }

/* (hookshot) AV1: OoT's movies. FFmpeg's AV1 decoder if the build has one, else Media Foundation (host/mf_av1.c).
   Initialize...ByTransferMemory(decoder, memory, size, ?, u16 width, u16 height, ...), ...ByRemoteMemory(decoder,
   size, u16 width, u16 height, ...) */
HLE(_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) {
    dec_init_sized(c, AV_CODEC_ID_AV1, "AV1", (int)(uint16_t)ARG(4), (int)(uint16_t)ARG(5));
}
HLE(_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) {
    dec_init_sized(c, AV_CODEC_ID_AV1, "AV1", (int)(uint16_t)ARG(2), (int)(uint16_t)ARG(3));
}
HLE(_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE) { dec_finalize(c); }
HLE(_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj) { dec_register(c); }
HLE(_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE) { RET(0); }
HLE(_ZN2nn5codec14StopAv1DecoderEPNS0_14Av1DecoderTypeE) { dec_flush(c, "Stop"); }
HLE(_ZN2nn5codec15FlushAv1DecoderEPNS0_14Av1DecoderTypeE) { dec_flush(c, "Flush"); }
HLE(_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE) { RET(ready_event(c)); }
HLE(_ZN2nn5codec27SubmitAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb) { dec_submit(c); }
HLE(_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE) { dec_acquire(c); }
HLE(_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE) { dec_release(c); }
HLE(_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj) { dec_properties(c); }

/* VP8 */
HLE(_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP8, "VP8"); }
HLE(_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP8, "VP8"); }
HLE(_ZN2nn5codec18FinalizeVp8DecoderEPNS0_14Vp8DecoderTypeE) { dec_finalize(c); }
HLE(_ZN2nn5codec24RegisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj) { dec_register(c); }
HLE(_ZN2nn5codec26UnregisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEj) { dec_unregister(c); }
HLE(_ZN2nn5codec15StartVp8DecoderEPNS0_14Vp8DecoderTypeE) { RET(0); }
HLE(_ZN2nn5codec14StopVp8DecoderEPNS0_14Vp8DecoderTypeE) { dec_flush(c, "Stop"); }
HLE(_ZN2nn5codec15FlushVp8DecoderEPNS0_14Vp8DecoderTypeE) { dec_flush(c, "Flush"); }
HLE(_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPNS0_14Vp8DecoderTypeE) { RET(ready_event(c)); }
HLE(_ZN2nn5codec27SubmitVp8DecoderInputBufferEPmPNS0_14Vp8DecoderTypeEPKvmNS_8TimeSpanEb) { dec_submit(c); }
HLE(_ZN2nn5codec29AcquireVp8DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp8DecoderTypeE) { dec_acquire(c); }
HLE(_ZN2nn5codec29ReleaseVp8DecoderOutputBufferEPNS0_14Vp8DecoderTypeEjNS_2gs16NativeSyncHandleE) { dec_release(c); }
HLE(_ZN2nn5codec35GetVp8DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp8DecoderTypeEj) { dec_properties(c); }
