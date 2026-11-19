#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long long LONG64;
static volatile int present_interval = 1;
static int64_t next_frame_at, window_start, frames_in_window, shown_at[16];
static volatile LONG64 presents, shown_count;
static int steps, sleeps, reads;
static int64_t clock_now;
#define GP(a) ((void *)(uintptr_t)(a))
#define os_atomic_inc(p) (++*(p))
static int64_t hos_now_ns(void) { ++reads; return clock_now; }
static void game_frame_timing(int64_t now) { (void)now; ++steps; }   /* (the game's patches) */
static int hos_bench_on(void) { return 0; }   /* (the bench setting off) */
static void hos_bench_present(int64_t now) { (void)now; }
static void hos_thread_fp_log(long long present) { (void)present; }   /* (log.fp off) */
void hos_code_ptr_report(void) {}   /* (runtime/code_ptrs.c) */
static void hos_log(const char *fmt, ...) { (void)fmt; }
static int hos_log_switch(setting_id id) { (void)id; return 0; }
static int64_t frame_period(void) { return 1000000000LL / 60 * present_interval; }   /* (nn_vi.c: the console's 60 here) */
@HELPER@
@PRESENTED@

int main(int argc, char **argv) {
    if (argc != 3) return 90;
    const int enabled = !strcmp(argv[1], "enabled");
    const int64_t times[] = {1000000000LL, 1016666667LL, 1066666667LL, 9066666667LL, 9083333334LL};
    for (int i = 0; i < 5; ++i) {
        clock_now = times[i];
        hos_vi_native_presented();
        if (enabled && i == 3) {
            FILE *probe = fopen(argv[2], "rb");
            if (!probe) return 1; /* old source is red: no trace */
            int lines = 0, c;
            while ((c = fgetc(probe)) != EOF) if (c == '\n') ++lines;
            fclose(probe);
            if (lines != 5) return 2; /* header + four returned frames flushed after the stall */
        }
    }
    if (presents != 5 || shown_count != 5 || steps != 5 || sleeps != 0 || reads != 5) return 3;
    printf("STATE %lld %lld %lld %d %d %d\n", (long long)presents, (long long)shown_count,
           (long long)next_frame_at, steps, sleeps, reads);
    if (!enabled) {
        FILE *probe = fopen(argv[2], "rb");
        if (probe) { fclose(probe); return 4; }
    }
    return 0; /* normal CRT exit flushes the remaining buffered row */
}
