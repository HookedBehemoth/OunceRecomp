// Game-free check of the NVN syncs as timelines (gpu/vk/vk_timelines.h, RX_VK_TIMELINES=1), with a model of the one
// Vulkan queue the replay has: submissions run in the order they were submitted, and one whose wait is not met stops
// everything behind it (the queue's submit thread on RADV). The model shows the rule the replay keeps: a wait whose
// signal is not submitted yet must not become a GPU-side wait (it deadlocks), and is held on the host instead, in replay
// order, until the signalling work is recorded; the work then reaches the queue signal first, wait after.
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "../vk_timelines.h"

namespace tl = mkwvk::tl;

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    failures++;
}

// ---- the one Vulkan queue: in order, a submission whose waits are not met holds everything behind it ----
struct Sub {
    std::string name;
    std::vector<std::pair<int, uint64_t>> waits, signals;   // (timeline, value)
};
struct OneQueue {
    std::deque<Sub> q;
    std::map<int, uint64_t> tl;   // timeline values reached
    std::vector<std::string> ran;
    void submit(Sub s) { q.push_back(std::move(s)); }
    void host_signal(int t, uint64_t v) { tl[t] = std::max(tl[t], v); }
    /// runs what it can; true when nothing is left
    bool run() {
        while (!q.empty()) {
            const Sub& s = q.front();
            for (const auto& [t, v] : s.waits)
                if (tl[t] < v) return false;   // (stopped: everything behind it waits too)
            for (const auto& [t, v] : s.signals) tl[t] = std::max(tl[t], v);
            ran.push_back(s.name);
            q.pop_front();
        }
        return true;
    }
};

// 1. The deadlock: queue B's work waits on the GPU for queue A's fence, and A's signalling work reaches the one queue
//    after it (A's work was held at an event wait when B submitted)
void deadlock_on_one_queue() {
    OneQueue gpu;
    gpu.submit({"b1 (waits tl_A >= 1)", {{0, 1}}, {}});
    gpu.submit({"a1 (signals tl_A = 1)", {}, {{0, 1}}});
    expect(!gpu.run(), "a GPU-side wait before its signal on one queue must stall (the model)");
    expect(gpu.ran.empty() && gpu.q.size() == 2, "the signal behind the wait never runs");
}

// ---- the replay's host-side hold (vk_replay.cpp qruns, in miniature): a queue's submission stops at a wait not met and
// its later submits queue behind it; whenever work is recorded, the stopped ones whose waits are met go on ----
struct Wait {
    bool fence;          // NOP_WAIT_FENCE (else an event wait on a CPU-set flag)
    uint64_t value;      // fence value, or the flag value wanted
};
struct Submission {
    uint64_t queue, value;
    std::vector<Wait> waits;   // (at its start)
};
struct Replay {
    OneQueue gpu;
    std::map<uint64_t, uint64_t> value_queue;
    struct Run { bool blocked = false; std::deque<Submission> held; };
    std::map<uint64_t, Run> runs;
    uint64_t flag = 0;   // (the CPU's event)
    tl::QueueSignals open;
    std::map<uint64_t, tl::Monotone> sent;
    int holds = 0, met = 0;
    bool recorded(uint64_t f) const {
        auto it = value_queue.find(f);
        if (it == value_queue.end()) return true;
        auto r = runs.find(it->second);
        const bool blocked = r != runs.end() && r->second.blocked;
        return tl::fence_recorded(true, blocked, blocked ? r->second.held.front().value : 0, f);
    }
    bool waits_met(const Submission& s) const {
        for (const Wait& w : s.waits)
            if (w.fence ? !recorded(w.value) : flag < w.value) return false;
        return true;
    }
    void record(const Submission& s) {   // (into a recording, submitted at once: the queues' timelines with it)
        open.add(s.queue, s.value);
        Sub g{"q" + std::to_string(s.queue) + ":" + std::to_string(s.value), {}, {}};
        for (const auto& [queue, value] : open.v)
            if (sent[queue].take(value)) g.signals.emplace_back((int)queue, value);
        open.clear();
        gpu.submit(g);
    }
    void schedule() {
        for (bool moved = true; moved;) {
            moved = false;
            for (auto& [queue, r] : runs) {
                while (r.blocked && waits_met(r.held.front())) {
                    record(r.held.front());
                    r.held.pop_front();
                    r.blocked = !r.held.empty();
                    moved = true;
                }
            }
        }
    }
    void submit(Submission s) {
        value_queue[s.value] = s.queue;
        Run& r = runs[s.queue];
        if (r.blocked || !waits_met(s)) {
            if (!r.blocked) holds++;
            r.blocked = true;
            r.held.push_back(std::move(s));
            return;
        }
        met++;
        record(s);
        schedule();
    }
    void cpu_signal(uint64_t v) {
        flag = v;
        schedule();
    }
    std::vector<std::string> order() {
        gpu.run();
        return gpu.ran;
    }
};

// 2. The same with the hold: A's submission 1 waits for the CPU's flag; B waits for A's fence 1 (QueueWaitSync)
void hold_orders_it() {
    Replay r;
    r.submit({1, 1, {{false, 1}}});   // A: held at the CPU gate (as OoT's frame work is)
    r.submit({2, 2, {{true, 1}}});    // B: QueueWaitSync(A's fence 1): A's work not recorded -> held
    r.submit({2, 3, {}});             // B's next: behind its held one
    expect(r.order().empty(), "nothing reaches the queue while A waits for the CPU");
    expect(!r.recorded(1) && !r.recorded(2), "the fences of held work are not recorded");
    r.cpu_signal(1);                  // the CPU's signal: A goes on, then B
    const auto o = r.order();
    expect(o.size() == 3 && o[0] == "q1:1" && o[1] == "q2:2" && o[2] == "q2:3", "signal first, then the waits, in order");
    expect(r.gpu.q.empty(), "no deadlock");
    expect(r.gpu.tl[1] == 1 && r.gpu.tl[2] == 3, "each queue's timeline reaches its newest value");
    expect(r.holds == 2, "two holds (A at the gate, B at the fence)");
}

// 3. A wait whose signal is recorded already orders nothing (one queue, replay order): no hold
void recorded_wait_is_ordering() {
    Replay r;
    r.submit({1, 1, {}});
    r.submit({2, 2, {{true, 1}}});
    expect(r.holds == 0 && r.met == 2, "a fence already recorded: no hold");
    const auto o = r.order();
    expect(o.size() == 2 && o[0] == "q1:1", "its work is ahead in the queue");
}

// 4. A CPU wait (nvnSyncWait) before its signal is recorded: a timeline lets the host wait for a signal not submitted
//    yet; the guard lets the held work go after RX_VK_HOLD_MS, once
void cpu_wait_plan() {
    tl::WaitPlan p;
    p.timeout_ns = ~0ull, p.guard_ns = 50000000;
    expect(p.next(true, false, 0) == tl::Step::Done, "signalled: done");
    expect(p.next(false, false, 1000) == tl::Step::Wait, "not recorded, early: wait");
    expect(p.slice(false, 1000) == p.slice_max_ns, "not recorded: short slices");
    expect(p.slice(true, 1000) == p.recorded_slice_ns, "recorded: long but bounded slices");
    expect(p.next(false, false, 50000000) == tl::Step::Force, "the guard: once");
    expect(p.next(false, false, 60000000) == tl::Step::Wait, "and only once");
    tl::WaitPlan poll;
    poll.timeout_ns = 1, poll.guard_ns = 50000000;
    expect(poll.next(false, false, 0) == tl::Step::Wait && poll.slice(false, 0) == 1, "a poll waits its timeout");
    expect(poll.next(false, false, 1) == tl::Step::TimedOut, "then times out, never forcing");
    tl::WaitPlan rec;
    rec.timeout_ns = ~0ull, rec.guard_ns = 0;
    expect(rec.next(false, true, 5) == tl::Step::Wait, "recorded work is never forced");
}

// 5. Signals: one per queue per recording, the newest value, never one not above the last
void signals() {
    tl::QueueSignals s;
    s.add(7, 3), s.add(9, 4), s.add(7, 5), s.add(0, 8), s.add(9, 0);
    expect(s.v.size() == 2 && s.v[0].second == 5 && s.v[1].second == 4, "a burst: the newest value per queue, none for queue 0");
    tl::QueueSignals b;
    b.add(9, 6);
    b.merge(s);
    expect(b.v.size() == 2 && b.v[0].second == 6, "a batch merges by max");
    tl::Monotone m;
    expect(m.take(5) && !m.take(5) && !m.take(4) && m.take(6), "a timeline's signals grow strictly");
}

void census() {
    tl::Census c;
    c.add(tl::CPU_BEFORE_SIGNAL, 3), c.add(tl::EV_HELD), c.hold(1500);
    char line[1024];
    c.take(line, sizeof line);
    expect(std::strstr(line, "cpu before-signal 3") && std::strstr(line, "event held 1") && std::strstr(line, "longest hold 1.50 ms"),
           "the census line");
    c.take(line, sizeof line);
    expect(std::strstr(line, "cpu before-signal 0") != nullptr, "counts restart");
    char tiny[16];
    c.take(tiny, sizeof tiny);
    expect(std::strlen(tiny) < sizeof tiny, "a short buffer is not overrun");
}
}  // namespace

int main() {
    deadlock_on_one_queue();
    hold_orders_it();
    recorded_wait_is_ordering();
    cpu_wait_plan();
    signals();
    census();
    if (failures) return 1;
    std::printf("PASS: timelines (one-queue deadlock shown, host hold orders it, waits, signals, census)\n");
    return 0;
}
