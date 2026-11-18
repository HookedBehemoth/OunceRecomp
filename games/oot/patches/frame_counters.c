/* The game's counters that add the frame step, or the step times a whole number, cut to whole frames (frame_whole.h):
   the lifter's Value probes at their conversions. */
#include "hos.h"
#include "os.h"
#include "frame_whole.h"

extern volatile uint32_t hos_frame_seconds_bits;   /* (runtime/nn_vi.c: the frame's seconds, 0 the game's own step) */

/* (the lifter's Value probes: PATCH_AT ... instead, runtime/patch.h) the whole frames a game counter at `at` takes for an amount of the frame
   step: with the frames' own time as the step, their fractions carried per counter; else cut as the game cuts it */
static FrameWhole frame_whole_state;
static os_mutex frame_whole_lock = OS_MUTEX_INIT;   /* (the game's update threads) */
PATCH_AT(0x80a88300, word(0x1e38000c), instead, w12, hos_frame_whole, as(add(x19, 864), at), as(s0, amount)) {   /* fcvtzs w12, s0 */
    os_mutex_lock(&frame_whole_lock);
    const uint32_t whole = frame_whole(&frame_whole_state, at, amount, __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0);
    os_mutex_unlock(&frame_whole_lock);
    {   /* HOS_DBG=whole (diagnostic): the first calls, and a count every 600 (which counters, what they took) */
        static int on = -1;
        static volatile long calls;
        if (on < 0) on = hos_dbg_item("whole");
        if (on) {
            const long n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
            if (n <= 8 || n % 600 == 0)
                hos_log("whole: call %ld counter %#llx amount %.4f took %u", n, (unsigned long long)at, (double)amount, whole);
        }
    }
    return whole;
}
PATCH_AT_SITE(0x800675f8, word(0x1e380009), instead, w9, hos_frame_whole, x21, s0);   /* fcvtzs w9, s0 */
