// Statistics counters the recording threads bump per op or per draw: each thread adds to its own cache-line-aligned block,
// so no line is written by two threads and no add is a locked instruction; the log's reader sums the blocks.
#pragma once

#include <atomic>
#include <cstdint>

namespace hsd {

/// N counters, a block per thread. Tag makes each set's thread-local block pointer its own (a type of the file that keeps the
/// set). take(k) is the sum added since the last take (one reader: the log's thread); total(k) the sum since the start
template <class Tag, int N>
class ThreadCounters {
public:
    void add(int k, int64_t n = 1) {
        std::atomic<int64_t>& c = mine().v[k];
        c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    int64_t take(int k) {
        int64_t n = 0;
        for (Block* b = all_.load(std::memory_order_acquire); b; b = b->next) {
            const int64_t v = b->v[k].load(std::memory_order_relaxed);
            n += v - b->seen[k], b->seen[k] = v;
        }
        return n;
    }
    int64_t total(int k) const {
        int64_t n = 0;
        for (const Block* b = all_.load(std::memory_order_acquire); b; b = b->next) n += b->v[k].load(std::memory_order_relaxed);
        return n;
    }

private:
    struct alignas(64) Block {
        std::atomic<int64_t> v[N]{};
        int64_t seen[N]{};   // (the reader's)
        Block* next = nullptr;
    };
    static inline thread_local Block* tl_ = nullptr;   // (a thread's block; threads live as long as the process, so do blocks)
    std::atomic<Block*> all_{nullptr};
    Block& mine() {
        if (__builtin_expect(tl_ != nullptr, 1)) return *tl_;
        Block* b = new Block();
        b->next = all_.load(std::memory_order_acquire);
        while (!all_.compare_exchange_weak(b->next, b, std::memory_order_release, std::memory_order_acquire)) {}
        return *(tl_ = b);
    }
};

}  // namespace hsd
