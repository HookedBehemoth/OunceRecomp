// Images and buffers in Vulkan Memory Allocator's blocks (v3.3.0, HOOKSHOT_VMA_HEADERS), cut down from gpu/vk/vk_vma.cpp:
// device-local images, host-visible buffers (staging, dumps). Its Vulkan functions come from vkGetInstanceProcAddr.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "direct_check.h"
#include "direct_internal.h"
#include <vk_mem_alloc.h>

#include <cstdio>

namespace hsd {

std::atomic<int64_t> st_objects{0};

namespace {
VmaAllocator vma = VK_NULL_HANDLE;
}

bool vma_init() {
    VmaVulkanFunctions f{};
    f.vkGetInstanceProcAddr = get_instance_proc;
    f.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    ci.physicalDevice = dev.physical;
    ci.device = dev.device;
    ci.instance = dev.instance;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &f;
    ci.preferredLargeHeapBlockSize = 128ull << 20;
    return vmaCreateAllocator(&ci, &vma) == VK_SUCCESS;
}

bool mem_image(const VkImageCreateInfo& ci, VkImage& image, Mem*& mem) {
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocation a = nullptr;
    if (vmaCreateImage(vma, &ci, &ai, &image, &a, nullptr) != VK_SUCCESS) return false;
    mem = (Mem*)a;
    object_made(1, "an image");
    return true;
}
void mem_image_free(VkImage image, Mem* mem) { vmaDestroyImage(vma, image, (VmaAllocation)mem); }

bool mem_buffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, VkBuffer& buffer, Mem*& mem, void** mapped) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = host_visible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (host_visible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &a, &info) != VK_SUCCESS) return false;
    mem = (Mem*)a;
    if (mapped) *mapped = info.pMappedData;
    object_made(1, "a buffer");
    return true;
}
void mem_buffer_free(VkBuffer buffer, Mem* mem) { vmaDestroyBuffer(vma, buffer, (VmaAllocation)mem); }
void mem_flush(Mem* mem) { vmaFlushAllocation(vma, (VmaAllocation)mem, 0, VK_WHOLE_SIZE); }
void mem_invalidate(Mem* mem) { vmaInvalidateAllocation(vma, (VmaAllocation)mem, 0, VK_WHOLE_SIZE); }
/// (M6f) Free VRAM as the driver sees it (VK_EXT_memory_budget: the largest device-local heap's budget minus its usage,
/// so the compositor and other processes count), plus the bytes of images already waiting to be destroyed; sampled once a
/// present (present path), read by the host (hsd_vram_free) for the game's cache eviction patch
std::atomic<int64_t> vram_free_now{-1};
void mem_budget_sample(int64_t pending) {
    static PFN_vkGetPhysicalDeviceMemoryProperties2 props2 =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)get_instance_proc(dev.instance, "vkGetPhysicalDeviceMemoryProperties2");
    if (!props2) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    p.pNext = &bp;
    props2(dev.physical, &p);
    int best = -1;
    for (uint32_t h = 0; h < p.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if ((p.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (best < 0 || p.memoryProperties.memoryHeaps[h].size > p.memoryProperties.memoryHeaps[best].size))
            best = (int)h;
    if (best < 0) return;
    vram_free_now.store((int64_t)bp.heapBudget[best] - (int64_t)bp.heapUsage[best] + pending, std::memory_order_relaxed);
}

/// (M6f) The device-local heaps: the driver's usage and budget (VK_EXT_memory_budget), and VMA's blocks and allocations in them
void mem_stats(char* out, size_t cap) {
    static PFN_vkGetPhysicalDeviceMemoryProperties2 props2 =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)get_instance_proc(dev.instance, "vkGetPhysicalDeviceMemoryProperties2");
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    p.pNext = &bp;
    if (props2) props2(dev.physical, &p);
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

uint64_t mem_size(Mem* mem) {
    VmaAllocationInfo info{};
    if (mem) vmaGetAllocationInfo(vma, (VmaAllocation)mem, &info);
    return info.size;
}

}  // namespace hsd
