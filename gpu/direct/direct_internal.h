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
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties)
#define HSD_VK_DEVICE(X)                                                                                               \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit2) X(vkQueueBindSparse) X(vkCreateSwapchainKHR)             \
    X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)                  \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)                   \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier2) X(vkCmdClearColorImage) X(vkCreateFence) X(vkWaitForFences)         \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue)      \
    X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory)          \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkGetBufferDeviceAddress) X(vkGetMemoryFdPropertiesKHR)         \
    X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) X(vkDestroyBuffer) X(vkCmdBeginRendering)               \
    X(vkCmdEndRendering) X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) \
    X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearDepthStencilImage) X(vkDestroyCommandPool) X(vkResetCommandBuffer)      \
    X(vkCmdExecuteCommands)

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
/// (M4f) a line longer than logf's 2 KB, in pieces: the first as it is, the rest each "<prefix>+ ..."
void log_long(const char* line, const char* prefix);
/// HOS_DBG=<name>,<name>...: a debug aid named there is on (the back end's only switch, read once)
bool dbg(const char* name);
/// HOS_DBG=...,<name>:<value>,...: the value (null: not named)
const char* dbg_value(const char* name);

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
    bool nested_rendering = false;   // (M3a: VK_KHR_maintenance7: secondaries and inline commands in one dynamic rendering)
    // the window (none: presents clear an image of their own)
    bool offscreen = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
};
extern Device dev;
extern PFN_vkGetInstanceProcAddr get_instance_proc;   // (direct_main.cpp: VMA's functions come from it)

// ---- direct_vma.cpp: images and buffers in VMA's blocks ----
struct Mem;   // (a VmaAllocation)
bool vma_init();
/// Who holds device memory, for the 600-present line "memory by owner" (mem_owner_stats). Every vkAllocateMemory of the back
/// end and every image and buffer in VMA's blocks is counted to one of them, in bytes and in allocations, apart for the
/// device-local heaps and the host's: what the driver reports past their sum is its own (command buffers, shader objects,
/// descriptor state) or an owner not counted here
enum MemOwner : int {
    MO_ARENA,       // (direct_draw.cpp: the frame arenas' chunks, a command slot's)
    MO_TASK,        // (direct_task.cpp: a command slot's task pool)
    MO_QUERY,       // (direct_query.cpp: a command slot's report buffer; its query pools are the driver's)
    MO_CONST,       // (direct_shader.cpp: the programs' constants, c1)
    MO_HEAP,        // (direct_heap.cpp, direct_draw.cpp: the descriptor heaps, the window table, the zero buffer)
    MO_UPLOAD,      // (direct_cmd.cpp: the threads' upload staging)
    MO_TRANSIENT,   // (the transient pool below)
    MO_L0_VRAM,     // (direct_pools.cpp: layer 0's VRAM blocks)
    MO_L0_IMPORT,   // (direct_pools.cpp: guest memory's chunks, imported)
    MO_IMAGE_STORE, // (direct_image.cpp: the images of textures from the store)
    MO_IMAGE,       // (direct_image.cpp: the game's other images)
    MO_OTHER,       // (the rest: the placeholder, shots, dumps and readbacks, the marks' buffer, the offscreen image)
    MO_N
};
/// bytes of memory type `type` made (negative: freed) for the owner. Two relaxed atomic adds: callable anywhere
void mem_account(MemOwner owner, uint32_t type, int64_t bytes);
/// The line: the owners, `held` (what has no byte figure: direct_cmd.cpp's and the shaders' counts), the sums against the driver's
void mem_owner_stats(char* out, size_t cap, const char* held);
bool mem_image(const VkImageCreateInfo& ci, VkImage& image, Mem*& mem, MemOwner owner = MO_OTHER);
void mem_image_free(VkImage image, Mem* mem);
/// The transient pool: one device allocation of size bytes of a device-local type in type_bits, made once (false: none)
bool mem_pool_make(uint64_t size, uint32_t type_bits);
uint64_t mem_pool_size();
/// An image bound in the pool at offset (false: no pool, a memory type or alignment it cannot take, past its end)
bool mem_pool_bind(VkImage image, uint64_t offset, const VkMemoryRequirements& req);
/// A buffer; host_visible: mapped (host-cached for reads where the device has it)
bool mem_buffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, VkBuffer& buffer, Mem*& mem, void** mapped,
                MemOwner owner = MO_OTHER);
void mem_buffer_free(VkBuffer buffer, Mem* mem);
void mem_flush(Mem* mem);
void mem_invalidate(Mem* mem);
uint64_t mem_size(Mem* mem);
void mem_stats(char* out, size_t cap);
void mem_budget_sample(int64_t pending);   // (M6f: free VRAM once a present, with the bytes of images waiting to die)
extern std::atomic<int64_t> vram_free_now;   // (M6f: the heaps' usage and budget, VMA's blocks)
extern std::atomic<int64_t> st_objects;   // (Vulkan objects made since the start: images, views, buffers, pools, command buffers)

// ---- direct_heap.cpp: mapped buffers with an allocation of their own ----
/// A buffer in device-local, host-visible memory (the BAR) where there is one, mapped, with its device address
bool mapped_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& map,
                   VkDeviceAddress& address, MemOwner owner);
/// One of them destroyed and its memory freed (the GPU is past it)
void mapped_buffer_free(VkBuffer buffer, VkDeviceMemory memory, MemOwner owner);

// ---- direct_sync.cpp: the queue's timeline, NVN fences, the guest's submits ----
bool sync_init();
uint64_t sync_pushed();      // (timeline values sent)
uint64_t sync_completed();   // (reached on the GPU)
bool sync_wait_value(uint64_t value, uint64_t timeout_ns);
void sync_fill_table(MkwVkRenderer& t);
/// One empty submission that signals the next timeline value (queue_mu taken here); the value, 0 on failure
uint64_t sync_send(uint64_t wait_value, const VkCommandBuffer* cbs = nullptr, uint32_t n = 0);
void sync_stats(char* out, size_t cap, double frames);
void sync_age_stats(char* out, size_t cap);   // (the transient pool: how late lists reach the queue)
/// The host's present, run in its queue's order (present_run)
void sync_present(const nvn_tex* t, const int crop[4]);   // (the submits' part of the 600-present line)

// ---- direct_lists.cpp ----
void lists_stats(char* out, size_t cap, double frames);
void sites_stats(char* out, size_t cap, double frames);

// ---- direct_main.cpp ----
void regs_stats(char* out, size_t cap);

// ---- direct_present.cpp ----
int present_start(void* hwnd);
void present_resize(int width, int height);
void present(const nvn_tex* t, const int crop[4]);   // (the host's call: noted, run in the queue's order)
/// The present's GPU work, when every submission handed to its queue before it has been sent (direct_sync.cpp's mu held)
void present_run(const nvn_tex& t, const int crop[4]);
uint64_t present_count();   // (presents the host asked for so far: the frame number)

// ---- direct_pools.cpp: layer 0 ----
bool layer0_on();
void layer0_pool_created(uint64_t gpu, uint64_t size);
void layer0_pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint64_t key, int on);
bool layer0_bound(uint64_t lo, uint64_t hi);
/// Guest [gpu, gpu + size) as a window buffer and offset, bound first if it is not (imported now: a wait); false: not
/// in one window, not guest memory, or not importable
bool layer0_span(uint64_t gpu, uint64_t size, VkBuffer& buffer, VkDeviceSize& offset);
void layer0_stats(char* out, size_t cap);
/// (M5, debug logs) the game's pool holding guest addr: base, size, NVN flags, whether addr's granule is in VRAM; false: none
bool layer0_pool_of(uint64_t addr, uint64_t& base, uint64_t& size, uint32_t& flags, bool& in_vram);

}  // namespace hsd
