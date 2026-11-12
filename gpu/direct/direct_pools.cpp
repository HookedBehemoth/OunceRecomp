// Layer 0 (docs/native-vk.md "Layer 0"; plan, Architecture): guest memory for the GPU in window buffers, one backing for every
// pool byte. Cut down from gpu/vk/vk_pools.cpp (its comments have the scheme and the measurements; the arithmetic is
// layer0_windows.h, a verbatim copy) and the dma-buf import of gpu/vk/vk_memory.cpp. As there, with its defaults as constants:
// 16 MB host chunks imported once as udmabufs of the memfd's own pages and bound into the two windows that hold them, ahead
// of use for pools up to 64 MB (an importer thread); CPU_NO_ACCESS pools in VRAM (the VRAM class, step 7b), kept 10 s after
// their finalize and handed back on the same pool again, 2048 MB at most. Left for M2 with the bindings: the per-binding
// path (a range bound at its first use), the shaders' page table and the BDA window table. Linux only.
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <thread>

#include "direct_internal.h"
#include "direct_query.h"
#include "layer0_windows.h"

namespace hsd {
void draw_window_made(uint32_t k, uint64_t a);   // (direct_draw.cpp)
}

namespace hsd {
namespace {

namespace l0 = mkwvk::l0;
using clk = std::chrono::steady_clock;
constexpr uint64_t G = l0::kGran;
constexpr uint32_t kCpuNoAccess = 0x1, kCpuUncached = 0x2, kCpuCached = 0x4, kGpuNoAccess = 0x8, kPhysical = 0x100,
                   kVirtual = 0x200;   // (NVNmemoryPoolFlags)
constexpr uint64_t kBlock = 256ull << 20;
constexpr uint64_t kChunkMb = 16, kPrefetchBytes = 64ull << 20, kVramLimit = 2048ull << 20;
constexpr double kKeepS = 10;

constexpr VkBufferUsageFlags kWindowUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

// ---- dma-bufs of guest memory (vk_memory.cpp dma_buf_memory, dmabuf_ready) ----
struct UdmabufCreate { uint32_t memfd, flags; uint64_t offset, size; };
constexpr unsigned long kUdmabufCreate = (1ul << 30) | (sizeof(UdmabufCreate) << 16) | ('u' << 8) | 0x42;   // (_IOW('u', 0x42, ...))
int dmabuf_memfd = -1, dmabuf_dev = -1, dmabuf_failures = 0;
uint64_t dmabuf_limit = 0;

bool dmabuf_open() {   // (once, at layer0_on)
    typedef int (*MemfdFn)(void);
    const MemfdFn fn = (MemfdFn)dlsym(RTLD_DEFAULT, "hos_posix_guest_memfd");
    const int fd = fn ? fn() : -1;
    if (fd >= 0 && dev.dma_buf_import) dmabuf_dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (fd < 0 || dmabuf_dev < 0) {
        logf("direct: layer 0: guest memory is not a memfd shared as dma-bufs (%s)", !fn ? "no memfd window" : fd < 0 ? "anonymous memory"
                                                                                                               : "/dev/udmabuf cannot be opened");
        return false;
    }
    dmabuf_memfd = fd;
    dmabuf_limit = 64ull << 20;
    if (FILE* f = fopen("/sys/module/udmabuf/parameters/size_limit_mb", "r")) {
        unsigned long long mb = 0;
        if (fscanf(f, "%llu", &mb) == 1 && mb) dmabuf_limit = mb << 20;
        fclose(f);
    }
    return true;
}
/// The memory of guest [lo, lo + size) as an imported dma-buf; VK_NULL_HANDLE when udmabuf or the import refused it
VkDeviceMemory dma_buf_memory(uint64_t lo, uint64_t size) {
    UdmabufCreate c{(uint32_t)dmabuf_memfd, 1 /* UDMABUF_FLAGS_CLOEXEC */, lo, size};
    const int fd = ioctl(dmabuf_dev, kUdmabufCreate, &c);
    if (fd < 0) {
        if (dmabuf_failures++ < 8) logf("direct: guest %#llx..%#llx: no udmabuf (errno %d)", (unsigned long long)lo, (unsigned long long)(lo + size), errno);
        return VK_NULL_HANDLE;
    }
    static uint32_t type = UINT32_MAX;   // (the same for every dma-buf of the window: asked once; the importer and the binders)
    static std::mutex type_mu;
    {
        std::lock_guard lk(type_mu);
        if (type == UINT32_MAX) {
            VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
            if (vkGetMemoryFdPropertiesKHR(dev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) == VK_SUCCESS) {
                constexpr VkMemoryPropertyFlags hc = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)   // (host visible, coherent, first)
                    if ((fp.memoryTypeBits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & hc) == hc) type = i;
                for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)
                    if (fp.memoryTypeBits >> i & 1) type = i;
            }
        }
    }
    if (type == UINT32_MAX) {
        close(fd);
        if (dmabuf_failures++ < 8) logf("direct: a dma-buf of guest memory has no memory type");
        return VK_NULL_HANDLE;
    }
    VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    im.fd = fd;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    im.pNext = &fl;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &im;
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult r = vkAllocateMemory(dev.device, &ai, nullptr, &memory);
    if (r != VK_SUCCESS) {   // (the fd is the caller's again when the import fails)
        close(fd);
        if (dmabuf_failures++ < 8)
            logf("direct: guest %#llx..%#llx: its dma-buf not imported (VkResult %d)", (unsigned long long)lo, (unsigned long long)(lo + size), (int)r);
        return VK_NULL_HANDLE;
    }
    return memory;
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
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
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
    if (vram_type == UINT32_MAX) {   // (the VRAM class's memory type: device-local, not host-visible, one the windows take)
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev.device, b, &req);
        for (int pass = 0; pass < 2 && vram_type == UINT32_MAX; pass++)
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && vram_type == UINT32_MAX; i++) {
                const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
                if ((req.memoryTypeBits >> i & 1) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (pass || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
                    vram_type = i;
            }
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
        std::lock_guard qk(dev.sparse_mu);
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
        const VkDeviceMemory m = dma_buf_memory(c * chunk, chunk);
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
        for (const auto& [c, m] : got) vkFreeMemory(dev.device, m, nullptr), chunk_memory[c] = VK_NULL_HANDLE, failed.push_back(c);
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
void worker_main() {
    pthread_setname_np(pthread_self(), "direct layer 0");
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

}  // namespace

bool layer0_on() {
    const int s = state.load(std::memory_order_acquire);
    if (s >= 0) return s != 0;
    if (!dev.device) return false;
    static std::mutex once;
    std::lock_guard lk(once);
    if (state.load(std::memory_order_relaxed) >= 0) return state.load(std::memory_order_relaxed) != 0;
    const char* why = nullptr;
    if (!(dev.features.sparseBinding && dev.features.sparseResidencyBuffer && dev.features.sparseResidencyAliased))
        why = "no sparse residency of aliased buffers";
    else if (!host->guest_size()) why = "no guest window";
    else if (!dmabuf_open()) why = "no dma-bufs of guest memory";
    if (why) {
        logf("direct: layer 0 off: %s", why);
        state.store(0, std::memory_order_release);
        return false;
    }
    chunk = l0::chunk_bytes(kChunkMb, dmabuf_limit);
    guest_bytes = std::min<uint64_t>(host->guest_size(), (uint64_t)l0::kWindows * l0::kStride);
    bits = new l0::ChunkBits(guest_bytes, chunk);
    ready = new l0::ChunkBits(guest_bytes, G);
    chunk_memory.assign(bits->chunks, VK_NULL_HANDLE);
    cstate.reset(new std::atomic<uint8_t>[bits->chunks]);
    for (uint64_t c = 0; c < bits->chunks; c++) cstate[c].store(CH_NONE, std::memory_order_relaxed);
    std::thread(worker_main).detach();
    logf("direct: layer 0 on: guest memory in 4 GB windows every 2 GB, chunks of %llu MB (udmabuf's limit %llu MB), pools up to "
         "%llu MB bound ahead; CPU_NO_ACCESS pools in VRAM (up to %llu MB, kept %.0f s)", (unsigned long long)(chunk >> 20),
         (unsigned long long)(dmabuf_limit >> 20), (unsigned long long)(kPrefetchBytes >> 20), (unsigned long long)(kVramLimit >> 20), kKeepS);
    state.store(1, std::memory_order_release);
    return true;
}

void layer0_pool_created(uint64_t gpu, uint64_t size) {
    if (!size || gpu >= (1ull << 40) || !layer0_on()) return;   // (virtual pools are ranges of physical ones)
    if (size <= kPrefetchBytes) prefetch_pool(gpu, std::min(gpu + size, guest_bytes));
}

void layer0_pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint64_t key, int on) {
    if (!size || gpu >= (1ull << 40)) return;
    if (!layer0_on()) {
        st_early.fetch_add(1, std::memory_order_relaxed);   // (before the device)
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
    if (!bits || hi <= lo) return false;
    uint64_t c0, c1;
    l0::chunk_span(lo, hi, chunk, c0, c1);
    for (uint64_t c = c0; c < c1 && c < bits->chunks; c++) {
        const uint8_t st = cstate[c].load(std::memory_order_acquire);
        if (st == CH_IMPORTING || st == CH_BOUND) return true;
    }
    return false;
}

bool layer0_span(uint64_t gpu, uint64_t size, VkBuffer& buffer, VkDeviceSize& offset) {
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

/// (M2b) guest [gpu, gpu + size) as a device address in its window (layer0_span's, bound first if it is not)
bool layer0_address(uint64_t gpu, uint64_t size, VkDeviceAddress& a) {
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
    int64_t kept = 0, livev = 0;
    for (const VPool& v : vpools) (v.live ? livev : kept)++;
    std::snprintf(out, cap, "layer 0: %lld chunks bound (%llu MB) in %lld windows, %lld bind calls, imports %.1f ms, binds %.1f ms, "
                  "longest %.1f ms, %lld failed; VRAM: %lld pools live, %lld kept, %llu MB in %zu runs (%zu blocks), %lld placed, "
                  "%lld handed back, %lld given back, %lld demoted, %lld not placed, %lld waits (%.1f ms); %lld pool calls before the device; "
                  "%lld copy spans bound at their use",
                  (long long)st_chunks, (unsigned long long)(st_chunks * (int64_t)chunk >> 20), (long long)st_windows,
                  (long long)st_bind_calls, st_import_ms, st_bind_ms, st_max_ms, (long long)st_failed, (long long)livev, (long long)kept,
                  (unsigned long long)(vram_used >> 20), vram.size(), blocks.size(), (long long)st_vpools, (long long)st_handback,
                  (long long)st_released, (long long)st_demoted, (long long)st_vram_failed, (long long)st_waits, st_wait_ms,
                  (long long)st_early.load(), (long long)st_span_binds.load());
}

}  // namespace hsd
