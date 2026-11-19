// The direct back end's shared state (renderer, ounce_renderer.so; docs/direct-backend-plan.md). Linux and RADV only.
// Vulkan is loaded from libvulkan.so.1 at start (VK_NO_PROTOTYPES, ounce/builder): the functions below, global, instance
// and device level.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <vulkan/vulkan.h>

#include "vk_api.h"   // (the table the host calls)
#include "settings.h"   // (the settings, through the host: vk_main.cpp)

#define GPU_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceVersion)
#define GPU_VK_INSTANCE(X)                                                                                             \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2)                    \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)                                    \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties)
#define GPU_VK_DEVICE(X)                                                                                               \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit2) X(vkQueueBindSparse) X(vkCreateSwapchainKHR)             \
    X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)                  \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)                   \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier2) X(vkCmdClearColorImage) X(vkCreateFence) X(vkWaitForFences)         \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue)      \
    X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory)          \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkGetBufferDeviceAddress) X(vkGetMemoryFdPropertiesKHR)         \
    X(vkGetMemoryHostPointerPropertiesEXT)                                                                             \
    X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) X(vkDestroyBuffer) X(vkCmdBeginRendering)               \
    X(vkCmdEndRendering) X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) \
    X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearDepthStencilImage) X(vkDestroyCommandPool) X(vkResetCommandBuffer)      \
    X(vkCmdExecuteCommands)

#define GPU_VK_DECLARE(f) extern PFN_##f f;
namespace hsd::vkfn {
GPU_VK_GLOBAL(GPU_VK_DECLARE)
GPU_VK_INSTANCE(GPU_VK_DECLARE)
GPU_VK_DEVICE(GPU_VK_DECLARE)
}  // namespace hsd::vkfn
#undef GPU_VK_DECLARE

namespace hsd {
using namespace vkfn;

extern const MkwVkHost* host;
void logf(const char* fmt, ...);
/// Prints the line at its first call under the key, and again only when its text changes
void logf_changed(const char* key, const char* text);
/// A line longer than logf's buffer, in pieces: the first as it is, the rest each "<prefix>+ ..."
void log_long(const char* line, const char* prefix);
/// A debug aid (a debug.* setting, renderer/api/settings.h) is on: a bool set, or a string given
bool dbg(setting_id id);
extern bool deferred_check;   // (debug.deferredcheck, vk_draw.cpp; set by mkwvk_init)
/// A debug log: its setting or debug.logall. Read it once into a static
bool dbg_log(setting_id id);
/// A debug aid's value (null: not given)
const char* dbg_value(setting_id id);
/// The Vulkan loader's own variables (VK_LOADER_LAYERS_ENABLE, VK_INSTANCE_LAYERS) enable a layer whose name has part
bool loader_layer_enabled(const char* part);

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
    VkQueue sparse_queue = VK_NULL_HANDLE;   // (layer 0's binds: create_device picks it; under sparse_mu)
    std::mutex sparse_mu;
    bool sparse_shared = false;   // (sparse_queue is queue: binds under queue_mu)
    bool dma_buf_import = false;
    bool host_import = false;   // (VK_EXT_external_memory_host)
    uint64_t host_align = 0;    // (its minImportedHostPointerAlignment)
    bool nested_rendering = false;   // (M3a: VK_KHR_maintenance7: secondaries and inline commands in one dynamic rendering)
    bool memory_budget = false;      // (VK_EXT_memory_budget: the driver's heap budgets and usage; without it, estimated)
    // the window (none: presents clear an image of their own)
    bool offscreen = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
};
extern Device dev;
extern PFN_vkGetInstanceProcAddr get_instance_proc;   // (vk_main.cpp: VMA's functions come from it)

// ---- vk_vma.cpp: images and buffers in VMA's blocks ----
struct Mem;   // (a VmaAllocation)
bool vma_init();
/// Memory by owner. Each allocation of device memory and each of the back end's larger heap allocations is counted to an
/// owner; the periodic memory lines print them against the driver's and the process's totals
enum MemOwner : int {
    MO_ARENA,
    MO_TASK,
    MO_QUERY,       // (the report buffers; the query pools are the driver's)
    MO_CONST,       // (the programs' constants)
    MO_TABLES,      // (the window table, the zero buffer)
    MO_UPLOAD,
    MO_TRANSIENT,
    MO_L0_VRAM,
    MO_L0_IMPORT,   // (guest memory imported: the guest's pages, not new memory)
    MO_IMAGE_STORE,
    MO_IMAGE,       // (the game's images not from the store)
    MO_OTHER,       // (the default: the placeholder, shots, dumps and readbacks)
    // (the process's own memory: mem_account_main)
    MO_M_LISTS,
    MO_M_SCRATCH,   // (the store readers' scratch)
    MO_M_SHADERS,   // (the variants' SPIR-V and constants, kept)
    MO_N
};
/// bytes of Vulkan memory type `type` made, or freed when negative. Lock-free
void mem_account(MemOwner owner, uint32_t type, int64_t bytes);
void mem_account_main(MemOwner owner, int64_t bytes);
/// A file mapped for reading: named in the main memory line, not summed (its resident part is not known)
void mem_main_mapped(size_t n);
/// What the images hold now, in bytes, by what they are for (a walk of the images under their lock)
struct ImageSplit {
    enum { STORE, TERRAIN, GLYPH, TEXELS, COPIED, NEVER, TARGET_DEPTH, TARGET_COLOUR, KINDS };
    uint64_t kind[KINDS] = {};   // (images with memory of their own, by their first source; rendered ones by aspect)
    uint64_t pool_use[3] = {};   // (the transient pool's ranges owned now: square depth (shadow maps), other depth, colour)
    uint64_t placeholder = 0, table = 0, images = 0;   // (table: the images' records, main memory)
};
void image_mem_split(ImageSplit& s);
constexpr uint64_t kMemoryPeriod = 600;   // (presents between two blocks of the memory lines)
/// The memory lines' text: VRAM by purpose, main memory by owner, the allocation counts
void mem_owner_stats(char* vram, char* main_mem, char* counts, size_t cap);
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

// ---- vk_pool.cpp: mapped buffers with an allocation of their own ----
/// A buffer in device-local, host-visible memory (the BAR) where there is one, mapped, with its device address
bool mapped_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& map,
                   VkDeviceAddress& address, MemOwner owner);
/// One of them destroyed and its memory freed (the GPU is past it)
void mapped_buffer_free(VkBuffer buffer, VkDeviceMemory memory, MemOwner owner);

// ---- vk_sync.cpp: the queue's timeline, NVN fences, the guest's submits ----
bool sync_init();
uint64_t sync_pushed();      // (timeline values sent)
uint64_t sync_completed();   // (reached on the GPU)
bool sync_wait_value(uint64_t value, uint64_t timeout_ns);
void sync_fill_table(MkwVkRenderer& t);
/// A submission that signals the next timeline value, handed to the sender thread (submitted in value order); the value
uint64_t sync_send(uint64_t wait_value, const VkCommandBuffer* cbs = nullptr, uint32_t n = 0);
/// The work handed to the sender and not submitted yet, submitted now (queue_mu held): before any other use of the queue
void sync_send_handed();
void sync_stats(char* out, size_t cap, double frames);
void sync_age_stats(char* out, size_t cap);   // (the transient pool: how late lists reach the queue)
/// The host's present, run in its queue's order (present_run)
void sync_present(const nvn_tex* t, const int crop[4]);   // (the submits' part of the 600-present line)

// ---- vk_lists.cpp ----
void lists_stats(char* out, size_t cap, double frames);
void sites_stats(char* out, size_t cap, double frames);

// ---- vk_main.cpp ----
void regs_stats(char* out, size_t cap);

// ---- vk_present.cpp ----
int present_start(void* hwnd);
void present_resize(int width, int height);
void present(const nvn_tex* t, const int crop[4]);   // (the host's call: noted, run in the queue's order)
/// The present's GPU work, when every submission handed to its queue before it has been sent (vk_sync.cpp's mu held)
void present_run(const nvn_tex& t, const int crop[4]);
uint64_t present_count();   // (presents the host asked for so far: the frame number)

// ---- vk_pools.cpp: layer 0 ----
bool layer0_on();
void layer0_pool_created(uint64_t gpu, uint64_t size);
void layer0_pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint64_t key, int on);
bool layer0_bound(uint64_t lo, uint64_t hi);
/// Guest [gpu, gpu + size) as a window buffer and offset, bound first if it is not (imported now: a wait); false: not
/// in one window, not guest memory, or not importable
bool layer0_span(uint64_t gpu, uint64_t size, VkBuffer& buffer, VkDeviceSize& offset);
void layer0_stats(char* out, size_t cap);
/// (debug logs) the game's pool holding guest addr: base, size, NVN flags, whether addr's granule is in VRAM; false: none
bool layer0_pool_of(uint64_t addr, uint64_t& base, uint64_t& size, uint32_t& flags, bool& in_vram);

}  // namespace hsd
