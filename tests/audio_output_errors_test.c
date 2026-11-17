/* Synthetic device calls around the real nn_audio.c output section. No device or game is opened: the test plays the
   device, pulling from the section's queue as SDL's thread would. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", scenario, __LINE__, #x); exit(1); } } while (0)

static const char *scenario;
static int open_calls, logs, locked, lock_calls;
static int last_channels;
static void (*device_pull)(void *user, uint8_t *out, int len);
static void *device_user;
static uint64_t now_ms;
static int fail_open, fail_open6;
static char last_log[512];

/* (single-threaded here: the lock is only checked to be taken and given back in pairs) */
void os_mutex_lock(os_mutex *m) { (void)m; CHECK(!locked); locked = 1; lock_calls++; }
void os_mutex_unlock(os_mutex *m) { (void)m; CHECK(locked); locked = 0; }
static const char *test_getenv(const char *name) { (void)name; return NULL; }
static void hos_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof last_log, format, args);
    va_end(args);
    logs++;
}
static const char *hos_sdl_error(void) { return "no audio device (test)"; }
static int hos_sdl_audio_open(int freq, int channels, int s16, void (*pull)(void *user, uint8_t *out, int len), void *user) {
    CHECK(!locked);
    CHECK((channels == 2 || channels == 6) && freq == 48000 && s16);
    last_channels = channels;
    open_calls++;
    if (fail_open || (fail_open6 && channels == 6)) return 0;
    device_pull = pull, device_user = user;
    return 1;
}
/* (the platform helpers nn_audio.c defines before its output section) */
static uint64_t au_ms(void) { return now_ms; }
static int speakers = 2;   /* (the game's speakers: 6 in the "surround" and "fallback" cases) */
static int au_speakers(void) { return speakers; }
#define getenv test_getenv
#include "audio_output.inc"

/* the bytes the device has played, in order */
static int16_t played[FRAME * 6 * 200];
static int played_bytes;
static void device_take(void *user, int len) {
    static uint8_t buf[FRAME * 2 * 6 * 4 + 64];
    CHECK(len <= (int)sizeof buf);
    memset(buf, 0x55, sizeof buf);
    device_pull(user, buf, len);
    CHECK(!locked);
    CHECK(buf[len] == 0x55);   /* (nothing past len written) */
    CHECK(played_bytes + len <= (int)sizeof played);
    memcpy((uint8_t *)played + played_bytes, buf, (size_t)len);
    played_bytes += len;
}
static int queued_count(void) {
    int n = 0;
    for (int i = 0; i < NBLOCKS; i++) n += queued[i] != 0;
    return n;
}
static void take_all(void) {
    while (queued_count()) device_take(device_user, block_bytes);
}
static int frames_made;
static void frames(int count, int complete) {
    int16_t pcm[FRAME * 6];
    for (int j = 0; j < count; j++, frames_made++) {
        for (int i = 0; i < FRAME * out_channels; i++) pcm[i] = (int16_t)(frames_made * 31 + i);
        output_frame(pcm);
        if (complete) take_all();
    }
}
/* the device played frames first..first+count-1 as made, frame after frame */
static void check_played(int at, int first, int count) {
    const int n = FRAME * out_channels;
    for (int f = 0; f < count; f++)
        for (int i = 0; i < n; i++) CHECK(played[at + f * n + i] == (int16_t)((first + f) * 31 + i));
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    scenario = argv[1];
    fail_open = !strcmp(scenario, "open");
    if (!strcmp(scenario, "surround") || !strcmp(scenario, "fallback")) speakers = 6;
    fail_open6 = !strcmp(scenario, "fallback");
    output_open();
    if (fail_open6) {
        CHECK(open_calls == 2 && last_channels == 2);   /* (5.1 refused, then stereo) */
        CHECK(out_channels == 2 && block_bytes == FRAME * 2 * 2 * 2);
        CHECK(strstr(last_log, "default device") && strstr(last_log, "stereo"));
        logs = 0;
        frames(40, 1);
        CHECK(out_played == 20 && played_bytes == 40 * FRAME * 4 && !logs);
        check_played(0, 0, 40);
    } else if (fail_open) {
        CHECK(open_calls == 1 && !out_device);   /* (stereo: nothing to fall back to) */
        CHECK(strstr(last_log, "hos_sdl_audio_open") && strstr(last_log, "no audio device (test)"));
        frames(40, 1);
        CHECK(!queued_count() && !out_played && logs == 1);
    } else if (open_calls != 1 || !out_device || logs != 1 || !strstr(last_log, "default device")) {
        CHECK(0);   /* (the default is opened first, and named in the log) */
    } else if ((logs = 0), !strcmp(scenario, "surround")) {
        CHECK(last_channels == 6 && out_channels == 6);
        CHECK(block_bytes == FRAME * 2 * 6 * 2);   /* (two frames of six 16-bit channels) */
        frames(40, 1);
        CHECK(out_played == 20 && played_bytes == 40 * FRAME * 12 && !logs);
        check_played(0, 0, 40);
    } else if (!strcmp(scenario, "stall")) {
        frames(20, 0);                   /* every block queued, the device takes none: it is deaf */
        CHECK(queued_count() == NBLOCKS);
        void *old = device_user;
        now_ms += 1000;
        frames(1, 0);                    /* the stall noticed */
        CHECK(open_calls == 1);
        now_ms += 1600;
        frames(1, 0);                    /* 1.6 s later: left, the default opened again */
        CHECK(open_calls == 2 && device_user != old && strstr(last_log, "default device"));
        CHECK(!queued_count() && fill_block == 0 && fill_frames == 0);
        frames(4, 0);
        CHECK(queued_count() == 2);
        played_bytes = 0;
        device_take(old, block_bytes);   /* the old device (never closed) plays silence, takes nothing */
        for (int i = 0; i < FRAME * 4; i++) CHECK(played[i] == 0);
        CHECK(queued_count() == 2);
        played_bytes = 0;
        take_all();                      /* the new one plays what came after */
        CHECK(played_bytes == 4 * FRAME * 4);
        check_played(0, 22, 4);
    } else if (!strcmp(scenario, "busy")) {
        frames(20, 0);
        int16_t before[FRAME * 4];
        memcpy(before, blocks[0], sizeof before);
        frames(5, 0);                    /* the device is behind: these are dropped, no queued block is touched */
        CHECK(out_played == 10 && fill_frames == 0 && fill_block == 0);
        CHECK(!memcmp(before, blocks[0], sizeof before));
        take_all();
        CHECK(played_bytes == 20 * FRAME * 4);
        check_played(0, 0, 20);
        frames(2, 1);
        CHECK(out_played == 11 && !logs);
        check_played(20 * FRAME * 2, 25, 2);
    } else if (!strcmp(scenario, "partial")) {
        frames(6, 0);                    /* three blocks queued */
        device_take(device_user, block_bytes / 3);
        CHECK(queued_count() == 3 && queued[0]);   /* a block is the device's until its last byte is taken */
        device_take(device_user, block_bytes - block_bytes / 3 + 6);
        CHECK(queued_count() == 2 && !queued[0] && play_block == 1 && play_pos == 6);
        frames(2, 0);                    /* block 3 queued while the device is inside block 1 */
        CHECK(queued_count() == 3);
        device_take(device_user, block_bytes * 3 - 6);
        CHECK(!queued_count() && play_block == 4 && play_pos == 0);
        CHECK(played_bytes == 8 * FRAME * 4);
        check_played(0, 0, 8);           /* in order, nothing lost or repeated across the pieces */
    } else if (!strcmp(scenario, "behind")) {
        device_take(device_user, 100);   /* nothing queued: silence */
        for (int i = 0; i < 50; i++) CHECK(played[i] == 0);
        frames(2, 0);
        played_bytes = 0;
        device_take(device_user, block_bytes + 64);   /* one block, then silence for the rest */
        check_played(0, 0, 2);
        for (int i = 0; i < 32; i++) CHECK(played[FRAME * 4 + i] == 0);
        CHECK(!queued_count() && !logs);
    } else {
        CHECK(!strcmp(scenario, "normal"));
        CHECK(last_channels == 2 && block_bytes == FRAME * 2 * 2 * 2);
        frames(40, 1);
        CHECK(out_played == 20 && fill_block == 0 && fill_frames == 0 && !logs);
        CHECK(played_bytes == 40 * FRAME * 4);
        check_played(0, 0, 40);
        CHECK(blocks[9][0] == 38 * 31 && blocks[9][FRAME * 2] == 39 * 31);
    }
    CHECK(!locked && lock_calls > 0);
    printf("PASS %s: actual output ring\n", scenario);
    return 0;
}
