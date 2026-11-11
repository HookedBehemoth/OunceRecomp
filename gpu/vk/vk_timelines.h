// (native-vk, plan step 6) The NVN syncs as timelines: the parts with no Vulkan in them, used by vk_replay.cpp and tested
// by gpu/vk/tests/timelines_test.cpp. RX_VK_TIMELINES=1 (off by default until it is proven in the game):
//
// - every NVN queue gets a timeline semaphore of its own; a recording signals, for each queue whose submissions it
//   finished, that queue's timeline with the newest of their values (QueueSignals). Values are the replay's submission
//   values (q_values), which grow along each queue: a queue's signals are strictly increasing (Monotone);
// - a CPU wait for a fence (nvnSyncWait) is vkWaitSemaphores on its queue's timeline. When the fence's work is not
//   recorded yet (its queue stopped at an event wait the replay holds on the host), the wait waits for a signal not yet
//   submitted, which a timeline allows on the host; the replay sends it once another thread's work lets that queue on.
//   No spinning and no forcing the stopped work past its wait, except the guard (WaitPlan): after RX_VK_HOLD_MS the
//   stopped work goes on as before, counted, so a wait the replay cannot meet does not hang the game;
// - a GPU wait between queues (nvnQueueWaitSync) is a record at the start of the waiting queue's next submission
//   (NOP_WAIT_FENCE): with the fence's work recorded it orders nothing (one Vulkan queue, in replay order); without, it
//   stops that queue on the host as an event wait does (fence_recorded). Never a GPU-side wait: on ONE Vulkan queue a
//   submission waiting for a signal submitted after it holds everything behind it, the signal too (a deadlock);
// - events stay host-held in replay order (vk_replay.cpp qruns), as before.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace mkwvk::tl {

/// The queues' newest values finished in one recording (or one batch of them): one signal per queue
struct QueueSignals {
    std::vector<std::pair<uint64_t, uint64_t>> v;   // (queue, value)
    void add(uint64_t queue, uint64_t value) {
        if (!queue || !value) return;   // (queue-less submits have no timeline of their own)
        for (auto& e : v)
            if (e.first == queue) {
                e.second = std::max(e.second, value);
                return;
            }
        v.emplace_back(queue, value);
    }
    void merge(const QueueSignals& o) {
        for (const auto& e : o.v) add(e.first, e.second);
    }
    bool empty() const { return v.empty(); }
    void clear() { v.clear(); }
};

/// A timeline's signals must grow strictly: the last one sent, and whether the next may go
struct Monotone {
    uint64_t last = 0;
    bool take(uint64_t value) {
        if (value <= last) return false;
        last = value;
        return true;
    }
};

/// Is the work up to fence f of a queue in the replay's command stream (recorded, or being recorded now)? known: the
/// fence's queue is known; blocked / oldest: that queue's stopped work (a submission held at a wait, and those after it)
/// and the oldest value of it. Not recorded = a wait for it now would be a wait before its signal
inline bool fence_recorded(bool known, bool blocked, uint64_t oldest, uint64_t f) {
    return !known || !blocked || oldest > f;
}

/// A CPU wait for a timeline value, in slices: what to do next
enum class Step { Done, Wait, Force, TimedOut };
struct WaitPlan {
    uint64_t timeout_ns = 0;   // (the game's)
    uint64_t guard_ns = 0;     // (the stopped work goes on after this, as before: RX_VK_HOLD_MS)
    uint64_t slice_max_ns = 2000000;          // (not recorded: 2 ms, the held work may go on any moment)
    uint64_t recorded_slice_ns = 100000000;   // (recorded: 100 ms)
    bool forced = false;
    /// signalled: the value is reached; recorded: its signal is submitted (or will be at the next flush)
    Step next(bool signalled, bool recorded, uint64_t elapsed_ns) {
        if (signalled) return Step::Done;
        if (elapsed_ns >= timeout_ns) return Step::TimedOut;
        if (!recorded && !forced && elapsed_ns >= guard_ns) {
            forced = true;
            return Step::Force;
        }
        return Step::Wait;
    }
    /// how long the next vkWaitSemaphores may block
    uint64_t slice(bool recorded, uint64_t elapsed_ns) const {
        const uint64_t left = timeout_ns > elapsed_ns ? timeout_ns - elapsed_ns : 0;
        // (its signal is on the GPU's way: long slices, still bounded so a signal never sent shows as a loop, not a hang)
        return std::min(left, recorded ? recorded_slice_ns : slice_max_ns);
    }
};

/// The census: every wait by kind (RX_VK_TIMELINES=1 or RX_VK_SYNC_CENSUS=1: logged every 600 presents)
enum Kind {
    CPU_DONE,            // nvnSyncWait: reached at the call
    CPU_WAITED,          //   waited, its signal recorded
    CPU_BEFORE_SIGNAL,   //   waited before its signal was recorded (its queue held at a wait): the host-side hold
    CPU_TIMED_OUT,       //   a poll that timed out
    CPU_GUARD,           //   the guard made the stopped work go on
    CPU_OTHER,           //   the old path (a fence of no known queue)
    RB_NONE,             // readback (invalidate): its bytes' write-back done or never made
    RB_WAITED,           //   waited for the recording that writes it back
    RB_BEFORE_RECORD,    //   waited for that recording to be made first
    RB_OTHER,            //   the old path
    EV_MET,              // event wait (CommandBufferWaitEvent) met in replay order: orders nothing on one queue
    EV_HELD,             //   not met: its queue held on the host (wait-before-signal)
    EV_FORCED,           //   gone past
    FW_MET,              // fence wait (QueueWaitSync): the fence's work recorded: orders nothing
    FW_HELD,             //   not recorded: its queue held on the host
    FW_FORCED,           //   gone past
    DR_PRESENT,          // stopped work made to go on: by a present
    DR_RELEASE,          //   a release
    DR_WAIT,             //   a CPU wait
    DR_TIMER,            //   RX_VK_HOLD_MS
    K_COUNT
};
inline const char* kind_name(int k) {
    static const char* const n[K_COUNT] = {
        "cpu reached", "cpu waited", "cpu before-signal", "cpu polls timed out", "cpu guard", "cpu other",
        "rb none", "rb waited", "rb before-record", "rb other",
        "event met", "event held", "event forced",
        "fence met", "fence held", "fence forced",
        "drain present", "drain release", "drain wait", "drain timer"};
    return k >= 0 && k < K_COUNT ? n[k] : "?";
}
struct Census {
    std::atomic<int64_t> n[K_COUNT]{};
    std::atomic<int64_t> hold_max_us{0};   // (the longest a queue stayed held, since the last line)
    void add(Kind k, int64_t d = 1) { n[k].fetch_add(d, std::memory_order_relaxed); }
    void hold(int64_t us) {
        int64_t m = hold_max_us.load(std::memory_order_relaxed);
        while (us > m && !hold_max_us.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
    }
    /// "name count, ..." of the counts since the last call (they restart at 0)
    int take(char* out, size_t cap) {
        int k = 0;
        out[0] = 0;
        for (int i = 0; i < K_COUNT; i++) {
            const int64_t v = n[i].exchange(0, std::memory_order_relaxed);
            k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "%s%s %lld", i ? ", " : "", kind_name(i), (long long)v);
            if (k >= (int)cap) return (int)cap - 1;
        }
        k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; longest hold %.2f ms",
                           hold_max_us.exchange(0, std::memory_order_relaxed) / 1000.0);
        return std::min(k, (int)cap - 1);
    }
};

}  // namespace mkwvk::tl
