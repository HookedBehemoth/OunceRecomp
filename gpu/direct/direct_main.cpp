// The direct back end's start (docs/direct/m1-interface.md): libvulkan, the instance, the device (one graphics and compute
// queue, the sparse-binding queue for layer 0), and the version-14 table the host calls (gpu/vk/vk_api.h). Cut down from
// gpu/vk/vk_main.cpp: no start-up screen, menu, upscalers, vendor hooks or switches.
#include <dlfcn.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "direct_internal.h"
#include "direct_api.h"

#define HSD_VK_DEFINE(f) PFN_##f f;
namespace hsd::vkfn {
HSD_VK_GLOBAL(HSD_VK_DEFINE)
HSD_VK_INSTANCE(HSD_VK_DEFINE)
HSD_VK_DEVICE(HSD_VK_DEFINE)
}  // namespace hsd::vkfn
#undef HSD_VK_DEFINE

namespace hsd {

const MkwVkHost* host;
Device dev;

void logf(const char* fmt, ...) {
    char b[2048];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (host) host->log(b);
}

bool dbg(const char* name) {
    static const std::string list = [] {
        const char* e = getenv("HOS_DBG");
        return "," + std::string(e ? e : "") + ",";
    }();
    return list.find("," + std::string(name) + ",") != std::string::npos;
}

namespace {

PFN_vkGetInstanceProcAddr get_instance_proc;

bool load_global() {
    void* m = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!m) return false;
    get_instance_proc = (PFN_vkGetInstanceProcAddr)dlsym(m, "vkGetInstanceProcAddr");
    if (!get_instance_proc) return false;
#define HSD_VK_LOAD(f) f = (PFN_##f)get_instance_proc(nullptr, #f);
    HSD_VK_GLOBAL(HSD_VK_LOAD)
#undef HSD_VK_LOAD
    return vkCreateInstance != nullptr;
}

bool has_extension(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const auto& e : all)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

bool create_instance() {
    uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) {
        logf("direct: the Vulkan loader is %u.%u: 1.3 is needed", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version));
        return false;
    }
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> all(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, all.data());
    // (every window system's surface the loader has, for whichever one SDL's window is on: X11 or Wayland)
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME};
    for (const char* want : {"VK_KHR_xlib_surface", "VK_KHR_xcb_surface", "VK_KHR_wayland_surface"})
        if (has_extension(all, want)) exts.push_back(want);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "hookshot-direct";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    if (vkCreateInstance(&ci, nullptr, &dev.instance) != VK_SUCCESS) {
        logf("direct: vkCreateInstance failed");
        return false;
    }
#define HSD_VK_LOAD(f) f = (PFN_##f)get_instance_proc(dev.instance, #f);
    HSD_VK_INSTANCE(HSD_VK_LOAD)
#undef HSD_VK_LOAD
    return true;
}

bool pick_device() {   // (the discrete GPU: this PC has one)
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(dev.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(dev.instance, &n, all.data());
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[i], &p);
        const int score = p.apiVersion < VK_API_VERSION_1_3 ? -1 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 0;
        if (score > best_score) best = (int)i, best_score = score;
    }
    if (best < 0 || best_score < 0) {
        logf("direct: no Vulkan 1.3 GPU");
        return false;
    }
    dev.physical = all[best];
    vkGetPhysicalDeviceProperties(dev.physical, &dev.props);
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &dev.memory);
    return true;
}

bool create_device() {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev.physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(dev.physical, nullptr, &n, exts.data());
    std::vector<const char*> want;
    for (const char* e : {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                          VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME}) {
        if (!has_extension(exts, e)) {
            logf("direct: the device lacks %s", e);
            return false;
        }
        want.push_back(e);
    }
    dev.dma_buf_import = true;

    // the queues: one family for graphics and compute; a sparse-binding-only one for layer 0's binds
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, families.data());
    dev.queue_family = UINT32_MAX;
    uint32_t sparse_family = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        const VkQueueFlags f = families[i].queueFlags;
        if (dev.queue_family == UINT32_MAX && (f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            dev.queue_family = i;
        else if (sparse_family == UINT32_MAX && (f & VK_QUEUE_SPARSE_BINDING_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
            sparse_family = i;
    }
    if (dev.queue_family == UINT32_MAX || sparse_family == UINT32_MAX) {
        logf("direct: no graphics and compute family, or no sparse-binding family");
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qs[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    qs[0].queueFamilyIndex = dev.queue_family, qs[0].queueCount = 1, qs[0].pQueuePriorities = &priority;
    qs[1].queueFamilyIndex = sparse_family, qs[1].queueCount = 1, qs[1].pQueuePriorities = &priority;

    // features: every core one the device has (the later milestones use them), but robust buffer access (it costs)
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13;
    if (dev.props.apiVersion >= VK_API_VERSION_1_4) f13.pNext = &f14;
    vkGetPhysicalDeviceFeatures2(dev.physical, &f);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore || !f12.bufferDeviceAddress) {
        logf("direct: the GPU lacks dynamic rendering, synchronization2, timeline semaphores or buffer device addresses");
        return false;
    }
    f.features.robustBufferAccess = VK_FALSE;
    f14.pipelineRobustness = VK_FALSE;
    dev.features = f.features;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f;
    ci.queueCreateInfoCount = 2;
    ci.pQueueCreateInfos = qs;
    ci.enabledExtensionCount = (uint32_t)want.size();
    ci.ppEnabledExtensionNames = want.data();
    if (vkCreateDevice(dev.physical, &ci, nullptr, &dev.device) != VK_SUCCESS) {
        logf("direct: vkCreateDevice failed");
        return false;
    }
#define HSD_VK_LOAD(f) f = (PFN_##f)vkGetDeviceProcAddr(dev.device, #f);
    HSD_VK_DEVICE(HSD_VK_LOAD)
#undef HSD_VK_LOAD
    vkGetDeviceQueue(dev.device, dev.queue_family, 0, &dev.queue);
    vkGetDeviceQueue(dev.device, sparse_family, 0, &dev.sparse_queue);
    return true;
}

// ---- the object layer: registrations accepted and counted (M1: nothing reads them) ----
enum { R_TEX, R_SAMP, R_TEX_POOL, R_TEX_MADE, R_TEX_GONE, R_PROG_SET, R_PROG_GONE, R_POOL, R_POOL_NATIVE, R_WRITE_TEXELS,
       R_READ_TEXELS, R_COUNT };
std::atomic<int64_t> regs[R_COUNT];
void count(int k) { regs[k].fetch_add(1, std::memory_order_relaxed); }

int start(void* hwnd) {
    if (!sync_init()) return 0;
    layer0_on();
    return present_start(hwnd);
}
const char* describe() {
    static std::string d;
    d = std::string(dev.props.deviceName) + " (Vulkan, direct)";
    return d.c_str();
}
void stats(int64_t* draws, int64_t* skipped, int64_t* pipelines) { *draws = *skipped = *pipelines = 0; }
void register_texture(int, const nvn_tex*, const nvn_view*) { count(R_TEX); }
void register_sampler(int, const nvn_samp_par*) { count(R_SAMP); }
void set_texture_pool_size(int) { count(R_TEX_POOL); }
void texture_released(const nvn_tex*) { count(R_TEX_GONE); }
void texture_created(const nvn_tex*) { count(R_TEX_MADE); }
void program_set(const nvn_prog*) { count(R_PROG_SET); }
void program_released(const nvn_prog*) { count(R_PROG_GONE); }
void read_texels(const nvn_tex*, unsigned, unsigned, const NVNcopyRegion*, uint8_t*, uint64_t, uint64_t) { count(R_READ_TEXELS); }
void write_texels(const nvn_tex*, unsigned, unsigned, const NVNcopyRegion*, const uint8_t*, uint64_t, uint64_t) { count(R_WRITE_TEXELS); }
void invalidate(uint64_t, uint64_t) {}
void vmap_changing(uint64_t, uint64_t) {}
void wait_writebacks_reading(const void*, uint64_t) {}
void pool_created(uint64_t gpu, uint64_t size) {
    count(R_POOL);
    layer0_pool_created(gpu, size);
}
void pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint64_t key, int on) {
    count(R_POOL_NATIVE);
    layer0_pool_native(gpu, size, flags, key, on);
}
int guest_imported(uint64_t lo, uint64_t hi) { return layer0_bound(lo, hi) ? 1 : 0; }

MkwVkRenderer table;

}  // namespace

/// The registrations since the start, for the 600-present line
void regs_stats(char* out, size_t cap) {
    std::snprintf(out, cap, "registrations: %lld textures, %lld samplers, %lld textures made, %lld finalized, %lld programs set, "
                  "%lld released, %lld pools (%lld native calls), %lld texel writes, %lld texel reads (answered with nothing)",
                  (long long)regs[R_TEX].load(), (long long)regs[R_SAMP].load(), (long long)regs[R_TEX_MADE].load(),
                  (long long)regs[R_TEX_GONE].load(), (long long)regs[R_PROG_SET].load(), (long long)regs[R_PROG_GONE].load(),
                  (long long)regs[R_POOL].load(), (long long)regs[R_POOL_NATIVE].load(), (long long)regs[R_WRITE_TEXELS].load(),
                  (long long)regs[R_READ_TEXELS].load());
}

}  // namespace hsd

extern "C" __attribute__((visibility("default"))) const MkwVkRenderer* mkwvk_init(const MkwVkHost* h) {
    using namespace hsd;
    if (!h || h->version != MKWVK_API_VERSION) return nullptr;
    host = h;
    if (!load_global()) {
        logf("direct: libvulkan.so.1 not found");
        return nullptr;
    }
    if (!create_instance() || !pick_device() || !create_device()) return nullptr;
    logf("direct: %s, Vulkan %u.%u, driver %#x: the direct back end (M1: every list a counted no-op)", dev.props.deviceName,
         VK_VERSION_MAJOR(dev.props.apiVersion), VK_VERSION_MINOR(dev.props.apiVersion), dev.props.driverVersion);
    table.version = MKWVK_API_VERSION;
    table.start = start;
    table.resize = present_resize;
    table.describe = describe;
    table.stats = stats;
    table.wait_writebacks_reading = wait_writebacks_reading;
    table.nvn_present = present;
    table.nvn_register_texture = register_texture;
    table.nvn_register_sampler = register_sampler;
    table.nvn_set_texture_pool_size = set_texture_pool_size;
    table.nvn_texture_released = texture_released;
    table.nvn_program_released = program_released;
    table.nvn_read_texels = read_texels;
    table.nvn_write_texels = write_texels;
    table.nvn_invalidate = invalidate;
    table.nvn_pool_created = pool_created;
    table.guest_imported = guest_imported;
    table.nvn_vmap_changing = vmap_changing;
    table.nvn_texture_created = texture_created;
    table.nvn_program_set = program_set;
    table.nvn_pool_native = pool_native;
    sync_fill_table(table);   // (nvn_submit, _q, fence, fence_q, pushed, replayed, wait, finish, event_signal)
    return &table;   // (option, option_set: none, the host's defaults)
}
