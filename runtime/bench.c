/* HOS_BENCH=<frames>[,<ready frames>]: a benchmark run, for numbers that compare between runs. The game steps exactly 1/60 s of
   game time a frame whatever the wall time (nn_vi.c realtime_step, rx_option 0 answered "per frame") and presents unpaced
   (hos_vi_native_acquire waits for nothing). Once the scene is ready, every part of it settled for <ready frames> presents in a
   row (60 by default), it runs <frames> more presents, writes bench.csv in the working folder (the run's), logs a summary
   and exits with code 0. The parts: in play (Link updated in the last presents, no map load, no event playing: games/oot/patches/oot_probes.c hos_oot_player_update),
   no texture work (agl::TextureDataMgr's "work pending", docs/direct/texture-streaming.md), no shader work (the back end's
   shader jobs queued or running, and no draw skipped for a shader not ready), no streaming (no file read by the game since
   the last present), the resource system quiet (resource_work). Unset: off, and nothing here runs but the checks of hos_bench_on. */
#include "hos.h"
#include "game_hooks.h"
#include "bench.h"
#include "nvn_list.h"
#include "os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rx_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines);   /* (rx_backend.c: the back end's) */
double hos_log_seconds(void);   /* (hos_thread.c: the log's seconds since the process started) */
#include "game_hooks.h"   /* (game_clock: the game's clock in 1/60 s) */

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

/* A message the game's worker threads took from their queues (hos_bench_message, from the game's patches): counted by
   thread group (the name without its trailing digits) while on */
#define MSG_GROUPS 64
static char msg_name[MSG_GROUPS][24];
static int64_t msg_count[MSG_GROUPS];
static int msg_groups;
static os_mutex msg_lock = OS_MUTEX_INIT;
static _Thread_local int msg_group = -1;
static int msg_group_of(const char *name) {
    char g[24];
    size_t m = strlen(name);
    if (m >= sizeof g) m = sizeof g - 1;
    memcpy(g, name, m);
    while (m && ((g[m - 1] >= '0' && g[m - 1] <= '9') || g[m - 1] == ' ' || g[m - 1] == '_')) m--;
    g[m] = 0;
    os_mutex_lock(&msg_lock);
    int i = 0;
    while (i < msg_groups && strcmp(msg_name[i], g)) i++;
    if (i == msg_groups && i < MSG_GROUPS) snprintf(msg_name[i], sizeof msg_name[i], "%s", g), msg_groups++;
    os_mutex_unlock(&msg_lock);
    return i;
}
void hos_bench_message(void) {
    if (!hos_bench_on()) return;
    if (msg_group < 0) {
        HosThread *t = hos_self();
        msg_group = msg_group_of(t ? hos_thread_name(t) : "host");
    }
    if (msg_group < MSG_GROUPS) __atomic_add_fetch(&msg_count[msg_group], 1, __ATOMIC_RELAXED);
}
/* The resource system's work in a frame: messages its loading threads took since the last call, past the one a frame that
   Resource Control and Resource Memory take in play. In play these are quiet but for BaseProcCreateAndDelete's few every 4 s
   (zoo_shadows); a map's load and what it requests after the player is in play (actors, models, effects: ARes Loading,
   GeneralFormatAsync, ParallelDecompThread, read from the archives in memory, so no file read shows it) take hundreds */
static int64_t resource_work(void) {
    static const char *const names[] = {"Resource Control", "Resource Memory", "ARes Loading", "GeneralFormatAsync", "ParallelDecompThread",
                                        "BaseProcCreateAndDelete"};
    enum { N = sizeof names / sizeof names[0] };
    static int64_t seen[N];
    int64_t work = 0;
    const int n = __atomic_load_n(&msg_groups, __ATOMIC_ACQUIRE);
    for (int k = 0; k < N; k++) {
        int64_t v = 0;
        for (int i = 0; i < n; i++)
            if (!strcmp(msg_name[i], names[k])) v = __atomic_load_n(&msg_count[i], __ATOMIC_RELAXED);
        const int64_t d = v - seen[k];
        seen[k] = v;
        work += k < 2 ? (d > 1 ? d - 1 : 0) : d;
    }
    return work;
}
/* (diagnostic) the messages each group took since the last call, the groups that took any */
static void msg_log(const char *when) {
    static int64_t seen[MSG_GROUPS];
    char line[1500];
    int len = 0;
    const int n = __atomic_load_n(&msg_groups, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n && len < (int)sizeof line - 64; i++) {
        const int64_t v = __atomic_load_n(&msg_count[i], __ATOMIC_RELAXED);
        if (v != seen[i]) len += snprintf(line + len, sizeof line - (size_t)len, " %s %lld,", msg_name[i], (long long)(v - seen[i]));
        seen[i] = v;
    }
    hos_log("bench: messages %s:%s", when, len ? line : " none");
}

typedef struct {
    double wall_ms, game_step;
    int64_t draws, waiting, shader_jobs, fs_reads, lists, list_calls;
} BenchRow;

/* ---- CPU time by thread group (the group is the thread's name without its trailing digits) ---- */
#define CPU_GROUPS 48
typedef struct { char name[20]; double secs; } CpuGroup;
typedef struct { CpuGroup *g; int n; } CpuGroups;
static void cpu_add(void *ctx, uint32_t tid, const char *thread, int64_t cpu_ns) {
    (void)tid;
    CpuGroups *c = (CpuGroups *)ctx;
    char name[20];
    snprintf(name, sizeof name, "%s", thread);
    size_t m = strlen(name);
    while (m && ((name[m - 1] >= '0' && name[m - 1] <= '9') || name[m - 1] == ' ' || name[m - 1] == '_' || name[m - 1] == '-')) m--;
    name[m] = 0;
    int i = 0;
    while (i < c->n && strcmp(c->g[i].name, name)) i++;
    if (i == c->n) {
        if (c->n == CPU_GROUPS) return;
        snprintf(c->g[c->n].name, sizeof c->g[c->n].name, "%s", name), c->g[c->n].secs = 0, c->n++;
    }
    c->g[i].secs += cpu_ns / 1e9;
}
static int cpu_sample(CpuGroup *g) {
    CpuGroups c = {g, 0};
    os_threads_cpu(cpu_add, &c);
    return c.n;
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
    int64_t draws = 0;
    for (int i = 0; i < n; i++)
        ms[i] = rows[i].wall_ms, ms[n + i] = rows[i].game_step, draws += rows[i].draws;
    const int64_t steps_off = bench_steps_off(ms + n, n);   /* (before ms + n becomes bench_summary's scratch) */
    const BenchSummary s = bench_summary(ms, n, ms + n);
    free(ms);
    hos_log("bench: %d frames in %.3f s (ready at %.2f s): frame mean %.3f ms, median %.3f, p99 %.3f, max %.3f; %.1f draws a "
            "frame; game steps not 1/60 s: %lld", n, s.total / 1e3, ready_at, s.mean, s.median, s.p99, s.max, (double)draws / n,
            (long long)steps_off);
    game_bench_report();
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
    static int64_t blocked[BENCH_PARTS];
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
    if (state == WAIT && frames_seen % 60 == 0) {
        char when[64];
        snprintf(when, sizeof when, "%s, 60 frames to present %lld", state == WAIT ? "waiting" : "measuring", hos_vi_present_number());
        msg_log(when);
    }
    if (state == WAIT) {
        const long long present = hos_vi_present_number();
        unsigned settled = 0;
        if (present - __atomic_load_n(&play_present, __ATOMIC_RELAXED) <= 3) settled |= BENCH_PLAY;
        if (game_textures_settled()) settled |= BENCH_TEXTURES;
        if (!jobs && waiting == last_waiting) settled |= BENCH_SHADERS;
        if (reads == last_reads) settled |= BENCH_STREAMING;
        const int64_t resources = resource_work();
        if (!resources) settled |= BENCH_RESOURCES;
        for (int i = 0; i < BENCH_PARTS; i++) blocked[i] += !(settled & (1u << i));
        if (!(settled & BENCH_PLAY)) play_since = -1;
        else if (play_since < 0) play_since = present;
        if (bench_ready_frame(&ready, settled, ready_need)) {
            ready_at = hos_log_seconds();
            hos_log("bench: ready at %.2f s since the start (present %lld, in play since present %lld; %d frames settled in a row), "
                    "%d frames to run; frames not settled before: in play %lld, textures %lld, shaders %lld, streaming %lld, resources %lld",
                    ready_at, present, play_since, ready_need, frames_wanted, (long long)blocked[0], (long long)blocked[1], (long long)blocked[2],
                    (long long)blocked[3], (long long)blocked[4]);
            rows = calloc((size_t)frames_wanted, sizeof *rows);
            if (!rows) { hos_log("bench: no memory for the rows"); state = DONE; return; }
            ncpu0 = cpu_sample(cpu0);
            state = RUN;
        } else if (frames_seen % 600 == 0) {
            hos_log("bench: not ready at present %lld: in play %d, texture work %s, shader jobs %lld, draws waiting for a shader %lld, "
                    "file reads %lld, resource messages %lld", present, !!(settled & BENCH_PLAY), settled & BENCH_TEXTURES ? "none" : "pending",
                    (long long)jobs, (long long)(waiting - last_waiting), (long long)(reads - last_reads), (long long)resources);
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
            void hos_code_ptr_report(void);
            hos_code_ptr_report();   /* code_ptrs.c */
            hos_log_flush();
            os_exit(0);
        }
    }
    last_now = now, last_draws = draws, last_waiting = waiting, last_reads = reads, last_lists = (int64_t)ls.made;
    last_calls = (int64_t)ls.calls, last_clock = clock;
}
