#pragma once
// Layer 0's window scheme arithmetic, kept apart from Vulkan so that it can be tested alone
// (the check renderer::layer0_windows). renderer/vk_pools.cpp is the rest.
//
// Guest memory is seen by the GPU through window buffers: 4 GB sparse buffers (maxBufferSize is 4 GB on RADV), one starting
// every 2 GB of guest address space, so window k covers guest [k * 2 GB, k * 2 GB + 4 GB) and every guest byte lies in two
// windows (k and k - 1), bound to the same memory in both (sparseResidencyAliased). A range of at most 2 GB starting at g is
// then whole in window g >> 31, at offset g & (2 GB - 1): a binding is (window[g >> 31], g & 0x7fffffff), with no lookup.
// Memory is bound in host chunks (a power of two from 2 MB to 64 MB, aligned, so a chunk never crosses a 2 GB line): a udmabuf
// of the guest memfd's own pages (offset = guest address), imported once and bound into both its windows. Nothing is copied:
// the bytes the game writes are the bytes the GPU reads (the host class: GPU reads cross PCIe). Larger chunks are
// fewer buffer objects, which RADV lists in every submission (~0.75 us each a submit).
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace mkwvk {
namespace l0 {

constexpr uint64_t kChunkMin = 2ull << 20, kChunkMax = 64ull << 20;   // (a host chunk: one udmabuf; its size_limit_mb is 64 by default)
constexpr uint64_t kStride = 2ull << 30;    // (a window starts every 2 GB)
// (a granule short of 4 GB, RADV's maxBufferSize being 4 GB - 4 (the validation layer's VUID-VkBufferCreateInfo-
// size-06409 on 4 GB windows): the last 64 KB of each 2 GB stretch is in its own window only, and the longest range
// one window holds whole wherever it starts is a granule short of 2 GB)
constexpr uint64_t kWindow = (4ull << 30) - (64ull << 10);
constexpr uint64_t kMaxRange = kStride - (64ull << 10);
constexpr uint32_t kWindows = 512;          // (the guest's 1 TB below the virtual pools: hos's window is 512 GB)

inline uint32_t window_of(uint64_t guest) { return (uint32_t)(guest >> 31); }
inline uint64_t window_offset(uint64_t guest) { return guest & (kStride - 1); }
inline uint64_t window_base(uint32_t k) { return (uint64_t)k * kStride; }
/// [guest, guest + size) is whole in window_of(guest): every binding up to 2 GB
inline bool fits_window(uint64_t guest, uint64_t size) {
    return size <= kMaxRange && window_of(guest) < kWindows && window_offset(guest) + size <= kWindow;
}
/// The chunk size for a wish in MB and udmabuf's limit in bytes: a power of two in [kChunkMin, min(kChunkMax, limit)]
inline uint64_t chunk_bytes(uint64_t want_mb, uint64_t limit) {
    uint64_t top = kChunkMax;
    while (top > kChunkMin && top > limit) top >>= 1;
    uint64_t c = kChunkMin;
    while (c < top && (c << 1) <= (want_mb << 20)) c <<= 1;
    return c;
}
/// The chunks [c0, c1) of `chunk` bytes that hold guest [lo, hi)
inline void chunk_span(uint64_t lo, uint64_t hi, uint64_t chunk, uint64_t& c0, uint64_t& c1) {
    c0 = lo / chunk;
    c1 = hi > lo ? (hi + chunk - 1) / chunk : c0;
}
/// The two (window, offset) places a chunk is bound at: window k at its offset there, and window k - 1 (if any) 2 GB further on
struct Place {
    uint32_t window;
    uint64_t offset;
};
inline int chunk_places(uint64_t c, uint64_t chunk, Place out[2]) {
    const uint64_t g = c * chunk;
    const uint32_t k = window_of(g);
    int n = 0;
    if (k < kWindows) out[n++] = {k, window_offset(g)};
    if (k > 0 && k - 1 < kWindows) out[n++] = {k - 1, window_offset(g) + kStride};
    return n;
}

/// The chunks bound, a bit each, read without a lock (a bit is set only after the chunk is bound and its windows made, with
/// release; read with acquire). Chunks are never unbound: the bit stays.
struct ChunkBits {
    uint64_t chunk = 0, chunks = 0;
    std::unique_ptr<std::atomic<uint64_t>[]> words;
    ChunkBits(uint64_t guest_bytes, uint64_t chunk_size)
        : chunk(chunk_size), chunks(guest_bytes / chunk_size), words(new std::atomic<uint64_t>[(guest_bytes / chunk_size + 63) / 64]) {
        for (uint64_t i = 0; i < (chunks + 63) / 64; i++) words[i].store(0, std::memory_order_relaxed);
    }
    bool has(uint64_t c) const {
        return c < chunks && (words[c / 64].load(std::memory_order_acquire) >> (c % 64) & 1);
    }
    void set(uint64_t c) {
        if (c < chunks) words[c / 64].fetch_or(1ull << (c % 64), std::memory_order_release);
    }
    void clear(uint64_t c) {
        if (c < chunks) words[c / 64].fetch_and(~(1ull << (c % 64)), std::memory_order_release);
    }
    /// bits [c0, c1) all set (a word at a time: a 2 GB range of 64 KB granules is 512 words)
    bool all_bits(uint64_t c0, uint64_t c1) const {
        if (c0 >= c1 || c1 > chunks) return false;
        while (c0 < c1) {
            const uint64_t w = c0 / 64, lo = c0 % 64, n = std::min<uint64_t>(64 - lo, c1 - c0);
            const uint64_t mask = (n == 64 ? ~0ull : ((1ull << n) - 1)) << lo;
            if ((words[w].load(std::memory_order_acquire) & mask) != mask) return false;
            c0 += n;
        }
        return true;
    }
    /// every chunk of guest [lo, hi) bound (false for an empty or out-of-range one)
    bool all(uint64_t lo, uint64_t hi) const {
        uint64_t c0, c1;
        chunk_span(lo, hi, chunk, c0, c1);
        return all_bits(c0, c1);
    }
    /// any chunk of guest [lo, hi) bound
    bool any(uint64_t lo, uint64_t hi) const {
        uint64_t c0, c1;
        chunk_span(lo, hi, chunk, c0, c1);
        for (uint64_t c = c0; c < c1 && c < chunks; c++)
            if (has(c)) return true;
        return false;
    }
};

/// What a first use binds: the chunks of the range, and of the whole pool holding it when the pool is at most `span` bytes
/// (a small pool's later bindings then find it bound); clipped to the guest window
inline void bind_span(uint64_t guest, uint64_t size, uint64_t pool_lo, uint64_t pool_hi, uint64_t span, uint64_t guest_bytes,
                      uint64_t& lo, uint64_t& hi) {
    lo = guest, hi = guest + size;
    if (pool_hi > pool_lo && pool_lo <= guest && hi <= pool_hi && pool_hi - pool_lo <= span) lo = pool_lo, hi = pool_hi;
    if (hi > guest_bytes) hi = guest_bytes;
    if (lo > hi) lo = hi;
}

// ---- the VRAM class ----
constexpr uint64_t kGran = 64ull << 10;   // (the sparse page: what a pool's VRAM is bound in)

/// The whole granules of [lo, hi) (edges shared with other data stay host): [g0, g1) in granule indices, empty when none
inline void interior_granules(uint64_t lo, uint64_t hi, uint64_t& g0, uint64_t& g1) {
    g0 = (lo + kGran - 1) / kGran;
    g1 = hi / kGran;
    if (g1 < g0) g1 = g0;
}

/// A guest range's pieces as bound: each piece within one 2 GB stretch, at its offset in window k and 2 GB further in k - 1
struct Piece {
    uint32_t window;
    uint64_t offset;      // (in the window)
    uint64_t from;        // (bytes from the range's start: the memory offset is the slot's plus this)
    uint64_t size;
};
inline void range_places(uint64_t lo, uint64_t hi, std::vector<Piece>& out) {
    for (uint64_t a = lo; a < hi;) {
        const uint64_t line = (a / kStride + 1) * kStride, b = std::min(hi, line);
        const uint32_t k = window_of(a);
        if (k < kWindows) out.push_back({k, window_offset(a), a - lo, b - a});
        const uint64_t prev = window_offset(a) + kStride;   // (window k - 1 holds it up to its end)
        if (k > 0 && k - 1 < kWindows && prev < kWindow) out.push_back({k - 1, prev, a - lo, std::min(b - a, kWindow - prev)});
        a = b;
    }
}

/// Free space of one VRAM block: first fit, freed ranges merged with their neighbours (offsets and sizes in granules' bytes)
struct SlotRanges {
    std::map<uint64_t, uint64_t> free;   // (offset -> length)
    SlotRanges() = default;
    explicit SlotRanges(uint64_t size) {
        if (size) free[0] = size;
    }
    bool take(uint64_t len, uint64_t& off) {
        for (auto it = free.begin(); it != free.end(); ++it)
            if (it->second >= len) {
                off = it->first;
                const uint64_t rest = it->second - len;
                free.erase(it);
                if (rest) free[off + len] = rest;
                return true;
            }
        return false;
    }
    void give(uint64_t off, uint64_t len) {
        auto next = free.lower_bound(off);
        if (next != free.end() && off + len == next->first) len += next->second, next = free.erase(next);
        if (next != free.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == off) {
                prev->second += len;
                return;
            }
        }
        free[off] = len;
    }
    uint64_t free_bytes() const {
        uint64_t n = 0;
        for (const auto& kv : free) n += kv.second;
        return n;
    }
};

/// [g0, g1) without the granules some other range covers (sorted, merged [a, b) granule ranges): the runs left, in order
inline void granules_minus(uint64_t g0, uint64_t g1, const std::vector<std::pair<uint64_t, uint64_t>>& cut,
                           std::vector<std::pair<uint64_t, uint64_t>>& out) {
    uint64_t a = g0;
    for (const auto& [c0, c1] : cut) {
        if (c1 <= a) continue;
        if (c0 >= g1) break;
        if (c0 > a) out.push_back({a, std::min(c0, g1)});
        a = std::max(a, c1);
        if (a >= g1) return;
    }
    if (a < g1) out.push_back({a, g1});
}

} // namespace l0
} // namespace mkwvk
