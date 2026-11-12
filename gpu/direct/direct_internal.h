// The direct back end's shared state (gpu/direct, hookshot_direct.so; docs/direct-backend-plan.md). Linux and RADV only.
// Vulkan is loaded from libvulkan.so.1 at start (VK_NO_PROTOTYPES, nx2/build.py): the functions below, global, instance
// and device level.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <vulkan/vulkan.h>

#include "vk_api.h"   // (gpu/vk: the table the host calls, version 14; not changed)

#define HSD_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceVersion)
#define HSD_VK_INSTANCE(X)                                                                                             \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2)                    \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)                                    \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define HSD_VK_DEVICE(X)                                                                                               \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit2) X(vkQueueBindSparse) X(vkCreateSwapchainKHR)             \
    X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)                  \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)                   \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier2) X(vkCmdClearColorImage) X(vkCreateFence) X(vkWaitForFences)         \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue)      \
    X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory)          \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkGetBufferDeviceAddress) X(vkGetMemoryFdPropertiesKHR)

#define HSD_VK_DECLARE(f) extern PFN_##f f;
namespace hsd::vkfn {
HSD_VK_GLOBAL(HSD_VK_DECLARE)
HSD_VK_INSTANCE(HSD_VK_DECLARE)
HSD_VK_DEVICE(HSD_VK_DECLARE)
}  // namespace hsd::vkfn
#undef HSD_VK_DECLARE

namespace hsd {
using namespace vkfn;

extern const MkwVkHost* host;
void logf(const char* fmt, ...);
/// HOS_DBG=<name>,<name>...: a debug aid named there is on (the back end's only switch, read once)
bool dbg(const char* name);

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceFeatures features{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;   // (the one queue: every submit and present, under queue_mu)
    std::mutex queue_mu;
    VkQueue sparse_queue = VK_NULL_HANDLE;   // (layer 0's binds: a sparse-binding-only family, under sparse_mu)
    std::mutex sparse_mu;
    bool dma_buf_import = false;
    // the window (none: presents clear an image of their own)
    bool offscreen = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
};
extern Device dev;

// ---- direct_sync.cpp: the queue's timeline, NVN fences, the guest's submits ----
bool sync_init();
uint64_t sync_pushed();      // (timeline values sent)
uint64_t sync_completed();   // (reached on the GPU)
bool sync_wait_value(uint64_t value, uint64_t timeout_ns);
void sync_fill_table(MkwVkRenderer& t);
/// One empty submission that signals the next timeline value (queue_mu taken here); the value, 0 on failure
uint64_t sync_send(uint64_t wait_value);
void sync_stats(char* out, size_t cap, double frames);   // (the submits' part of the 600-present line)

// ---- direct_lists.cpp ----
void lists_stats(char* out, size_t cap, double frames);
void sites_stats(char* out, size_t cap, double frames);

// ---- direct_main.cpp ----
void regs_stats(char* out, size_t cap);

// ---- direct_present.cpp ----
int present_start(void* hwnd);
void present_resize(int width, int height);
void present(const nvn_tex* t, const int crop[4]);

// ---- direct_pools.cpp: layer 0 ----
bool layer0_on();
void layer0_pool_created(uint64_t gpu, uint64_t size);
void layer0_pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint64_t key, int on);
bool layer0_bound(uint64_t lo, uint64_t hi);
void layer0_stats(char* out, size_t cap);

}  // namespace hsd
