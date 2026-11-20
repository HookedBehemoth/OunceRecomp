/* nvn_texels.h: a game patch's storage-layout bytes to a texture's image */
#include "hos.h"
#include "renderer.h"
#include "rx_nvn.h"
#include "nvn_texels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

int nvn_texels_storage(uint32_t source, const void *tex, const uint8_t *src, uint64_t n, bl_stats *st) {
    const nvn_tex *t = (const nvn_tex *)tex;
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (bpb <= 0 || n != t->L.size) return 0;
    if (nvn_tex_is_linear(&t->d)) {   /* (pitch-linear: its rows as they are) */
        if (t->d.target != NVN_TEXTURE_TARGET_2D || !t->d.stride) return 0;
        NVNcopyRegion r = {0, 0, 0, (int)t->d.width, (int)t->d.height, 1};
        rx_direct()->texels(source, t, 0, 0, &r, src, t->d.stride, 0);
        os_atomic_inc(&st->uploads);
        os_atomic_add(&st->bytes, (int64_t)n);
        return 1;
    }
    const uint32_t depth = t->d.target == NVN_TEXTURE_TARGET_3D ? (t->d.depth ? t->d.depth : 1) : t->L.layers;
    const uint64_t lin = (uint64_t)((t->d.width + bw - 1) / bw) * ((t->d.height + bh - 1) / bh) * (uint64_t)bpb * depth;
    static _Thread_local struct { uint8_t *p; uint64_t cap; } buf;   /* (grows to the largest texture this thread uploads) */
    if (lin > buf.cap) {
        free(buf.p);
        buf.p = (uint8_t *)malloc(lin), buf.cap = buf.p ? lin : 0;
        if (!buf.p) return 0;
    }
    const int64_t t0 = os_now_ns();
    if (nvn_tex_level0_linear(&t->d, src, n, buf.p) != lin) return 0;
    const int64_t ns = os_now_ns() - t0;
    NVNcopyRegion r = {0, 0, 0, (int)t->d.width, (int)t->d.height, (int)depth};
    rx_direct()->texels(source, t, 0, 0, &r, buf.p, 0, 0);
    const int64_t k = os_atomic_inc(&st->uploads);
    os_atomic_add(&st->bytes, (int64_t)n);
    os_atomic_add(&st->ns, ns);
    for (int64_t m = st->max_ns; ns > m && !__sync_bool_compare_and_swap(&st->max_ns, m, ns); m = st->max_ns) {}
    if (k <= 3)
        hos_log("texels: deswizzled %ux%ux%u format %#x (%llu bytes, block height %u) in %.3f ms on thread %s", t->d.width,
                t->d.height, depth, t->d.format, (unsigned long long)n, t->L.tileH, ns / 1e6, hos_thread_name(hos_self()));
    return 1;
}


void nvn_texels_cpu_made(const void *tex, uint64_t src) {
    static bl_stats st;
    const nvn_tex *t = (const nvn_tex *)tex;
    if (!src) return;   /* (no data: an image the GPU writes) */
    const char *why = t->d.levels > 1 ? "more than one level"
                    : !hos_guest_range(src, t->L.size) ? "its data outside guest memory"
                    : !nvn_texels_storage(GPU_TEXELS_CPU, t, (const uint8_t *)GP(src), t->L.size, &st) ? "a layout not deswizzled" : NULL;
    if (!why) return;
    char line[200];
    snprintf(line, sizeof line, "texture %ux%u format %#x at %#llx: %s", t->d.width, t->d.height, t->d.format,
             (unsigned long long)t->gpu, why);
    hos_log("texels: ERROR: a texture the game's code made, not handed over: %s", line);
    rx_direct()->host_event("a texture the game's code made, not handed over", line);
}

int nvn_tex_live_at(uint64_t gpu, nvn_tex *out);
void nvn_texels_not_handed(const char *site, uint64_t dst, uint64_t n, const void *tex) {
    static volatile int32_t told;
    const nvn_tex *t = (const nvn_tex *)tex;
    char line[240];
    snprintf(line, sizeof line, "%s: %#llx (%llu bytes) is no live texture's whole storage (%s %llu bytes)", site,
             (unsigned long long)dst, (unsigned long long)n, t ? "the texture there has" : "no texture there",
             (unsigned long long)(t ? t->L.size : 0));
    if (os_atomic_inc(&told) <= 16) hos_log("texels: ERROR: a CPU texel write not handed over: %s", line);
    rx_direct()->host_event("a CPU texel write not handed over", line);
}
void nvn_texels_fill(const char *site, uint64_t dst, int c, uint64_t n) {
    static bl_stats st;
    nvn_tex t;
    const int live = nvn_tex_live_at(dst, &t);
    uint8_t *b = live && n == t.L.size ? (uint8_t *)malloc(n) : NULL;
    if (b) memset(b, c, n);
    if (!b || !nvn_texels_storage(GPU_TEXELS_CPU, &t, b, n, &st)) {
        nvn_texels_not_handed(site, dst, n, live ? &t : NULL);
        if (hos_guest_range(dst, n)) memset(GP(dst), c, n);
    }
    free(b);
}
