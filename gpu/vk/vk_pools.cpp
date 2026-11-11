// (native-vk, layer 0: RX_VK_LAYER0=2 (the default) steps 7a + 7b, =1 step 7a alone, =0 the old path) Guest memory for the GPU in window buffers, one
// backing for every pool byte. The scheme and its arithmetic: layer0_windows.h; measured on this GPU: work/proto/layer0/l0_poc.c.
//
// What it replaces (vk_memory.cpp, all still there with the switch off): pools imported as 16 MB chunks on demand, merged and
// imported again as they grow, retired and given back; VRAM mirrors kept in step from the submit captures; write-backs of
// the GPU's writes with their shadows and diffs. Here a chunk of guest memory is imported once (a udmabuf of the memfd's own
// pages), bound into the two window buffers that hold it, and stays: a binding is (window[g >> 31], g & 0x7fffffff), the
// GPU's writes land in guest memory, a readback waits only for the submission that wrote the range (wb_pages_note in
// gpu_buffer_bind). The pointer-following shaders' page table gets each chunk's window address.
//
// Step 7a (=1) is the host class only: the GPU reads every pool over PCIe (28.6 GB/s streaming, 10.3 GB/s scattered 64-byte
// reads against VRAM's 560 / 444 GB/s, l0_poc.out): a correctness milestone, expected slower than the mirrors.
// Step 7b (=2) adds the VRAM class for pools the CPU cannot reach (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS: the render-target and
// GPU-only pools): their whole 64 KB granules are bound to device-local memory instead, in slots that follow the guest order
// (a pool is one contiguous run of a 256 MB block, or a block of its own: scattered sparse binds cost 127 us each and read 5x
// slower). Nothing is copied (the CPU never wrote them) and the CPU's view does not change. Granules a CPU-visible pool also
// covers, and a pool's partial edge granules, stay host. A finalized VRAM pool is kept for RX_VK_LAYER0_KEEP_S seconds and
// handed back whole on an Initialize with the same base, size and flags (the 288 MB and 32 MB render-target pools at every
// map change, second-map.md); a different pool over it, or the time, gives its granules back to the host chunk (or unbinds
// them), once the GPU is past the work handed out until its finalize. The pools come with their flags and their creation key
// (a hash of the creating call chain, host/nvn_native.c) through MkwVkRenderer::nvn_pool_native (API version 14).
// The submit-time captures stay on: they serve the replay's own CPU reads as submitted (memory_read_as_submitted) and the
// textures' storage; RX_VK_CAPTURE=0 drops them once the syncs are honest (step 6). A CPU read by the replay of a VRAM granule
// would see guest memory, not what the GPU wrote there: counted and logged (layer0_cpu_read).
//
// Switches, read once: RX_VK_LAYER0=1|2; RX_VK_LAYER0_CHUNK_MB=<2..64> the chunk size (default 16: a fresh 64 MB chunk cost up to 225 ms to import; 64 is fewer buffer objects, which
// RADV lists in every submission, ~0.7 us each); RX_VK_LAYER0_EAGER=1 a pool's chunks bound when the game makes it rather than
// at its first binding; RX_VK_LAYER0_SPAN_MB=<n> a first use binds the whole pool when it is at most n MB (default 64);
// RX_VK_LAYER0_LOG=1 each chunk bound and each pool placed (with its key); (=2) RX_VK_LAYER0_KEEP_S=<s> (default 10, 0: none);
// RX_VK_LAYER0_HOST_KEYS=<hex>,<hex>... creation keys kept host whatever their flags; RX_VK_LAYER0_VRAM_MB=<n> the VRAM
// class's limit (default 2048).
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <list>
#include <thread>
#include <map>
#include <mutex>
#include <vector>

#include "vk_internal.h"
#include "vk_pools.h"
#include "layer0_windows.h"

namespace mkwvk {
namespace {

using clk = std::chrono::steady_clock;
constexpr uint64_t G = l0::kGran;
constexpr uint32_t kCpuNoAccess = 0x1, kCpuUncached = 0x2, kCpuCached = 0x4, kGpuNoAccess = 0x8, kPhysical = 0x100,
                   kVirtual = 0x200;   // (NVNmemoryPoolFlags)
constexpr uint64_t kBlock = 256ull << 20;

constexpr VkBufferUsageFlags kWindowUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;   // (as vk_memory.cpp's imports)

struct Window {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
};
// (7b) a VRAM pool: its granule runs, live or kept after its finalize
struct VPool {
    uint64_t base, size, key;
    uint32_t flags;
    std::vector<std::pair<uint64_t, uint64_t>> runs;   // (granule indices [g0, g1))
    bool live = true;
    uint64_t value = 0;   // (finalized: the newest submission handed out then)
    clk::time_point dead_at{};
};
struct Run {
    uint64_t g1;
    uint32_t block;
    uint64_t off;   // (the slot of granule g0 in the block)
    VPool* owner;
};
struct Block {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint64_t size = 0;
    l0::SlotRanges slots;
};
struct LivePool {
    uint64_t size;
    uint32_t flags;
    bool vram;
};

std::mutex mu;   // (binding: imports, windows, the sparse binds, the VRAM class; never held while taking vk_memory.cpp's pools_mu)
int level = 0;
Window windows[l0::kWindows];
l0::ChunkBits* bits = nullptr;     // (host chunks imported and bound: never cleared)
l0::ChunkBits* ready = nullptr;    // (64 KB granules bound, host or VRAM: the bindings' check, without the lock)
l0::ChunkBits* vhint = nullptr;    // (2 MB regions that ever held a VRAM granule: the CPU-read check's quick no)
std::vector<VkDeviceMemory> chunk_memory;           // (by chunk index: its import; mu held)
uint64_t chunk = 0, guest_bytes = 0, span_bytes = 0, vram_limit = 0, vram_used = 0;
double keep_s = 10;
std::vector<uint64_t> host_keys;
VkFence bind_fence = VK_NULL_HANDLE;
uint32_t vram_type = UINT32_MAX;
bool log_each = false, eager = false;
uint64_t prefetch_bytes = 0;
// (chunk states, read and moved without mu: NONE -> QUEUED (a pool made: the worker imports it ahead) -> IMPORTING (claimed by
// the worker or by a binding that needs it now) -> BOUND or FAILED. A binding that finds a chunk IMPORTING by another waits for
// it on cv instead of importing it again)
enum : uint8_t { CH_NONE, CH_QUEUED, CH_IMPORTING, CH_BOUND, CH_FAILED };
std::unique_ptr<std::atomic<uint8_t>[]> cstate;
std::mutex cv_mu;
std::condition_variable cv, work_cv;
std::deque<uint64_t> prefetch_q;   // (cv_mu)
std::atomic<int64_t> st_prefetched{0}, st_waited{0}, st_wait_us{0}, st_own{0};
std::multimap<uint64_t, LivePool> live;   // (7b: every pool the game has, by base)
std::list<VPool> vpools;
std::map<uint64_t, Run> vram;             // (by first granule: disjoint)
std::vector<Block> blocks;
// counts (mu held unless atomic), logged every 600 submissions
int64_t st_chunks = 0, st_windows = 0, st_bind_calls = 0, st_failed = 0;
int64_t st_vpools = 0, st_handback = 0, st_released = 0, st_demoted = 0, st_vram_failed = 0, st_waits = 0;
uint64_t st_handback_bytes = 0, st_mixed_granules = 0;
double st_import_ms = 0, st_bind_ms = 0, st_max_ms = 0, st_wait_ms = 0;
std::atomic<int64_t> st_refused_pool{0}, st_refused_long{0}, st_slow{0}, st_fast{0}, st_cpu_reads{0}, st_early{0};

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
    if (vram_type == UINT32_MAX) {   // (the VRAM class's memory type: device-local, not host-visible, one the windows take)
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev.device, b, &req);
        for (int pass = 0; pass < 2 && vram_type == UINT32_MAX; pass++)
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && vram_type == UINT32_MAX; i++) {
                const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
                if ((req.memoryTypeBits >> i & 1) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                    (pass || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
                    vram_type = i;
            }
    }
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    w.address = vkGetBufferDeviceAddress(dev.device, &da);
    w.buffer = b;
    st_windows++;
    return &w;
}

struct One {
    uint32_t window;
    VkSparseMemoryBind bind;
};
/// Binds into the windows, one vkQueueBindSparse waited for (mu held). On the queue, as vk_memory.cpp's sparse imports: the
/// batched submits out first
VkResult submit_binds(std::vector<One>& binds) {
    if (binds.empty()) return VK_SUCCESS;
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
    if (dev.sparse_queue) {   // (a queue of its own: the bind does not wait for the replay's work queued before it)
        std::lock_guard qk(dev.sparse_queue_mu);
        r = vkQueueBindSparse(dev.sparse_queue, 1, &si, bind_fence);
    } else {
        std::lock_guard qk(dev.present_mu);
        queue_flush_locked();
        r = vkQueueBindSparse(dev.queue, 1, &si, bind_fence);
    }
    if (r == VK_SUCCESS) {
        r = vkWaitForFences(dev.device, 1, &bind_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev.device, 1, &bind_fence);
    }
    st_bind_calls++;
    return r;
}
/// guest [lo, hi) bound to mem at off (VK_NULL_HANDLE: unbound) in both windows of each of its pieces, appended to binds
void add_binds(std::vector<One>& binds, uint64_t lo, uint64_t hi, VkDeviceMemory mem, uint64_t off) {
    std::vector<l0::Piece> ps;
    l0::range_places(lo, hi, ps);
    for (const l0::Piece& p : ps)
        if (window_make(p.window)) binds.push_back({p.window, {p.offset, p.size, mem, mem ? off + p.from : 0, 0}});
}
/// the page table's entries of guest [lo, hi): each 2 GB stretch through the window of its start (0: unbound)
void table_set(uint64_t lo, uint64_t hi, bool bound) {
    for (uint64_t a = lo; a < hi;) {
        const uint64_t b = std::min(hi, (a / l0::kStride + 1) * l0::kStride);
        guest_pages_set(a, b, bound ? windows[l0::window_of(a)].address + l0::window_offset(a) : 0);
        a = b;
    }
}
/// the VRAM runs over granules [g0, g1), merged (mu held)
void vram_cut(uint64_t g0, uint64_t g1, std::vector<std::pair<uint64_t, uint64_t>>& out) {
    auto it = vram.upper_bound(g0);
    if (it != vram.begin()) --it;
    for (; it != vram.end() && it->first < g1; ++it)
        if (it->second.g1 > g0) out.push_back({it->first, it->second.g1});
}

/// Claimed chunks (IMPORTING, by this thread) imported, then under mu their host granules (all but the VRAM runs) bound into
/// their windows in one vkQueueBindSparse (waited for: nothing submitted can use them before), their page-table entries, bits
/// and states; the waiters told. The imports run without mu (a fresh 16 MB chunk is 5-100 ms: the other bindings go on)
void bind_claimed(const std::vector<uint64_t>& claimed, const char* who) {
    if (claimed.empty()) return;
    const auto t0 = clk::now();
    std::vector<std::pair<uint64_t, VkDeviceMemory>> got;
    std::vector<uint64_t> failed;
    for (uint64_t c : claimed) {
        const VkDeviceMemory m = memory_guest_dma_buf(c * chunk, chunk);
        if (m) got.push_back({c, m});
        else failed.push_back(c);
    }
    const auto t1 = clk::now();
    clk::time_point t2 = t1;
    {
        std::lock_guard lk(mu);
        std::vector<One> binds;
        for (const auto& [c, m] : got) {
            l0::Place pl[2];
            const int n = l0::chunk_places(c, chunk, pl);
            for (int i = 0; i < n; i++) window_make(pl[i].window);
            chunk_memory[c] = m;
            const uint64_t base = c * chunk;
            std::vector<std::pair<uint64_t, uint64_t>> cut, host;
            vram_cut(base / G, (base + chunk) / G, cut);
            l0::granules_minus(base / G, (base + chunk) / G, cut, host);
            for (const auto& [g0, g1] : host) add_binds(binds, g0 * G, g1 * G, m, g0 * G - base);
        }
        const VkResult r = got.empty() ? VK_SUCCESS : submit_binds(binds);
        t2 = clk::now();
        if (r != VK_SUCCESS) {
            logf("vk: layer 0: %zu chunks not bound (VkResult %d): their bindings stay empty", got.size(), (int)r);
            for (const auto& [c, m] : got) vkFreeMemory(dev.device, m, nullptr), chunk_memory[c] = VK_NULL_HANDLE, failed.push_back(c);
            got.clear();
        }
        for (const auto& [c, m] : got) {
            const uint64_t g = c * chunk;
            table_set(g, g + chunk, true);
            bits->set(c);
            for (uint64_t q = g / G; q < (g + chunk) / G; q++) ready->set(q);
            cstate[c].store(CH_BOUND, std::memory_order_release);
            if (log_each)
                logf("vk: layer 0: chunk %#llx..%#llx bound (window %u at %#llx)", (unsigned long long)g, (unsigned long long)(g + chunk),
                     l0::window_of(g), (unsigned long long)l0::window_offset(g));
        }
        for (uint64_t c : failed) {
            cstate[c].store(CH_FAILED, std::memory_order_release);
            if (st_failed++ < 8)
                logf("vk: layer 0: chunk %#llx..%#llx not imported: its bindings stay empty", (unsigned long long)(c * chunk),
                     (unsigned long long)((c + 1) * chunk));
        }
        const double ms_imp = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double ms_bind = std::chrono::duration<double, std::milli>(t2 - t1).count();
        st_chunks += (int64_t)got.size(), st_import_ms += ms_imp, st_bind_ms += ms_bind;
        st_max_ms = std::max(st_max_ms, ms_imp + ms_bind);
        static int slow_logged;
        if (ms_imp + ms_bind > 20 && slow_logged++ < 12)
            logf("vk: layer 0: %zu chunks (%llu MB) bound in %.1f ms (imports %.1f, binds %.1f) %s", got.size(),
                 (unsigned long long)(got.size() * chunk >> 20), ms_imp + ms_bind, ms_imp, ms_bind, who);
    }
    { std::lock_guard ck(cv_mu); }
    cv.notify_all();
}

/// The chunks of guest [lo, hi) not bound whose granules there are not all bound already (VRAM): those nobody imports claimed
/// and bound here; those another thread imports waited for (no mu held)
void bind_range(uint64_t lo, uint64_t hi) {
    uint64_t c0, c1;
    l0::chunk_span(lo, hi, chunk, c0, c1);
    if (c1 > bits->chunks) c1 = bits->chunks;
    std::vector<uint64_t> claimed, others;
    for (uint64_t c = c0; c < c1; c++) {
        if (bits->has(c)) continue;
        const uint64_t a = std::max(lo, c * chunk), b = std::min(hi, (c + 1) * chunk);
        if (a < b && ready->all(a, b)) continue;   // (the part needed is VRAM already)
        uint8_t st = cstate[c].load(std::memory_order_acquire);
        for (;;) {
            if ((st == CH_NONE || st == CH_QUEUED) &&
                !cstate[c].compare_exchange_weak(st, CH_IMPORTING, std::memory_order_acq_rel))
                continue;
            if (st == CH_NONE || st == CH_QUEUED) claimed.push_back(c);
            else if (st == CH_IMPORTING) others.push_back(c);
            break;
        }
    }
    st_own.fetch_add((int64_t)claimed.size(), std::memory_order_relaxed);
    bind_claimed(claimed, "at a binding");
    if (others.empty()) return;
    const auto w0 = clk::now();
    std::unique_lock ck(cv_mu);
    cv.wait(ck, [&] {
        for (uint64_t c : others)
            if (cstate[c].load(std::memory_order_acquire) == CH_IMPORTING) return false;
        return true;
    });
    st_waited.fetch_add((int64_t)others.size(), std::memory_order_relaxed);
    st_wait_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - w0).count(), std::memory_order_relaxed);
}

/// The importer: chunks queued by the pools the game makes, imported and bound ahead of their first binding (a few at a time:
/// one vkQueueBindSparse for them)
void worker_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk layer 0");
    for (;;) {
        std::vector<uint64_t> claimed;
        {
            std::unique_lock ck(cv_mu);
            work_cv.wait(ck, [] { return !prefetch_q.empty(); });
            while (!prefetch_q.empty() && claimed.size() < 8) {
                const uint64_t c = prefetch_q.front();
                prefetch_q.pop_front();
                uint8_t st = CH_QUEUED;
                if (cstate[c].compare_exchange_strong(st, CH_IMPORTING, std::memory_order_acq_rel)) claimed.push_back(c);
            }
        }
        st_prefetched.fetch_add((int64_t)claimed.size(), std::memory_order_relaxed);
        bind_claimed(claimed, "ahead");
    }
}
/// A pool made: its chunks queued for the importer (pools up to RX_VK_LAYER0_PREFETCH_MB; the parts not VRAM already)
void prefetch_pool(uint64_t lo, uint64_t hi) {
    uint64_t c0, c1;
    l0::chunk_span(lo, hi, chunk, c0, c1);
    if (c1 > bits->chunks) c1 = bits->chunks;
    bool any = false;
    {
        std::lock_guard ck(cv_mu);
        for (uint64_t c = c0; c < c1; c++) {
            const uint64_t a = std::max(lo, c * chunk), b = std::min(hi, (c + 1) * chunk);
            if (bits->has(c) || (a < b && ready->all(a, b))) continue;
            uint8_t st = CH_NONE;
            if (cstate[c].compare_exchange_strong(st, CH_QUEUED, std::memory_order_acq_rel)) prefetch_q.push_back(c), any = true;
        }
    }
    if (any) work_cv.notify_one();
}

// ---- (7b) the VRAM class ----
bool slot_take(uint64_t len, uint32_t& block, uint64_t& off) {   // (mu held; contiguous, first fit)
    if (vram_used + len > vram_limit) return false;
    for (uint32_t i = 0; i < blocks.size(); i++)
        if (blocks[i].mem && blocks[i].slots.take(len, off)) {
            block = i, vram_used += len;
            return true;
        }
    if (vram_type == UINT32_MAX) return false;
    Block b;
    b.size = std::max(kBlock, (len + G - 1) / G * G);
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fl;
    ai.allocationSize = b.size;
    ai.memoryTypeIndex = vram_type;
    if (vkAllocateMemory(dev.device, &ai, nullptr, &b.mem) != VK_SUCCESS) return false;
    b.slots = l0::SlotRanges(b.size);
    uint32_t at = 0;
    while (at < blocks.size() && blocks[at].mem) at++;
    if (at == blocks.size()) blocks.emplace_back();
    blocks[at] = std::move(b);
    blocks[at].slots.take(len, off);
    block = at, vram_used += len;
    return true;
}
void slot_give(uint32_t block, uint64_t off, uint64_t len) {   // (mu held; the GPU is past every use of it)
    Block& b = blocks[block];
    b.slots.give(off, len);
    vram_used -= len;
    if (b.slots.free_bytes() == b.size) {   // (an empty block goes back to the driver)
        vkFreeMemory(dev.device, b.mem, nullptr);
        b = Block{};
    }
}
void wait_for(uint64_t value) {   // (bounded: vk_timeline_wait gives up after 250 ms)
    if (!value || vk_timeline_completed() >= value) return;
    const auto t0 = clk::now();
    vk_timeline_wait(value);
    st_waits++, st_wait_ms += std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}
/// Granules [g0, g1) out of the VRAM class: back to their host chunk where it is bound, else unbound (and out of the bindings'
/// and the page table's view); their slots freed. After the GPU is past `value` (mu held)
void vram_release(uint64_t g0, uint64_t g1, uint64_t value) {
    std::vector<std::pair<uint64_t, uint64_t>> cut;
    vram_cut(g0, g1, cut);
    if (cut.empty()) return;
    wait_for(value);
    std::vector<One> binds;
    std::vector<std::pair<uint64_t, uint64_t>> unbound;
    for (auto [a, b] : cut) {
        auto it = vram.find(a);
        const uint64_t r0 = it->first;
        Run run = it->second;
        vram.erase(it);
        const uint64_t x0 = std::max(r0, g0), x1 = std::min(run.g1, g1);
        if (r0 < x0) vram[r0] = Run{x0, run.block, run.off, run.owner};
        if (x1 < run.g1) vram[x1] = Run{run.g1, run.block, run.off + (x1 - r0) * G, run.owner};
        slot_give(run.block, run.off + (x0 - r0) * G, (x1 - x0) * G);
        if (run.owner) {   // (the owner's runs: what is left of them)
            std::vector<std::pair<uint64_t, uint64_t>> left;
            for (const auto& [p0, p1] : run.owner->runs) l0::granules_minus(p0, p1, {{x0, x1}}, left);
            run.owner->runs.swap(left);
        }
        for (uint64_t q = x0; q < x1;) {   // (by host chunk)
            const uint64_t c = q * G / chunk, end = std::min(x1, (c + 1) * chunk / G);
            if (bits->has(c)) add_binds(binds, q * G, end * G, chunk_memory[c], q * G - c * chunk);
            else add_binds(binds, q * G, end * G, VK_NULL_HANDLE, 0), unbound.push_back({q, end});
            q = end;
        }
    }
    for (const auto& [a, b] : unbound)   // (the bindings' check first: nothing new reaches them)
        for (uint64_t q = a; q < b; q++) ready->clear(q);
    if (submit_binds(binds) != VK_SUCCESS) logf("vk: layer 0: granules %#llx..%#llx not given back", (unsigned long long)(g0 * G),
                                                (unsigned long long)(g1 * G));
    for (const auto& [a, b] : unbound) table_set(a * G, b * G, false);
}
void vpool_release(std::list<VPool>::iterator it) {   // (a kept pool's runs given back, the pool forgotten; mu held)
    const auto runs = it->runs;
    for (const auto& [a, b] : runs) vram_release(a, b, it->value);
    vpools.erase(it);
    st_released++;
}
void kept_expire() {   // (mu held)
    const auto now = clk::now();
    for (auto it = vpools.begin(); it != vpools.end();) {
        auto next = std::next(it);
        if (!it->live && std::chrono::duration<double>(now - it->dead_at).count() >= keep_s) vpool_release(it);
        it = next;
    }
}
/// Kept pools overlapping guest [lo, hi) given back; live VRAM granules there moved out (a CPU-visible pool over them: they are
/// mixed, the CPU's view wins) (mu held)
void vram_clear_over(uint64_t lo, uint64_t hi, bool demote_live) {
    for (auto it = vpools.begin(); it != vpools.end();) {
        auto next = std::next(it);
        if (!it->live && it->base < hi && lo < it->base + it->size) vpool_release(it);
        it = next;
    }
    if (!demote_live) return;
    const uint64_t g0 = lo / G, g1 = (hi + G - 1) / G;
    std::vector<std::pair<uint64_t, uint64_t>> cut;
    vram_cut(g0, g1, cut);
    for (const auto& [a, b] : cut) {
        const uint64_t x0 = std::max(a, g0), x1 = std::min(b, g1);
        st_demoted++, st_mixed_granules += x1 - x0;
        vram_release(x0, x1, vk_timeline_pushed());
    }
}
bool vram_class(uint64_t size, uint32_t flags, uint64_t key) {
    // (a pool the CPU can map stays in system memory, CPU_UNCACHED ones too: the game reads some back every frame, e.g. the view
    // uniform pools of f_806dc520 / f_80354ac0 (~11 KB a frame) and the 16 MB compute-constant ring of f_801102e0 (~33 KB), all
    // flags 0x22 (work/assets/native/uniform-buffers.md); from BAR that would be ~4 MB/s. Only CPU_NO_ACCESS pools go to VRAM)
    if (level < 2 || !(flags & kCpuNoAccess) || (flags & (kCpuUncached | kCpuCached | kGpuNoAccess | kPhysical | kVirtual)) || size < G)
        return false;
    return std::find(host_keys.begin(), host_keys.end(), key) == host_keys.end();
}
/// A kept pool of the same base, size and flags made live again: true (mu held)
bool vram_hand_back(uint64_t base, uint64_t size, uint32_t flags, uint64_t key) {
    for (auto it = vpools.begin(); it != vpools.end(); ++it)
        if (!it->live && it->base == base && it->size == size && it->flags == flags) {
            it->live = true, it->key = key;
            st_handback++;
            for (const auto& [a, b] : it->runs) st_handback_bytes += (b - a) * G;
            if (log_each) logf("vk: layer 0: pool %#llx+%#llx (flags %#x) handed back", (unsigned long long)base, (unsigned long long)size, flags);
            return true;
        }
    return false;
}
/// A VRAM-class pool made: its interior granules not covered by a CPU-visible pool, in slots that follow the guest order (mu held)
void vram_pool_made(uint64_t base, uint64_t size, uint32_t flags, uint64_t key) {
    vram_clear_over(base, base + size, false);   // (kept ones under it go first)
    uint64_t g0, g1;
    l0::interior_granules(base, base + size, g0, g1);
    std::vector<std::pair<uint64_t, uint64_t>> cut, runs;
    for (const auto& [b, p] : live)   // (CPU-visible pools over it: their granules stay host)
        if (!p.vram && b < g1 * G && g0 * G < b + p.size) cut.push_back({b / G, (b + p.size + G - 1) / G});
    vram_cut(g0, g1, cut);   // (and another live VRAM pool's)
    std::sort(cut.begin(), cut.end());
    l0::granules_minus(g0, g1, cut, runs);
    uint64_t want = 0;
    for (const auto& [a, b] : runs) want += b - a;
    st_mixed_granules += (g1 - g0) - want;
    if (runs.empty()) return;
    window_make(l0::window_of(base));   // (the first one also finds the VRAM memory type: the pools before any binding were left host)
    vpools.push_back(VPool{base, size, key, flags, {}, true, 0, {}});
    VPool* vp = &vpools.back();
    std::vector<One> binds;
    std::vector<std::pair<uint64_t, uint64_t>> placed;
    for (const auto& [a, b] : runs) {
        uint32_t blk;
        uint64_t off;
        if (!slot_take((b - a) * G, blk, off)) {
            if (st_vram_failed++ < 8)
                logf("vk: layer 0: pool %#llx+%#llx: no VRAM for %llu MB (limit %llu MB): stays host", (unsigned long long)base,
                     (unsigned long long)size, (unsigned long long)((b - a) * G >> 20), (unsigned long long)(vram_limit >> 20));
            continue;
        }
        add_binds(binds, a * G, b * G, blocks[blk].mem, off);
        vram[a] = Run{b, blk, off, vp};
        placed.push_back({a, b});
    }
    if (placed.empty()) {
        vpools.pop_back();
        return;
    }
    if (submit_binds(binds) != VK_SUCCESS) {
        logf("vk: layer 0: pool %#llx+%#llx not bound in VRAM", (unsigned long long)base, (unsigned long long)size);
        for (const auto& [a, b] : placed) {
            const Run r = vram[a];
            vram.erase(a);
            slot_give(r.block, r.off, (b - a) * G);
        }
        vpools.pop_back();
        return;
    }
    vp->runs = placed;
    for (const auto& [a, b] : placed) {
        for (uint64_t q = a; q < b; q++) ready->set(q);
        for (uint64_t r = a * G >> 21; r <= (b * G - 1) >> 21; r++) vhint->set(r);
        table_set(a * G, b * G, true);
    }
    st_vpools++;
    if (log_each)
        logf("vk: layer 0: pool %#llx+%#llx (flags %#x, key %016llx) in VRAM: %zu runs, %llu of %llu granules", (unsigned long long)base,
             (unsigned long long)size, flags, (unsigned long long)key, placed.size(), (unsigned long long)want,
             (unsigned long long)(g1 - g0));
}

/// The first use of a range: its chunks (or its small pool's) bound; true when all of the range's granules are
bool bind_now(uint64_t guest, uint64_t size, uint64_t pool_lo, uint64_t pool_hi) {
    uint64_t lo, hi;
    l0::bind_span(guest, size, pool_lo, pool_hi, span_bytes, guest_bytes, lo, hi);
    bind_range(lo, hi);
    return ready->all(guest, guest + size);
}

/// (guest, size): in one game pool and whole in a window, its granules bound; false: an empty binding
bool ready_for(uint64_t guest, uint64_t size) {
    if (!size || !l0::fits_window(guest, size)) {
        st_refused_long.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint64_t lo, hi;
    if (!memory_game_pool(guest, &lo, &hi) || size > hi - guest) {   // (as the old lookup: a range past its pool's end binds nothing)
        st_refused_pool.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (ready->all(guest, guest + size)) return true;
    st_slow.fetch_add(1, std::memory_order_relaxed);
    return bind_now(guest, size, lo, hi);
}

} // namespace

int layer0_wanted() {   // (RX_VK_LAYER0: 2 by default, 1 the host class only, 0 the old path: imports, mirrors, write-backs)
    static const int want = [] {
        const char* e = getenv("RX_VK_LAYER0");
        return e && *e ? std::clamp(atoi(e), 0, 2) : 2;
    }();
    return want;
}

int layer0_level() {
    return layer0_on() ? layer0_wanted() : 0;
}

bool layer0_on() {
    const int want = layer0_wanted();
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
        logf("vk: layer 0 (RX_VK_LAYER0=%d) off: %s", want, why);
        state.store(0, std::memory_order_release);
        return false;
    }
    level = want;
    const char* c = getenv("RX_VK_LAYER0_CHUNK_MB");
    chunk = l0::chunk_bytes(c ? (uint64_t)std::max(1, atoi(c)) : 16, limit);
    const char* sp = getenv("RX_VK_LAYER0_SPAN_MB");
    span_bytes = (uint64_t)(sp ? std::max(0, atoi(sp)) : 64) << 20;
    eager = getenv("RX_VK_LAYER0_EAGER") && atoi(getenv("RX_VK_LAYER0_EAGER")) != 0;   // (every pool queued ahead, whatever its size)
    const char* pf = getenv("RX_VK_LAYER0_PREFETCH_MB");
    prefetch_bytes = (uint64_t)(pf ? std::max(0, atoi(pf)) : 64) << 20;
    log_each = getenv("RX_VK_LAYER0_LOG") && atoi(getenv("RX_VK_LAYER0_LOG")) != 0;
    if (const char* k = getenv("RX_VK_LAYER0_KEEP_S")) keep_s = std::max(0.0, atof(k));
    const char* vm = getenv("RX_VK_LAYER0_VRAM_MB");
    vram_limit = (uint64_t)(vm ? std::max(0, atoi(vm)) : 2048) << 20;
    if (const char* hk = getenv("RX_VK_LAYER0_HOST_KEYS"))
        for (const char* p = hk; *p;) {
            char* end = nullptr;
            const unsigned long long v = strtoull(p, &end, 16);
            if (end == p) break;
            host_keys.push_back(v);
            p = *end ? end + 1 : end;
        }
    guest_bytes = std::min<uint64_t>(host->guest_size(), (uint64_t)l0::kWindows * l0::kStride);
    bits = new l0::ChunkBits(guest_bytes, chunk);
    ready = new l0::ChunkBits(guest_bytes, G);
    vhint = new l0::ChunkBits(guest_bytes, 2ull << 20);
    chunk_memory.assign(bits->chunks, VK_NULL_HANDLE);
    cstate.reset(new std::atomic<uint8_t>[bits->chunks]);
    for (uint64_t c = 0; c < bits->chunks; c++) cstate[c].store(CH_NONE, std::memory_order_relaxed);
    std::thread(worker_main).detach();
    logf("vk: layer 0 on (step %s): guest memory in 4 GB windows every 2 GB, chunks of %llu MB (udmabuf's limit %llu MB), bound %s; "
         "no imports, mirrors or write-backs", level >= 2 ? "7b: CPU_NO_ACCESS pools in VRAM" : "7a: host class",
         (unsigned long long)(chunk >> 20), (unsigned long long)(limit >> 20), eager ? "ahead when the game makes a pool (all)" : prefetch_bytes ? "ahead when the game makes a pool (up to the prefetch size)" : "at their first use");
    if (level >= 2)
        logf("vk: layer 0: VRAM class up to %llu MB, finalized pools kept %.0f s, %zu creation keys kept host",
             (unsigned long long)(vram_limit >> 20), keep_s, host_keys.size());
    state.store(1, std::memory_order_release);
    return true;
}

BufferRef layer0_bind(uint64_t guest, uint64_t size) {
    if (!ready_for(guest, size)) return {};
    st_fast.fetch_add(1, std::memory_order_relaxed);
    const uint32_t k = l0::window_of(guest);
    return {windows[k].buffer, l0::window_offset(guest), size, 0};
}

BufferRef layer0_buffer(uint64_t guest, uint64_t size) {
    if (!ready_for(guest, size)) return {};
    const uint32_t k = l0::window_of(guest);
    return {windows[k].buffer, l0::window_offset(guest), size, windows[k].address + l0::window_offset(guest)};
}

void layer0_pool_created(uint64_t gpu, uint64_t size) {
    if (!size || gpu >= (1ull << 40) || !layer0_on()) return;   // (virtual pools are ranges of physical ones)
    if (eager || size <= prefetch_bytes) prefetch_pool(gpu, std::min(gpu + size, guest_bytes));
}

void layer0_pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint64_t key, int on) {
    if (!size || gpu >= (1ull << 40)) return;
    if (!layer0_on()) {
        st_early.fetch_add(1, std::memory_order_relaxed);   // (before the device, or off)
        return;
    }
    if (level < 2) return;
    const bool vr = vram_class(size, flags, key);
    if (on && vr) {
        {   // (the same pool again, kept since its finalize: handed back, nothing bound)
            std::lock_guard lk(mu);
            if (vram_hand_back(gpu, size, flags, key)) {
                live.insert({gpu, LivePool{size, flags, vr}});
                return;
            }
        }
        // (a VRAM pool over granules the GPU may still use as host memory: the work handed out so far finished first, outside
        // the lock: the replay that finishes it may need the lock for its own bindings)
        uint64_t g0, g1;
        l0::interior_granules(gpu, gpu + size, g0, g1);
        if (g1 > g0 && ready->any(g0 * G, g1 * G)) wait_for(vk_timeline_pushed());
    }
    std::lock_guard lk(mu);
    kept_expire();
    if (on) {
        live.insert({gpu, LivePool{size, flags, vr}});
        if (vr && !vram_hand_back(gpu, size, flags, key)) vram_pool_made(gpu, size, flags, key);
        else vram_clear_over(gpu, gpu + size, true);   // (a CPU-visible pool over VRAM granules: they go back to host)
        return;
    }
    for (auto [it, end] = live.equal_range(gpu); it != end; ++it)
        if (it->second.size == size && it->second.flags == flags) {
            live.erase(it);
            break;
        }
    for (auto it = vpools.begin(); it != vpools.end(); ++it)
        if (it->live && it->base == gpu && it->size == size && it->flags == flags) {
            it->live = false, it->value = vk_timeline_pushed(), it->dead_at = clk::now();
            if (keep_s <= 0) vpool_release(it);
            break;
        }
}

bool layer0_bound(uint64_t lo, uint64_t hi) {   // (bound or being imported: a dma-buf may pin its pages any moment now)
    if (!bits || hi <= lo) return false;
    uint64_t c0, c1;
    l0::chunk_span(lo, hi, chunk, c0, c1);
    for (uint64_t c = c0; c < c1 && c < bits->chunks; c++) {
        const uint8_t st = cstate[c].load(std::memory_order_acquire);
        if (st == CH_IMPORTING || st == CH_BOUND) return true;
    }
    return false;
}

void layer0_cpu_read(uint64_t lo, uint64_t n) {
    if (!vhint || !n || !vhint->any(lo, lo + n)) return;
    std::lock_guard lk(mu);
    std::vector<std::pair<uint64_t, uint64_t>> cut;
    vram_cut(lo / G, (lo + n + G - 1) / G, cut);
    if (cut.empty()) return;
    static int logged;
    if (st_cpu_reads.fetch_add(1, std::memory_order_relaxed) < 20 && logged++ < 20)
        logf("vk: layer 0: the replay read %#llx+%#llx on the CPU, which is VRAM (the GPU's writes there are not in guest memory)",
             (unsigned long long)lo, (unsigned long long)n);
}

void layer0_fill_table() {
    std::lock_guard lk(mu);
    for (uint64_t c = 0; c < bits->chunks; c++)
        if (bits->has(c)) table_set(c * chunk, (c + 1) * chunk, true);
    for (const auto& [g0, r] : vram) table_set(g0 * G, r.g1 * G, true);
}

void layer0_submission() {
    static int64_t n = 0;
    if (++n % 600) return;
    std::lock_guard lk(mu);
    logf("vk: layer 0: %lld chunks bound (%llu MB) in %lld windows; %lld bind calls, imports %.1f ms, binds %.1f ms, "
         "longest %.1f ms; bindings (600 submits): %lld, %lld first uses, %lld outside a pool, %lld over 2 GB; %lld failed",
         (long long)st_chunks, (unsigned long long)(st_chunks * (int64_t)chunk >> 20), (long long)st_windows, (long long)st_bind_calls,
         st_import_ms, st_bind_ms, st_max_ms, (long long)st_fast.exchange(0), (long long)st_slow.exchange(0),
         (long long)st_refused_pool.exchange(0), (long long)st_refused_long.exchange(0), (long long)st_failed);
    logf("vk: layer 0: chunks imported ahead %lld, at a binding %lld; bindings that waited for the importer %lld (%.1f ms)",
         (long long)st_prefetched.load(), (long long)st_own.load(), (long long)st_waited.load(), st_wait_us.load() / 1000.0);
    if (level >= 2) {
        int64_t kept = 0, livev = 0;
        for (const VPool& v : vpools) (v.live ? livev : kept)++;
        logf("vk: layer 0 VRAM: %lld pools live, %lld kept; %llu MB in %zu runs (%zu blocks); %lld placed, %lld handed back (%llu MB), "
             "%lld given back, %lld demoted, %llu mixed granules host; %lld waits (%.1f ms); %lld not placed; CPU reads of VRAM %lld; "
             "%lld pool calls before the device",
             (long long)livev, (long long)kept, (unsigned long long)(vram_used >> 20), vram.size(), blocks.size(), (long long)st_vpools,
             (long long)st_handback, (unsigned long long)(st_handback_bytes >> 20), (long long)st_released, (long long)st_demoted,
             (unsigned long long)st_mixed_granules, (long long)st_waits, st_wait_ms, (long long)st_vram_failed,
             (long long)st_cpu_reads.load(), (long long)st_early.load());
    }
}

} // namespace mkwvk
