#pragma once
// (native-vk, layer 0, RX_VK_LAYER0=1) The window scheme's arithmetic, kept apart from Vulkan so that it can be tested alone
// (tests/layer0_windows_test.cpp). gpu/vk/vk_pools.cpp is the rest.
//
// Guest memory is seen by the GPU through window buffers: 4 GB sparse buffers (maxBufferSize is 4 GB on RADV), one starting
// every 2 GB of guest address space, so window k covers guest [k * 2 GB, k * 2 GB + 4 GB) and every guest byte lies in two
// windows (k and k - 1), bound to the same memory in both (sparseResidencyAliased). A range of at most 2 GB starting at g is
// then whole in window g >> 31, at offset g & (2 GB - 1): a binding is (window[g >> 31], g & 0x7fffffff), with no lookup.
// Memory is bound in host chunks (a power of two from 2 MB to 64 MB, aligned, so a chunk never crosses a 2 GB line): a udmabuf
// of the guest memfd's own pages (offset = guest address), imported once and bound into both its windows. Nothing is copied:
// the bytes the game writes are the bytes the GPU reads (step 7a, the host class: GPU reads cross PCIe). Larger chunks are
// fewer buffer objects, which RADV lists in every submission (~0.75 us each a submit: work/proto/layer0/l0_poc.out).
#include <atomic>
#include <cstdint>
#include <memory>

namespace mkwvk {
namespace l0 {

constexpr uint64_t kChunkMin = 2ull << 20, kChunkMax = 64ull << 20;   // (a host chunk: one udmabuf; its size_limit_mb is 64 by default)
constexpr uint64_t kStride = 2ull << 30;    // (a window starts every 2 GB)
constexpr uint64_t kWindow = 4ull << 30;    // (and is 4 GB long)
constexpr uint64_t kMaxRange = kStride;     // (the longest range one window holds whole wherever it starts)
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
/// release; read with acquire). Chunks are never unbound in step 7a: the bit stays.
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
    /// every chunk of guest [lo, hi) bound (false for an empty or out-of-range one)
    bool all(uint64_t lo, uint64_t hi) const {
        uint64_t c0, c1;
        chunk_span(lo, hi, chunk, c0, c1);
        if (c0 >= c1 || c1 > chunks) return false;
        for (uint64_t c = c0; c < c1; c++)
            if (!has(c)) return false;
        return true;
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
/// (as the old importer's RX_VK_IMPORT_SPAN_MB: a small pool's later bindings then find it bound); clipped to the guest window
inline void bind_span(uint64_t guest, uint64_t size, uint64_t pool_lo, uint64_t pool_hi, uint64_t span, uint64_t guest_bytes,
                      uint64_t& lo, uint64_t& hi) {
    lo = guest, hi = guest + size;
    if (pool_hi > pool_lo && pool_lo <= guest && hi <= pool_hi && pool_hi - pool_lo <= span) lo = pool_lo, hi = pool_hi;
    if (hi > guest_bytes) hi = guest_bytes;
    if (lo > hi) lo = hi;
}

} // namespace l0
} // namespace mkwvk
