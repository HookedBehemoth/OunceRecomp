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
#include <cstring>
#include <deque>
#include <tuple>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "vk_internal.h"
#include "vk_check.h"
#include "vk_lists.h"
#include "vk_readback.h"
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
    const char* by = nullptr;      // (its pool's claiming patch and flags: the check's and the measurement's names)
    uint32_t flags = 0;
    struct Track* track = nullptr; // (debug.claimuse: its pages' hashes)
    std::unique_ptr<std::atomic<uint64_t>[]> copied;   // (a load-once claim's pages copied so far, a bit each: set by the preparing thread)
    // (an Upload claim's 16-byte grains that submitted lists read since its last copy, a bit each, and its words with any
    // set, a bit each; listed: in dirty_regions)
    std::unique_ptr<std::atomic<uint64_t>[]> dirty, dirty_sum;
    uint64_t dirty_words = 0;
    // (its 16-byte grains that GPU work writes, a bit each, made at the first such write: the GPU's from the first
    // submission that writes them. Bound ranges never copy them again, so a copy cannot overwrite what the GPU wrote)
    std::atomic<std::atomic<uint64_t>*> gpu{nullptr};
    std::atomic<bool> listed{false};
    // (a VRAM claim's ranges the CPU reads, merged and sorted (reads_mu): copied out after each submission that writes it)
    std::atomic<bool> has_reads{false};
    std::vector<std::pair<uint64_t, uint64_t>> reads;
};
std::mutex reads_mu;
/// the bits [a, b) of a 64-bit word (b <= 64)
inline uint64_t bits(uint64_t a, uint64_t b) { return (b == 64 ? ~0ull : (1ull << b) - 1) & ~((1ull << a) - 1); }
/// a load-once region's pages [lo, hi) all copied: its uses need no note
bool all_copied(const Region* r, uint64_t lo, uint64_t hi) {
    const uint64_t p0 = (lo - r->lo) >> kPageShift, p1 = ((hi - r->lo - 1) >> kPageShift) + 1;
    for (uint64_t w = p0 >> 6; w <= (p1 - 1) >> 6; w++) {
        const uint64_t m = bits(w == p0 >> 6 ? p0 & 63 : 0, w == (p1 - 1) >> 6 ? ((p1 - 1) & 63) + 1 : 64);
        if ((r->copied[w].load(std::memory_order_relaxed) & m) != m) return false;
    }
    return true;
}

/// Upload regions with grains to copy at the next submission's first chunk (dirty_mu)
constexpr uint32_t kGrainShift = 4;
std::mutex dirty_mu;
std::vector<Region*> dirty_regions;
/// The grains of a submission's Upload ranges, a word's bits gathered over its lists and set at once. They are set at the
/// guest's submit, so what the CPU wrote before it is copied, by whichever submission's first chunk goes next (the copy
/// runs before its work, and before every later one's). Words then their summary, then the list: the taker clears them in
/// the other order (seq_cst), so a grain set while it takes is either taken or left for the next
struct DirtyAcc {
    Region* r = nullptr;
    uint64_t w = 0, m = 0;
    void flush() {
        if (!m) return;
        if ((r->dirty[w].load() & m) != m) r->dirty[w].fetch_or(m);
        const uint64_t sm = 1ull << (w & 63);
        if (!(r->dirty_sum[w >> 6].load() & sm)) r->dirty_sum[w >> 6].fetch_or(sm);
        if (!r->listed.load() && !r->listed.exchange(true)) {
            std::lock_guard lk(dirty_mu);
            dirty_regions.push_back(r);
        }
        m = 0;
    }
    ~DirtyAcc() { flush(); }
};
void mark_dirty(DirtyAcc& acc, Region* r, uint64_t lo, uint64_t hi) {
    const uint64_t g0 = (lo - r->lo) >> kGrainShift, g1 = ((hi - r->lo - 1) >> kGrainShift) + 1;
    for (uint64_t w = g0 >> 6; w <= (g1 - 1) >> 6; w++) {
        const uint64_t m = bits(w == g0 >> 6 ? g0 & 63 : 0, w == (g1 - 1) >> 6 ? ((g1 - 1) & 63) + 1 : 64);
        if (acc.r != r || acc.w != w) acc.flush(), acc.r = r, acc.w = w;
        acc.m |= m;
    }
}
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
    const char* by;
};
std::multimap<uint64_t, Pool> pools;   // (mu; every pool the game has, by base)
uint32_t vram_type = UINT32_MAX;
std::vector<Region*> whole_uploads;   // (mu; regions whose bytes come from guest memory, made since the last submit: copied whole then)
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
std::atomic<uint64_t> assign_gen{1};   // (region_assign's calls: the span caches' generation)
void region_assign(Region* r, uint64_t lo, uint64_t hi) {
    assign_gen.fetch_add(1, std::memory_order_acq_rel);
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
    regions_made.push_back(std::make_unique<Region>());
    Region* r = regions_made.back().get();
    r->lo = lo, r->hi = hi, r->buffer = b, r->memory = m, r->address = vkGetBufferDeviceAddress(dev.device, &da), r->claim = claim;
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
/// A VRAM, Upload or Readback claim's region over guest pages [lo, hi): a buffer in a device-local arena. The GPU reads
/// and writes only it; staged copies move the CPU's writes in (Upload) and the GPU's out (Readback). Null on failure
/// (mu held)
Region* region_local(uint64_t lo, uint64_t hi, uint32_t claim) {
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
    Region* r = region_publish(lo, hi, b, VK_NULL_HANDLE, claim);
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


// ---- the check and the measurement (debug.claimcheck, debug.claimuse): the guest ranges each submission's lists use ----
bool tracking = false, track_check = false, track_use = false, track_all = false;   // (set once at claims_on)
bool track_census = false;   // (debug.poolcensus: each submission's ranges told to the runtime's census, runtime/poolcensus.c)
const char* const kUseNames[CU_KINDS] = {"uniform", "vertex", "index", "indirect", "storage", "storage written", "copy source", "copy destination",
                                         "fill", "texel copy", "condition", "task record"};
/// the uses after which the GPU may have written the range
constexpr uint32_t kGpuWrites = 1u << CU_STORAGE_WRITTEN | 1u << CU_COPY_DST | 1u << CU_FILL | 1u << CU_CONDITION;
/// the uses that write the range: a Readback claim's are copied out after the submission
constexpr uint32_t kOutUses = 1u << CU_STORAGE_WRITTEN | 1u << CU_COPY_DST | 1u << CU_FILL;
/// a region of the back end's own memory whose bytes come from guest memory: copies move the CPU's writes in
bool local_copied(const Region* r) {
    return r->claim == NVN_CLAIM_UPLOAD || r->claim == NVN_CLAIM_READBACK || r->claim == NVN_CLAIM_LOAD_ONCE ||
           (r->claim == NVN_CLAIM_VRAM && !(r->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS));
}

uint64_t hash_bytes(const uint8_t* p, uint64_t n) {   // (a page's: the measurement's change test)
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n;
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
/// (debug.claimuse) a region's pages: their hashes when the pool was made and when a submission last used them, which have
/// changed since it was made, and the presents then
struct Track {
    uint64_t made_at;
    std::vector<uint64_t> made, seen;
    std::vector<uint8_t> changed;   // (since it was made)
};
uint64_t page_hash(uint64_t g) {
    uint8_t b[kPage];
    return host->gpu_read(g, b, kPage) ? hash_bytes(b, kPage) : 0;
}
Track* track_made(Region* r) {   // (mu held)
    if (!track_use) return nullptr;
    Track* t = new Track;
    t->made_at = present_count();
    const uint64_t n = (r->hi - r->lo) >> kPageShift;
    t->made.resize(n), t->seen.resize(n), t->changed.assign(n, 0);
    for (uint64_t i = 0; i < n; i++) t->made[i] = t->seen[i] = page_hash(r->lo + (i << kPageShift));
    return t;
}

/// A submission's ranges, merged by region: what the check snapshots and compares, and what the measurement counts
struct SubRange {
    uint64_t lo, hi;
    Region* r;
    uint32_t uses;   // (bit ClaimUse)
};
/// Staging: host-visible chunks, each holding one copy's bytes (in or out) until its submission completes (check_mu)
struct Staging {
    VkBuffer buf = VK_NULL_HANDLE;
    Mem* mem = nullptr;
    uint8_t* map = nullptr;
    uint64_t size = 0, busy = 0;   // (busy: the value that frees it; UINT64_MAX while a submission holds it)
};
std::vector<Staging> staging;
std::atomic<int64_t> st_memcpy_ns{0}, st_note_ns{0}, st_up_once{0}, st_up_subs{0}, st_up_ranges{0}, st_up_bytes{0}, st_up_whole{0}, st_up_ns{0}, st_up_skipped_gpu{0};
int64_t staging_take(uint64_t n) {   // (check_mu held: a chunk of at least n bytes no work uses, made when none is)
    const uint64_t done = sync_completed();
    int best = -1;
    for (size_t i = 0; i < staging.size(); i++)
        if (staging[i].busy <= done && staging[i].size >= n && (best < 0 || staging[i].size < staging[best].size)) best = (int)i;
    if (best < 0) {
        Staging c;
        c.size = std::max<uint64_t>(16ull << 20, (n + (1 << 20) - 1) & ~((1ull << 20) - 1));
        void* map = nullptr;
        if (!mem_buffer(c.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, c.buf, c.mem, &map, MO_UPLOAD) || !map)
            return -1;
        c.map = (uint8_t*)map;
        staging.push_back(c);
        best = (int)staging.size() - 1;
    }
    staging[best].busy = UINT64_MAX;
    return best;
}

/// a copy from guest memory into a region
struct Up {
    Region* r;
    uint64_t lo, hi;
};
struct ClaimSubImpl {
    std::vector<Up> once;   // (the load-once ranges the lists marked now read, not all copied)
    std::vector<Up> outs;   // (the Readback ranges its work writes)
    // (its copy out: each range's place in its staging chunk, copied into guest memory once it completes)
    struct Out {
        uint64_t lo, hi;
        int64_t chunk;
        uint64_t at;
    };
    std::vector<Out> out_sent;
    uint64_t seq = 0;       // (its first copy's number: the CPU's reports after it drop their ranges from its copy out)
    bool has_outs = false;
    std::vector<Up> gpu_writes;   // (the ranges their GPU work writes: the GPU's grains from now on)
    std::vector<SubRange> ranges;   // (the check and the measurement: the marked lists' ranges merged by region)
    uint64_t frame = 0, run_frame = 0, run_behind = 0;   // (presents at its submit and at its first chunk; values the GPU was behind)
    // (the check, a part per copy: the guest bytes when the copy was made, and the region's bytes the GPU copied before the
    // work)
    struct Part {
        std::vector<SubRange> checked;
        std::vector<uint8_t> want;
        VkBuffer buf = VK_NULL_HANDLE;
        Mem* mem = nullptr;
        uint8_t* map = nullptr;
    };
    std::vector<Part> parts;
    std::vector<VkCommandBuffer> cbs;   // (its copies' command buffers: its first chunk's, and one after each wait it was held at)
    std::vector<uint32_t> sites;   // (the lists' return addresses, for the event's text)
    uint64_t census = 0;           // (debug.poolcensus: its number there)
    // (each range it copied from staging: refreshed at a held event wait from the guest bytes then, and compared by the check
    // once it completes, before any wait on it returns: a difference is a CPU write while the GPU may still read the range)
    struct Sent {
        uint64_t lo, hi;
        Region* r;
        int64_t chunk;   // (its staging chunk: held until the submission completes)
        uint64_t at;
    };
    std::vector<Sent> sent;
    std::vector<int64_t> chunks;
    // (the check: the bytes a refresh copied again into the staging the first copy reads; that copy holds the old or the
    // new ones, as the GPU ran it before or after, so the check leaves them out)
    std::vector<std::pair<uint64_t, uint64_t>> refreshed;
};
void gather_list(const List* l, std::vector<BoundRange>& out, std::vector<uint32_t>& sites, int depth) {
    out.insert(out.end(), l->bound.begin(), l->bound.end());
    if (!l->bound.empty() && sites.size() < 8) sites.push_back((uint32_t)l->info.lr);
    if (depth > 16) return;
    for (const Seg& s : l->segs)
        if (s.callee) gather_list(s.callee, out, sites, depth + 1);
    for (const List* j : l->executed) gather_list(j, out, sites, depth + 1);
}

// (debug.claimuse) per pool group (claiming patch, flags, claim): bytes and ranges a frame by use, the merged bytes, and
// the pages' changes; the data shaders reach through stored addresses
struct UseAgg {
    double merged = 0, by_use[CU_KINDS] = {}, ranges = 0, pointed = 0;
    uint64_t pages_used = 0, pages_changed_since_made = 0, page_changes = 0, first_change_after = UINT64_MAX;
    std::vector<std::string> pointer_sites;
    struct Big {
        uint64_t hi;
        uint32_t uses;
        double bytes;
    };
    std::unordered_map<uint64_t, Big> ranges_by_lo;   // (its merged ranges, for the largest)
};
std::mutex use_mu;
std::map<std::tuple<std::string, uint32_t, uint32_t>, UseAgg> use_agg;
int64_t use_frames_from = -1;
UseAgg& agg_of(const Region* r) {   // (use_mu held)
    return use_agg[{r->by ? r->by : "?", r->flags, r->claim}];
}
void measure(ClaimSubImpl& s, const std::vector<BoundRange>& raw) {
    std::lock_guard lk(use_mu);
    if (use_frames_from < 0) use_frames_from = (int64_t)present_count();
    for (const BoundRange& b : raw) {
        Region* r = region_at(b.lo);
        if (!r) continue;
        UseAgg& a = agg_of(r);
        a.by_use[b.use] += b.size, a.ranges++;
    }
    for (const SubRange& x : s.ranges) {
        UseAgg& a = agg_of(x.r);
        a.merged += (double)(x.hi - x.lo);
        if (a.ranges_by_lo.size() < 8192 || a.ranges_by_lo.count(x.lo)) {
            UseAgg::Big& g = a.ranges_by_lo[x.lo];
            g.hi = std::max(g.hi, x.hi), g.uses |= x.uses, g.bytes += (double)(x.hi - x.lo);
        }
        Track* t = __atomic_load_n(&x.r->track, __ATOMIC_ACQUIRE);
        if (!t) continue;
        for (uint64_t g = x.lo & ~(kPage - 1); g < x.hi; g += kPage) {
            const uint64_t i = (g - x.r->lo) >> kPageShift;
            if (i >= t->made.size()) break;
            const uint64_t h = page_hash(g);
            a.pages_used++;
            if (h != t->seen[i]) a.page_changes++, t->seen[i] = h;
            if (h != t->made[i] && !t->changed[i]) {
                t->changed[i] = 1, a.pages_changed_since_made++;
                a.first_change_after = std::min<uint64_t>(a.first_change_after, present_count() - t->made_at);
            }
        }
    }
}

// (debug.claimcheck) the check's command buffers: a pool of its own, used under vk_sync.cpp's mu, each one given back once
// the GPU is past it
PFN_vkCmdCopyBuffer copy_buffer = nullptr;
PFN_vkCmdFillBuffer fill_buffer = nullptr;
std::mutex check_mu;
VkCommandPool check_pool = VK_NULL_HANDLE;
std::vector<VkCommandBuffer> check_free;
std::atomic<int64_t> st_checked_ranges{0}, st_checked_bytes{0}, st_mismatches{0}, st_skipped_gpu_written{0};
VkCommandBuffer check_buffer() {
    std::lock_guard lk(check_mu);
    if (!check_pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = dev.queue_family;
        vkCreateCommandPool(dev.device, &pi, nullptr, &check_pool);
        copy_buffer = (PFN_vkCmdCopyBuffer)vkGetDeviceProcAddr(dev.device, "vkCmdCopyBuffer");
        fill_buffer = (PFN_vkCmdFillBuffer)vkGetDeviceProcAddr(dev.device, "vkCmdFillBuffer");
    }
    if (!check_free.empty()) {
        VkCommandBuffer cb = check_free.back();
        check_free.pop_back();
        vkResetCommandBuffer(cb, 0);
        return cb;
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = check_pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev.device, &ai, &cb);
    return cb;
}
/// The pool holding guest g, named: its base, size, flags, claim and claiming patch
std::string pool_name(uint64_t g) {
    std::lock_guard lk(mu);
    for (auto it = pools.begin(); it != pools.end() && it->first <= g; ++it)
        if (g < it->first + it->second.size) {
            char b[200];
            std::snprintf(b, sizeof b, "pool %#llx+%#llx flags %#x claimed %u by %s", (unsigned long long)it->first,
                          (unsigned long long)it->second.size, it->second.flags, it->second.claim, it->second.by ? it->second.by : "?");
            return b;
        }
    return "no pool";
}
/// r's grains [lo, hi) the GPU's (see Region::gpu)
void mark_gpu_written(Region* r, uint64_t lo, uint64_t hi) {
    std::atomic<uint64_t>* g = r->gpu.load(std::memory_order_acquire);
    if (!g) {
        auto* made = new std::atomic<uint64_t>[((((r->hi - r->lo) >> kGrainShift) + 63) >> 6)]();
        if (r->gpu.compare_exchange_strong(g, made, std::memory_order_acq_rel)) g = made;
        else delete[] made;
    }
    const uint64_t g0 = (lo - r->lo) >> kGrainShift, g1 = ((hi - r->lo - 1) >> kGrainShift) + 1;
    for (uint64_t w = g0 >> 6; w <= (g1 - 1) >> 6; w++) {
        const uint64_t m = bits(w == g0 >> 6 ? g0 & 63 : 0, w == (g1 - 1) >> 6 ? ((g1 - 1) & 63) + 1 : 64);
        if ((g[w].load(std::memory_order_relaxed) & m) != m) g[w].fetch_or(m, std::memory_order_relaxed);
    }
}
/// any grain of r's [lo, hi) the GPU's
bool any_gpu_written(const Region* r, uint64_t lo, uint64_t hi) {
    const std::atomic<uint64_t>* g = r->gpu.load(std::memory_order_acquire);
    if (!g) return false;
    const uint64_t g0 = (lo - r->lo) >> kGrainShift, g1 = ((hi - r->lo - 1) >> kGrainShift) + 1;
    for (uint64_t w = g0 >> 6; w <= (g1 - 1) >> 6; w++) {
        const uint64_t m = bits(w == g0 >> 6 ? g0 & 63 : 0, w == (g1 - 1) >> 6 ? ((g1 - 1) & 63) + 1 : 64);
        if (g[w].load(std::memory_order_relaxed) & m) return true;
    }
    return false;
}
std::mutex miss_mu;
std::map<std::string, int64_t> misses;   // (the check's mismatches by pool group and use, since the start)
std::atomic<int64_t> st_late_writes{0};
/// (the check) the uploaded ranges the CPU wrote since their copy, the submission complete
void compare_sent(ClaimSubImpl& s) {
    for (const auto& x : s.sent) {
        const uint64_t n = x.hi - x.lo;
        const uint8_t* was;
        {
            std::lock_guard lk(check_mu);
            was = staging[x.chunk].map + x.at;
        }
        uint64_t avail = 0;
        const uint8_t* now = host->gpu_ptr(x.lo, &avail);
        if (!now || avail < n || !std::memcmp(now, was, n)) continue;
        uint64_t first = 0;
        while (first < n && now[first] == was[first]) first++;
        if (st_late_writes.fetch_add(1, std::memory_order_relaxed) < 12) {
            char lists[120];
            int k = 0;
            lists[0] = 0;
            for (uint32_t lr : s.sites) k += std::snprintf(lists + k, k < (int)sizeof lists ? sizeof lists - k : 0, " %#x", lr);
            logf("direct: claims: check: %#llx+%#llx written by the CPU after its copy, before the submission completed, first at "
                 "+%#llx: %s; submitted at present %llu, completed at %llu; lists%s", (unsigned long long)x.lo, (unsigned long long)n,
                 (unsigned long long)first, pool_name(x.lo).c_str(), (unsigned long long)s.frame, (unsigned long long)present_count(), lists);
        }
    }
}
void compare_part(ClaimSubImpl& s, ClaimSubImpl::Part& p) {
    if (!p.map) return;
    mem_invalidate(p.mem);
    uint64_t at = 0;
    for (const SubRange& x : p.checked) {
        const uint64_t n = x.hi - x.lo;
        if (std::memcmp(p.want.data() + at, p.map + at, n)) {
            uint64_t first = 0;
            for (;; first++) {   // (the first difference no refresh explains)
                while (first < n && p.want[at + first] == p.map[at + first]) first++;
                if (first == n) break;
                bool refreshed = false;
                for (const auto& [lo, hi] : s.refreshed) refreshed |= x.lo + first >= lo && x.lo + first < hi;
                if (!refreshed) break;
            }
            if (first == n) {
                at += n;
                continue;
            }            std::string uses;
            for (int u = 0; u < CU_KINDS; u++)
                if (x.uses >> u & 1) uses += std::string(uses.empty() ? "" : "+") + kUseNames[u];
            char lists[120];
            int k = 0;
            lists[0] = 0;
            for (uint32_t lr : s.sites) k += std::snprintf(lists + k, k < (int)sizeof lists ? sizeof lists - k : 0, " %#x", lr);
            const std::string pool = pool_name(x.lo);
            int64_t seen;
            {
                std::lock_guard lk(miss_mu);
                char key[160];
                std::snprintf(key, sizeof key, "%s flags %#x (%s)", x.r->by ? x.r->by : "?", x.r->flags, uses.c_str());
                seen = misses[key]++;
            }
            st_mismatches.fetch_add(1, std::memory_order_relaxed);
            if (seen < 4)   // (the first of each pool group and use)
                logf("direct: claims: ERROR: check: %#llx+%#llx (%s) differs from guest memory at +%#llx: %s; submitted at present %llu, "
                     "sent at %llu with the GPU %llu values behind, compared at %llu; lists%s",
                     (unsigned long long)x.lo, (unsigned long long)n, uses.c_str(), (unsigned long long)first, pool.c_str(),
                     (unsigned long long)s.frame, (unsigned long long)s.run_frame, (unsigned long long)s.run_behind,
                     (unsigned long long)present_count(), lists);
            strict_event("a bound range the GPU reads differs from guest memory", "%#llx+%#llx (%s): %s", (unsigned long long)x.lo,
                         (unsigned long long)n, uses.c_str(), pool.c_str());
        }
        at += n;
    }
    mem_buffer_free(p.buf, p.mem);
    p.buf = VK_NULL_HANDLE, p.mem = nullptr, p.map = nullptr;
}
void compare(ClaimSubImpl& s) {
    compare_sent(s);
    for (ClaimSubImpl::Part& p : s.parts) compare_part(s, p);
}


}  // namespace

struct ClaimSub : ClaimSubImpl {
    std::vector<List*> lists;   // (referenced until it completes)
};

namespace {
/// list l's ranges, its callees' and the secondaries' it executes: Upload grains marked, load-once ranges and GPU writes
/// kept for the copy being made
void note_list(ClaimSub* s, DirtyAcc& acc, const List* l, int depth) {
    for (const BoundRange& b : l->bound) {
        Region* r = (Region*)b.region;
        if (!r->pages) continue;   // (retired)
        const uint64_t hi = std::min<uint64_t>(b.lo + b.size, r->hi);
        if ((1u << b.use) & kGpuWrites) {
            s->gpu_writes.push_back({r, b.lo, hi});
            if (r->claim == NVN_CLAIM_READBACK && (1u << b.use) & kOutUses) s->outs.push_back({r, b.lo, hi});
            else if (r->has_reads.load(std::memory_order_acquire) && (1u << b.use) & kOutUses) {
                std::lock_guard lk(reads_mu);
                for (const auto& [a, e] : r->reads)
                    if (a < hi && e > b.lo) s->outs.push_back({r, std::max(a, b.lo), std::min(e, hi)});
            }
        } else if (r->claim == NVN_CLAIM_UPLOAD || r->claim == NVN_CLAIM_READBACK) {
            mark_dirty(acc, r, b.lo, hi);
        }
        else if (r->claim == NVN_CLAIM_LOAD_ONCE && !all_copied(r, b.lo, hi)) s->once.push_back({r, b.lo, hi});
    }
    if (depth > 16) return;
    for (const Seg& g : l->segs)
        if (g.callee) note_list(s, acc, g.callee, depth + 1);
    for (const List* j : l->executed) note_list(s, acc, j, depth + 1);
}
}  // namespace

namespace {
bool any_outs(const List* l, int depth) {
    if (l->outs) return true;
    if (depth > 16) return false;
    for (const Seg& g : l->segs)
        if (g.callee && any_outs(g.callee, depth + 1)) return true;
    for (const List* j : l->executed)
        if (any_outs(j, depth + 1)) return true;
    return false;
}
}  // namespace

ClaimSub* claims_sub_new(List* const* lists, size_t n) {
    if (!tracking) return nullptr;
    auto* s = new ClaimSub;
    for (size_t i = 0; i < n; i++)
        if (lists[i]) list_ref(lists[i]), s->lists.push_back(lists[i]), s->has_outs = s->has_outs || any_outs(lists[i], 0);
    s->frame = present_count();
    if (track_census && (s->census = host->census_begin())) {   // (its ranges as the guest submits it)
        std::vector<BoundRange> raw;
        std::vector<uint32_t> sites;
        for (List* l : s->lists) gather_list(l, raw, sites, 0);
        for (const BoundRange& b : raw)
            if (((const Region*)b.region)->pages) host->census_use(s->census, b.lo, b.size, b.use, (1u << b.use) & kGpuWrites ? 1 : 0);
    }
    return s;
}

bool claims_sub_outs(const ClaimSub* s) { return s->has_outs; }



namespace {
/// (the check and the measurement) the lists' ranges merged by region
void gather(ClaimSub* s) {
    std::vector<BoundRange> raw;
    for (List* l : s->lists) gather_list(l, raw, s->sites, 0);
    if (raw.empty()) return;
    // (in address order, each range's region as it was noted: a retired one is left out)
    auto lt = [](const BoundRange& a, const BoundRange& b) { return a.lo < b.lo; };
    if (!std::is_sorted(raw.begin(), raw.end(), lt)) std::sort(raw.begin(), raw.end(), lt);
    for (const BoundRange& b : raw) {
        Region* r = (Region*)b.region;
        if (!r->pages) continue;
        const uint64_t hi = std::min<uint64_t>(b.lo + b.size, r->hi);
        if (!s->ranges.empty() && s->ranges.back().r == r && b.lo <= s->ranges.back().hi) {
            SubRange& x = s->ranges.back();
            x.hi = std::max(x.hi, hi), x.uses |= 1u << b.use;
            continue;
        }
        s->ranges.push_back({b.lo, hi, r, 1u << b.use});
    }
    if (track_use) measure(*s, raw);
}
}  // namespace

namespace {
/// guest [lo, hi) of region r (grain-aligned) into ups, the grains the GPU writes left out: those are the GPU's, and the
/// CPU's writes there reach the GPU only through a writer's report
void up_add(std::vector<Up>& ups, Region* r, uint64_t lo, uint64_t hi) {
    auto add = [&](uint64_t a, uint64_t b) {
        if (!ups.empty() && ups.back().r == r && ups.back().hi >= a) ups.back().hi = std::max(ups.back().hi, b);
        else ups.push_back({r, a, b});
    };
    const std::atomic<uint64_t>* g = r->gpu.load(std::memory_order_acquire);
    if (!g) return add(lo, hi);   // (the common case: no grain of it is the GPU's)
    const uint64_t g0 = (lo - r->lo) >> kGrainShift, g1 = ((hi - r->lo - 1) >> kGrainShift) + 1;
    for (uint64_t w = g0 >> 6; w <= (g1 - 1) >> 6; w++) {   // (the grains no GPU work writes, in runs)
        const uint64_t m = bits(w == g0 >> 6 ? g0 & 63 : 0, w == (g1 - 1) >> 6 ? ((g1 - 1) & 63) + 1 : 64);
        const uint64_t gw = g[w].load(std::memory_order_relaxed);
        if (gw & m) st_up_skipped_gpu.fetch_add(1, std::memory_order_relaxed);
        for (uint64_t x = ~gw & m; x;) {
            const uint32_t b = (uint32_t)__builtin_ctzll(x);
            const uint64_t y = ~(x >> b);
            const uint32_t e = y ? std::min<uint32_t>(64, b + (uint32_t)__builtin_ctzll(y)) : 64;
            add(r->lo + ((w << 6 | b) << kGrainShift), std::min(hi, r->lo + (((w << 6) + e) << kGrainShift)));
            x &= e == 64 ? 0 : ~0ull << e;
        }
    }
}
/// Upload region r's marked grains, cleared, into ups in runs (see mark_dirty)
void take_dirty(Region* r, std::vector<Up>& ups) {
    r->listed.store(false);
    uint64_t run_lo = 0, run_hi = 0;   // (grains)
    auto flush = [&] {
        if (run_hi > run_lo) up_add(ups, r, r->lo + (run_lo << kGrainShift), std::min(r->hi, r->lo + (run_hi << kGrainShift)));
    };
    for (uint64_t sw = 0; sw < (r->dirty_words + 63) >> 6; sw++) {
        if (!r->dirty_sum[sw].load(std::memory_order_relaxed)) continue;
        for (uint64_t sm = r->dirty_sum[sw].exchange(0); sm; sm &= sm - 1) {
            const uint64_t w = sw << 6 | (uint64_t)__builtin_ctzll(sm);
            for (uint64_t x = r->dirty[w].exchange(0); x;) {
                const uint32_t b = (uint32_t)__builtin_ctzll(x);
                const uint64_t y = ~(x >> b);
                const uint32_t e = y ? std::min<uint32_t>(64, b + (uint32_t)__builtin_ctzll(y)) : 64;
                const uint64_t g0 = w << 6 | b, g1 = (w << 6) + e;
                if (g0 == run_hi) run_hi = g1;
                else flush(), run_lo = g0, run_hi = g1;
                x &= e == 64 ? 0 : ~0ull << e;
            }
        }
    }
    flush();
}
}  // namespace

namespace {
std::atomic<int64_t> st_refreshes{0}, st_refreshed{0}, st_refresh_ns{0};
/// After a held event wait: each range s copied whose guest bytes changed since, copied again (256 bytes at a time)
/// through its staging bytes, before the work after the wait
VkCommandBuffer refresh(ClaimSub* s) {
    const auto t0 = std::chrono::steady_clock::now();
    VkCommandBuffer cb = check_buffer();
    s->cbs.push_back(cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    std::vector<VkBufferCopy> cs;
    const ClaimSubImpl::Sent* group = nullptr;   // (the copies so far: from its chunk into its region)
    bool any = false;
    auto flush = [&] {
        if (cs.empty()) return;
        if (!any) record_dep(cb, Dep{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                                     VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0}), any = true;
        Staging* st;
        {
            std::lock_guard lk(check_mu);
            st = &staging[group->chunk];
            mem_flush(st->mem);
            copy_buffer(cb, st->buf, group->r->buffer, (uint32_t)cs.size(), cs.data());
        }
        cs.clear();
    };
    for (const ClaimSubImpl::Sent& x : s->sent) {
        uint8_t* was;
        {
            std::lock_guard lk(check_mu);
            was = staging[x.chunk].map + x.at;
        }
        uint64_t avail = 0;
        const uint8_t* now = host->gpu_ptr(x.lo, &avail);
        if (!now || avail < x.hi - x.lo) continue;
        for (uint64_t o = 0; o < x.hi - x.lo; o += 256) {
            const uint64_t n = std::min<uint64_t>(256, x.hi - x.lo - o);
            if (!std::memcmp(now + o, was + o, n)) continue;
            std::memcpy(was + o, now + o, n);
            if (track_check) s->refreshed.push_back({x.lo + o, x.lo + o + n});
            if (group && (group->chunk != x.chunk || group->r != x.r)) flush();
            group = &x;
            if (!cs.empty() && cs.back().srcOffset + cs.back().size == x.at + o && cs.back().dstOffset + cs.back().size == x.lo - x.r->lo + o)
                cs.back().size += n;
            else
                cs.push_back({x.at + o, x.lo - x.r->lo + o, n});
            st_refreshed.fetch_add((int64_t)n, std::memory_order_relaxed);
        }
    }
    flush();
    if (any)
        record_dep(cb, Dep{VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, 0});
    vkEndCommandBuffer(cb);
    st_refreshes.fetch_add(1, std::memory_order_relaxed);
    st_refresh_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
    return cb;
}
}  // namespace

namespace {
// ---- the CPU's reported writes (claims_cpu_write) and the copies out of Readback ranges ----
std::mutex report_mu;
std::vector<std::pair<uint64_t, uint64_t>> reported;   // (report_mu: the CPU's writes for the next first copy)
/// a range the CPU wrote after the first copy of submissions up to `upto` was made: dropped from their copies out, so the
/// GPU's older value does not land over the CPU's write (report_mu; in order of upto)
struct Cancel {
    uint64_t lo, hi, upto;
};
std::deque<Cancel> cancels;
struct Fill {
    uint64_t lo, hi;
    uint32_t value;
};
std::vector<Fill> fills;   // (report_mu: the resets for the next first copy, in their order)
uint64_t first_seq = 0;   // (report_mu: first copies made)
std::atomic<int64_t> st_fills{0}, st_fill_bytes{0}, st_read_windows{0};
std::atomic<int64_t> st_reports{0}, st_report_bytes{0}, st_out_subs{0}, st_out_ranges{0}, st_out_bytes{0}, st_out_dropped{0};

// (debug.claimcheck) the pages the GPU owns that the CPU can write: a VRAM claim's from its copy, and those of Upload and
// Readback claims that GPU work writes. Their guest bytes change only through a reported write or a copy out; any other
// change is a CPU write the GPU never sees, found every 600 presents
std::mutex owned_mu;
struct Owned {
    uint64_t hash, mask;   // (its owned bytes' hash, and which grains those were)
};
std::unordered_map<uint64_t, Owned> owned;   // (by page)
std::atomic<int64_t> st_owned_writes{0};
/// which of page p's grains the GPU owns, hashed (owned_hash's mask)
uint64_t owned_mask(const Region* r, uint64_t p) {
    if (r->claim == NVN_CLAIM_VRAM) return ~0ull;
    const std::atomic<uint64_t>* g = r->gpu.load(std::memory_order_acquire);
    uint64_t mask = 0x9e3779b97f4a7c15ull;
    const uint64_t g0 = (p - r->lo) >> kGrainShift;
    for (uint32_t k = 0; k < (kPage >> kGrainShift) / 64; k++)
        mask = (mask ^ (g ? g[(g0 >> 6) + k].load(std::memory_order_relaxed) : 0)) * 0xff51afd7ed558ccdull;
    return mask;
}
/// page p's bytes the GPU owns, hashed: a VRAM claim's whole page, else the grains GPU work writes
Owned owned_hash(uint64_t p) {
    const Region* r = region_at(p);
    uint8_t b[kPage];
    if (!r || !host->gpu_read(p, b, kPage)) return {0, 0};
    if (r->claim == NVN_CLAIM_VRAM) return {hash_bytes(b, kPage), ~0ull};
    const std::atomic<uint64_t>* g = r->gpu.load(std::memory_order_acquire);
    uint64_t mask = 0x9e3779b97f4a7c15ull;
    const uint64_t g0 = (p - r->lo) >> kGrainShift;   // (a page's 256 grains: 4 words, word-aligned)
    for (uint32_t k = 0; k < (kPage >> kGrainShift) / 64; k++) {
        const uint64_t m = g ? g[(g0 >> 6) + k].load(std::memory_order_relaxed) : 0;
        mask = (mask ^ m) * 0xff51afd7ed558ccdull;
        for (uint32_t i = 0; i < 64; i++)
            if (!(m >> i & 1)) std::memset(b + ((k * 64 + i) << kGrainShift), 0, 1u << kGrainShift);
    }
    return {hash_bytes(b, kPage), mask};
}
void own_pages(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(owned_mu);
    for (uint64_t p = lo & ~(kPage - 1); p < hi; p += kPage)
        if (const Region* r = region_at(p); r && local_copied(r)) {
            auto it = owned.find(p);
            if (it == owned.end()) owned.emplace(p, owned_hash(p));
            else if (owned_mask(r, p) != it->second.mask) it->second = owned_hash(p);   // (more grains the GPU's)
        }
}
void owned_rehash(uint64_t lo, uint64_t hi) {   // (owned_mu held)
    for (uint64_t p = lo & ~(kPage - 1); p < hi; p += kPage)
        if (auto it = owned.find(p); it != owned.end()) it->second = owned_hash(p);
}
void owned_scan() {
    std::lock_guard lk(owned_mu);
    int64_t found = 0;
    for (auto& [p, o] : owned) {
        const Owned now = owned_hash(p);
        const bool changed = now.mask == o.mask && now.hash != o.hash;   // (grains the GPU took since are not judged)
        o = now;
        if (!changed) continue;
        found++;
        int64_t seen;
        {
            const Region* r = region_at(p);
            std::lock_guard mk(miss_mu);
            char key[160];
            std::snprintf(key, sizeof key, "%s flags %#x (a CPU write to a page the GPU owns)", r && r->by ? r->by : "?", r ? r->flags : 0);
            seen = misses[key]++;
        }
        st_owned_writes.fetch_add(1, std::memory_order_relaxed);
        if (seen < 4) {
            logf("direct: claims: ERROR: check: page %#llx the GPU owns written by the CPU with no report: %s", (unsigned long long)p,
                 pool_name(p).c_str());
            strict_event("a CPU write the GPU does not see", "page %#llx: %s", (unsigned long long)p, pool_name(p).c_str());
        }
    }
    if (found) st_mismatches.fetch_add(found, std::memory_order_relaxed);
}

/// the CPU's reported ranges into ups, a run per region of the back end's own copied memory, and the resets into fs
void take_reported(ClaimSub* s, std::vector<Up>& ups, std::vector<Fill>& fs) {
    std::vector<std::pair<uint64_t, uint64_t>> mine;
    {
        std::lock_guard lk(report_mu);
        s->seq = ++first_seq;
        mine.swap(reported);
        fs.swap(fills);
    }
    for (const auto& [lo, hi] : mine)
        for (uint64_t a = lo; a < hi;) {
            const uint64_t e = std::min(hi, (a & ~(kPage - 1)) + kPage);
            Region* r = region_at(a);
            if (r && r->pages && local_copied(r)) {
                if (!ups.empty() && ups.back().r == r && ups.back().hi == a) ups.back().hi = e;
                else ups.push_back({r, a, e});
            }
            a = e;
        }
}

/// After its last work: the Readback ranges s's work wrote, copied into staging
VkCommandBuffer record_outs(ClaimSub* s) {
    VkCommandBuffer cb = check_buffer();
    s->cbs.push_back(cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    auto& o = s->outs;
    std::sort(o.begin(), o.end(), [](const Up& a, const Up& b) { return a.r != b.r ? a.r < b.r : a.lo < b.lo; });
    size_t k = 0;
    for (size_t i = 0; i < o.size(); i++) {
        if (!o[i].r->pages) continue;   // (retired)
        if (k && o[k - 1].r == o[i].r && o[i].lo <= o[k - 1].hi) o[k - 1].hi = std::max(o[k - 1].hi, o[i].hi);
        else o[k++] = o[i];
    }
    o.resize(k);
    uint64_t total = 0;
    for (const Up& u : o) total += u.hi - u.lo;
    Staging* st = nullptr;
    int64_t chunk = -1;
    if (total) {
        std::lock_guard lk(check_mu);
        chunk = staging_take(total);
        if (chunk >= 0) st = &staging[chunk], s->chunks.push_back(chunk);
    }
    if (total && !st) logf("direct: claims: ERROR: no readback staging of %llu bytes", (unsigned long long)total);
    if (st) {
        record_dep(cb, Dep{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT, 0});
        uint64_t at = 0;
        std::vector<VkBufferCopy> cs;
        for (size_t i = 0; i < o.size(); i++) {
            const Up& u = o[i];
            cs.push_back({u.lo - u.r->lo, at, u.hi - u.lo});
            s->out_sent.push_back({u.lo, u.hi, chunk, at});
            at += u.hi - u.lo;
            if (i + 1 == o.size() || o[i + 1].r != u.r) copy_buffer(cb, u.r->buffer, st->buf, (uint32_t)cs.size(), cs.data()), cs.clear();
        }
        record_dep(cb, Dep{VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_ACCESS_2_HOST_READ_BIT, 0});
        st_out_subs.fetch_add(1, std::memory_order_relaxed), st_out_ranges.fetch_add((int64_t)o.size(), std::memory_order_relaxed);
        st_out_bytes.fetch_add((int64_t)total, std::memory_order_relaxed);
    }
    o.clear();
    vkEndCommandBuffer(cb);
    return cb;
}

/// s complete: its copied-out ranges into guest memory, but the parts the CPU reported writing after its first copy
void copy_out(ClaimSub& s) {
    if (s.out_sent.empty()) return;
    std::vector<Cancel> cs;
    {
        std::lock_guard lk(report_mu);
        while (!cancels.empty() && cancels.front().upto < s.seq) cancels.pop_front();   // (no later submission is that old)
        cs.assign(cancels.begin(), cancels.end());
    }
    Staging* st;
    {
        std::lock_guard lk(check_mu);
        st = &staging[s.out_sent[0].chunk];
    }
    mem_invalidate(st->mem);
    std::unique_lock<std::mutex> ok;
    if (track_check) ok = std::unique_lock<std::mutex>(owned_mu);
    for (const ClaimSubImpl::Out& x : s.out_sent) {
        for (uint64_t a = x.lo; a < x.hi;) {   // (the parts no report covers, in runs)
            uint64_t e = x.hi;
            bool dropped = false;
            for (const Cancel& c : cs) {
                if (c.hi <= a || c.lo >= e) continue;
                if (c.lo <= a) dropped = true, e = std::min(e, c.hi);
                else e = c.lo;
            }
            if (dropped) st_out_dropped.fetch_add((int64_t)(e - a), std::memory_order_relaxed);
            else host->gpu_write(a, st->map + x.at + (a - x.lo), e - a);
            a = e;
        }
        if (track_check) owned_rehash(x.lo, x.hi);
    }
}
}  // namespace

void claims_cpu_write(uint64_t lo, uint64_t size) {
    if (!size || state.load(std::memory_order_acquire) != 1) return;
    {
        std::lock_guard lk(report_mu);
        reported.push_back({lo, lo + size});
        cancels.push_back({lo, lo + size, first_seq});
    }
    st_reports.fetch_add(1, std::memory_order_relaxed), st_report_bytes.fetch_add((int64_t)size, std::memory_order_relaxed);
    if (track_check) {
        std::lock_guard lk(owned_mu);
        owned_rehash(lo, lo + size);
    }
}

void claims_gpu_fill(uint64_t lo, uint64_t size, uint32_t value) {
    if (!size || state.load(std::memory_order_acquire) != 1) return;
    if ((lo | size) & 3) {
        logf("direct: claims: ERROR: a fill of %#llx+%#llx is not 4-byte aligned", (unsigned long long)lo, (unsigned long long)size);
        return;
    }
    {
        std::lock_guard lk(report_mu);
        fills.push_back({lo, lo + size, value});
        cancels.push_back({lo, lo + size, first_seq});
    }
    st_fills.fetch_add(1, std::memory_order_relaxed), st_fill_bytes.fetch_add((int64_t)size, std::memory_order_relaxed);
    if (track_check) {
        std::lock_guard lk(owned_mu);
        owned_rehash(lo, lo + size);
    }
}

void claims_read_window(uint64_t lo, uint64_t size) {
    if (!size || state.load(std::memory_order_acquire) != 1) return;
    Region* r = region_at(lo);
    if (!r || r->claim != NVN_CLAIM_VRAM || lo + size > r->hi) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 8)
            logf("direct: claims: ERROR: a read window %#llx+%#llx outside a VRAM claim: %s", (unsigned long long)lo, (unsigned long long)size,
                 pool_name(lo).c_str());
        return;
    }
    std::lock_guard lk(reads_mu);
    auto& w = r->reads;
    auto it = std::lower_bound(w.begin(), w.end(), std::make_pair(lo, uint64_t(0)));
    if (it != w.begin() && std::prev(it)->second >= lo + size && std::prev(it)->first <= lo) return;   // (held already)
    if (it != w.end() && it->first == lo && it->second >= lo + size) return;
    w.insert(it, {lo, lo + size});
    size_t k = 0;   // (merged)
    for (size_t i = 1; i < w.size(); i++)
        if (w[i].first <= w[k].second) w[k].second = std::max(w[k].second, w[i].second);
        else w[++k] = w[i];
    w.resize(k + 1);
    st_read_windows.fetch_add(1, std::memory_order_relaxed);
    r->has_reads.store(true, std::memory_order_release);
}

void claims_cpu_reads(uint64_t lo, uint64_t size) {
    if (!track_check || !size || state.load(std::memory_order_acquire) != 1) return;
    Region* r = region_at(lo);
    if (!r || r->claim == NVN_CLAIM_READBACK || r->claim == NVN_CLAIM_LAYER0 || r->claim == NVN_CLAIM_VRAM) {
        if (!r || r->claim != NVN_CLAIM_VRAM || !any_gpu_written(r, lo, std::min(lo + size, r->hi))) return;   // (nothing the GPU wrote yet)
        if (r->has_reads.load(std::memory_order_acquire)) {   // (the game invalidates a list to read its header: a range it copies out)
            std::lock_guard lk(reads_mu);
            for (const auto& [a, e] : r->reads)
                if (a < lo + size && e > lo) return;
        }
    } else if (!any_gpu_written(r, lo, std::min(lo + size, r->hi))) {
        return;
    }
    static std::atomic<int64_t> told{0};
    st_mismatches.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard mk(miss_mu);
        char key[160];
        std::snprintf(key, sizeof key, "%s flags %#x (the CPU reads GPU writes outside a Readback claim)", r->by ? r->by : "?", r->flags);
        misses[key]++;
    }
    if (told.fetch_add(1, std::memory_order_relaxed) < 8) {
        logf("direct: claims: ERROR: check: the CPU invalidates %#llx+%#llx to read what the GPU wrote, outside a Readback claim: %s",
             (unsigned long long)lo, (unsigned long long)size, pool_name(lo).c_str());
        strict_event("a CPU read of GPU writes outside a Readback claim", "%#llx+%#llx: %s", (unsigned long long)lo, (unsigned long long)size,
                     pool_name(lo).c_str());
    }
}

void claims_guest_write(uint64_t guest, const void* src, uint64_t n) {
    if (!track_check) return void(host->gpu_write(guest, src, n));
    std::lock_guard lk(owned_mu);
    host->gpu_write(guest, src, n);
    owned_rehash(guest, guest + n);
}

VkCommandBuffer claims_sub_prepare(ClaimSub* s, ClaimPrep what) {
    if (what == CLAIM_PREP_AFTER_WAIT) return refresh(s);
    if (what == CLAIM_PREP_OUTS) return record_outs(s);
    if (s->census) host->census_phase(s->census, 1);
    const auto t0 = std::chrono::steady_clock::now();
    {
        DirtyAcc acc;
        for (List* l : s->lists) note_list(s, acc, l, 0);
    }
    st_note_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
    for (const Up& w : s->gpu_writes) mark_gpu_written(w.r, w.lo, w.hi);   // (their grains the GPU's from now on: see up_add)
    if (track_check)
        for (const Up& w : s->gpu_writes) own_pages(w.lo, w.hi);
    if (track_all) gather(s);
    s->run_frame = present_count(), s->run_behind = sync_pushed() - sync_completed();
    // (Upload and Readback regions made since the last submit: whole; the load-once pages its lists read not copied yet;
    // the Upload grains submitted lists read since the last first chunk)
    std::vector<Up> ups;
    {
        std::lock_guard lk(mu);
        for (Region* r : whole_uploads)
            if (r->pages) ups.push_back({r, r->lo, r->hi}), st_up_whole.fetch_add(1, std::memory_order_relaxed);
        whole_uploads.clear();
    }
    if (track_check)
        for (const Up& u : ups)
            if (u.r->claim == NVN_CLAIM_VRAM) own_pages(u.lo, u.hi);
    const size_t whole_n = ups.size();   // (copies no work of it reads: left out of the check of CPU writes during flight)
    for (const Up& x : s->once)
        for (uint64_t a = x.lo & ~(kPage - 1); a < x.hi; a += kPage) {
            const uint64_t p = (a - x.r->lo) >> kPageShift, m = 1ull << (p & 63);
            if (x.r->copied[p >> 6].fetch_or(m, std::memory_order_relaxed) & m) continue;
            if (!ups.empty() && ups.back().r == x.r && ups.back().hi == a) ups.back().hi = a + kPage;
            else ups.push_back({x.r, a, a + kPage});
            st_up_once.fetch_add(1, std::memory_order_relaxed);
        }
    s->once.clear();
    std::vector<Region*> regions;
    {
        std::lock_guard lk(dirty_mu);
        regions.swap(dirty_regions);
    }
    for (Region* r : regions)
        if (r->pages) take_dirty(r, ups);
        else r->listed.store(false);
    std::vector<Fill> fs;
    take_reported(s, ups, fs);
    uint64_t up_total = 0;
    for (const Up& u : ups) up_total += u.hi - u.lo;
    std::vector<uint64_t> up_at(ups.size());   // (each upload's place in the staging chunk)
    // (the check: what the CPU writes and the GPU only reads, after the uploads)
    uint64_t total = 0;
    ClaimSubImpl::Part* part = track_check ? &s->parts.emplace_back() : nullptr;
    if (part)
        for (const SubRange& x : s->ranges) {
            if (x.r->claim != NVN_CLAIM_LAYER0 && !local_copied(x.r)) continue;
            if (x.uses & kGpuWrites || any_gpu_written(x.r, x.lo, x.hi)) {
                st_skipped_gpu_written.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            part->checked.push_back(x), total += x.hi - x.lo;
        }
    VkCommandBuffer cb = check_buffer();
    s->cbs.push_back(cb);
    int64_t staged = -1;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    if (up_total) {
        Staging* st = nullptr;
        {
            std::lock_guard lk(check_mu);
            staged = staging_take(up_total);
            if (staged >= 0) st = &staging[staged], s->chunks.push_back(staged);
        }
        if (!st) {
            logf("direct: claims: ERROR: no upload staging of %llu bytes", (unsigned long long)up_total);
        } else {
            record_dep(cb, Dep{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                               VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0});
            uint64_t at = 0;
            std::vector<VkBufferCopy> cs;
            for (size_t i = 0; i < ups.size(); i++) {
                const Up& u = ups[i];
                up_at[i] = at;
                const auto m0 = std::chrono::steady_clock::now();
                host->gpu_read(u.lo, st->map + at, u.hi - u.lo);
                st_memcpy_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m0).count(), std::memory_order_relaxed);
                cs.push_back({at, u.lo - u.r->lo, u.hi - u.lo});
                at += u.hi - u.lo;
                if (i + 1 == ups.size() || ups[i + 1].r != u.r) copy_buffer(cb, st->buf, u.r->buffer, (uint32_t)cs.size(), cs.data()), cs.clear();
            }
            mem_flush(st->mem);
            for (size_t i = whole_n; i < ups.size(); i++) s->sent.push_back({ups[i].lo, ups[i].hi, ups[i].r, staged, up_at[i]});
            record_dep(cb, Dep{VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, 0});
            st_up_subs.fetch_add(1, std::memory_order_relaxed), st_up_ranges.fetch_add((int64_t)ups.size(), std::memory_order_relaxed);
            st_up_bytes.fetch_add((int64_t)up_total, std::memory_order_relaxed);
        }
    }
    void* map = nullptr;
    if (total && (!mem_buffer(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, part->buf, part->mem, &map, MO_OTHER) || !map)) {
        logf("direct: claims: ERROR: check: no buffer of %llu bytes", (unsigned long long)total);
        part->checked.clear(), total = 0;
    }
    if (total) {
        part->map = (uint8_t*)map;
        part->want.resize(total);
        record_dep(cb, Dep{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT, 0});
        uint64_t at = 0;
        for (const SubRange& x : part->checked) {
            const uint64_t n = x.hi - x.lo;
            // (a range this submission uploads: the bytes it copied, so a write racing the copy is not a mismatch; the check
            // is for what no copy covers: load-once and pointer-reached data, and the layer-0 claims' memory)
            for (uint64_t a = x.lo; a < x.hi;) {
                const Up* u = nullptr;
                for (const Up& w : ups)
                    if (w.r == x.r && w.lo <= a && a < w.hi) u = &w;
                const uint64_t e = u ? std::min(x.hi, u->hi) : x.hi;
                if (u && staged >= 0) std::memcpy(part->want.data() + at + (a - x.lo), staging[staged].map + up_at[u - ups.data()] + (a - u->lo), e - a);
                else host->gpu_read(a, part->want.data() + at + (a - x.lo), e - a);
                a = e;
            }
            const VkBufferCopy c{x.lo - x.r->lo, at, n};
            copy_buffer(cb, x.r->buffer, part->buf, 1, &c);
            at += n;
        }
        record_dep(cb, Dep{VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_ACCESS_2_MEMORY_WRITE_BIT, 0});
        st_checked_ranges.fetch_add((int64_t)part->checked.size(), std::memory_order_relaxed);
        st_checked_bytes.fetch_add((int64_t)total, std::memory_order_relaxed);
    }
    if (!fs.empty()) {   // (the resets, after the copies: a reset wins over an older copy of its bytes)
        record_dep(cb, Dep{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0});
        for (const Fill& f : fs)
            if (Region* r = region_at(f.lo); r && r->pages && r->buffer && f.hi <= r->hi) fill_buffer(cb, r->buffer, f.lo - r->lo, f.hi - f.lo, f.value);
        record_dep(cb, Dep{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, 0});
    }
    vkEndCommandBuffer(cb);
    st_up_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                       std::memory_order_relaxed);
    return cb;
}

void claims_sub_sent(ClaimSub* s, uint64_t value) {
    if (!s) return;
    readback_then(value, [s] {
        if (s->census) host->census_phase(s->census, 2);
        copy_out(*s);
        compare(*s);
        for (List* l : s->lists) list_unref(l);
        std::lock_guard lk(check_mu);
        check_free.insert(check_free.end(), s->cbs.begin(), s->cbs.end());
        for (int64_t c : s->chunks) staging[c].busy = 0;
        delete s;
    });
}

void claims_pointer(uint64_t addr, uint64_t program, const char* how) {
    if (!track_use) return;
    Region* r = region_at(addr);
    std::lock_guard lk(use_mu);
    if (!r) {
        UseAgg& a = use_agg[{"(no region)", 0, 0}];
        a.pointed++;
        return;
    }
    UseAgg& a = agg_of(r);
    a.pointed++;
    if (a.pointer_sites.size() < 4) {
        char b[80];
        std::snprintf(b, sizeof b, "%016llx %s", (unsigned long long)program, how);
        if (std::find(a.pointer_sites.begin(), a.pointer_sites.end(), b) == a.pointer_sites.end()) a.pointer_sites.push_back(b);
    }
}

void claims_use_log() {
    if (track_check) owned_scan();
    if (track_check) {
        char b[640];
        std::snprintf(b, sizeof b, "direct: claims: check since the start: %lld ranges, %lld MB compared, %lld mismatches, %lld ranges left out "
                      "(the GPU may write them); uploaded ranges the CPU wrote before their submission completed: %lld; CPU writes no report "
                      "hands over to pages the GPU owns: %lld (in the mismatches)",
                      (long long)st_checked_ranges.load(), (long long)(st_checked_bytes.load() >> 20), (long long)st_mismatches.load(),
                      (long long)st_skipped_gpu_written.load(), (long long)st_late_writes.load(), (long long)st_owned_writes.load());
        logf("%s", b);
        std::lock_guard lk(miss_mu);
        std::string t;
        for (const auto& [k, n] : misses) t += " " + k + " " + std::to_string(n) + ";";
        if (!t.empty()) log_long(("direct: claims: check's mismatches by pool and use:" + t).c_str(), "direct: claims:");
    }
    if (!track_use) return;
    std::lock_guard lk(use_mu);
    const double frames = std::max<double>(1, (double)present_count() - (double)use_frames_from);
    std::vector<std::pair<double, const decltype(use_agg)::value_type*>> order;
    double total = 0;
    for (const auto& e : use_agg) order.push_back({e.second.merged, &e}), total += e.second.merged;
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    logf("direct: claims: use over %.0f presents: %.2f MB bound a frame in all (merged per submission), %zu pool groups", frames,
         total / frames / 1048576, order.size());
    for (const auto& [m, e] : order) {
        const auto& [key, a] = *e;
        char b[1200];
        int k = std::snprintf(b, sizeof b, "direct: claims: use %s flags %#x claim %u: %.1f KB a frame merged, %.1f ranges;", std::get<0>(key).c_str(),
                              std::get<1>(key), std::get<2>(key), a.merged / frames / 1024, a.ranges / frames);
        for (int u = 0; u < CU_KINDS; u++)
            if (a.by_use[u] > 0) k += std::snprintf(b + k, sizeof b - k, " %s %.1f KB", kUseNames[u], a.by_use[u] / frames / 1024);
        k += std::snprintf(b + k, sizeof b - k, "; pages used %llu, changed since the pool was made %llu (first %lld presents after), "
                           "changes between uses %llu; reached through stored addresses %.1f a frame",
                           (unsigned long long)a.pages_used, (unsigned long long)a.pages_changed_since_made,
                           a.first_change_after == UINT64_MAX ? -1ll : (long long)a.first_change_after, (unsigned long long)a.page_changes,
                           a.pointed / frames);
        for (const std::string& p : a.pointer_sites) k += std::snprintf(b + k, k < (int)sizeof b ? sizeof b - k : 0, " [%s]", p.c_str());
        logf("%s", b);
        std::vector<std::pair<double, uint64_t>> big;
        for (const auto& [lo, g] : a.ranges_by_lo) big.push_back({g.bytes, lo});
        std::sort(big.begin(), big.end(), std::greater<>());
        k = std::snprintf(b, sizeof b, "direct: claims: use %s flags %#x: largest ranges (KB a frame):", std::get<0>(key).c_str(), std::get<1>(key));
        for (size_t i = 0; i < big.size() && i < 8; i++) {
            const UseAgg::Big& g = a.ranges_by_lo.at(big[i].second);
            std::string uses;
            for (int u = 0; u < CU_KINDS; u++)
                if (g.uses >> u & 1) uses += std::string(uses.empty() ? "" : "+") + kUseNames[u];
            k += std::snprintf(b + k, k < (int)sizeof b ? sizeof b - k : 0, " %#llx+%#llx %s %.1f;", (unsigned long long)big[i].second,
                               (unsigned long long)(g.hi - big[i].second), uses.c_str(), g.bytes / frames / 1024);
        }
        logf("%s", b);
    }
    use_agg.clear();
    use_frames_from = (int64_t)present_count();
}

namespace {

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
    track_check = dbg(SET_DBG_CLAIMCHECK), track_use = dbg(SET_DBG_CLAIMUSE), tracking = true;
    track_census = *host->setting_str(SET_DBG_POOLCENSUS) != 0;
    track_all = track_check || track_use || track_census;
    granule_region.reset(new std::atomic<uintptr_t>[(guest_bytes >> kGranuleShift) + 1]);
    for (uint64_t j = 0; j <= guest_bytes >> kGranuleShift; j++) granule_region[j].store(0, std::memory_order_relaxed);
    logf("direct: claims on: a region per pool the GPU reads; VRAM claims in memory type %d, layer-0 claims imported as type %u",
         (int)vram_type, import_type);
    state.store(1, std::memory_order_release);
    return true;
}

void claims_pool(uint64_t gpu, uint64_t size, uint32_t flags, uint32_t claim, const char* by, int on) {
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
    else if (claim == NVN_CLAIM_VRAM || claim == NVN_CLAIM_UPLOAD || claim == NVN_CLAIM_READBACK || claim == NVN_CLAIM_LOAD_ONCE)
        r = region_local(lo, hi, claim);
    // (its contents now, at the next submit: the claims whose bytes come from guest memory)
    if (r && (claim == NVN_CLAIM_UPLOAD || claim == NVN_CLAIM_READBACK || (claim == NVN_CLAIM_VRAM && !(flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS))))
        whole_uploads.push_back(r);
    if (r && claim == NVN_CLAIM_LOAD_ONCE) r->copied.reset(new std::atomic<uint64_t>[(((hi - lo) >> kPageShift) + 63) >> 6]());
    if (r && (claim == NVN_CLAIM_UPLOAD || claim == NVN_CLAIM_READBACK)) {
        r->dirty_words = (((hi - lo) >> kGrainShift) + 63) >> 6;
        r->dirty.reset(new std::atomic<uint64_t>[r->dirty_words]());
        r->dirty_sum.reset(new std::atomic<uint64_t>[(r->dirty_words + 63) >> 6]());
    }
    if ((claim == NVN_CLAIM_VRAM || claim == NVN_CLAIM_UPLOAD || claim == NVN_CLAIM_READBACK || claim == NVN_CLAIM_LOAD_ONCE ||
         (claim == NVN_CLAIM_LAYER0 && gpu_reads)) && !r &&
        st_failed++ < 8)
        logf("direct: claims: pool %#llx+%#llx flags %#x (claim %u): no region", (unsigned long long)gpu, (unsigned long long)size, flags, claim);
    if (r) r->by = by, r->flags = flags, __atomic_store_n(&r->track, track_made(r), __ATOMIC_RELEASE);
    pools.insert({gpu, Pool{size, flags, claim, r, by}});
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

namespace {
/// The region holding all of guest [gpu, hi), or null: through the calling thread's last whole granule while no region
/// moved (region_assign counts the moves)
struct SpanCache {
    uint64_t gen = 0, lo = 0, hi = 0;
    Region* r = nullptr;
};
thread_local SpanCache span_cache;
Region* region_over(uint64_t gpu, uint64_t hi) {
    SpanCache& c = span_cache;
    const uint64_t gen = assign_gen.load(std::memory_order_acquire);
    if (c.gen == gen && gpu >= c.lo && hi <= c.hi) return c.r;
    Region* r = region_at(gpu);
    if (!r || hi > r->hi || ((hi - 1) >> kPageShift != gpu >> kPageShift && region_at(hi - 1) != r)) return nullptr;
    if (!(granule_region[gpu >> kGranuleShift].load(std::memory_order_acquire) & 1)) {
        const uint64_t g0 = gpu >> kGranuleShift << kGranuleShift;
        c.gen = gen, c.lo = std::max<uint64_t>(g0, r->lo), c.hi = std::min<uint64_t>(g0 + (1ull << kGranuleShift), r->hi), c.r = r;
    }
    return r;
}
/// guest [gpu, gpu + size) used by list l's command: its region, the use noted where a copy needs it; null outside one (an
/// error, named)
Region* claim_use(List* l, ClaimUse use, uint64_t gpu, uint64_t size) {
    if (!size || gpu >= guest_bytes || state.load(std::memory_order_acquire) != 1) return nullptr;
    const uint64_t hi = gpu + size;
    Region* r = region_over(gpu, hi);
    if (!r) {
        if (st_span_missed.fetch_add(1, std::memory_order_relaxed) < 8) {   // (named: the pool and its claim, which keeps it off the GPU)
            uint64_t base = 0, psize = 0;
            uint32_t flags = 0, claim = NVN_CLAIM_NONE;
            {
                std::lock_guard lk(mu);
                for (auto it = pools.begin(); it != pools.end() && it->first <= gpu; ++it)
                    if (gpu < it->first + it->second.size) base = it->first, psize = it->second.size, flags = it->second.flags, claim = it->second.claim;
            }
            strict_event("a GPU use of memory no region holds", "%s %#llx+%#llx in pool %#llx+%#llx flags %#x, claimed %u", kUseNames[use],
                         (unsigned long long)gpu, (unsigned long long)size, (unsigned long long)base, (unsigned long long)psize, flags, claim);
            logf("direct: claims: ERROR: %#llx+%#llx used by the GPU (%s, list lr %#llx), outside a region: pool %#llx+%#llx flags %#x, claim %u",
                 (unsigned long long)gpu, (unsigned long long)size, kUseNames[use], l ? (unsigned long long)l->info.lr : 0ull,
                 (unsigned long long)base, (unsigned long long)psize, flags, claim);
        }
        return nullptr;
    }
    // (noted where a copy needs it: Upload and load-once claims; every claim while the check or the measurement runs. A
    // range that overlaps or follows the list's last one of the same region and use extends it, but for the measurement)
    const bool windowed = r->has_reads.load(std::memory_order_relaxed) && (1u << use) & kOutUses;
    if (l && (r->claim == NVN_CLAIM_READBACK || windowed) && (1u << use) & kOutUses) l->outs = true;
    if (l && (track_all || r->claim == NVN_CLAIM_UPLOAD || r->claim == NVN_CLAIM_READBACK || windowed ||
              (r->claim == NVN_CLAIM_LOAD_ONCE && !all_copied(r, gpu, hi)))) {
        const uint32_t n = (uint32_t)std::min<uint64_t>(size, UINT32_MAX);
        if (!l->bound.empty()) {
            BoundRange& b = l->bound.back();
            if (b.region == r && b.use == use && gpu >= b.lo && gpu <= b.lo + b.size && (!track_use || gpu == b.lo)) {
                b.size = (uint32_t)std::min<uint64_t>(std::max<uint64_t>(b.lo + b.size, gpu + n) - b.lo, UINT32_MAX);
                return r;
            }
        }
        l->bound.push_back({gpu, n, use, r});
    }
    return r;
}
}  // namespace

bool claims_span(List* l, ClaimUse use, uint64_t gpu, uint64_t size, VkBuffer& buffer, VkDeviceSize& offset) {
    Region* r = claim_use(l, use, gpu, size);
    if (!r) return false;
    buffer = r->buffer, offset = gpu - r->lo;
    return true;
}

bool claims_address(List* l, ClaimUse use, uint64_t gpu, uint64_t size, VkDeviceAddress& a) {
    Region* r = claim_use(l, use, gpu, size);
    if (!r) return false;
    a = r->address + (gpu - r->lo);
    return true;
}

void claims_stats(char* out, size_t cap) {
    if (state.load() != 1) return void(std::snprintf(out, cap, "claims off"));
    std::lock_guard lk(mu);
    retired_sweep();
    std::snprintf(out, cap, "claims: pools made by claim: textures %lld, targets %lld, host %lld, vram %lld, upload %lld, readback %lld, "
                  "load once %lld, layer 0 %lld; regions %lld made, %lld freed, %zu retiring, %lld failed; VRAM %llu MB live; imports %llu MB live (%lld "
                  "arenas, %lld of their own); spans outside a region %lld",
                  (long long)st_by_claim[NVN_CLAIM_TEXTURES], (long long)st_by_claim[NVN_CLAIM_TARGETS], (long long)st_by_claim[NVN_CLAIM_HOST],
                  (long long)st_by_claim[NVN_CLAIM_VRAM], (long long)st_by_claim[NVN_CLAIM_UPLOAD], (long long)st_by_claim[NVN_CLAIM_READBACK], (long long)st_by_claim[NVN_CLAIM_LOAD_ONCE],
                  (long long)st_by_claim[NVN_CLAIM_LAYER0], (long long)st_regions, (long long)st_freed, retired.size(), (long long)st_failed,
                  (unsigned long long)(vram_live >> 20), (unsigned long long)(import_live >> 20), (long long)st_arenas,
                  (long long)st_own_imports, (long long)st_span_missed.load());
    const size_t k = std::strlen(out);
    std::snprintf(out + k, cap - k, "; uploads since the start: %lld submissions, %lld ranges, %lld MB, %lld whole regions, %.1f ms on the "
                  "submitting threads, %lld ranges or pages left to the GPU (it writes them); load-once pages copied %lld; staging %zu chunks; noted in %.1f ms; guest reads %.1f ms; after held event waits %lld refreshes, %lld KB again in %.1f ms; "
                  "copied out %lld submissions, %lld ranges, %lld MB, %lld KB of them dropped for the CPU's later writes; CPU writes reported %lld, %lld KB; "
                  "resets made on the GPU %lld, %lld KB; read windows %lld",
                  (long long)st_up_subs.load(), (long long)st_up_ranges.load(), (long long)(st_up_bytes.load() >> 20), (long long)st_up_whole.load(),
                  st_up_ns.load() / 1e6, (long long)st_up_skipped_gpu.load(), (long long)st_up_once.load(), staging.size(), st_note_ns.load() / 1e6, st_memcpy_ns.load() / 1e6, (long long)st_refreshes.load(),
                  (long long)(st_refreshed.load() >> 10), st_refresh_ns.load() / 1e6, (long long)st_out_subs.load(), (long long)st_out_ranges.load(),
                  (long long)(st_out_bytes.load() >> 20), (long long)(st_out_dropped.load() >> 10), (long long)st_reports.load(),
                  (long long)(st_report_bytes.load() >> 10), (long long)st_fills.load(), (long long)(st_fill_bytes.load() >> 10),
                  (long long)st_read_windows.load());
}

}  // namespace hsd
