// Images and buffers in Vulkan Memory Allocator's blocks (v3.3.0, OUNCE_VMA_HEADERS), cut down from gpu/legacy/vk/vk_vma.cpp:
// device-local images, host-visible buffers (staging, dumps). Its Vulkan functions come from vkGetInstanceProcAddr.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_check.h"
#include "vk_internal.h"
#include "os.h"
extern "C" size_t hos_mem_ledger(char* out, size_t cap, uint64_t* ours, uint64_t* guest_committed);   // (runtime/hos_mem.c, exported)
#include <vk_mem_alloc.h>


#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hsd {

std::atomic<int64_t> st_objects{0};

namespace {
VmaAllocator vma = VK_NULL_HANDLE;

// ---- memory by owner ----
// [owner][0: a device-local heap, 1: a host heap, 2: the process's own memory]. A VMA allocation keeps its owner (+ 1) as
// its user data, so its free needs no owner argument.
std::atomic<int64_t> acct_bytes[MO_N][3], acct_count[MO_N][3];
const char* const kOwnerNames[MO_N] = {"arenas", "task pools", "query reports", "constants", "heaps and tables", "uploads",
                                       "transient pool", "layer 0 vram", "layer 0 imports", "images from the store",
                                       "the game's other images", "other", "lists", "store scratch", "shader code"};
std::atomic<int64_t> mapped_bytes{0};
int heap_class(uint32_t type) {
    return type < dev.memory.memoryTypeCount &&
           (dev.memory.memoryHeaps[dev.memory.memoryTypes[type].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? 0 : 1;
}
void account_vma(VmaAllocation a, int sign) {
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(vma, a, &info);
    if (info.pUserData) mem_account((MemOwner)((uintptr_t)info.pUserData - 1), info.memoryType, sign * (int64_t)info.size);
}
}  // namespace

void mem_account(MemOwner owner, uint32_t type, int64_t bytes) {
    const int c = heap_class(type);
    acct_bytes[owner][c].fetch_add(bytes, std::memory_order_relaxed);
    acct_count[owner][c].fetch_add(bytes < 0 ? -1 : 1, std::memory_order_relaxed);
}
void mem_account_main(MemOwner owner, int64_t bytes) { acct_bytes[owner][2].fetch_add(bytes, std::memory_order_relaxed); }
void mem_main_mapped(size_t n) { mapped_bytes.fetch_add((int64_t)n, std::memory_order_relaxed); }

bool vma_init() {
    VmaVulkanFunctions f{};
    f.vkGetInstanceProcAddr = get_instance_proc;
    f.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | (dev.memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    ci.physicalDevice = dev.physical;
    ci.device = dev.device;
    ci.instance = dev.instance;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &f;
    ci.preferredLargeHeapBlockSize = 128ull << 20;
    return vmaCreateAllocator(&ci, &vma) == VK_SUCCESS;
}

bool mem_image(const VkImageCreateInfo& ci, VkImage& image, Mem*& mem, MemOwner owner) {
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ai.pUserData = (void*)(uintptr_t)(owner + 1);
    VmaAllocation a = nullptr;
    if (vmaCreateImage(vma, &ci, &ai, &image, &a, nullptr) != VK_SUCCESS) return false;
    mem = (Mem*)a;
    account_vma(a, 1);
    object_made(1, "an image");
    return true;
}
void mem_image_free(VkImage image, Mem* mem) {
    if (mem) account_vma((VmaAllocation)mem, -1);
    vmaDestroyImage(vma, image, (VmaAllocation)mem);
}

// ---- the transient pool (docs/direct/transient-pool.md) ----
// One device allocation for the game's transient render targets (games/oot/patches/rt_slots.c's slot range), made once when the host
// names the range. A target's image is bound in it at its storage offset, so targets alias as on the console and no
// device memory is allocated or freed for them in play. Its own vkAllocateMemory, outside VMA's blocks: nothing else
// may land in it.
namespace {
VkDeviceMemory pool_mem = VK_NULL_HANDLE;
uint64_t pool_bytes = 0;
uint32_t pool_type = 0;
}  // namespace
bool mem_pool_make(uint64_t size, uint32_t type_bits) {
    if (pool_mem) return size <= pool_bytes;
    int type = -1;
    for (int pass = 0; pass < 2 && type < 0; pass++)   // (device-local and not host-visible first: VRAM only)
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount; i++) {
            const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
            if (!(type_bits >> i & 1) || !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
            if (pass == 0 && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            type = (int)i;
            break;
        }
    if (type < 0) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(dev.device, &ai, nullptr, &pool_mem) != VK_SUCCESS) return pool_mem = VK_NULL_HANDLE, false;
    pool_bytes = size, pool_type = (uint32_t)type;
    mem_account(MO_TRANSIENT, pool_type, (int64_t)size);
    return true;
}
uint64_t mem_pool_size() { return pool_bytes; }
bool mem_pool_bind(VkImage image, uint64_t offset, const VkMemoryRequirements& req) {
    if (!pool_mem || !(req.memoryTypeBits >> pool_type & 1) || offset % std::max<VkDeviceSize>(req.alignment, 1) ||
        offset + req.size > pool_bytes)
        return false;
    return vkBindImageMemory(dev.device, image, pool_mem, offset) == VK_SUCCESS;
}

bool mem_buffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, VkBuffer& buffer, Mem*& mem, void** mapped,
                MemOwner owner) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = host_visible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (host_visible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.pUserData = (void*)(uintptr_t)(owner + 1);
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &a, &info) != VK_SUCCESS) return false;
    mem = (Mem*)a;
    if (mapped) *mapped = info.pMappedData;
    mem_account(owner, info.memoryType, (int64_t)info.size);
    object_made(1, "a buffer");
    return true;
}
void mem_buffer_free(VkBuffer buffer, Mem* mem) {
    if (mem) account_vma((VmaAllocation)mem, -1);
    vmaDestroyBuffer(vma, buffer, (VmaAllocation)mem);
}
void mem_flush(Mem* mem) { vmaFlushAllocation(vma, (VmaAllocation)mem, 0, VK_WHOLE_SIZE); }
void mem_invalidate(Mem* mem) { vmaInvalidateAllocation(vma, (VmaAllocation)mem, 0, VK_WHOLE_SIZE); }
namespace {
/// The heaps with their budgets and usage: the driver's (VK_EXT_memory_budget, so the compositor and other processes
/// count), or without it an estimate as VMA makes one: 80% of a heap's size, and the back end's accounted bytes with the
/// free room in VMA's blocks on the largest heap of each kind (device-local, host)
void heap_budgets(VkPhysicalDeviceMemoryProperties2& p, VkPhysicalDeviceMemoryBudgetPropertiesEXT& bp) {
    static PFN_vkGetPhysicalDeviceMemoryProperties2 props2 =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)get_instance_proc(dev.instance, "vkGetPhysicalDeviceMemoryProperties2");
    p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    bp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    if (dev.memory_budget) p.pNext = &bp;
    if (props2) props2(dev.physical, &p);
    else p.memoryProperties = dev.memory;
    if (dev.memory_budget && props2) return;
    VmaBudget vb[VK_MAX_MEMORY_HEAPS] = {};
    if (vma) vmaGetHeapBudgets(vma, vb);
    int largest[2] = {-1, -1};
    int64_t used[2] = {};
    for (uint32_t h = 0; h < p.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        const VkMemoryHeap& m = p.memoryProperties.memoryHeaps[h];
        const int c = m.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? 0 : 1;
        bp.heapBudget[h] = m.size / 10 * 8;
        used[c] += (int64_t)vb[h].statistics.blockBytes - (int64_t)vb[h].statistics.allocationBytes;
        if (largest[c] < 0 || m.size > p.memoryProperties.memoryHeaps[largest[c]].size) largest[c] = (int)h;
    }
    for (int o = 0; o < MO_N; o++)
        for (int c = 0; c < 2; c++) used[c] += acct_bytes[o][c].load(std::memory_order_relaxed);
    for (int c = 0; c < 2; c++)
        if (largest[c] >= 0) bp.heapUsage[largest[c]] = (VkDeviceSize)std::max<int64_t>(used[c], 0);
}
}  // namespace

/// Free VRAM: the largest device-local heap's budget minus its usage (heap_budgets), plus the bytes of images already
/// waiting to be destroyed; sampled once a present (present path), read by the host (hsd_vram_free) for the game's cache
/// eviction patch
std::atomic<int64_t> vram_free_now{-1};
void mem_budget_sample(int64_t pending) {
    VkPhysicalDeviceMemoryProperties2 p;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp;
    heap_budgets(p, bp);
    int best = -1;
    for (uint32_t h = 0; h < p.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if ((p.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (best < 0 || p.memoryProperties.memoryHeaps[h].size > p.memoryProperties.memoryHeaps[best].size))
            best = (int)h;
    if (best < 0) return;
    vram_free_now.store((int64_t)bp.heapBudget[best] - (int64_t)bp.heapUsage[best] + pending, std::memory_order_relaxed);
}

/// The device-local heaps: the driver's usage and budget (heap_budgets), and VMA's blocks and allocations in them
void mem_stats(char* out, size_t cap) {
    VkPhysicalDeviceMemoryProperties2 p;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp;
    heap_budgets(p, bp);
    VmaTotalStatistics st{};
    if (vma) vmaCalculateStatistics(vma, &st);
    int k = std::snprintf(out, cap, "memory (MB):");
    for (uint32_t h = 0; h < p.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        const VmaStatistics& s = st.memoryHeap[h].statistics;
        k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " heap %u (%s, %llu): driver usage %llu of budget %llu, VMA blocks %llu "
                           "(%u), allocations %llu (%u);", h,
                           p.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "host",
                           (unsigned long long)(p.memoryProperties.memoryHeaps[h].size >> 20),
                           (unsigned long long)(bp.heapUsage[h] >> 20), (unsigned long long)(bp.heapBudget[h] >> 20),
                           (unsigned long long)(s.blockBytes >> 20), s.blockCount, (unsigned long long)(s.allocationBytes >> 20),
                           s.allocationCount);
    }
}

// VRAM: VMA's blocks count whole with the driver, so their free room is added to the owners' sum; what is left as
// "unaccounted" is the driver's own (command buffers, shader objects, descriptor state, the swapchain) or an owner missing.
// Main memory: os_mem_use_get, whose counters cost nothing to read. Its three
// kinds keep the parts apart: the guest is a memfd, so its touched pages are the shared kind; code and the store's maps are
// the file kind (mincore would answer for the page cache, not for this process); the owners count heap, the anonymous kind.
// The Vulkan host heaps are system memory the driver maps without it counting as resident, so they are printed apart; the
// imports there are the guest's pages a second time.
void mem_owner_stats(char* vram, char* main_mem, char* counts, size_t cap) {
    VkPhysicalDeviceMemoryProperties2 p;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp;
    heap_budgets(p, bp);
    VmaBudget vb[VK_MAX_MEMORY_HEAPS] = {};
    if (vma) vmaGetHeapBudgets(vma, vb);
    int64_t usage[2] = {}, sum[3] = {}, slack[2] = {}, b[MO_N][3];
    for (uint32_t h = 0; h < p.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        const int c = p.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? 0 : 1;
        usage[c] += (int64_t)bp.heapUsage[h];
        slack[c] += (int64_t)vb[h].statistics.blockBytes - (int64_t)vb[h].statistics.allocationBytes;
    }
    for (int o = 0; o < MO_N; o++)
        for (int c = 0; c < 3; c++) sum[c] += b[o][c] = acct_bytes[o][c].load(std::memory_order_relaxed);
    ImageSplit im;
    image_mem_split(im);
    char* out = vram;
    size_t k = 0;
    const auto put = [&](const char* fmt, auto... a) {
        const int n = std::snprintf(out + k, k < cap ? cap - k : 0, fmt, a...);
        if (n > 0) k += (size_t)n;
    };
    const auto mb = [](int64_t v) { return (long long)(v / (1 << 20)); };
    const auto M = [](uint64_t v) { return (long long)(v >> 20); };

    // (the counters are the sum, the walk only splits it: what the walk missed goes with "other")
    uint64_t walked = im.placeholder;
    for (uint64_t v : im.kind) walked += v;
    const int64_t unwalked = b[MO_IMAGE_STORE][0] + b[MO_IMAGE][0] + b[MO_OTHER][0] - (int64_t)walked;
    put("vram (MB): textures: store %lld, terrain %lld, glyphs %lld, texel writes %lld, copied or cleared %lld, never written %lld, "
        "placeholder %.2f; targets: transient pool %lld allocated, in use %lld (shadow maps %lld, other depth %lld, colour %lld), "
        "outside it depth %lld, colour %lld; buffers: arenas %lld, task pools %lld, constants %lld, heaps and tables %lld, layer 0 "
        "vram %lld, others' on these heaps %lld; shots, dumps, offscreen and images beside the walk %lld; room free in VMA's blocks %lld",
        M(im.kind[ImageSplit::STORE]), M(im.kind[ImageSplit::TERRAIN]), M(im.kind[ImageSplit::GLYPH]), M(im.kind[ImageSplit::TEXELS]),
        M(im.kind[ImageSplit::COPIED]), M(im.kind[ImageSplit::NEVER]), im.placeholder / 1048576.0, mb(b[MO_TRANSIENT][0]),
        M(im.pool_use[0] + im.pool_use[1] + im.pool_use[2]), M(im.pool_use[0]), M(im.pool_use[1]), M(im.pool_use[2]),
        M(im.kind[ImageSplit::TARGET_DEPTH]), M(im.kind[ImageSplit::TARGET_COLOUR]), mb(b[MO_ARENA][0]), mb(b[MO_TASK][0]),
        mb(b[MO_CONST][0]), mb(b[MO_HEAP][0]), mb(b[MO_L0_VRAM][0]),
        mb(b[MO_QUERY][0] + b[MO_UPLOAD][0] + b[MO_L0_IMPORT][0]), mb(unwalked), mb(slack[0]));
    {   // (the difference taken of the MB printed, so the three figures add up as read)
        const long long acc = mb(sum[0] + slack[0]), use = mb(usage[0]);
        put("; accounted %lld of driver usage %lld: unaccounted %lld (the driver's: command buffers, shader objects, the swapchain)",
            acc, use, use - acc);
    }

    // ---- main memory ----
    os_mem_use mu;
    os_mem_use_get(&mu);
    const long long rss = (long long)(mu.resident >> 10), anon = (long long)(mu.private_bytes >> 10),
                    file = (long long)(mu.file_bytes >> 10), shmem = (long long)(mu.shared_bytes >> 10);   // (kB)
    char host_part[512];
    uint64_t host_sum = 0, committed = 0;
    hos_mem_ledger(host_part, sizeof host_part, &host_sum, &committed);
    const int64_t ours = sum[2] + (int64_t)im.table + (int64_t)host_sum;
    const long long rest = (rss >> 10) - mb(ours);
    out = main_mem, k = 0;
    put("main memory (MB): process %lld resident = anonymous %lld + file %lld + shared %lld; anonymous: ours "
        "%lld (back end: lists %lld, store scratch %lld, shader code %lld, image records %lld; host: %s), unaccounted %lld (the "
        "driver's and the C library's heaps, stacks, tables not counted); file: the program's code and the store's maps (%lld "
        "mapped); shared: the guest's pages touched (its memory manager has handed out %lld of address space); process - ours "
        "%lld, the guest %lld: difference %lld",
        rss >> 10, anon >> 10, file >> 10, shmem >> 10, mb(ours), mb(b[MO_M_LISTS][2]), mb(b[MO_M_SCRATCH][2]), mb(b[MO_M_SHADERS][2]),
        M(im.table), host_part, (anon >> 10) - mb(ours), mb(mapped_bytes.load(std::memory_order_relaxed)), M(committed), rest,
        shmem >> 10, rest - (shmem >> 10));
    {
        const long long acc = mb(sum[1] + slack[1]), use = mb(usage[1]);
        put("; the Vulkan host heaps (system memory the driver maps, not in the resident figures): uploads %lld, layer 0 imports "
            "%lld (the guest's pages again), query reports %lld, other %lld, room free in VMA's blocks %lld: accounted %lld of "
            "driver usage %lld: unaccounted %lld", mb(b[MO_UPLOAD][1]), mb(b[MO_L0_IMPORT][1]), mb(b[MO_QUERY][1]),
            mb(sum[1] - b[MO_UPLOAD][1] - b[MO_L0_IMPORT][1] - b[MO_QUERY][1]), mb(slack[1]), acc, use, use - acc);
    }

    // ---- counts ----
    out = counts, k = 0;
    put("memory counts: allocations:");
    for (int o = 0; o < MO_M_LISTS; o++)
        put(" %s %lld%s", kOwnerNames[o], (long long)(acct_count[o][0].load(std::memory_order_relaxed) + acct_count[o][1].load(std::memory_order_relaxed)),
            o + 1 < MO_M_LISTS ? "," : "");
    put("; images %llu", (unsigned long long)im.images);
}

uint64_t mem_size(Mem* mem) {
    VmaAllocationInfo info{};
    if (mem) vmaGetAllocationInfo(vma, (VmaAllocation)mem, &info);
    return info.size;
}

}  // namespace hsd
