/* The game's frame loop and the clock: its present's event poll and its calc jobs' waits slept the host's way, and its
   frame-end clock read, which names the graphics module (runtime/hos_thread.c's sleeps and ticks). */
#include "hos.h"
#include "crit.h"
#include "os.h"
#include <stdlib.h>
#include <string.h>

void hos_sleep_logged(int64_t ns, uint64_t lr);   /* (hos_thread.c) */
uint64_t hos_tick_read(uint64_t lr);             /* (hos_thread.c) */
extern volatile uint64_t hos_gfx_module;         /* (frame_rate.c) */

/* (an At row in place of f_805902c0 frame_present_submit's SleepThread call at 0x8059051c, the present's event poll: ~1.6 ms
   between looks; log.sleep counts it by caller. Polling the event between yields instead was measured: no fps change, and a
   core kept busy) */
PATCH_AT(0x8059051c, word(0x94db4a31), instead, none, hos_oot_present_sleep, as(x0, ns)) { hos_sleep_logged(ns, 0x80590520); }
/* The calc jobs' runner f_800998a0 (label job_calc_runner: main and ModuleSystemWorker1-5 in calc stage C,
   docs/direct/frame-loop.md) waits for its sub-stages by SleepThread(20 us) and looking again, at three
   sites, ~30 times a frame on each of its six threads: the five others wait while one thread builds the occlusion grid
   (~0.5 ms) and while the last shape items finish. A sleep under a millisecond was yield-spun to its time (hos_sleep_ns), so
   the six threads stayed on the CPU for the whole phase: 4 of its 10 ms of CPU a frame (market, uncapped). Here it is
   a timed sleep with a 1 us timer slack (the thread's slack is set once; Linux's default 50 us would stretch each
   look): the phase keeps its wall time and uses ~3.9 ms less CPU a frame. Linux only
   (Windows' timers are ~0.5 ms coarse). The three SleepThread calls (bl at 0x8009a474,
   0x8009afec, 0x8009b230) are replaced by At rows (PATCH_AT ... instead, below) calling hos_oot_calc_sleep: SleepThread
   itself is the same for every caller */
static int calc_poll_nap(int64_t ns) {
    if (ns <= 0 || ns >= 1000000) return 0;
    const int away = hos_block_begin(CRIT_BLOCK_SLEEP, (const void *)(uintptr_t)ns);
    os_sleep_ns(ns);   /* (os.h: the thread's timer slack is 1 ns) */
    if (away) hos_block_end();
    return 1;
}

/* (At rows in place of f_800998a0 job_calc_runner's three SleepThread calls: x0 the time; lr the call's return address,
   for log.sleep) */
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
