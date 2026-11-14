/* HOS_BENCH=<frames>[,<ready frames>]: a benchmark run, for numbers that compare between runs. The game steps exactly 1/60 s of
   game time a frame whatever the wall time (nn_vi.c realtime_step, rx_option 0 answered "per frame") and presents unpaced
   (hos_vi_native_acquire waits for nothing). Once the scene is ready, every part of it settled for <ready frames> presents in a
   row (60 by default), it runs <frames> more presents, writes bench.csv in the working folder (the run's), logs a summary
   and exits with code 0. The parts: in play (Link updated in the last presents, no map load, no event playing: hos_main.c),
   no texture work (agl::TextureDataMgr's "work pending", docs/direct/texture-streaming.md), no shader work (the back end's
   shader jobs queued or running, and no draw skipped for a shader not ready), no streaming (no file read by the game since
   the last present). Unset: off, and nothing here runs but the checks of hos_bench_on. */
#include "hos.h"
#include "bench.h"
#include "nvn_list.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   /* (ExitProcess) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#endif

void rx_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines);   /* (rx_backend.c: the back end's) */
double hos_log_seconds(void);   /* (hos_thread.c: the log's seconds since the process started) */
extern volatile uint64_t hos_step_probe;   /* (nn_vi.c: the frame step's object; +0xd58 the game clock in 1/60 s) */

static int frames_wanted = -1, ready_need = 60;
int hos_bench_on(void) {
    if (frames_wanted < 0) {
        const char *e = getenv("HOS_BENCH");
        frames_wanted = e && atoi(e) > 0 ? atoi(e) : 0;
        const char *k = e ? strchr(e, ',') : NULL;
        if (k && atoi(k + 1) > 0) ready_need = atoi(k + 1);
    }
    return frames_wanted;
}

static int64_t fs_reads;   /* (the game's file reads, nn_fs.c) */
void hos_bench_fs_read(void) { __atomic_add_fetch(&fs_reads, 1, __ATOMIC_RELAXED); }
static int64_t play_present = -1000;   /* (the present of the last player update that was in play) */
void hos_bench_player(int loading, int event_playing) {
    if (!loading && !event_playing) __atomic_store_n(&play_present, hos_vi_present_number(), __ATOMIC_RELAXED);
    else __atomic_store_n(&play_present, -1000, __ATOMIC_RELAXED);
}

typedef struct {
    double wall_ms, game_step;
    int64_t draws, waiting, shader_jobs, fs_reads, lists, list_calls;
} BenchRow;

/* ---- CPU time by thread group (Linux: /proc/self/task; the group is the thread's name without its trailing digits) ---- */
#define CPU_GROUPS 48
typedef struct { char name[20]; double secs; } CpuGroup;
static int cpu_sample(CpuGroup *g) {
    int n = 0;
#ifdef __linux__
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    const double tick = (double)sysconf(_SC_CLK_TCK);
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char path[64], line[512];
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        const size_t len = fread(line, 1, sizeof line - 1, f);
        fclose(f);
        line[len] = 0;
        char *open = strchr(line, '('), *close = strrchr(line, ')');
        if (!open || !close) continue;
        char name[20];
        size_t m = (size_t)(close - open - 1);
        if (m >= sizeof name) m = sizeof name - 1;
        memcpy(name, open + 1, m);
        while (m && ((name[m - 1] >= '0' && name[m - 1] <= '9') || name[m - 1] == ' ' || name[m - 1] == '_' || name[m - 1] == '-')) m--;
        name[m] = 0;
        unsigned long long ut = 0, st = 0;   /* (fields 14 and 15, after the state at field 3) */
        if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        int i = 0;
        while (i < n && strcmp(g[i].name, name)) i++;
        if (i == n) {
            if (n == CPU_GROUPS) continue;
            snprintf(g[n].name, sizeof g[n].name, "%s", name), g[n].secs = 0, n++;
        }
        g[i].secs += (double)(ut + st) / tick;
    }
    closedir(d);
#else
    (void)g;
#endif
    return n;
}

static double game_clock(void) {
    const uint64_t p = hos_step_probe;
    double v = 0;
    if (p) memcpy(&v, GP(p + 0xd58), 8);
    return v;
}

static void bench_finish(const BenchRow *rows, int n, double ready_at, const CpuGroup *cpu0, int ncpu0) {
    FILE *f = fopen("bench.csv", "w");
    if (f) {
        fputs("frame,wall_ms,game_step,draws,draws_waiting,shader_jobs,fs_reads,lists_made,list_calls\n", f);
        for (int i = 0; i < n; i++)
            fprintf(f, "%d,%.4f,%.4f,%lld,%lld,%lld,%lld,%lld,%lld\n", i, rows[i].wall_ms, rows[i].game_step, (long long)rows[i].draws,
                    (long long)rows[i].waiting, (long long)rows[i].shader_jobs, (long long)rows[i].fs_reads, (long long)rows[i].lists,
                    (long long)rows[i].list_calls);
        fclose(f);
    } else {
        hos_log("bench: could not write bench.csv");
    }
    double *ms = malloc(sizeof(double) * 2 * (size_t)n);
    if (!ms) return;
    int64_t draws = 0, steps_off = 0;
    for (int i = 0; i < n; i++) {
        ms[i] = rows[i].wall_ms, draws += rows[i].draws;
        if (rows[i].game_step != 1.0) steps_off++;
    }
    const BenchSummary s = bench_summary(ms, n, ms + n);
    free(ms);
    hos_log("bench: %d frames in %.3f s (ready at %.2f s): frame mean %.3f ms, median %.3f, p99 %.3f, max %.3f; %.1f draws a "
            "frame; game steps not 1/60 s: %lld", n, s.total / 1e3, ready_at, s.mean, s.median, s.p99, s.max, (double)draws / n,
            (long long)steps_off);
    CpuGroup cpu1[CPU_GROUPS];
    const int ncpu1 = cpu_sample(cpu1);
    if (ncpu1) {   /* (the CPU seconds of the measured frames by thread group, the busiest first) */
        CpuGroup d[CPU_GROUPS];
        for (int i = 0; i < ncpu1; i++) {
            d[i] = cpu1[i];
            for (int k = 0; k < ncpu0; k++)
                if (!strcmp(cpu0[k].name, cpu1[i].name)) d[i].secs -= cpu0[k].secs;
        }
        char line[1024];
        int len = 0;
        double all = 0;
        for (int i = 0; i < ncpu1; i++) all += d[i].secs;
        for (int shown = 0; shown < 10; shown++) {
            int best = -1;
            for (int i = 0; i < ncpu1; i++)
                if (d[i].secs > 0.005 && (best < 0 || d[i].secs > d[best].secs)) best = i;
            if (best < 0) break;
            len += snprintf(line + len, len < (int)sizeof line ? sizeof line - (size_t)len : 0, "%s %s %.2f", shown ? "," : "",
                            d[best].name, d[best].secs);
            d[best].secs = 0;
        }
        hos_log("bench: CPU %.2f s over the frames, by thread group:%s", all, len ? line : " none");
    }
}

/* At each present (nn_vi.c hos_vi_native_presented, the Presentation Thread), while on */
void hos_bench_present(int64_t now) {
    static enum { WAIT, RUN, DONE } state;
    static BenchReady ready;
    static int64_t last_now, last_draws, last_waiting, last_reads, last_lists, last_calls, frames_seen;
    static double last_clock, ready_at;
    static BenchRow *rows;
    static int nrows, ncpu0;
    static int64_t blocked[4];
    static long long play_since = -1;   /* (the present the current stretch in play began at) */   /* (the frames each part was not settled while waiting: what the wait was for) */
    static CpuGroup cpu0[CPU_GROUPS];
    if (state == DONE) return;
    int64_t draws, waiting, jobs;
    rx_stats(&draws, &waiting, &jobs);
    nvn_list_stats ls;
    nvn_list_get_stats(&ls);
    const int64_t reads = __atomic_load_n(&fs_reads, __ATOMIC_RELAXED);
    const double clock = game_clock();
    frames_seen++;
    if (state == WAIT) {
        const long long present = hos_vi_present_number();
        const uint64_t mgr = G64(0x84ca9bf0);   /* (agl::TextureDataMgr; +0x338 its Replace thread's "work pending") */
        unsigned settled = 0;
        if (present - __atomic_load_n(&play_present, __ATOMIC_RELAXED) <= 3) settled |= BENCH_PLAY;
        if (mgr && !*(const uint8_t *)GP(mgr + 0x338)) settled |= BENCH_TEXTURES;
        if (!jobs && waiting == last_waiting) settled |= BENCH_SHADERS;
        if (reads == last_reads) settled |= BENCH_STREAMING;
        for (int i = 0; i < 4; i++) blocked[i] += !(settled & (1u << i));
        if (!(settled & BENCH_PLAY)) play_since = -1;
        else if (play_since < 0) play_since = present;
        if (bench_ready_frame(&ready, settled, ready_need)) {
            ready_at = hos_log_seconds();
            hos_log("bench: ready at %.2f s since the start (present %lld, in play since present %lld; %d frames settled in a row), "
                    "%d frames to run; frames not settled before: in play %lld, textures %lld, shaders %lld, streaming %lld", ready_at,
                    present, play_since, ready_need, frames_wanted, (long long)blocked[0], (long long)blocked[1], (long long)blocked[2],
                    (long long)blocked[3]);
            rows = calloc((size_t)frames_wanted, sizeof *rows);
            if (!rows) { hos_log("bench: no memory for the rows"); state = DONE; return; }
            ncpu0 = cpu_sample(cpu0);
            state = RUN;
        } else if (frames_seen % 600 == 0) {
            hos_log("bench: not ready at present %lld: in play %d, texture work %s, shader jobs %lld, draws waiting for a shader %lld, "
                    "file reads %lld", present, !!(settled & BENCH_PLAY), mgr ? (settled & BENCH_TEXTURES ? "none" : "pending") : "(no manager)",
                    (long long)jobs, (long long)(waiting - last_waiting), (long long)(reads - last_reads));
        }
    } else {
        BenchRow *r = &rows[nrows++];
        r->wall_ms = (now - last_now) / 1e6;
        r->game_step = clock - last_clock;
        r->draws = draws - last_draws, r->waiting = waiting - last_waiting, r->shader_jobs = jobs, r->fs_reads = reads - last_reads;
        r->lists = (int64_t)ls.made - last_lists, r->list_calls = (int64_t)ls.calls - last_calls;
        if (nrows == frames_wanted) {
            state = DONE;
            bench_finish(rows, nrows, ready_at, cpu0, ncpu0);
            hos_log_flush();
            ExitProcess(0);
        }
    }
    last_now = now, last_draws = draws, last_waiting = waiting, last_reads = reads, last_lists = (int64_t)ls.made;
    last_calls = (int64_t)ls.calls, last_clock = clock;
}
