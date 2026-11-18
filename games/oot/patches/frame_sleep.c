/* The game's frame loop and the clock: its present's event poll and its calc jobs' waits slept the host's way, and its
   frame-end clock read, which names the graphics module (runtime/hos_thread.c's sleeps and ticks). */
#include "hos.h"
#include "direct_crit.h"
#include "os.h"
#include <stdlib.h>
#include <string.h>
#include <emmintrin.h>

void hos_sleep_logged(int64_t ns, uint64_t lr);   /* (hos_thread.c) */
uint64_t hos_tick_read(uint64_t lr);             /* (hos_thread.c) */
extern volatile uint64_t hos_gfx_module;         /* (hos_thread.c) */

/* The present waits for its frame's GPU work by polling an NVN event, sleeping ~1.6 ms between looks
   (f_805902c0 frame_present_submit at 0x80590500: `while (nvnEventGetValue(e) != 1) SleepThread(...)`, ~3.5 sleeps a frame, 2 ms each on Windows): the
   frame noticed the GPU's signal late, ~7 ms of the 25 ms frame asleep on its critical path. The GPU writes the event
   into guest memory itself, so the sleep looks at it between yields and ends once it is
   1, at the latest when the asked time is up. Only where the poll is nvnEventGetValue. HOS_PRESENT_POLL=1 (an
   experiment: no fps change measured, and it keeps a core busy while it looks) */
static int present_poll(int64_t ns, uint64_t x22, uint64_t x23, uint64_t x24) {
    static int on = -1;
    static uint64_t fn_ok, fn_bad;
    if (on < 0) on = getenv("HOS_PRESENT_POLL") && atoi(getenv("HOS_PRESENT_POLL")) != 0;
    if (!on || ns <= 0 || ns > 20000000) return 0;
    const uint64_t fp = G64(x24);
    if (fp != fn_ok) {
        if (fp == fn_bad) return 0;
        const char *hos_hostcall_name(uint64_t addr);
        const char *n = hos_hostcall_name(fp);
        if (!n || strcmp(n, "nvnEventGetValue")) { fn_bad = fp; return 0; }
        fn_ok = fp;
        hos_log("sleep: the present's poll looks at its event between yields (HOS_PRESENT_POLL)");
    }
    int hos_nvn_event_value(uint64_t guest_event, uint32_t *v);
    const uint64_t ev = x23 + (uint64_t)G32(x22 + 0x164) * 64;
    uint32_t v = 0;
    if (!hos_nvn_event_value(ev, &v)) return 0;
    const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)8);
    const int64_t end = hos_now_ns() + ns;
    while (v != 1 && hos_now_ns() < end) {
        os_yield();
        for (int i = 0; i < 64; i++) _mm_pause();
        hos_nvn_event_value(ev, &v);
    }
    if (away) hos_block_end();
    return 1;
}
/* (an At row in place of f_805902c0's SleepThread call at 0x8059051c, the present's event poll: x0 the time, x22-x24
   the poll loop's registers: x24 the address of the poll function, x23 + 64 * u32 [x22 + 0x164] its event) */
PATCH_AT(0x8059051c, word(0x94db4a31), instead, none, hos_oot_present_sleep, as(x0, ns), x22, x23, x24) {
    if (present_poll(ns, x22, x23, x24)) return;
    static int sleep_log = -1;
    if (sleep_log < 0) sleep_log = getenv("HOS_SLEEP_LOG") != NULL;
    if (sleep_log) {   /* (diagnostic: the present's poll) */
        static int logged;
        if (logged++ < 3) {
            const char *hos_hostcall_name(uint64_t addr);
            const uint64_t fp = G64(x24), sync = x23 + (uint64_t)G32(x22 + 0x164) * 64;
            const char *n = hos_hostcall_name(fp);
            hos_log("sleep: the present's poll: fn %#llx (%s), arg %#llx, sleep %lld ns", (unsigned long long)a64_guest_of(fp),
                    n ? n : "guest code", (unsigned long long)sync, (long long)ns);
        }
    }
    hos_sleep_logged(ns, 0x80590520);
}
/* The calc jobs' runner f_800998a0 (label job_calc_runner: main and ModuleSystemWorker1-5 in calc stage C,
   docs/direct/frame-loop.md) waits for its sub-stages by SleepThread(20 us) and looking again, at three
   sites, ~30 times a frame on each of its six threads: the five others wait while one thread builds the occlusion grid
   (~0.5 ms) and while the last shape items finish. A sleep under a millisecond was yield-spun to its time (hos_sleep_ns), so
   the six threads stayed on the CPU for the whole phase: 4 of its 10 ms of CPU a frame (market, uncapped). Here it is
   a timed sleep with a 1 us timer slack (the thread's slack is set once; Linux's default 50 us would stretch each
   look): the phase keeps its wall time and uses ~3.9 ms less CPU a frame. Linux only
   (Windows' timers are ~0.5 ms coarse). HOS_CALC_NAP=0: spun as before. The three SleepThread calls (bl at 0x8009a474,
   0x8009afec, 0x8009b230) are replaced by At rows (PATCH_AT ... instead, below) calling hos_oot_calc_sleep: SleepThread
   itself is the same for every caller */
static int calc_poll_nap(int64_t ns) {
    static int on = -1;
    if (on < 0) on = !(getenv("HOS_CALC_NAP") && atoi(getenv("HOS_CALC_NAP")) == 0);
    if (!on || ns <= 0 || ns >= 1000000) return 0;
    const int away = hos_block_begin(CRIT_BLOCK_SLEEP, (const void *)(uintptr_t)ns);
    os_sleep_ns(ns);   /* (os.h: the thread's timer slack is 1 ns) */
    if (away) hos_block_end();
    return 1;
}

/* (At rows in place of f_800998a0 job_calc_runner's three SleepThread calls: x0 the time; lr the call's return address,
   for HOS_SLEEP_LOG) */
PATCH_AT(0x8009a474, word(0x94ef225b), instead, none, hos_oot_calc_sleep, as(x0, ns), as(k(0x8009a478), lr)) {
    if (!calc_poll_nap(ns)) hos_sleep_logged(ns, lr);
}
PATCH_AT_SITE(0x8009afec, word(0x94ef1f7d), instead, none, hos_oot_calc_sleep, x0, k(0x8009aff0));
PATCH_AT_SITE(0x8009b230, word(0x94ef1eec), instead, none, hos_oot_calc_sleep, x0, k(0x8009b234));

/* (an At row in place of f_8348c680 frame_body's GetSystemTick call at 0x8348c76c, its frame-end clock read: x19 the
   graphics module, for nn_vi.c's real-time step) the tick, as GetSystemTick */
PATCH_AT(0x8348c76c, word(0x941f5619), instead, x0, hos_oot_frame_end_tick, as(x19, module)) {
    hos_gfx_module = module;
    return hos_tick_read(0x8348c770);
}
