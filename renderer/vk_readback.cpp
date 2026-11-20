// GPU writes copied into guest memory once their submission completes (vk_readback.h).
#include "vk_readback.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>

#include "os.h"

namespace hsd {
namespace {

struct Pending {
    uint64_t value;
    std::vector<ReadbackItem> items;
};
std::mutex mu;   // (the queue; held while copying, so a drain that returns has copied everything done before it)
std::condition_variable cv;
std::deque<Pending> queue;
int64_t st_items = 0, st_bytes = 0, st_subs = 0;

void drain_locked() {
    if (queue.empty()) return;
    const uint64_t done = sync_completed();
    Mem* last = nullptr;
    while (!queue.empty() && queue.front().value <= done) {
        Pending& p = queue.front();
        for (const ReadbackItem& it : p.items) {
            if (it.mem != last) mem_invalidate(it.mem), last = it.mem;
            host->gpu_write(it.guest, it.src, it.size);
            st_bytes += it.size;
        }
        st_items += (int64_t)p.items.size(), st_subs++;
        queue.pop_front();
    }
}

void thread_main() {
    for (;;) {
        uint64_t v;
        {
            std::unique_lock lk(mu);
            cv.wait(lk, [] { return !queue.empty(); });
            v = queue.front().value;
        }
        if (sync_wait_value(v, 100'000'000)) {
            std::lock_guard lk(mu);
            drain_locked();
        }
    }
}

}  // namespace

void readback_init() { os_thread_release(os_thread_start([](void*) { thread_main(); }, nullptr, 0, "direct readback")); }

void readback_add(uint64_t value, std::vector<ReadbackItem>&& items) {
    {
        std::lock_guard lk(mu);
        queue.push_back({value, std::move(items)});
    }
    cv.notify_one();
}

void readback_drain() {
    std::lock_guard lk(mu);
    drain_locked();
}

void readback_stats(char* out, size_t cap, double frames) {
    std::lock_guard lk(mu);
    std::snprintf(out, cap, "readback a frame: %.1f submissions, %.1f items, %.0f bytes copied into guest memory; %zu submissions waiting",
                  st_subs / frames, st_items / frames, st_bytes / frames, queue.size());
    st_items = st_bytes = st_subs = 0;
}

}  // namespace hsd
