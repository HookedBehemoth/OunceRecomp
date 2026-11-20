// The memory the GPU reads the game's pools through, by their claims (renderer/api/nvn_claims.h,
// docs/direct/pool-claims.md). A pool whose claim the GPU reads gets a region: one plain buffer over the pool's guest
// range, whose device addresses run on contiguously, and the address table's entries (bda_emit.h: one per 64 KB granule,
// the distance from guest to device address) for its granules pointing into it. A granule that regions share (the game
// leaves one page between pools) is split into 16 page entries. Claims that keep a pool off the GPU (textures, render
// targets, host-only memory) get no region: an address there reads the null window.
//
// A VRAM claim's region is device-local memory of the back end's own. A layer-0 claim's (until its pools move to their
// own kinds) is the pool's guest pages imported: a plain buffer bound into an arena, a 64 MB piece of the guest window
// imported once (dma-bufs of the memfd's pages on Linux, host pointers on Windows; an import costs ~11 ms however small),
// or an import of its own when it crosses an arena's end. A finalized pool's region leaves the table at once and is freed
// once the GPU is past the work recorded with it.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "vk_internal.h"
#include "vk_check.h"
#include "vk_pool.h"
#include "vk_query.h"
#include "nvn_claims.h"
#include "os_guest.h"

namespace hsd {
void draw_table_set(uint32_t index, uint64_t value);   // (vk_draw.cpp)
uint64_t draw_table_null(uint64_t guest);
}

namespace hsd {
namespace {

using clk = std::chrono::steady_clock;
constexpr uint64_t kGuestLimit = 1ull << 39;   // (the address table's reach: 512 GB of guest space)
constexpr uint32_t kGranuleShift = 16, kPageShift = 12;   // (bda_emit.h kTableShift, and its pages)
constexpr uint32_t kGranulePages = 1u << (kGranuleShift - kPageShift);
constexpr uint32_t kSplitFirst = (1u << (39 - kGranuleShift)) + 8, kSplits = 1u << 16;   // (bda_emit.h's table layout)
constexpr uint64_t kPage = 1ull << kPageShift;

constexpr VkBufferUsageFlags kRegionUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

std::atomic<int> state{-1};   // (-1: not started; 0 off, 1 on)
std::mutex mu;                // (regions made, assigned and retired; the pools)
uint64_t guest_bytes = 0;

// ---- layer-0 claims: guest memory exported to the GPU (os_guest.h), imported by the handle's kind ----
int import_failures = 0;
uint64_t export_limit = 0;
/// The memory type every import takes and the export's handle kind: chosen once by import_probe at claims_on
uint32_t import_type = UINT32_MAX;
VkExternalMemoryHandleTypeFlagBits import_handle{};

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
/// The memory of guest [lo, lo + size) as an import of its export; VK_NULL_HANDLE when the export or the import refused it
VkDeviceMemory guest_memory(uint64_t lo, uint64_t size) {
    os_gpu_handle h;
    if (const int r = os_guest_export(lo, size, &h)) {
        if (import_failures++ < 8) logf("direct: guest %#llx..%#llx: not exported (%d)", (unsigned long long)lo, (unsigned long long)(lo + size), r);
        return VK_NULL_HANDLE;
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (h.kind == OS_GPU_DMABUF_FD) {
        r = import_as(import_type, size, h.fd, nullptr, memory);
        if (r != VK_SUCCESS) os_gpu_handle_close(&h);   // (the fd is the caller's again when the import fails)
    } else if (h.kind == OS_GPU_HOST_POINTER) {
        if (!(((uintptr_t)h.ptr | size) % (dev.host_align ? dev.host_align : 1))) r = import_as(import_type, size, -1, h.ptr, memory);
    } else {
        os_gpu_handle_close(&h);
    }
    if (r != VK_SUCCESS) {
        if (import_failures++ < 8)
            logf("direct: guest %#llx..%#llx: not imported (VkResult %d)", (unsigned long long)lo, (unsigned long long)(lo + size), (int)r);
        return VK_NULL_HANDLE;
    }
    mem_account(MO_CLAIM_IMPORT, import_type, (int64_t)size);
    return memory;
}

/// The import's memory type, chosen once from one export of guest memory (64 KB of the system region): of the types the
/// driver lists for the handle, the first that imports. Nothing maps the imported memory (the CPU reaches those pages
/// through the guest window), so the type matters for how the GPU reaches them:
/// - dma-bufs: host-visible and coherent first (RADV: the GPU's accesses snoop the CPU's caches), then the rest. NVIDIA's
///   Linux driver lists a host-visible type for a dma-buf of system memory and refuses it, and imports its device type;
/// - host pointers: host-visible, coherent and cached first (NVIDIA's Windows driver gives an uncached type and a cached
///   one, and shaders that loop over a CPU-visible buffer, OoT's light sums, ran 100x slower on the first), then
///   host-visible and coherent, then the rest.
bool import_probe() {
    const char* note = nullptr;
    if (!os_guest_exportable(&export_limit, &note)) return logf("direct: claims: guest memory is not exported (%s)", note ? note : "-"), false;
    const uint64_t at = 16ull << 20, size = std::max<uint64_t>(64 << 10, dev.host_align);
    os_gpu_handle h;
    if (const int r = os_guest_export(at, size, &h)) return logf("direct: claims: guest memory not exported (%d)", r), false;
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
    VkDeviceMemory probe = VK_NULL_HANDLE;
    for (int pass = 0; pass <= 3 && import_type == UINT32_MAX; pass++)
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && import_type == UINT32_MAX; i++) {
            const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
            if (!(bits >> i & 1) || (done >> i & 1) || (pass < 3 && (!first[pass] || (f & first[pass]) != first[pass]))) continue;
            done |= 1u << i;
            const VkResult r = import_as(i, size, h.kind == OS_GPU_DMABUF_FD ? h.fd : -1, h.kind == OS_GPU_HOST_POINTER ? h.ptr : nullptr, probe);
            tried += " " + std::to_string(i) + ":" + std::to_string(f) + "=" + std::to_string((int)r);
            if (r == VK_SUCCESS) import_type = i;
        }
    if (import_type == UINT32_MAX) os_gpu_handle_close(&h);   // (a dma-buf's fd is the memory's once imported)
    if (probe) vkFreeMemory(dev.device, probe, nullptr);
    logf("direct: claims: layer-0 pools imported as %s, memory type %d (of types %#x; tried type:flags=VkResult:%s)",
         h.kind == OS_GPU_DMABUF_FD ? "dma-bufs" : h.kind == OS_GPU_HOST_POINTER ? "host pointers" : "nothing", (int)import_type, bits, tried.c_str());
    return import_type != UINT32_MAX;
}

// ---- regions ----
struct Region {
    uint64_t lo, hi;
    VkBuffer buffer;
    VkDeviceMemory memory;   // (its own: a VRAM claim's, or an import of its own; null in an arena)
    VkDeviceAddress address;
    uint32_t claim;
    uint64_t pages = 0;      // (of its pages, how many the table still gives it; none: it is retired)
    uint32_t block = UINT32_MAX;   // (a VRAM region's place in the arenas)
    uint64_t block_off = 0, block_size = 0;
};
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
/// A region out of the table (its pool finalized, or its pages taken by another): its buffer and memory freed once
/// nothing can use them. A second after it left, the newest submission is noted (every list recorded before then has
/// been sent); once the GPU is past that, it goes
struct Retired {
    Region* r;
    clk::time_point at;
    uint64_t value = 0;
};
std::vector<Retired> retired;   // (mu)
struct Pool {
    uint64_t size;
    uint32_t flags, claim;
    Region* region;
};
std::multimap<uint64_t, Pool> pools;   // (mu; every pool the game has, by base)
uint32_t vram_type = UINT32_MAX;
uint64_t vram_live = 0, import_live = 0;
int64_t st_regions = 0, st_failed = 0, st_freed = 0, st_split_full = 0, st_arenas = 0, st_own_imports = 0;
int64_t st_by_claim[NVN_CLAIM_KINDS] = {};
std::atomic<int64_t> st_span_missed{0};

Region* region_at(uint64_t g) {   // (the region holding guest g's page; read without mu)
    if (g >= guest_bytes) return nullptr;
    const uintptr_t v = granule_region[g >> kGranuleShift].load(std::memory_order_acquire);
    if (!(v & 1)) return (Region*)v;
    return ((Split*)(v & ~(uintptr_t)1))->page[g >> kPageShift & (kGranulePages - 1)].load(std::memory_order_acquire);
}
void block_give(uint32_t bi, uint64_t off, uint64_t size);   // (below: the VRAM arenas)
void retired_sweep() {   // (mu held)
    const auto now = clk::now();
    for (size_t i = 0; i < retired.size();) {
        Retired& t = retired[i];
        if (!t.value && now - t.at >= std::chrono::seconds(1)) t.value = sync_pushed();
        if (!t.value || sync_completed() < t.value) {
            i++;
            continue;
        }
        Region* r = t.r;
        vkDestroyBuffer(dev.device, r->buffer, nullptr);
        if (r->block != UINT32_MAX) block_give(r->block, r->block_off, r->block_size), vram_live -= r->block_size;
        if (r->memory) {   // (an import of its own)
            vkFreeMemory(dev.device, r->memory, nullptr);
            mem_account(MO_CLAIM_IMPORT, import_type, -(int64_t)(r->hi - r->lo)), import_live -= r->hi - r->lo;
        }
        r->buffer = VK_NULL_HANDLE, r->memory = VK_NULL_HANDLE;
        st_freed++;
        retired[i] = retired.back();
        retired.pop_back();
    }
}
/// `pages` pages of `from` taken from it: retired when it has none left (mu held)
void region_lose(Region* from, uint64_t pages) {
    if (!from || !from->pages) return;
    from->pages -= std::min(from->pages, pages);
    if (!from->pages) retired.push_back({from, clk::now()});
}
/// r's pages [lo, hi) in the table and the owner array (r null: the null window's), whole granules as one entry, shared
/// ones split (mu held). The table is written first: a reader that finds r there finds its entries
void region_assign(Region* r, uint64_t lo, uint64_t hi) {
    const uint64_t gsize = 1ull << kGranuleShift;
    for (uint64_t k = lo >> kGranuleShift; k <= (hi - 1) >> kGranuleShift; k++) {
        const uint64_t g0 = k << kGranuleShift, a = std::max(lo, g0), b = std::min(hi, g0 + gsize);
        const uint64_t distance = r ? r->address - r->lo : draw_table_null(g0);
        const uintptr_t v = granule_region[k].load(std::memory_order_relaxed);
        if (a == g0 && b == g0 + gsize) {   // (the whole granule)
            if (v & 1) {
                Split* s = (Split*)(v & ~(uintptr_t)1);
                for (auto& p : s->page)
                    if (Region* o = p.load(std::memory_order_relaxed); o != r) region_lose(o, 1);
            } else if ((Region*)v != r) {
                region_lose((Region*)v, kGranulePages);
            }
            draw_table_set((uint32_t)k, distance);
            granule_region[k].store((uintptr_t)r, std::memory_order_release);
            continue;
        }
        // (a part: the granule split, its pages' entries made from their owner first when it was whole)
        Split* s;
        auto it = splits.find(k);
        if (it != splits.end()) {
            s = it->second.get();
        } else {
            if (splits_used >= kSplits) {   // (no room: the granule goes to r whole)
                if (st_split_full++ < 8) logf("direct: claims: no room for another split granule (%u): granule %#llx is the newest region's", kSplits, (unsigned long long)g0);
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
        for (uint64_t q = a; q < b; q += kPage) {
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
/// a new region's buffer (mu held): published over guest [lo, hi), the pages' earlier owners losing them
Region* region_publish(uint64_t lo, uint64_t hi, VkBuffer b, VkDeviceMemory m, uint32_t claim) {
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    regions_made.push_back(std::make_unique<Region>(Region{lo, hi, b, m, vkGetBufferDeviceAddress(dev.device, &da), claim}));
    Region* r = regions_made.back().get();
    r->pages = (hi - lo) >> kPageShift;
    region_assign(r, lo, hi);
    st_regions++;
    return r;
}
VkBuffer region_buffer(uint64_t size, const void* next) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = next;
    bi.size = size;
    bi.usage = kRegionUsage | query_window_usage();
    VkBuffer b = VK_NULL_HANDLE;
    return vkCreateBuffer(dev.device, &bi, nullptr, &b) == VK_SUCCESS ? b : VK_NULL_HANDLE;
}

/// The guest window's arenas: 64 MB pieces imported once each and kept, which every layer-0 region inside one binds its
/// buffer into at its offset
constexpr uint64_t kArena = 64ull << 20;
std::unordered_map<uint64_t, VkDeviceMemory> arenas;   // (mu; by arena index)
VkDeviceMemory arena(uint64_t a) {   // (mu held: arena a's memory, imported the first time; null on failure)
    auto it = arenas.find(a);
    if (it != arenas.end()) return it->second;
    const uint64_t lo = a * kArena, size = std::min(kArena, guest_bytes - lo);
    const VkDeviceMemory m = guest_memory(lo, size);
    if (m) arenas.emplace(a, m), st_arenas++, import_live += size;
    return m;
}
/// A layer-0 claim's region over guest pages [lo, hi): a plain buffer over their import; null on failure (mu held)
Region* region_import(uint64_t lo, uint64_t hi) {
    bool in_arena = lo / kArena == (hi - 1) / kArena;
    VkDeviceMemory m = in_arena ? arena(lo / kArena) : VK_NULL_HANDLE;
    if (!m) in_arena = false, m = guest_memory(lo, hi - lo);   // (an arena that cannot be imported: the first, over the null guard)
    if (!m) return nullptr;
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = import_handle;
    const VkBuffer b = region_buffer(hi - lo, &eb);
    if (!b || vkBindBufferMemory(dev.device, b, m, in_arena ? lo % kArena : 0) != VK_SUCCESS) {
        if (b) vkDestroyBuffer(dev.device, b, nullptr);
        if (!in_arena) vkFreeMemory(dev.device, m, nullptr), mem_account(MO_CLAIM_IMPORT, import_type, -(int64_t)(hi - lo));
        return nullptr;
    }
    if (!in_arena) import_live += hi - lo, st_own_imports++;
    return region_publish(lo, hi, b, in_arena ? VK_NULL_HANDLE : m, NVN_CLAIM_LAYER0);
}
/// The VRAM arenas: device-local blocks of kBlock (a pool larger than that has a block of its size), which VRAM regions
/// are sub-allocated from, first fit; a range goes back when its region is freed (mu)
constexpr uint64_t kBlock = 256ull << 20;
struct Block {
    VkDeviceMemory mem;
    uint64_t size;
    std::map<uint64_t, uint64_t> free;   // (offset -> length)
};
std::vector<Block> blocks;
bool block_take(uint64_t size, uint64_t align, uint32_t type_bits, uint32_t& bi, uint64_t& off) {
    for (uint32_t i = 0; i < blocks.size(); i++)
        for (auto& [o, len] : blocks[i].free) {
            const uint64_t a = (o + align - 1) / align * align;
            if (a + size > o + len) continue;
            const uint64_t o0 = o, end = o + len;
            blocks[i].free.erase(o0);
            if (a > o0) blocks[i].free[o0] = a - o0;
            if (a + size < end) blocks[i].free[a + size] = end - (a + size);
            bi = i, off = a;
            return true;
        }
    if (vram_type == UINT32_MAX || !(type_bits >> vram_type & 1)) return false;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fl;
    ai.allocationSize = std::max(kBlock, (size + align - 1) / align * align);
    ai.memoryTypeIndex = vram_type;
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev.device, &ai, nullptr, &m) != VK_SUCCESS) return false;
    mem_account(MO_CLAIM_VRAM, vram_type, (int64_t)ai.allocationSize);
    blocks.push_back(Block{m, ai.allocationSize, {}});
    if (size < ai.allocationSize) blocks.back().free[size] = ai.allocationSize - size;
    bi = (uint32_t)blocks.size() - 1, off = 0;
    return true;
}
void block_give(uint32_t bi, uint64_t off, uint64_t size) {   // (mu held; the GPU is past every use of it)
    auto& f = blocks[bi].free;
    auto it = f.emplace(off, size).first;
    if (auto n = std::next(it); n != f.end() && it->first + it->second == n->first) it->second += n->second, f.erase(n);
    if (it != f.begin())
        if (auto p = std::prev(it); p->first + p->second == it->first) p->second += it->second, f.erase(it);
}
/// A VRAM claim's region over guest pages [lo, hi): a buffer in a VRAM arena. The CPU never reads or writes such a
/// pool; the GPU's copies fill it. Null on failure (mu held)
Region* region_vram(uint64_t lo, uint64_t hi) {
    const VkBuffer b = region_buffer(hi - lo, nullptr);
    if (!b) return nullptr;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.device, b, &req);
    uint32_t bi;
    uint64_t off;
    if (!block_take(req.size, req.alignment, req.memoryTypeBits, bi, off) || vkBindBufferMemory(dev.device, b, blocks[bi].mem, off) != VK_SUCCESS) {
        vkDestroyBuffer(dev.device, b, nullptr);
        return nullptr;
    }
    vram_live += req.size;
    Region* r = region_publish(lo, hi, b, VK_NULL_HANDLE, NVN_CLAIM_VRAM);
    r->block = bi, r->block_off = off, r->block_size = req.size;
    return r;
}
/// a region out of the table: the pages it still holds the null window's again, and it retired (mu held)
void region_drop(Region* r) {
    if (!r || !r->pages) return;
    for (uint64_t q = r->lo; q < r->hi && r->pages; q += kPage) {
        if (region_at(q) != r) continue;
        uint64_t e = q + kPage;
        while (e < r->hi && region_at(e) == r) e += kPage;
        region_assign(nullptr, q, e);
        q = e - kPage;
    }
}

}  // namespace

bool claims_on() {
    const int s = state.load(std::memory_order_acquire);
    if (s >= 0) return s != 0;
    if (!dev.device) return false;
    static std::mutex once;
    std::lock_guard lk(once);
    if (state.load(std::memory_order_relaxed) >= 0) return state.load(std::memory_order_relaxed) != 0;
    guest_bytes = std::min<uint64_t>(host->guest_size(), kGuestLimit);
    if (!guest_bytes || !import_probe()) {
        logf("direct: claims off: %s", guest_bytes ? "guest memory cannot be imported" : "no guest window");
        state.store(0, std::memory_order_release);
        return false;
    }
    // (the VRAM claims' memory type: device-local and not host-visible, else device-local; one regions' buffers take)
    VkMemoryRequirements req{};
    if (const VkBuffer b = region_buffer(64 << 10, nullptr)) vkGetBufferMemoryRequirements(dev.device, b, &req), vkDestroyBuffer(dev.device, b, nullptr);
    for (int pass = 0; pass < 2 && vram_type == UINT32_MAX; pass++)
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && vram_type == UINT32_MAX; i++) {
            const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits >> i & 1) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (pass || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
                vram_type = i;
        }
    granule_region.reset(new std::atomic<uintptr_t>[(guest_bytes >> kGranuleShift) + 1]);
    for (uint64_t j = 0; j <= guest_bytes >> kGranuleShift; j++) granule_region[j].store(0, std::memory_order_relaxed);
    logf("direct: claims on: a region per pool the GPU reads; VRAM claims in memory type %d, layer-0 claims imported as type %u",
         (int)vram_type, import_type);
    state.store(1, std::memory_order_release);
    return true;
}

void claims_pool(uint64_t gpu, uint64_t size, uint32_t flags, uint32_t claim, int on) {
    if (!size || gpu >= guest_bytes || claim >= NVN_CLAIM_KINDS || !claims_on()) return;
    std::lock_guard lk(mu);
    retired_sweep();
    if (!on) {
        for (auto [it, end] = pools.equal_range(gpu); it != end; ++it)
            if (it->second.size == size && it->second.flags == flags) {
                region_drop(it->second.region);
                pools.erase(it);
                break;
            }
        return;
    }
    st_by_claim[claim]++;
    Region* r = nullptr;
    const uint64_t lo = gpu / kPage * kPage, hi = std::min((gpu + size + kPage - 1) / kPage * kPage, guest_bytes);
    const bool gpu_reads = !(flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS);
    if (claim == NVN_CLAIM_LAYER0 && gpu_reads) r = region_import(lo, hi);
    else if (claim == NVN_CLAIM_VRAM) r = region_vram(lo, hi);
    else if (claim != NVN_CLAIM_LAYER0 && claim != NVN_CLAIM_TEXTURES && claim != NVN_CLAIM_TARGETS && claim != NVN_CLAIM_HOST)
        logf("direct: claims: pool %#llx+%#llx: claim %u has no memory yet", (unsigned long long)gpu, (unsigned long long)size, claim);
    if ((claim == NVN_CLAIM_VRAM || (claim == NVN_CLAIM_LAYER0 && gpu_reads)) && !r && st_failed++ < 8)
        logf("direct: claims: pool %#llx+%#llx flags %#x (claim %u): no region", (unsigned long long)gpu, (unsigned long long)size, flags, claim);
    pools.insert({gpu, Pool{size, flags, claim, r}});
}

bool claims_bound(uint64_t lo, uint64_t hi) {   // (an import may pin the pages: the host keeps them committed)
    if (hi <= lo || state.load(std::memory_order_acquire) != 1) return false;
    for (uint64_t g = lo & ~(kPage - 1); g < hi && g < guest_bytes; g += kPage)
        if (Region* r = region_at(g); r && r->claim == NVN_CLAIM_LAYER0) return true;
    return false;
}

bool claims_pool_of(uint64_t addr, uint64_t& base, uint64_t& size, uint32_t& flags, bool& in_vram) {
    std::lock_guard lk(mu);
    bool found = false;
    for (auto it = pools.begin(); it != pools.end() && it->first <= addr; ++it)
        if (addr < it->first + it->second.size) base = it->first, size = it->second.size, flags = it->second.flags, found = true;
    Region* r = state.load(std::memory_order_acquire) == 1 ? region_at(addr) : nullptr;
    in_vram = r && r->claim == NVN_CLAIM_VRAM;
    return found;
}

bool claims_holds(uint64_t gpu, uint64_t size) {
    if (!size || gpu >= guest_bytes || state.load(std::memory_order_acquire) != 1) return false;
    const uint64_t hi = std::min(gpu + size, guest_bytes);
    Region* r = region_at(gpu);
    return r && gpu >= r->lo && hi <= r->hi && region_at(hi - 1) == r;
}

bool claims_span(uint64_t gpu, uint64_t size, VkBuffer& buffer, VkDeviceSize& offset) {
    if (!size || gpu >= guest_bytes || state.load(std::memory_order_acquire) != 1) return false;
    Region* r = region_at(gpu);
    if (!claims_holds(gpu, size)) {
        if (st_span_missed.fetch_add(1, std::memory_order_relaxed) < 8) {   // (named: the pool and its claim, which keeps it off the GPU)
            uint64_t base = 0, psize = 0;
            uint32_t flags = 0, claim = NVN_CLAIM_NONE;
            {
                std::lock_guard lk(mu);
                for (auto it = pools.begin(); it != pools.end() && it->first <= gpu; ++it)
                    if (gpu < it->first + it->second.size) base = it->first, psize = it->second.size, flags = it->second.flags, claim = it->second.claim;
            }
            strict_event("a GPU use of memory no region holds", "%#llx+%#llx in pool %#llx+%#llx flags %#x, claimed %u", (unsigned long long)gpu,
                         (unsigned long long)size, (unsigned long long)base, (unsigned long long)psize, flags, claim);
            logf("direct: claims: ERROR: %#llx+%#llx used by the GPU, outside a region: pool %#llx+%#llx flags %#x, claim %u", (unsigned long long)gpu,
                 (unsigned long long)size, (unsigned long long)base, (unsigned long long)psize, flags, claim);
        }
        return false;
    }
    buffer = r->buffer, offset = gpu - r->lo;
    return true;
}

bool claims_address(uint64_t gpu, uint64_t size, VkDeviceAddress& a) {
    VkBuffer b;
    VkDeviceSize off;
    if (!claims_span(gpu, size, b, off)) return false;
    a = region_at(gpu)->address + off;
    return true;
}

void claims_stats(char* out, size_t cap) {
    if (state.load() != 1) return void(std::snprintf(out, cap, "claims off"));
    std::lock_guard lk(mu);
    retired_sweep();
    std::snprintf(out, cap, "claims: pools made by claim: textures %lld, targets %lld, host %lld, vram %lld, upload %lld, readback %lld, "
                  "layer 0 %lld; regions %lld made, %lld freed, %zu retiring, %lld failed; VRAM %llu MB live; imports %llu MB live (%lld "
                  "arenas, %lld of their own); spans outside a region %lld",
                  (long long)st_by_claim[NVN_CLAIM_TEXTURES], (long long)st_by_claim[NVN_CLAIM_TARGETS], (long long)st_by_claim[NVN_CLAIM_HOST],
                  (long long)st_by_claim[NVN_CLAIM_VRAM], (long long)st_by_claim[NVN_CLAIM_UPLOAD], (long long)st_by_claim[NVN_CLAIM_READBACK],
                  (long long)st_by_claim[NVN_CLAIM_LAYER0], (long long)st_regions, (long long)st_freed, retired.size(), (long long)st_failed,
                  (unsigned long long)(vram_live >> 20), (unsigned long long)(import_live >> 20), (long long)st_arenas,
                  (long long)st_own_imports, (long long)st_span_missed.load());
}

}  // namespace hsd
