// (native-vk, layer 0, step 7a: RX_VK_LAYER0=1, off by default) Guest memory for the GPU in window buffers, one backing for
// every pool byte. The scheme and its arithmetic: layer0_windows.h; measured on this GPU: work/proto/layer0/l0_poc.c.
//
// What it replaces (vk_memory.cpp, all still there with the switch off): pools imported as 16 MB chunks on demand, merged and
// imported again as they grow, retired and given back; VRAM mirrors kept in step from the submit captures; write-backs of
// the GPU's writes with their shadows and diffs. Here a chunk of guest memory is imported once (a udmabuf of the memfd's own
// pages), bound into the two window buffers that hold it, and stays: a binding is (window[g >> 31], g & 0x7fffffff), the
// GPU's writes land in guest memory, a readback waits only for the submission that wrote the range (wb_pages_note in
// gpu_buffer_bind). The pointer-following shaders' page table gets each chunk's window address.
//
// Step 7a is the host class only: the GPU reads every pool over PCIe (28.6 GB/s streaming, 10.7 GB/s scattered 64-byte
// reads against VRAM's 560 / 450 GB/s, l0_poc.out): a correctness milestone, expected slower than the mirrors. The VRAM class
// (CPU_NO_ACCESS pools, then the write-only ones), virtual pools as sparse binds and the policy by creation chain come next
// (work/assets/native/layer0-impl.md). The submit-time captures stay on: they serve the replay's own CPU reads as submitted
// (memory_read_as_submitted) and the textures' storage; RX_VK_CAPTURE=0 drops them once the syncs are honest (step 6).
//
// Switches, read once: RX_VK_LAYER0=1 on; RX_VK_LAYER0_CHUNK_MB=<2..64> the chunk size (default 64: fewer buffer objects,
// which RADV lists in every submission, ~0.75 us each); RX_VK_LAYER0_EAGER=1 a pool's chunks bound when the game makes it
// (on the game's thread) rather than at its first binding; RX_VK_LAYER0_SPAN_MB=<n> a first use binds the whole pool when
// it is at most n MB (default 64), else only the chunks of the range; RX_VK_LAYER0_LOG=1 each chunk bound.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "vk_internal.h"
#include "vk_pools.h"
#include "layer0_windows.h"

namespace mkwvk {
namespace {

constexpr VkBufferUsageFlags kWindowUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;   // (as vk_memory.cpp's imports)

struct Window {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
};

std::mutex mu;   // (binding: imports, windows, the sparse binds; never held while taking vk_memory.cpp's pools_mu)
Window windows[l0::kWindows];
l0::ChunkBits* bits = nullptr;                      // (made with the switch's answer, never freed)
std::vector<VkDeviceMemory> chunk_memory;           // (by chunk index: its import; mu held)
uint64_t chunk = 0, guest_bytes = 0, span_bytes = 0;
VkFence bind_fence = VK_NULL_HANDLE;
bool log_each = false, eager = false;
// counts (mu held), logged every 600 submissions
int64_t st_chunks = 0, st_windows = 0, st_bind_calls = 0, st_failed = 0, st_eager = 0;
double st_import_ms = 0, st_bind_ms = 0, st_max_ms = 0;
std::atomic<int64_t> st_refused_pool{0}, st_refused_long{0}, st_slow{0}, st_fast{0};

Window* window_make(uint32_t k) {   // (mu held)
    Window& w = windows[k];
    if (w.buffer) return &w;
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT;
    bi.size = l0::kWindow;
    bi.usage = kWindowUsage;
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &b) != VK_SUCCESS) {
        logf("vk: layer 0: window %u (guest %#llx): no sparse buffer", k, (unsigned long long)l0::window_base(k));
        return nullptr;
    }
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    w.address = vkGetBufferDeviceAddress(dev.device, &da);
    w.buffer = b;
    st_windows++;
    return &w;
}

/// The chunks of guest [lo, hi) not bound yet: imported, bound into their windows in one vkQueueBindSparse (waited for:
/// nothing submitted can use them before), then their page-table entries and their bits (mu held)
void bind_range_locked(uint64_t lo, uint64_t hi) {
    uint64_t c0, c1;
    l0::chunk_span(lo, hi, chunk, c0, c1);
    if (c1 > bits->chunks) c1 = bits->chunks;
    std::vector<uint64_t> fresh;
    for (uint64_t c = c0; c < c1; c++)
        if (!bits->has(c)) fresh.push_back(c);
    if (fresh.empty()) return;
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    struct One { uint32_t window; VkSparseMemoryBind bind; };
    std::vector<One> binds;
    std::vector<uint64_t> made;
    for (uint64_t c : fresh) {
        l0::Place pl[2];
        const int n = l0::chunk_places(c, chunk, pl);
        bool ok = n > 0;
        for (int i = 0; i < n && ok; i++) ok = window_make(pl[i].window) != nullptr;
        const VkDeviceMemory m = ok ? memory_guest_dma_buf(c * chunk, chunk) : VK_NULL_HANDLE;
        if (!m) {
            if (st_failed++ < 8)
                logf("vk: layer 0: chunk %#llx..%#llx not imported: its bindings stay empty", (unsigned long long)(c * chunk),
                     (unsigned long long)((c + 1) * chunk));
            continue;
        }
        chunk_memory[c] = m;
        made.push_back(c);
        for (int i = 0; i < n; i++) binds.push_back({pl[i].window, {pl[i].offset, chunk, m, 0, 0}});
    }
    const auto t1 = clk::now();
    if (made.empty()) return;
    std::sort(binds.begin(), binds.end(), [](const One& a, const One& b) {
        return a.window != b.window ? a.window < b.window : a.bind.resourceOffset < b.bind.resourceOffset;
    });
    std::vector<VkSparseMemoryBind> flat;
    std::vector<VkSparseBufferMemoryBindInfo> infos;
    flat.reserve(binds.size());
    for (const One& o : binds) flat.push_back(o.bind);
    for (size_t i = 0; i < binds.size();) {   // (one buffer bind info a window, its binds in offset order)
        size_t j = i;
        while (j < binds.size() && binds[j].window == binds[i].window) j++;
        infos.push_back({windows[binds[i].window].buffer, (uint32_t)(j - i), flat.data() + i});
        i = j;
    }
    VkBindSparseInfo si{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    si.bufferBindCount = (uint32_t)infos.size();
    si.pBufferBinds = infos.data();
    if (!bind_fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev.device, &fi, nullptr, &bind_fence);
    }
    VkResult r;
    {   // (on the queue, as vk_memory.cpp's sparse imports: the batched submits out first)
        std::lock_guard qk(dev.present_mu);
        queue_flush_locked();
        r = vkQueueBindSparse(dev.queue, 1, &si, bind_fence);
    }
    if (r == VK_SUCCESS) {
        r = vkWaitForFences(dev.device, 1, &bind_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev.device, 1, &bind_fence);
    }
    const auto t2 = clk::now();
    if (r != VK_SUCCESS) {
        logf("vk: layer 0: %zu chunks not bound (VkResult %d): their bindings stay empty", made.size(), (int)r);
        for (uint64_t c : made) vkFreeMemory(dev.device, chunk_memory[c], nullptr), chunk_memory[c] = VK_NULL_HANDLE;
        st_failed += (int64_t)made.size();
        return;
    }
    for (uint64_t c : made) {
        const uint64_t g = c * chunk;
        const uint32_t k = l0::window_of(g);
        guest_pages_set(g, g + chunk, windows[k].address + l0::window_offset(g));
        bits->set(c);
        if (log_each)
            logf("vk: layer 0: chunk %#llx..%#llx bound (window %u at %#llx)", (unsigned long long)g, (unsigned long long)(g + chunk), k,
                 (unsigned long long)l0::window_offset(g));
    }
    const double ms_imp = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double ms_bind = std::chrono::duration<double, std::milli>(t2 - t1).count();
    st_chunks += (int64_t)made.size(), st_bind_calls++, st_import_ms += ms_imp, st_bind_ms += ms_bind;
    st_max_ms = std::max(st_max_ms, ms_imp + ms_bind);
    static int slow_logged;
    if (ms_imp + ms_bind > 20 && slow_logged++ < 12)
        logf("vk: layer 0: %zu chunks (%llu MB) bound in %.1f ms (imports %.1f, binds %.1f)", made.size(),
             (unsigned long long)(made.size() * chunk >> 20), ms_imp + ms_bind, ms_imp, ms_bind);
}

/// The first use of a range: its chunks (or its small pool's) bound; true when all of the range's are
bool bind_now(uint64_t guest, uint64_t size, uint64_t pool_lo, uint64_t pool_hi) {
    uint64_t lo, hi;
    l0::bind_span(guest, size, pool_lo, pool_hi, span_bytes, guest_bytes, lo, hi);
    std::lock_guard lk(mu);
    bind_range_locked(lo, hi);
    return bits->all(guest, guest + size);
}

/// (guest, size): in one game pool and whole in a window, its chunks bound; false: an empty binding
bool ready(uint64_t guest, uint64_t size) {
    if (!size || !l0::fits_window(guest, size)) {
        st_refused_long.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint64_t lo, hi;
    if (!memory_game_pool(guest, &lo, &hi) || size > hi - guest) {   // (as the old lookup: a range past its pool's end binds nothing)
        st_refused_pool.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (bits->all(guest, guest + size)) return true;
    st_slow.fetch_add(1, std::memory_order_relaxed);
    return bind_now(guest, size, lo, hi);
}

} // namespace

bool layer0_on() {
    static const bool want = [] {
        const char* e = getenv("RX_VK_LAYER0");
        return e && atoi(e) != 0;
    }();
    if (!want) return false;
    static std::atomic<int> state{-1};   // (-1: not asked with a device yet; 0 off, 1 on)
    const int s = state.load(std::memory_order_acquire);
    if (s >= 0) return s != 0;
    if (!dev.device) return false;
    static std::mutex once;
    std::lock_guard lk(once);
    if (state.load(std::memory_order_relaxed) >= 0) return state.load(std::memory_order_relaxed) != 0;
    const char* why = nullptr;
#ifndef __linux__
    why = "Linux only (udmabuf)";
#endif
    if (!why && !(dev.sparse_buffers && dev.features.sparseResidencyBuffer && dev.features.sparseResidencyAliased))
        why = "no sparse residency of aliased buffers on the queue";
    if (!why && !host->guest_size()) why = "no guest window";
    const uint64_t limit = why ? 0 : memory_dma_buf_limit();
    if (!why && !limit) why = "guest memory is not a memfd shared as dma-bufs";
    if (why) {
        logf("vk: layer 0 (RX_VK_LAYER0=1) off: %s", why);
        state.store(0, std::memory_order_release);
        return false;
    }
    const char* c = getenv("RX_VK_LAYER0_CHUNK_MB");
    chunk = l0::chunk_bytes(c ? (uint64_t)std::max(1, atoi(c)) : 64, limit);
    const char* sp = getenv("RX_VK_LAYER0_SPAN_MB");
    span_bytes = (uint64_t)(sp ? std::max(0, atoi(sp)) : 64) << 20;
    eager = getenv("RX_VK_LAYER0_EAGER") && atoi(getenv("RX_VK_LAYER0_EAGER")) != 0;
    log_each = getenv("RX_VK_LAYER0_LOG") && atoi(getenv("RX_VK_LAYER0_LOG")) != 0;
    guest_bytes = std::min<uint64_t>(host->guest_size(), (uint64_t)l0::kWindows * l0::kStride);
    bits = new l0::ChunkBits(guest_bytes, chunk);
    chunk_memory.assign(bits->chunks, VK_NULL_HANDLE);
    logf("vk: layer 0 on: guest memory in 4 GB windows every 2 GB, chunks of %llu MB (udmabuf's limit %llu MB), bound %s; "
         "no imports, mirrors or write-backs", (unsigned long long)(chunk >> 20), (unsigned long long)(limit >> 20),
         eager ? "when the game makes a pool" : "at their first use");
    state.store(1, std::memory_order_release);
    return true;
}

BufferRef layer0_bind(uint64_t guest, uint64_t size) {
    if (!ready(guest, size)) return {};
    st_fast.fetch_add(1, std::memory_order_relaxed);
    const uint32_t k = l0::window_of(guest);
    return {windows[k].buffer, l0::window_offset(guest), size, 0};
}

BufferRef layer0_buffer(uint64_t guest, uint64_t size) {
    if (!ready(guest, size)) return {};
    const uint32_t k = l0::window_of(guest);
    return {windows[k].buffer, l0::window_offset(guest), size, windows[k].address + l0::window_offset(guest)};
}

void layer0_pool_created(uint64_t gpu, uint64_t size) {
    if (!eager || !size || gpu >= (1ull << 40) || !layer0_on()) return;   // (virtual pools are ranges of physical ones)
    std::lock_guard lk(mu);
    st_eager++;
    bind_range_locked(gpu, std::min(gpu + size, guest_bytes));
}

bool layer0_bound(uint64_t lo, uint64_t hi) {
    return bits && bits->any(lo, hi);
}

void layer0_fill_table() {
    std::lock_guard lk(mu);
    for (uint64_t c = 0; c < bits->chunks; c++)
        if (bits->has(c)) {
            const uint64_t g = c * chunk;
            guest_pages_set(g, g + chunk, windows[l0::window_of(g)].address + l0::window_offset(g));
        }
}

void layer0_submission() {
    static int64_t n = 0;
    if (++n % 600) return;
    std::lock_guard lk(mu);
    logf("vk: layer 0: %lld chunks bound (%llu MB) in %lld windows; %lld bind calls (%lld eager), imports %.1f ms, binds %.1f ms, "
         "longest %.1f ms; bindings (600 submits): %lld, %lld first uses, %lld outside a pool, %lld over 2 GB; %lld failed",
         (long long)st_chunks, (unsigned long long)(st_chunks * (int64_t)chunk >> 20), (long long)st_windows, (long long)st_bind_calls,
         (long long)st_eager, st_import_ms, st_bind_ms, st_max_ms, (long long)st_fast.exchange(0), (long long)st_slow.exchange(0),
         (long long)st_refused_pool.exchange(0), (long long)st_refused_long.exchange(0), (long long)st_failed);
}

} // namespace mkwvk
