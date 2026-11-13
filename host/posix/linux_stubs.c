/* (linux) What the Windows build has and Linux does without:

   - Media Foundation's AV1 decoder (host/mf_av1.c): FFmpeg decodes AV1 itself here, with dav1d (host/nn_video.c asks
     FFmpeg first), so these are never reached with a dav1d build; without one, AV1 movies are skipped. */
#include "../hos.h"

typedef void (*mf_av1_picture_fn)(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts);
void *mf_av1_open(int w, int h) { (void)w, (void)h; return NULL; }
void mf_av1_decode(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user) {
    (void)m, (void)data, (void)n, (void)pts, (void)fn, (void)user;
}
void mf_av1_flush(void *m) { (void)m; }
void mf_av1_close(void *m) { (void)m; }
