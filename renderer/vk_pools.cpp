// Layer 0 (plan, Architecture): guest memory for the GPU, one backing for every pool byte: window buffers on Linux,
// regions on Windows (below). The windows are cut down from gpu/legacy/vk/vk_pools.cpp (its comments have the scheme and the measurements; the arithmetic is
// layer0_windows.h, a verbatim copy) and the dma-buf import of gpu/legacy/vk/vk_memory.cpp. As there, with its defaults as constants:
// 16 MB host chunks imported once as os_guest's handles (udmabufs of the memfd's own pages on Linux; host pointers where a
// platform gives those) and bound into the two windows that hold them, ahead
// of use for pools up to 64 MB (an importer thread); CPU_NO_ACCESS pools in VRAM (the VRAM class, step 7b), kept 10 s after
// their finalize and handed back on the same pool again, 2048 MB at most. Left for M2 with the bindings: the per-binding
// path (a range bound at its first use), the shaders' page table and the BDA window table.
//
// Regions (layer 0's backing on Windows, where the windows cannot take the export's memory: NVIDIA's Windows driver binds
// only device-local memory into sparse buffers, and its host-pointer imports are system memory). Every
// GPU-readable pool is held by a region of its own: a plain buffer over the pool's pages, so its device addresses run on
// contiguously, which is all the translated shaders' table and layer0_span ask. The table (bda_emit.h) has an entry per
// 64 KB granule, the distance from guest to device address; a granule that regions share (the game leaves one page between
// pools) is split into 16 page entries. A host region's buffer is bound into an arena, a 64 MB piece of the guest window
// imported once (an import costs ~11 ms however small); a region that crosses an arena's end has an import of its own. A
// CPU_NO_ACCESS pool's region is device-local memory of its own (the VRAM class). A region whose pages a later pool takes
// (the game reusing a finalized pool's memory) is freed once the GPU is past the work recorded with it (retired_sweep).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <unordered_map>
#include <memory>
#include <thread>

#include "vk_internal.h"
#include "vk_pool.h"
#include "vk_query.h"
#include "layer0_windows.h"
#include "vk_assets.h"
#include "nvn_claims.h"
#include "os.h"
#include "os_guest.h"

namespace hsd {
void draw_window_made(uint32_t k, uint64_t a);   // (vk_draw.cpp)
void draw_table_set(uint32_t index, uint64_t value);
uint64_t draw_table_null(uint64_t guest);
}

namespace hsd {
namespace {

namespace l0 = mkwvk::l0;
using clk = std::chrono::steady_clock;
constexpr uint64_t G = l0::kGran;
constexpr uint32_t kCpuNoAccess = 0x1, kCpuUncached = 0x2, kCpuCached = 0x4, kGpuNoAccess = 0x8, kPhysical = 0x100,
                   kVirtual = 0x200;   // (NVNmemoryPoolFlags)
constexpr uint64_t kBlock = 256ull << 20;
constexpr uint64_t kChunkMb = 16,   // (64 MB: 1.7 GB more pinned in Kokiri, boot binds 4x slower, no submit gain)
                kVramLimit = 2048ull << 20;
constexpr double kKeepS = 10;

constexpr VkBufferUsageFlags kWindowUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

// ---- guest memory exported to the GPU (os_guest.h): imported by the handle's kind ----
int import_failures = 0;
uint64_t export_limit = 0;   // (the largest range one export takes: udmabuf's size limit on Linux)

bool export_open() {   // (once, at layer0_on)
    const char* note = nullptr;
    if (!os_guest_exportable(&export_limit, &note)) {
        logf("direct: layer 0: guest memory is not exported to the GPU (%s)", note ? note : "-");
        return false;
    }
    return true;
}
VkDeviceMemory import_dma_buf(uint64_t lo, uint64_t size, int fd);
VkDeviceMemory import_host(uint64_t lo, uint64_t size, void* ptr);
/// The memory type every import of guest memory takes, and the export's handle kind: chosen once, by import_probe at
/// layer0_on, before any other import (also what mem_account counts them under)
uint32_t import_type = UINT32_MAX;
VkExternalMemoryHandleTypeFlagBits import_handle{};
/// The memory of guest [lo, lo + size) as an import of its export; VK_NULL_HANDLE when the export or the import refused it
VkDeviceMemory guest_memory(uint64_t lo, uint64_t size) {
    os_gpu_handle h;
    const int r = os_guest_export(lo, size, &h);
    if (r) {
        if (import_failures++ < 8) logf("direct: guest %#llx..%#llx: not exported (%d)", (unsigned long long)lo, (unsigned long long)(lo + size), r);
        return VK_NULL_HANDLE;
    }
    switch (h.kind) {
    case OS_GPU_DMABUF_FD: return import_dma_buf(lo, size, h.fd);
    case OS_GPU_HOST_POINTER: return import_host(lo, size, h.ptr);
    default:
        os_gpu_handle_close(&h);
        if (import_failures++ < 8) logf("direct: guest memory exported as handle kind %d, which this back end does not import", h.kind);
        return VK_NULL_HANDLE;
    }
}
/// one import of a dma-buf (fd: the caller's; closed when the import fails, the memory's when it succeeds) or a host
/// pointer, as memory type `type`
VkResult import_as(uint32_t type, uint64_t size, int fd, void* ptr, VkDeviceMemory& memory) {
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkImportMemoryFdInfoKHR fi{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &fl, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd};
    VkImportMemoryHostPointerInfoEXT hi{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, &fl, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr};
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = ptr ? (const void*)&hi : (const void*)&fi;
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;
    memory = VK_NULL_HANDLE;
    return vkAllocateMemory(dev.device, &ai, nullptr, &memory);
}
VkDeviceMemory import_dma_buf(uint64_t lo, uint64_t size, int fd) {
    VkDeviceMemory memory;
    const VkResult r = import_as(import_type, size, fd, nullptr, memory);
    if (r != VK_SUCCESS) {   // (the fd is the caller's again when the import fails)
        os_gpu_handle h{OS_GPU_DMABUF_FD, fd, nullptr, size};
        os_gpu_handle_close(&h);
        if (import_failures++ < 8)
            logf("direct: guest %#llx..%#llx: its dma-buf not imported (VkResult %d)", (unsigned long long)lo, (unsigned long long)(lo + size), (int)r);
        return VK_NULL_HANDLE;
    }
    mem_account(MO_L0_IMPORT, import_type, (int64_t)size);
    return memory;
}
/// a host pointer's import (VK_EXT_external_memory_host): the window's own pages
VkDeviceMemory import_host(uint64_t lo, uint64_t size, void* ptr) {
    VkDeviceMemory memory;
    const VkResult r = ((uintptr_t)ptr | size) % (dev.host_align ? dev.host_align : 1) ? VK_ERROR_INVALID_EXTERNAL_HANDLE
                                                                                       : import_as(import_type, size, -1, ptr, memory);
    if (r != VK_SUCCESS) {
        if (import_failures++ < 8)
            logf("direct: guest %#llx..%#llx: its host pointer not imported (VkResult %d)", (unsigned long long)lo, (unsigned long long)(lo + size), (int)r);
        return VK_NULL_HANDLE;
    }
    mem_account(MO_L0_IMPORT, import_type, (int64_t)size);
    return memory;
}

/// The import's memory type, chosen once from one export of guest memory (64 KB of the system region): of the types the
/// driver lists for the handle, the first that imports. Nothing maps the imported memory (the CPU reaches those pages
/// through the guest window), so the type matters for how the GPU reaches them, and as the accounting's type:
/// - dma-bufs: host-visible and coherent first (RADV: the GPU's accesses snoop the CPU's caches, so its writes to the
///   same pages are seen), then the rest. NVIDIA's Linux driver lists a host-visible type for a dma-buf of system memory
///   and answers VK_ERROR_OUT_OF_DEVICE_MEMORY to it, and imports it as its plain device type;
/// - host pointers: host-visible, coherent and cached first (NVIDIA's Windows driver gives an uncached type and a cached
///   one, and shaders that loop over a CPU-visible buffer, OoT's light sums, ran 100x slower on the first), then
///   host-visible and coherent, then the rest.
/// The probe's memory is kept in `probe` for the backing's choice (choose_backing), which frees it. False: no import
bool import_probe(VkDeviceMemory& probe, uint64_t& probe_size) {
    const uint64_t at = 16ull << 20;
    probe_size = std::max<uint64_t>(64 << 10, dev.host_align);
    os_gpu_handle h;
    if (const int r = os_guest_export(at, probe_size, &h)) return logf("direct: layer 0: guest memory not exported (%d)", r), false;
    uint32_t bits = 0;
    VkMemoryPropertyFlags first[3] = {};   // (the property sets taken in order, then any)
    if (h.kind == OS_GPU_DMABUF_FD) {
        import_handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (vkGetMemoryFdPropertiesKHR(dev.device, import_handle, h.fd, &fp) == VK_SUCCESS) bits = fp.memoryTypeBits;
        first[0] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    } else if (h.kind == OS_GPU_HOST_POINTER && dev.host_import) {
        import_handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vkGetMemoryHostPointerPropertiesEXT(dev.device, import_handle, h.ptr, &hp) == VK_SUCCESS) bits = hp.memoryTypeBits;
        first[0] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        first[1] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    std::string tried;
    uint32_t done = 0;   // (types tried)
    for (int pass = 0; pass <= 3 && import_type == UINT32_MAX; pass++)
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && import_type == UINT32_MAX; i++) {
            const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
            if (!(bits >> i & 1) || (done >> i & 1) || (pass < 3 && (!first[pass] || (f & first[pass]) != first[pass]))) continue;
            done |= 1u << i;
            const VkResult r = import_as(i, probe_size, h.kind == OS_GPU_DMABUF_FD ? h.fd : -1, h.kind == OS_GPU_HOST_POINTER ? h.ptr : nullptr, probe);
            tried += " " + std::to_string(i) + ":" + std::to_string(f) + "=" + std::to_string((int)r);
            if (r == VK_SUCCESS) import_type = i;
        }
    if (import_type == UINT32_MAX) os_gpu_handle_close(&h);   // (a dma-buf's fd is the memory's once imported)
    logf("direct: layer 0: guest memory imported as %s, memory type %d (of types %#x; tried type:flags=VkResult:%s)",
         h.kind == OS_GPU_DMABUF_FD ? "dma-bufs" : h.kind == OS_GPU_HOST_POINTER ? "host pointers" : "nothing", (int)import_type, bits, tried.c_str());
    return import_type != UINT32_MAX;
}

// ---- the windows (vk_pools.cpp) ----
struct Window {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
};
struct VPool {   // (a VRAM pool: its granule runs, live or kept after its finalize)
    uint64_t base, size, key;
    uint32_t flags;
    std::vector<std::pair<uint64_t, uint64_t>> runs;   // (granule indices [g0, g1))
    bool live = true;
    uint64_t value = 0;   // (finalized: the newest submission sent then)
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

std::atomic<int> state{-1};   // (-1: not started; 0 off, 1 on)
std::mutex mu;   // (imports' binds, the windows, the VRAM class)
Window windows[l0::kWindows];
l0::ChunkBits* bits = nullptr;    // (host chunks imported and bound: never cleared)
l0::ChunkBits* ready = nullptr;   // (64 KB granules bound, host or VRAM)
std::vector<VkDeviceMemory> chunk_memory;   // (by chunk index; mu held)
uint64_t chunk = 0, guest_bytes = 0, vram_used = 0;
VkFence bind_fence = VK_NULL_HANDLE;
uint32_t vram_type = UINT32_MAX;
// (chunk states, read and moved without mu: NONE -> QUEUED (a pool made) -> IMPORTING (the importer) -> BOUND or FAILED)
enum : uint8_t { CH_NONE, CH_QUEUED, CH_IMPORTING, CH_BOUND, CH_FAILED };
std::unique_ptr<std::atomic<uint8_t>[]> cstate;
std::mutex cv_mu;
std::condition_variable work_cv;
std::deque<uint64_t> prefetch_q;   // (cv_mu)
std::multimap<uint64_t, LivePool> live;   // (every pool the game has, by base)
std::list<VPool> vpools;
std::map<uint64_t, Run> vram;   // (by first granule: disjoint)
std::vector<Block> blocks;
int64_t st_chunks = 0, st_windows = 0, st_bind_calls = 0, st_failed = 0, st_vpools = 0, st_handback = 0, st_released = 0,
        st_demoted = 0, st_vram_failed = 0, st_waits = 0;
double st_import_ms = 0, st_bind_ms = 0, st_max_ms = 0, st_wait_ms = 0;
std::atomic<int64_t> st_early{0}, st_span_binds{0};

Window* window_make(uint32_t k) {   // (mu held)
    Window& w = windows[k];
    if (w.buffer) return &w;
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = (dev.dma_buf_import ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT : 0) |
                     (dev.host_import ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT : 0);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT;
    bi.size = l0::kWindow;
    bi.usage = kWindowUsage | query_window_usage();   // (M5: conditional rendering reads guest memory)
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &b) != VK_SUCCESS) {
        logf("direct: layer 0: window %u (guest %#llx): no sparse buffer", k, (unsigned long long)l0::window_base(k));
        return nullptr;
    }
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    w.address = vkGetBufferDeviceAddress(dev.device, &da);
    __atomic_store_n(&w.buffer, b, __ATOMIC_RELEASE);   // (M3b: published after its address: layer0_span reads it without mu)
    st_windows++;
    draw_window_made(k, w.address);   // (M2b: the BDA window table's entry)
    return &w;
}

struct One {
    uint32_t window;
    VkSparseMemoryBind bind;
};
/// Binds into the windows, one vkQueueBindSparse on the sparse queue, waited for (mu held)
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
    {
        std::lock_guard qk(dev.sparse_shared ? dev.queue_mu : dev.sparse_mu);
        r = vkQueueBindSparse(dev.sparse_queue, 1, &si, bind_fence);
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
/// the VRAM runs over granules [g0, g1), merged (mu held)
void vram_cut(uint64_t g0, uint64_t g1, std::vector<std::pair<uint64_t, uint64_t>>& out) {
    auto it = vram.upper_bound(g0);
    if (it != vram.begin()) --it;
    for (; it != vram.end() && it->first < g1; ++it)
        if (it->second.g1 > g0) out.push_back({it->first, it->second.g1});
}

/// Claimed chunks (IMPORTING, by this thread) imported, then under mu their host granules (all but the VRAM runs) bound into
/// their windows in one vkQueueBindSparse (waited for), their bits and states. The imports run without mu
void bind_claimed(const std::vector<uint64_t>& claimed) {
    if (claimed.empty()) return;
    const auto t0 = clk::now();
    std::vector<std::pair<uint64_t, VkDeviceMemory>> got;
    std::vector<uint64_t> failed;
    for (uint64_t c : claimed) {
        const VkDeviceMemory m = guest_memory(c * chunk, chunk);
        if (m) got.push_back({c, m});
        else failed.push_back(c);
    }
    const auto t1 = clk::now();
    std::lock_guard lk(mu);
    std::vector<One> binds;
    for (const auto& [c, m] : got) {
        l0::Place pl[2];
        const int n = l0::chunk_places(c, chunk, pl);
        for (int i = 0; i < n; i++) window_make(pl[i].window);
        chunk_memory[c] = m;
        const uint64_t base = c * chunk;
        std::vector<std::pair<uint64_t, uint64_t>> cut, host_runs;
        vram_cut(base / G, (base + chunk) / G, cut);
        l0::granules_minus(base / G, (base + chunk) / G, cut, host_runs);
        for (const auto& [g0, g1] : host_runs) add_binds(binds, g0 * G, g1 * G, m, g0 * G - base);
    }
    const VkResult r = got.empty() ? VK_SUCCESS : submit_binds(binds);
    const auto t2 = clk::now();
    if (r != VK_SUCCESS) {
        logf("direct: layer 0: %zu chunks not bound (VkResult %d)", got.size(), (int)r);
        for (const auto& [c, m] : got) {
            vkFreeMemory(dev.device, m, nullptr), chunk_memory[c] = VK_NULL_HANDLE, failed.push_back(c);
            mem_account(MO_L0_IMPORT, import_type, -(int64_t)chunk);
        }
        got.clear();
    }
    for (const auto& [c, m] : got) {
        bits->set(c);
        for (uint64_t q = c * chunk / G; q < (c + 1) * chunk / G; q++) ready->set(q);
        cstate[c].store(CH_BOUND, std::memory_order_release);
    }
    for (uint64_t c : failed) {
        cstate[c].store(CH_FAILED, std::memory_order_release);
        if (st_failed++ < 8)
            logf("direct: layer 0: chunk %#llx..%#llx not imported", (unsigned long long)(c * chunk), (unsigned long long)((c + 1) * chunk));
    }
    const double ms_imp = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double ms_bind = std::chrono::duration<double, std::milli>(t2 - t1).count();
    st_chunks += (int64_t)got.size(), st_import_ms += ms_imp, st_bind_ms += ms_bind;
    st_max_ms = std::max(st_max_ms, ms_imp + ms_bind);
}

/// The importer: chunks queued by the pools the game makes, imported and bound ahead of their use (8 at a time)
void worker_main(void*) {
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
        bind_claimed(claimed);
    }
}
/// A pool made: its chunks queued for the importer (the parts not VRAM already)
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

// ---- the VRAM class (step 7b) ----
bool slot_take(uint64_t len, uint32_t& block, uint64_t& off) {   // (mu held; contiguous, first fit)
    if (vram_used + len > kVramLimit) return false;
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
    mem_account(MO_L0_VRAM, vram_type, (int64_t)b.size);
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
        mem_account(MO_L0_VRAM, vram_type, -(int64_t)b.size);
        b = Block{};
    }
}
void wait_for(uint64_t value) {
    if (!value || sync_completed() >= value) return;
    const auto t0 = clk::now();
    sync_wait_value(value, UINT64_MAX);
    st_waits++, st_wait_ms += std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}
/// Granules [g0, g1) out of the VRAM class: back to their host chunk where it is bound, else unbound; their slots freed.
/// After the GPU is past `value` (mu held)
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
    for (const auto& [a, b] : unbound)
        for (uint64_t q = a; q < b; q++) ready->clear(q);
    if (submit_binds(binds) != VK_SUCCESS)
        logf("direct: layer 0: granules %#llx..%#llx not given back", (unsigned long long)(g0 * G), (unsigned long long)(g1 * G));
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
        if (!it->live && std::chrono::duration<double>(now - it->dead_at).count() >= kKeepS) vpool_release(it);
        it = next;
    }
}
/// Kept pools overlapping guest [lo, hi) given back; live VRAM granules there moved out (a CPU-visible pool over them) (mu held)
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
        st_demoted++;
        vram_release(std::max(a, g0), std::min(b, g1), sync_pushed());
    }
}
bool vram_class(uint64_t size, uint32_t flags) {   // (only CPU_NO_ACCESS pools: vk_pools.cpp vram_class has why)
    return (flags & kCpuNoAccess) && !(flags & (kCpuUncached | kCpuCached | kGpuNoAccess | kPhysical | kVirtual)) && size >= G;
}
/// A kept pool of the same base, size and flags made live again: true (mu held)
bool vram_hand_back(uint64_t base, uint64_t size, uint32_t flags, uint64_t key) {
    for (auto it = vpools.begin(); it != vpools.end(); ++it)
        if (!it->live && it->base == base && it->size == size && it->flags == flags) {
            it->live = true, it->key = key;
            st_handback++;
            return true;
        }
    return false;
}
/// A VRAM-class pool made: its interior granules not covered by a CPU-visible pool, in slots that follow the guest order (mu held)
void vram_pool_made(uint64_t base, uint64_t size, uint32_t flags, uint64_t key) {
    vram_clear_over(base, base + size, false);
    uint64_t g0, g1;
    l0::interior_granules(base, base + size, g0, g1);
    std::vector<std::pair<uint64_t, uint64_t>> cut, runs;
    for (const auto& [b, p] : live)   // (CPU-visible pools over it: their granules stay host)
        if (!p.vram && b < g1 * G && g0 * G < b + p.size) cut.push_back({b / G, (b + p.size + G - 1) / G});
    vram_cut(g0, g1, cut);   // (and another live VRAM pool's)
    std::sort(cut.begin(), cut.end());
    l0::granules_minus(g0, g1, cut, runs);
    if (runs.empty()) return;
    window_make(l0::window_of(base));   // (the first one also finds the VRAM memory type)
    vpools.push_back(VPool{base, size, key, flags, {}, true, 0, {}});
    VPool* vp = &vpools.back();
    std::vector<One> binds;
    std::vector<std::pair<uint64_t, uint64_t>> placed;
    for (const auto& [a, b] : runs) {
        uint32_t blk;
        uint64_t off;
        if (!slot_take((b - a) * G, blk, off)) {
            if (st_vram_failed++ < 8)
                logf("direct: layer 0: pool %#llx+%#llx: no VRAM for %llu MB: stays host", (unsigned long long)base,
                     (unsigned long long)size, (unsigned long long)((b - a) * G >> 20));
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
        logf("direct: layer 0: pool %#llx+%#llx not bound in VRAM", (unsigned long long)base, (unsigned long long)size);
        for (const auto& [a, b] : placed) {
            const Run r = vram[a];
            vram.erase(a);
            slot_give(r.block, r.off, (b - a) * G);
        }
        vpools.pop_back();
        return;
    }
    vp->runs = placed;
    for (const auto& [a, b] : placed)
        for (uint64_t q = a; q < b; q++) ready->set(q);
    st_vpools++;
}

/// A pool made: bound in full now (its host chunks queued for the importer), whatever its size, when the GPU can read it.
/// Shaders follow pointers the game stores in memory (the probe tables of a 150 MB pool in Kokiri), so any
/// GPU-readable pool may be read with no binding naming it; before, only pools up to 64 MB were bound ahead and the rest
/// at a binding's use, and memory reached only by a stored pointer read the null window: zeros. Only pools claimed as
/// layer 0's (nvn_claims.h) come here; of those, the ones the GPU cannot read are not bound. A VRAM-class pool's
/// interior granules are in VRAM already (vram_pool_made); its partial edge granules are host and bound here. One line a
/// pool of a chunk or more, the rest counted (mu held)
std::atomic<int64_t> st_ahead_pools{0}, st_ahead_bytes{0}, st_left_pools[2], st_claimed_off{0};
void bind_ahead(uint64_t gpu, uint64_t size, uint32_t flags, bool vr) {
    const int why = (flags & kGpuNoAccess) ? 1 : 0;
    static const char* const kWhy[2] = {"bound in full", "not bound (the GPU cannot read it)"};
    if (size >= chunk) logf("direct: layer 0: pool %#llx+%#llx flags %#x%s: %s", (unsigned long long)gpu, (unsigned long long)size, flags,
                            vr ? " (VRAM class: its edges)" : "", kWhy[why]);
    if (why) return void(st_left_pools[why].fetch_add(1, std::memory_order_relaxed));
    st_ahead_pools.fetch_add(1, std::memory_order_relaxed), st_ahead_bytes.fetch_add((int64_t)size, std::memory_order_relaxed);
    prefetch_pool(gpu, std::min(gpu + size, guest_bytes));
}

// ---- regions (Windows): see the top ----
struct Region {
    uint64_t lo, hi;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    bool vram = false;   // (device-local memory of its own, not the guest's pages: a CPU_NO_ACCESS pool's)
    uint64_t pages = 0;  // (of its pages, how many the table still gives it; none: it is retired)
};
bool regions_mode = false;   // (the backing, chosen once at layer0_on: choose_backing)
constexpr uint32_t kRegionShift = 16, kPageShift = 12;   // (the shaders' table granule, bda_emit.h kTableShift, and its pages)
constexpr uint32_t kGranulePages = 1u << (kRegionShift - kPageShift);
constexpr uint32_t kSplitFirst = (1u << (39 - kRegionShift)) + 8, kSplits = 1u << 16;   // (bda_emit.h's table layout)
/// a granule's owner: 0 none, a Region* (the whole granule), or a Split* | 1 (pages of several). Published after the table
std::unique_ptr<std::atomic<uintptr_t>[]> granule_region;
/// a granule regions share: its pages' owners, and its 16 page entries in the table at `index` (made once a granule, kept)
struct Split {
    uint32_t index;
    std::atomic<Region*> page[kGranulePages];
};
std::unordered_map<uint64_t, std::unique_ptr<Split>> splits;   // (mu; by granule)
uint32_t splits_used = 0;
std::vector<std::unique_ptr<Region>> regions_made;   // (mu; a Region itself is never freed: readers hold it without mu)
/// A region whose pages others took over (a pool made over a finalized pool's pages): its buffer and memory freed once
/// nothing can use them. A second after its last page went, the newest submission is noted (every list recorded before
/// then has been sent); once the GPU is past that, it goes
struct Retired {
    Region* r;
    clk::time_point at;
    uint64_t value = 0;
};
std::vector<Retired> retired;   // (mu)
int64_t st_regions = 0, st_region_merges = 0, st_region_failed = 0, st_region_freed = 0, st_split_full = 0;
uint64_t st_region_bytes = 0, region_live_bytes = 0;
double st_region_ms = 0;

Region* region_at(uint64_t g) {   // (the region holding guest g's page; read without mu)
    if (g >= guest_bytes) return nullptr;
    const uintptr_t v = granule_region[g >> kRegionShift].load(std::memory_order_acquire);
    if (!(v & 1)) return (Region*)v;
    return ((Split*)(v & ~(uintptr_t)1))->page[g >> kPageShift & (kGranulePages - 1)].load(std::memory_order_acquire);
}
void retired_sweep() {   // (mu held)
    const auto now = clk::now();
    for (size_t i = 0; i < retired.size();) {
        Retired& t = retired[i];
        if (!t.value && now - t.at >= std::chrono::seconds(1)) t.value = sync_pushed();
        if (!t.value || sync_completed() < t.value) {
            i++;
            continue;
        }
        vkDestroyBuffer(dev.device, t.r->buffer, nullptr);
        if (!t.r->memory) {   // (a region in an arena: the buffer was all its own)
        } else if (vkFreeMemory(dev.device, t.r->memory, nullptr), t.r->vram) {
            mem_account(MO_L0_VRAM, vram_type, -(int64_t)(t.r->hi - t.r->lo));
            vram_used -= t.r->hi - t.r->lo;
        } else {
            mem_account(MO_L0_IMPORT, import_type, -(int64_t)(t.r->hi - t.r->lo));
            region_live_bytes -= t.r->hi - t.r->lo;
        }
        t.r->buffer = VK_NULL_HANDLE, t.r->memory = VK_NULL_HANDLE;
        st_region_freed++;
        retired[i] = retired.back();
        retired.pop_back();
    }
}
/// `pages` pages of `from` given to another region: retired when it has none left (mu held)
void region_lose(Region* from, uint64_t pages) {
    if (!from) return;
    from->pages -= std::min(from->pages, pages);
    if (!from->pages) retired.push_back({from, clk::now()});
}
/// r's pages [lo, hi) in the table and the owner array, whole granules as one entry, shared ones split (mu held). The
/// table is written first: a reader that finds r there finds its entries
void region_assign(Region* r, uint64_t lo, uint64_t hi) {
    const uint64_t distance = r->address - r->lo, gsize = 1ull << kRegionShift;
    for (uint64_t k = lo >> kRegionShift; k <= (hi - 1) >> kRegionShift; k++) {
        const uint64_t g0 = k << kRegionShift, a = std::max(lo, g0), b = std::min(hi, g0 + gsize);
        const uintptr_t v = granule_region[k].load(std::memory_order_relaxed);
        if (a == g0 && b == g0 + gsize) {   // (the whole granule)
            if (v & 1) {
                Split* s = (Split*)(v & ~(uintptr_t)1);
                for (auto& p : s->page) region_lose(p.load(std::memory_order_relaxed) == r ? nullptr : p.load(std::memory_order_relaxed), 1);
            } else if ((Region*)v != r) {
                region_lose((Region*)v, kGranulePages);
            }
            draw_table_set((uint32_t)k, distance);
            granule_region[k].store((uintptr_t)r, std::memory_order_release);
            continue;
        }
        // (a part: the granule split, its pages' entries made from their owners first when it was whole)
        Split* s;
        auto it = splits.find(k);
        if (it != splits.end()) {
            s = it->second.get();
        } else {
            if (splits_used >= kSplits) {   // (no room: the granule goes to r whole, as the regions' first design did)
                if (st_split_full++ < 8) logf("direct: layer 0: no room for another split granule (%u): granule %#llx is the newest region's", kSplits, (unsigned long long)g0);
                if (!(v & 1) && (Region*)v != r) region_lose((Region*)v, kGranulePages);
                draw_table_set((uint32_t)k, distance);
                granule_region[k].store((uintptr_t)r, std::memory_order_release);
                continue;
            }
            auto made = std::make_unique<Split>();
            made->index = kSplitFirst + 16 * splits_used++;
            for (auto& p : made->page) p.store(nullptr, std::memory_order_relaxed);
            s = made.get();
            splits.emplace(k, std::move(made));
        }
        if (!(v & 1)) {   // (whole until now: its owner's pages, the rest the null window's)
            Region* w = (Region*)v;
            for (uint32_t i = 0; i < kGranulePages; i++) {
                s->page[i].store(w, std::memory_order_relaxed);
                draw_table_set(s->index + i, w ? w->address - w->lo : draw_table_null(g0));
            }
        }
        for (uint64_t q = a; q < b; q += 1ull << kPageShift) {
            const uint32_t i = (uint32_t)(q >> kPageShift & (kGranulePages - 1));
            Region* old = s->page[i].load(std::memory_order_relaxed);
            if (old != r) region_lose(old, 1);
            draw_table_set(s->index + i, distance);
            s->page[i].store(r, std::memory_order_release);
        }
        draw_table_set((uint32_t)k, (uint64_t)s->index << 1 | 1);
        granule_region[k].store((uintptr_t)s | 1, std::memory_order_release);
    }
}
/// a new region's buffer over memory m (mu held): published over guest [lo, hi), the pages' earlier owners losing them
Region* region_publish(uint64_t lo, uint64_t hi, VkBuffer b, VkDeviceMemory m, bool vram) {
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    regions_made.push_back(std::make_unique<Region>(Region{lo, hi, b, m, vkGetBufferDeviceAddress(dev.device, &da), vram}));
    Region* r = regions_made.back().get();
    r->pages = (hi - lo) >> kPageShift;
    region_assign(r, lo, hi);
    st_regions++;
    return r;
}
/// A host region over guest [lo, hi) (pages): one host-pointer import and a plain buffer over it; null on failure (mu held)
/// The guest window's arenas: 64 MB pieces imported once each and kept, which every region inside one binds its buffer into
/// at its offset. An import costs ~11 ms however small (NVIDIA's Windows driver), so a pool a call of its own made a boot of
/// 3000 pools spend 80 s importing; a region that crosses an arena's end (or is larger) has an import of its own
constexpr uint64_t kArena = 64ull << 20;
std::unordered_map<uint64_t, VkDeviceMemory> arenas;   // (mu; by arena index)
int64_t st_arenas = 0, st_own_imports = 0;
VkDeviceMemory arena(uint64_t a) {   // (mu held: arena a's memory, imported the first time; null on failure)
    auto it = arenas.find(a);
    if (it != arenas.end()) return it->second;
    const uint64_t lo = a * kArena, size = std::min(kArena, guest_bytes - lo);
    const VkDeviceMemory m = guest_memory(lo, size);
    if (m) arenas.emplace(a, m), st_arenas++, region_live_bytes += size, st_region_bytes += size;
    return m;
}

Region* region_import(uint64_t lo, uint64_t hi) {
    const auto t0 = clk::now();
    bool in_arena = lo / kArena == (hi - 1) / kArena;
    VkDeviceMemory m = in_arena ? arena(lo / kArena) : VK_NULL_HANDLE;
    if (!m) in_arena = false, m = guest_memory(lo, hi - lo);   // (an arena that cannot be imported: the first, over the null guard)
    if (!m) {
        if (st_region_failed++ < 8) logf("direct: layer 0: region %#llx..%#llx not imported", (unsigned long long)lo, (unsigned long long)hi);
        return nullptr;
    }
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = import_handle;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.size = hi - lo;
    bi.usage = kWindowUsage | query_window_usage();
    VkBuffer b = VK_NULL_HANDLE;
    const VkDeviceSize at = in_arena ? lo % kArena : 0;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &b) != VK_SUCCESS || vkBindBufferMemory(dev.device, b, m, at) != VK_SUCCESS) {
        if (st_region_failed++ < 8) logf("direct: layer 0: region %#llx..%#llx: no buffer over its import", (unsigned long long)lo, (unsigned long long)hi);
        if (b) vkDestroyBuffer(dev.device, b, nullptr);
        if (!in_arena) {
            vkFreeMemory(dev.device, m, nullptr);
            mem_account(MO_L0_IMPORT, import_type, -(int64_t)(hi - lo));
        }
        return nullptr;
    }
    // (a region in an arena owns its buffer alone: the arena's memory stays)
    Region* r = region_publish(lo, hi, b, in_arena ? VK_NULL_HANDLE : m, false);
    if (!in_arena) region_live_bytes += hi - lo, st_region_bytes += hi - lo, st_own_imports++;
    st_region_ms += std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    return r;
}
/// A pool's region (mu held): exactly its pages, taken from whoever held them (a finalized pool's region, a span's)
Region* region_pool(uint64_t lo, uint64_t hi) {
    retired_sweep();
    const uint64_t page = 1ull << kPageShift;
    return region_import(lo / page * page, std::min((hi + page - 1) / page * page, guest_bytes));
}
/// A region holding guest [lo, hi) for a use outside the pools held (the texture data pool's, at its uses): the one there,
/// else a new host region over it and over the regions that hold pages of it (mu held); null when one of those is VRAM
Region* region_cover(uint64_t lo, uint64_t hi) {
    if (Region* r = region_at(lo); r && lo >= r->lo && hi <= r->hi && region_at(hi - 1) == r) return r;
    retired_sweep();
    const uint64_t page = 1ull << kPageShift;
    lo = lo / page * page, hi = std::min((hi + page - 1) / page * page, guest_bytes);
    for (bool grew = true; grew;) {
        grew = false;
        for (uint64_t q = lo; q < hi; q += page)
            if (Region* r = region_at(q)) {
                if (r->vram) return nullptr;
                if (r->lo < lo || r->hi > hi) lo = std::min(lo, r->lo), hi = std::max(hi, r->hi), grew = true, st_region_merges++;
            }
    }
    return region_import(lo, hi);
}
/// A CPU_NO_ACCESS pool in VRAM (the VRAM class, as the windows' step 7b): a region of device-local memory of its own over
/// the pool's pages, when the class has room; false: it stays host (mu held). The CPU never reads or writes such a pool, so
/// its guest pages are not imported; a pool made over its pages later (the game's allocator reusing them once this one
/// is finalized) takes them as any region's, and the region goes
bool region_vram(uint64_t lo, uint64_t hi) {
    const uint64_t page = 1ull << kPageShift;
    if (lo % page || hi % page || hi > guest_bytes || vram_used + (hi - lo) > kVramLimit) return false;
    retired_sweep();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = hi - lo;
    bi.usage = kWindowUsage | query_window_usage();
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &b) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.device, b, &req);
    const bool fits = vram_type != UINT32_MAX && (req.memoryTypeBits >> vram_type & 1);   // (choose_backing's type)
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fl;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vram_type;
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (!fits || vkAllocateMemory(dev.device, &ai, nullptr, &m) != VK_SUCCESS || vkBindBufferMemory(dev.device, b, m, 0) != VK_SUCCESS) {
        if (m) vkFreeMemory(dev.device, m, nullptr);
        vkDestroyBuffer(dev.device, b, nullptr);
        if (st_vram_failed++ < 8) logf("direct: layer 0: pool %#llx+%#llx: no VRAM for it: stays host", (unsigned long long)lo, (unsigned long long)(hi - lo));
        return false;
    }
    mem_account(MO_L0_VRAM, vram_type, (int64_t)(hi - lo));
    vram_used += hi - lo;
    region_publish(lo, hi, b, m, true);
    st_vpools++;
    return true;
}

/// The backing, chosen once from what the driver does with the probe's import (import_probe): the sparse windows when a
/// sparse window buffer takes the import's memory type (its memoryTypeBits) and a bind of the probe into one succeeds;
/// else the regions. RADV takes dma-bufs of system memory into sparse buffers; NVIDIA's Windows driver binds only
/// device-local memory there (and its host-pointer imports are system memory). Frees the probe's memory
void choose_backing(VkDeviceMemory probe, uint64_t probe_size) {
    const char* why = nullptr;
    char note[160] = "";
    if (!(dev.features.sparseBinding && dev.features.sparseResidencyBuffer && dev.features.sparseResidencyAliased))
        why = "no sparse residency of aliased buffers";
    else if (!dev.sparse_queue) why = "no queue for sparse binding";
    VkBuffer b = VK_NULL_HANDLE;
    if (!why) {
        VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        eb.handleTypes = import_handle;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.pNext = &eb;
        bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT;
        bi.size = l0::kWindow;
        bi.usage = kWindowUsage | query_window_usage();
        VkMemoryRequirements req{};
        if (vkCreateBuffer(dev.device, &bi, nullptr, &b) != VK_SUCCESS) why = "no sparse window buffer";
        else if (vkGetBufferMemoryRequirements(dev.device, b, &req), !(req.memoryTypeBits >> import_type & 1))
            why = "a sparse window takes none of the import's memory", std::snprintf(note, sizeof note, " (its types %#x)", req.memoryTypeBits);
        else if (probe_size % req.alignment) why = "the probe is not a whole sparse page";
    }
    if (!why) {   // (the probe bound at the window's start and unbound again)
        VkSparseMemoryBind mb{0, probe_size, probe, 0, 0};
        VkSparseBufferMemoryBindInfo bb{b, 1, &mb};
        VkBindSparseInfo si{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        si.bufferBindCount = 1, si.pBufferBinds = &bb;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkResult r = bind_fence ? VK_SUCCESS : vkCreateFence(dev.device, &fi, nullptr, &bind_fence);   // (submit_binds' fence)
        for (int k = 0; k < 2 && r == VK_SUCCESS; k++) {
            {
                std::lock_guard qk(dev.sparse_shared ? dev.queue_mu : dev.sparse_mu);
                r = vkQueueBindSparse(dev.sparse_queue, 1, &si, bind_fence);
            }
            if (r == VK_SUCCESS) r = vkWaitForFences(dev.device, 1, &bind_fence, VK_TRUE, UINT64_MAX), vkResetFences(dev.device, 1, &bind_fence);
            mb.memory = VK_NULL_HANDLE;
        }
        if (r != VK_SUCCESS) why = "a bind of the import into a sparse window failed", std::snprintf(note, sizeof note, " (VkResult %d)", (int)r);
    }
    regions_mode = why != nullptr;
    // (the VRAM class's memory type: device-local and not host-visible, else device-local; one the backing's buffers take)
    VkMemoryRequirements req{};
    if (regions_mode) {
        if (b) vkDestroyBuffer(dev.device, b, nullptr);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 64 << 10;
        bi.usage = kWindowUsage | query_window_usage();
        b = VK_NULL_HANDLE;
        if (vkCreateBuffer(dev.device, &bi, nullptr, &b) != VK_SUCCESS) b = VK_NULL_HANDLE;
    }
    if (b) vkGetBufferMemoryRequirements(dev.device, b, &req), vkDestroyBuffer(dev.device, b, nullptr);
    for (int pass = 0; pass < 2 && vram_type == UINT32_MAX; pass++)
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && vram_type == UINT32_MAX; i++) {
            const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits >> i & 1) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (pass || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
                vram_type = i;
        }
    vkFreeMemory(dev.device, probe, nullptr);
    if (why) logf("direct: layer 0: backing: regions (%s%s)", why, note);
    else logf("direct: layer 0: backing: sparse windows (memory type %u binds into them)", import_type);
    logf("direct: layer 0: placement: the game's pools in its own pages, imported as memory type %u (flags %#x); CPU_NO_ACCESS "
         "pools in VRAM, memory type %d (flags %#x)", import_type, dev.memory.memoryTypes[import_type].propertyFlags, (int)vram_type,
         vram_type == UINT32_MAX ? 0 : dev.memory.memoryTypes[vram_type].propertyFlags);
}

}  // namespace

bool layer0_on() {
    const int s = state.load(std::memory_order_acquire);
    if (s >= 0) return s != 0;
    if (!dev.device) return false;
    static std::mutex once;
    std::lock_guard lk(once);
    if (state.load(std::memory_order_relaxed) >= 0) return state.load(std::memory_order_relaxed) != 0;
    const char* why = nullptr;
    VkDeviceMemory probe = VK_NULL_HANDLE;
    uint64_t probe_size = 0;
    if (!host->guest_size()) why = "no guest window";
    else if (!export_open()) why = "no export of guest memory";
    else if (!import_probe(probe, probe_size)) why = "guest memory cannot be imported";
    else choose_backing(probe, probe_size);
    if (why) {
        logf("direct: layer 0 off: %s", why);
        state.store(0, std::memory_order_release);
        return false;
    }
    chunk = l0::chunk_bytes(kChunkMb, export_limit);
    guest_bytes = std::min<uint64_t>(host->guest_size(), (uint64_t)l0::kWindows * l0::kStride);
    bits = new l0::ChunkBits(guest_bytes, chunk);
    ready = new l0::ChunkBits(guest_bytes, G);
    chunk_memory.assign(bits->chunks, VK_NULL_HANDLE);
    cstate.reset(new std::atomic<uint8_t>[bits->chunks]);
    for (uint64_t c = 0; c < bits->chunks; c++) cstate[c].store(CH_NONE, std::memory_order_relaxed);
    if (regions_mode) {
        granule_region.reset(new std::atomic<uintptr_t>[(guest_bytes >> kRegionShift) + 1]);
        for (uint64_t j = 0; j <= guest_bytes >> kRegionShift; j++) granule_region[j].store(0, std::memory_order_relaxed);
        logf("direct: layer 0 on: guest memory in regions (a plain buffer each over a pool's pages, in 64 MB imported arenas), "
             "every GPU-readable pool claimed as layer 0's held at its creation; CPU_NO_ACCESS pools in VRAM (up to %llu MB)",
             (unsigned long long)(kVramLimit >> 20));
        state.store(1, std::memory_order_release);
        return true;
    }
    os_thread_release(os_thread_start(worker_main, nullptr, 0, "direct layer 0"));
    logf("direct: layer 0 on: guest memory in 4 GB windows every 2 GB, chunks of %llu MB (an export's limit %llu MB), every "
         "GPU-readable pool claimed as layer 0's bound in full at its creation; CPU_NO_ACCESS pools in VRAM (up to %llu MB, "
         "kept %.0f s)", (unsigned long long)(chunk >> 20), (unsigned long long)(export_limit >> 20), (unsigned long long)(kVramLimit >> 20), kKeepS);
    state.store(1, std::memory_order_release);
    return true;
}

void layer0_pool_created(uint64_t gpu, uint64_t size) {   // (M4f: bound in layer0_pool_native, which has the flags)
    (void)gpu, (void)size;
}

void layer0_pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint32_t claim, uint64_t key, int on) {
    if (!size || gpu >= (1ull << 40)) return;
    if (claim != NVN_CLAIM_LAYER0) {   // (a pool its claim keeps off the GPU: texture storage, render targets, host-only)
        if (on) st_claimed_off.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!layer0_on()) {
        st_early.fetch_add(1, std::memory_order_relaxed);   // (before the device)
        return;
    }
    if (regions_mode) {
        std::lock_guard lk(mu);
        if (on) {
            live.insert({gpu, LivePool{size, flags, false}});
            const int why = (flags & kGpuNoAccess) ? 1 : 0;
            if (why) return void(st_left_pools[why].fetch_add(1, std::memory_order_relaxed));
            st_ahead_pools.fetch_add(1, std::memory_order_relaxed), st_ahead_bytes.fetch_add((int64_t)size, std::memory_order_relaxed);
            if (!(vram_class(size, flags) && region_vram(gpu, gpu + size))) region_pool(gpu, std::min(gpu + size, guest_bytes));
            return;
        }
        for (auto [it, end] = live.equal_range(gpu); it != end; ++it)
            if (it->second.size == size && it->second.flags == flags) {
                live.erase(it);
                break;
            }
        return;
    }
    const bool vr = vram_class(size, flags);
    if (on && vr) {
        {   // (the same pool again, kept since its finalize: handed back, nothing bound)
            std::lock_guard lk(mu);
            if (vram_hand_back(gpu, size, flags, key)) {
                live.insert({gpu, LivePool{size, flags, vr}});
                return;
            }
        }
        uint64_t g0, g1;   // (a VRAM pool over granules the GPU may still use as host memory: the work sent so far done first)
        l0::interior_granules(gpu, gpu + size, g0, g1);
        if (g1 > g0 && ready->any(g0 * G, g1 * G)) wait_for(sync_pushed());
    }
    std::lock_guard lk(mu);
    kept_expire();
    if (on) {
        live.insert({gpu, LivePool{size, flags, vr}});
        if (vr && !vram_hand_back(gpu, size, flags, key)) vram_pool_made(gpu, size, flags, key);
        else vram_clear_over(gpu, gpu + size, true);   // (a CPU-visible pool over VRAM granules: they go back to host)
        bind_ahead(gpu, size, flags, vr);
        return;
    }
    for (auto [it, end] = live.equal_range(gpu); it != end; ++it)
        if (it->second.size == size && it->second.flags == flags) {
            live.erase(it);
            break;
        }
    for (auto it = vpools.begin(); it != vpools.end(); ++it)
        if (it->live && it->base == gpu && it->size == size && it->flags == flags) {
            it->live = false, it->value = sync_pushed(), it->dead_at = clk::now();
            break;
        }
}

bool layer0_bound(uint64_t lo, uint64_t hi) {   // (bound or being imported: a dma-buf may pin its pages any moment now)
    if (regions_mode) {   // (a region's pages stay imported for the process's life)
        if (hi <= lo || state.load(std::memory_order_acquire) != 1) return false;
        for (uint64_t g = lo & ~((1ull << kPageShift) - 1); g < hi && g < guest_bytes; g += 1ull << kPageShift)
            if (region_at(g)) return true;
        return false;
    }
    if (!bits || hi <= lo) return false;
    uint64_t c0, c1;
    l0::chunk_span(lo, hi, chunk, c0, c1);
    for (uint64_t c = c0; c < c1 && c < bits->chunks; c++) {
        const uint8_t st = cstate[c].load(std::memory_order_acquire);
        if (st == CH_IMPORTING || st == CH_BOUND) return true;
    }
    return false;
}

bool layer0_pool_of(uint64_t addr, uint64_t& base, uint64_t& size, uint32_t& flags, bool& in_vram) {
    std::lock_guard lk(mu);
    bool found = false;
    for (auto it = live.begin(); it != live.end() && it->first <= addr; ++it)
        if (addr < it->first + it->second.size) base = it->first, size = it->second.size, flags = it->second.flags, found = true;
    in_vram = false;
    const uint64_t g = addr / G;
    auto r = vram.upper_bound(g);
    if (r != vram.begin() && (--r, g < r->second.g1)) in_vram = true;
    return found;
}

/// (regions) guest [gpu, gpu + size)'s region, made now when none holds it (the texture data pool's uses, ranges outside the
/// pools): read without mu when one does
Region* region_span(uint64_t gpu, uint64_t size) {
    if (!size || gpu >= guest_bytes || state.load(std::memory_order_acquire) != 1) return nullptr;
    const uint64_t hi = std::min(gpu + size, guest_bytes);
    if (Region* r = region_at(gpu); r && gpu >= r->lo && hi <= r->hi && region_at(hi - 1) == r) return r;
    std::lock_guard lk(mu);
    st_span_binds.fetch_add(1, std::memory_order_relaxed);
    return region_cover(gpu, hi);
}

bool layer0_span(uint64_t gpu, uint64_t size, VkBuffer& buffer, VkDeviceSize& offset) {
    if (regions_mode) {
        Region* r = region_span(gpu, size);
        if (!r) return false;
        buffer = r->buffer, offset = gpu - r->lo;
        return true;
    }
    if (!size || gpu >= (1ull << 40) || !l0::fits_window(gpu, size) || state.load(std::memory_order_acquire) != 1) return false;
    const uint64_t hi = std::min(gpu + size, guest_bytes);
    if (hi <= gpu) return false;
    if (!ready->all(gpu, hi)) {   // (not bound ahead: its chunks imported now, on this thread; others' imports waited for)
        uint64_t c0, c1;
        l0::chunk_span(gpu, hi, chunk, c0, c1);
        std::vector<uint64_t> claimed;
        for (uint64_t c = c0; c < c1 && c < bits->chunks; c++)
            for (uint8_t st : {CH_NONE, CH_QUEUED}) {
                uint8_t want = st;
                if (cstate[c].compare_exchange_strong(want, CH_IMPORTING, std::memory_order_acq_rel)) claimed.push_back(c);
            }
        bind_claimed(claimed);
        for (uint64_t c = c0; c < c1 && c < bits->chunks; c++)
            while (cstate[c].load(std::memory_order_acquire) == CH_IMPORTING) std::this_thread::sleep_for(std::chrono::microseconds(100));
        st_span_binds.fetch_add(1, std::memory_order_relaxed);
        if (!ready->all(gpu, hi)) return false;
    }
    // (M3b: a window, once made, never changes: read without mu, which is taken only to make one. The draws call this for
    // every vertex, index and uniform buffer)
    if (VkBuffer made = __atomic_load_n(&windows[l0::window_of(gpu)].buffer, __ATOMIC_ACQUIRE)) {
        buffer = made, offset = l0::window_offset(gpu);
        return true;
    }
    std::lock_guard lk(mu);
    Window* w = window_make(l0::window_of(gpu));
    if (!w) return false;
    buffer = w->buffer, offset = l0::window_offset(gpu);
    return true;
}

/// guest [gpu, gpu + size) as a device address in its window (layer0_span's, bound first if it is not)
bool layer0_address(uint64_t gpu, uint64_t size, VkDeviceAddress& a) {
    if (regions_mode) {
        Region* r = region_span(gpu, size);
        if (!r) return false;
        a = r->address + (gpu - r->lo);
        return true;
    }
    VkBuffer b;
    VkDeviceSize off;
    if (!layer0_span(gpu, size, b, off)) return false;
    a = windows[l0::window_of(gpu)].address + off;
    return true;
}

void layer0_stats(char* out, size_t cap) {
    if (state.load() != 1) {
        std::snprintf(out, cap, "layer 0 off");
        return;
    }
    std::lock_guard lk(mu);
    if (regions_mode) {
        retired_sweep();
        std::snprintf(out, cap, "layer 0: %lld arenas and %lld imports of their own; %lld regions made (%llu MB imported in all, %llu MB live; %lld taken over by others, "
                      "%lld of those freed), %lld merges, %lld failed; VRAM: %lld pools, %llu MB live, %lld not placed; in "
                      "%.1f ms; %lld spans made at their use; pools held at creation %lld (%lld MB), not held: %lld the GPU cannot read, "
                      "%lld its claim keeps off the GPU",
                      (long long)st_arenas, (long long)st_own_imports, (long long)st_regions, (unsigned long long)(st_region_bytes >> 20), (unsigned long long)(region_live_bytes >> 20),
                      (long long)(st_region_freed + (int64_t)retired.size()), (long long)st_region_freed, (long long)st_region_merges,
                      (long long)st_region_failed, (long long)st_vpools, (unsigned long long)(vram_used >> 20), (long long)st_vram_failed,
                      st_region_ms, (long long)st_span_binds.load(), (long long)st_ahead_pools.load(),
                      (long long)(st_ahead_bytes.load() >> 20), (long long)st_left_pools[1].load(), (long long)st_claimed_off.load());
        return;
    }
    int64_t kept = 0, livev = 0;
    for (const VPool& v : vpools) (v.live ? livev : kept)++;
    std::snprintf(out, cap, "layer 0: %lld chunks bound (%llu MB) in %lld windows, %lld bind calls, imports %.1f ms, binds %.1f ms, "
                  "longest %.1f ms, %lld failed; VRAM: %lld pools live, %lld kept, %llu MB in %zu runs (%zu blocks), %lld placed, "
                  "%lld handed back, %lld given back, %lld demoted, %lld not placed, %lld waits (%.1f ms); %lld pool calls before the device; "
                  "%lld copy spans bound at their use; pools bound in full at creation %lld (%lld MB), not bound: %lld the GPU cannot read, "
                  "%lld its claim keeps off the GPU",
                  (long long)st_chunks, (unsigned long long)(st_chunks * (int64_t)chunk >> 20), (long long)st_windows,
                  (long long)st_bind_calls, st_import_ms, st_bind_ms, st_max_ms, (long long)st_failed, (long long)livev, (long long)kept,
                  (unsigned long long)(vram_used >> 20), vram.size(), blocks.size(), (long long)st_vpools, (long long)st_handback,
                  (long long)st_released, (long long)st_demoted, (long long)st_vram_failed, (long long)st_waits, st_wait_ms,
                  (long long)st_early.load(), (long long)st_span_binds.load(), (long long)st_ahead_pools.load(),
                  (long long)(st_ahead_bytes.load() >> 20), (long long)st_left_pools[1].load(), (long long)st_claimed_off.load());
}

}  // namespace hsd
