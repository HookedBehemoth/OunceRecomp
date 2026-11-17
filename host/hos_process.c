/* The process around the guest (was mkw_glue.c): the 64-bit guest window's commit (a range check: the window is mapped
   whole), crash reports with the guest's state, the guest backtrace, HOS_STATS_AFTER, the Switch 2's counter
   (CNTPCT_EL0), and stand-ins for the hooks of TOTKPort files not taken into host/ (its profiler's memory checkpoints,
   its emulated GPU's submissions, a frame-step input filter). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include <execinfo.h>

/* ---- the guest window ---------------------------------------------------------------------------------------
   The window is mapped whole, readable and writable (hos_main.c), its pages given on first touch: what the runtime
   hands out (image, system region, heap, address regions: hos_commit) needs nothing more than to be the guest's. */
void hos_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    if (!hos_guest_range(addr, size))   /* (hookshot: guest pages only) */
        hos_log("mem: commit of %#llx bytes at %#llx: outside the guest window", (unsigned long long)size, (unsigned long long)addr);
}

/* A crash nothing handles: what, where (hookshot+offset: nx2/samap.py names the lifted function), and the guest
   state of the thread, as of its last call out of lifted code, with its frame chain from x29. */
static uint64_t peek_guest(uint64_t a) {
    if (a >= HOS_GUEST_SPACE - 8 || !hos_guest_range(a, 8)) return 0;
    return G64(a);
}

static int report_crash(const os_fault *f) {
    static const char *const kinds[] = {"?", "access violation", "illegal instruction", "division by zero", "trap"};
    char name[OS_PATH_MAX] = "?";
    const uintptr_t mod = os_module_of((const void *)f->pc, name, sizeof name);
    if (!mod) snprintf(name, sizeof name, "?");   /* (the offset is then the address itself) */
    const char *base = strrchr(name, '/');
    const uint64_t addr = f->addr;
    hos_log("CRASH: %s (%s %#llx, guest %#llx) at %s+%#llx", kinds[f->kind >= 1 && f->kind <= 4 ? f->kind : 0],
            f->write ? "write" : "read", (unsigned long long)addr, (unsigned long long)(addr - (uint64_t)A64_MEM),
            base ? base + 1 : name, (unsigned long long)(f->pc - mod));
    {   /* (hookshot) the host stack as module+offset, from glibc (the frames above the signal's included) */
        void *pcs[24];
        const int got = backtrace(pcs, 24);
        char hb[1024];
        int n = snprintf(hb, sizeof hb, "CRASH: host backtrace:");
        for (int i = 0; i < got && n < (int)sizeof hb - 48; i++) {
            char mn[OS_PATH_MAX] = "?";
            const uintptr_t m = os_module_of(pcs[i], mn, sizeof mn);
            const uint64_t off = (uint64_t)(uintptr_t)pcs[i] - (uint64_t)m;
            if (!m) snprintf(mn, sizeof mn, "?");
            const char *mb = strrchr(mn, '/');
            n += snprintf(hb + n, sizeof hb - n, " %s+%#llx", mb ? mb + 1 : mn, (unsigned long long)off);
        }
        hos_log("%s", hb);
    }
    Ctx *c = hos_ctx();
    if (c) {
        hos_log("CRASH: thread %s, sp %#llx", hos_thread_name(hos_self()), (unsigned long long)c->x[31]);
        for (int i = 0; i < 31; i += 4)
            hos_log("CRASH:   x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx", i,
                    (unsigned long long)c->x[i], i + 1, (unsigned long long)c->x[i + 1], i + 2,
                    (unsigned long long)c->x[i + 2], i + 3, i + 3 < 31 ? (unsigned long long)c->x[i + 3] : 0ULL);
        for (int i = 0; i < 31; i++)   /* (a code pointer is a host function pointer: its guest function) */
            if (c->x[i] >> 32 && a64_guest_of(c->x[i]) != c->x[i])
                hos_log("CRASH:   x%d is the code pointer of f_%08llx", i, (unsigned long long)a64_guest_of(c->x[i]));
        char bt[1024];
        int n = snprintf(bt, sizeof bt, "CRASH: guest backtrace: lr %#llx", (unsigned long long)c->x[30]);
        uint64_t fp = c->x[29];
        for (int i = 0; i < 40 && fp && !(fp & 15) && n < (int)sizeof bt - 24; i++) {
            const uint64_t lr = peek_guest(fp + 8), next = peek_guest(fp);
            n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)lr);
            if (next <= fp) break;
            fp = next;
        }
        hos_log("%s", bt);
        /* the memory x0, x1 and x19 point to (the object a crash is usually about) */
        static const int dumped[] = {0, 1, 19};
        for (int k = 0; k < 3; k++) {
            const int r = dumped[k];
            const uint64_t base = c->x[r];
            if (base < 0x100000 || !peek_guest(base) && !peek_guest(base + 8) && !peek_guest(base + 0x100)) continue;
            for (int row = 0; row < 0x120; row += 32)
                hos_log("CRASH: [x%d+%#05x] %016llx %016llx %016llx %016llx", r, row,
                        (unsigned long long)peek_guest(base + row), (unsigned long long)peek_guest(base + row + 8),
                        (unsigned long long)peek_guest(base + row + 16), (unsigned long long)peek_guest(base + row + 24));
        }
    }
    hos_log_flush();
    void hos_hostcall_stats(void);
    hos_hostcall_stats();   /* (HOS_HOSTCALL_STATS=1: the host calls made, and the last 256 of them) */
    return OS_FAULT_PASS;
}

/* (hos_prof.c's, for hos_fatal) the current thread's guest frame chain from x29 */
void hos_backtrace(void) {
    Ctx *c = hos_ctx();
    if (!c) return;
    char bt[1024];
    int n = snprintf(bt, sizeof bt, "guest backtrace: lr %#llx", (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 40 && fp && !(fp & 15) && n < (int)sizeof bt - 24; i++) {
        const uint64_t lr = peek_guest(fp + 8), next = peek_guest(fp);
        n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    fprintf(stderr, "[hos] %s\n", bt);
    fflush(stderr);
}

/* HOS_STATS_AFTER=<seconds> (with HOS_HOSTCALL_STATS=1): the host call counts and the last calls at that time, for
   runs that are stopped rather than exit (what a quiet game is doing) */
static void stats_after(void *p) {
    os_sleep_ns((int64_t)((uint32_t)(uintptr_t)p * 1000) * 1000000);
    void hos_hostcall_stats(void);
    hos_log("stats: after %u s", (unsigned)(uintptr_t)p);
    void hos_thread_dump(void);
    hos_thread_dump();
    hos_log_flush();
    hos_hostcall_stats();
}

void hos_commit_init(void) {
    os_fault_set_last(report_crash);
    const char *after = getenv("HOS_STATS_AFTER");
    if (after && atoi(after) > 0) os_thread_release(os_thread_start(stats_after, (void *)(uintptr_t)atoi(after), 0, "stats after"));
}

/* ---- stand-ins --------------------------------------------------------------------------------------------- */
/* HOS_PROFILE=<seconds> (in place of TOTKPort's hos_prof.c): every interval, the process's busiest threads and their
   share of a core (guest threads by their guest names, host threads by their descriptions): which one a frame rate
   waits for */
void hos_mem_checkpoint(const char *what) { (void)what; }        /* (hos_prof.c: HOS_MEM_TIMELINE) */
uint64_t hos_frame_step_buttons(uint64_t buttons) { return buttons; }   /* (nn_vi.c) */
/* (hookshot) hos_sdl_pads, hos_sdl_rumble: host/pad_sdl.c (from TOTKPort) */

/* TOTK's emulated GPU (gpu.c, its Maxwell command processor), which nvdrv.c and the debug commands call into: MKW
   has none (its NVN2 calls go to nvn_native.c), so a submission is only counted. */
int hos_gpu_async(void) { return 0; }
void hos_gpu_submit(int channel_fd, const uint64_t *entries, uint32_t n) {
    static volatile int32_t seen;
    if (os_atomic_inc(&seen) <= 4) hos_log("gpu: submit of %u entries on channel %d (no GPU yet)", n, channel_fd);
    (void)entries;
}
void hos_gpu_queue_submit(int channel_fd, const uint64_t *entries, uint32_t n, int incr_id) {
    (void)incr_id;
    hos_gpu_submit(channel_fd, entries, n);
}

/* CNTPCT_EL0 at the Switch 2's 31.25 MHz (rt/a64_rt.c's default counts the Switch 1's 19.2 MHz). The SDK checks
   nn::os::GetSystemTickFrequency against 31250000 at init and aborts otherwise. */
uint64_t a64_cntpct_host(void) {
    static int64_t base;
    static uint64_t mult;
    if (!__atomic_load_n(&mult, __ATOMIC_ACQUIRE)) {
        base = os_now_ns();
        __atomic_store_n(&mult, (HOS_TICK_HZ << 32) / 1000000000ULL, __ATOMIC_RELEASE);
    }
    return (uint64_t)(((unsigned __int128)(uint64_t)(os_now_ns() - base) * mult) >> 32);
}

void hos_host_thread_register_self(const char *name) {   /* (hos_prof.c: names for its stats; here the thread's name) */
    os_thread_set_name(os_thread_current(), name);
}
