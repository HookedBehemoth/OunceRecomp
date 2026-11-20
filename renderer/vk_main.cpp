// The direct back end's start: libvulkan, the instance, the device (one graphics and compute
// queue, the sparse-binding queue for layer 0), and the version-14 table the host calls (renderer/api/vk_api.h). Cut down from
// gpu/legacy/vk/vk_main.cpp: no start-up screen, menu, upscalers, vendor hooks or switches.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "vk_compute.h"
#include "vk_marks.h"
#include "vk_passtime.h"
#include "vk_pstats.h"
#include "vk_internal.h"
#include "os.h"
#include "vk_list_api.h"
#include "vk_image.h"
#include "vk_pool.h"
#include "vk_shader.h"
#include "vk_query.h"
#include "vk_upscale.h"
#include "vk_check.h"
#include "vk_draw.h"
#include "vk_work.h"

#define GPU_VK_DEFINE(f) PFN_##f f;
namespace hsd::vkfn {
GPU_VK_GLOBAL(GPU_VK_DEFINE)
GPU_VK_INSTANCE(GPU_VK_DEFINE)
GPU_VK_DEVICE(GPU_VK_DEFINE)
}  // namespace hsd::vkfn
#undef GPU_VK_DEFINE

namespace hsd {

const MkwVkHost* host;
Device dev;

void logf(const char* fmt, ...) {
    char b[4096];   // (a line cut here ends in "~")
    va_list a;
    va_start(a, fmt);
    const int want = std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (want >= (int)sizeof b) b[sizeof b - 2] = '~';
    if (host) host->log(b);
}

void logf_changed(const char* key, const char* text) {
    static std::mutex mu;
    static std::map<std::string, std::string> last;
    {
        std::lock_guard lk(mu);
        std::string& was = last[key];
        if (was == text) return;
        was = text;
    }
    logf("%s", text);
}

void log_long(const char* line, const char* prefix) {
    const size_t n = std::strlen(line), piece = 1900;
    for (size_t at = 0; at < n; at += piece) {
        if (!at) logf("%.*s", (int)std::min(piece, n), line);
        else logf("%s+ %.*s", prefix, (int)std::min(piece, n - at), line + at);
    }
}

bool dbg(setting_id id) { return host->setting_int(id) != 0; }

bool dbg_log(setting_id id) { return dbg(id) || dbg(SET_DBG_LOGALL); }

const char* dbg_value(setting_id id) { return host->setting_given(id) ? host->setting_str(id) : nullptr; }

bool loader_layer_enabled(const char* part) {
    for (const char* v : {"VK_LOADER_LAYERS_ENABLE", "VK_INSTANCE_LAYERS"})
        if (const char* e = getenv(v); e && std::strstr(e, part)) return true;
    return false;
}

PFN_vkGetInstanceProcAddr get_instance_proc;


namespace {

bool load_global() {
    os_lib* m = os_lib_open(OS_LIB("libvulkan.so.1", "vulkan-1.dll"));
    if (!m) return false;
    get_instance_proc = (PFN_vkGetInstanceProcAddr)os_lib_sym(m, "vkGetInstanceProcAddr");
    if (!get_instance_proc) return false;
#define GPU_VK_LOAD(f) f = (PFN_##f)get_instance_proc(nullptr, #f);
    GPU_VK_GLOBAL(GPU_VK_LOAD)
#undef GPU_VK_LOAD
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
    // (every window system's surface the loader has, for whichever one SDL's window is on: X11, Wayland or Win32)
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME};
    for (const char* want : {"VK_KHR_xlib_surface", "VK_KHR_xcb_surface", "VK_KHR_wayland_surface", "VK_KHR_win32_surface"})
        if (has_extension(all, want)) exts.push_back(want);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ounce-renderer";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    validation_instance_info(ci, exts, all);   // (M6b: the messenger, when the validation layer is in the process)
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    if (vkCreateInstance(&ci, nullptr, &dev.instance) != VK_SUCCESS) {
        logf("direct: vkCreateInstance failed");
        return false;
    }
#define GPU_VK_LOAD(f) f = (PFN_##f)get_instance_proc(dev.instance, #f);
    GPU_VK_INSTANCE(GPU_VK_LOAD)
#undef GPU_VK_LOAD
    validation_instance_made(dev.instance);
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
    if (!has_extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        logf("direct: the device lacks %s", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        return false;
    }
    want.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (has_extension(exts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && has_extension(exts, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
        want.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME), want.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        dev.dma_buf_import = true;
    }
    if (has_extension(exts, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        want.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        dev.host_import = true;
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &hp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.host_align = hp.minImportedHostPointerAlignment;
    }

    // the queues: one family for graphics and compute; layer 0's binds on a queue of their own that has sparse binding (a
    // sparse-binding-only family, else another family, else a second queue of the graphics family), else the graphics queue
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, families.data());
    dev.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < n && dev.queue_family == UINT32_MAX; i++)
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            dev.queue_family = i;
    if (dev.queue_family == UINT32_MAX) {
        logf("direct: the device has no queue family with both graphics and compute");
        return false;
    }
    const auto sparse = [&](uint32_t i) { return (families[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0; };
    const auto plain = [&](uint32_t i) { return !(families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)); };
    uint32_t sparse_family = UINT32_MAX, sparse_index = 0;
    const char* kind = "";
    for (int pass = 0; pass < 4 && sparse_family == UINT32_MAX; pass++) {
        static const char* const kinds[4] = {"sparse binding only", "another family", "a second graphics queue", "the graphics queue"};
        kind = kinds[pass];
        for (uint32_t i = 0; i < n && sparse_family == UINT32_MAX; i++) {
            if (!sparse(i)) continue;
            if (pass == 0 && plain(i)) sparse_family = i;
            if (pass == 1 && i != dev.queue_family) sparse_family = i;
            if (pass == 2 && i == dev.queue_family && families[i].queueCount > 1) sparse_family = i, sparse_index = 1;
            if (pass == 3 && i == dev.queue_family) sparse_family = i;
        }
    }
    dev.sparse_shared = sparse_family == dev.queue_family && sparse_index == 0;
    if (sparse_family == UINT32_MAX) logf("direct: no queue for sparse binding: layer 0 is off");
    else logf("direct: layer 0's binds on queue family %u index %u (%s)", sparse_family, sparse_index, kind);
    const float priority[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qs[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    uint32_t nq = 1;
    qs[0].queueFamilyIndex = dev.queue_family, qs[0].queueCount = 1, qs[0].pQueuePriorities = priority;
    if (sparse_family != UINT32_MAX && sparse_family != dev.queue_family)
        qs[nq].queueFamilyIndex = sparse_family, qs[nq].queueCount = 1, qs[nq].pQueuePriorities = priority, nq++;
    else if (sparse_index == 1)
        qs[0].queueCount = 2;

    // features: every core one the device has (the later milestones use them), but robust buffer access (it costs)
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13;
    if (dev.props.apiVersion >= VK_API_VERSION_1_4) f13.pNext = &f14;
    void** tail = dev.props.apiVersion >= VK_API_VERSION_1_4 ? &f14.pNext : &f13.pNext;
    draw_device_extensions(exts, want, tail);   // (M2b: shader objects, null descriptors, dynamic state 3)
    query_device_extensions(exts, want, tail);   // (M5: conditional rendering, calibrated timestamps)
    marks_device_extensions(exts, want);   // (M6c: debug.marks, breadcrumbs for a lost device)
    // (M3a: a job is a rendering secondary executed among its caller's inline commands in one rendering)
    VkPhysicalDeviceMaintenance7FeaturesKHR m7{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR};
    const bool has_m7 = has_extension(exts, VK_KHR_MAINTENANCE_7_EXTENSION_NAME);
    if (has_m7) want.push_back(VK_KHR_MAINTENANCE_7_EXTENSION_NAME), *tail = &m7, tail = &m7.pNext;
    // (the driver's heap budgets and usage, for the free VRAM the host's eviction patch reads, and for VMA)
    if ((dev.memory_budget = has_extension(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))) want.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    else logf("direct: no VK_EXT_memory_budget: free VRAM is estimated (80%% of the heap less the back end's own bytes)");
    vkGetPhysicalDeviceFeatures2(dev.physical, &f);
    draw_features_resolved(f.features, f12, dev.props.apiVersion >= VK_API_VERSION_1_4 ? &f14 : nullptr);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) {
        logf("direct: the GPU lacks dynamic rendering, synchronization2 or timeline semaphores");
        return false;
    }
    if (!caps.ok) {   // (the draws' needs: shader objects, the pools' set, device addresses, dynamic state 3)
        logf("direct: the GPU lacks what the draws need: %s", caps.why);
        return false;
    }
    dev.nested_rendering = has_m7 && m7.maintenance7;
    if (!dev.nested_rendering) logf("direct: no VK_KHR_maintenance7: jobs are not executed");
    f.features.robustBufferAccess = VK_FALSE;
    f14.pipelineRobustness = VK_FALSE;
    dev.features = f.features;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f;
    ci.queueCreateInfoCount = nq;
    ci.pQueueCreateInfos = qs;
    ci.enabledExtensionCount = (uint32_t)want.size();
    ci.ppEnabledExtensionNames = want.data();
    if (vkCreateDevice(dev.physical, &ci, nullptr, &dev.device) != VK_SUCCESS) {
        logf("direct: vkCreateDevice failed");
        return false;
    }
#define GPU_VK_LOAD(f) f = (PFN_##f)vkGetDeviceProcAddr(dev.device, #f);
    GPU_VK_DEVICE(GPU_VK_LOAD)
#undef GPU_VK_LOAD
    vkGetDeviceQueue(dev.device, dev.queue_family, 0, &dev.queue);
    if (sparse_family != UINT32_MAX) vkGetDeviceQueue(dev.device, sparse_family, sparse_index, &dev.sparse_queue);
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
    shader_warm();
    stats_after_start();
    return present_start(hwnd);
}
const char* describe() {
    static std::string d;
    d = std::string(dev.props.deviceName) + " (Vulkan, direct)";
    return d.c_str();
}
// (the host's bench: draws recorded since the start, draws skipped since the start for a program not ready, shader jobs
// queued or running now)
void stats(int64_t* draws, int64_t* skipped, int64_t* pipelines) {
    draw_totals(draws, skipped);
    *pipelines = (int64_t)work::pending();
}
void register_texture(int id, const nvn_tex* t, const nvn_view* v) {
    count(R_TEX);
    image_texture_registered(t);
    pool_texture_registered(id, t, v);
}
void register_sampler(int id, const nvn_samp_par* p) {
    count(R_SAMP);
    pool_sampler_registered(id, p);
}
void set_texture_pool_size(int) { count(R_TEX_POOL); }
void texture_released(const nvn_tex* t) {
    count(R_TEX_GONE);
    image_texture_released(t);
}
void texture_created(const nvn_tex* t) {
    count(R_TEX_MADE);
    image_texture_created(t);
}
}  // namespace
}  // namespace hsd
extern "C" MKWVK_EXPORT int64_t gpu_vram_free(void) {   // (M6f: -1 not known yet)
    return hsd::vram_free_now.load(std::memory_order_relaxed);
}
extern "C" MKWVK_EXPORT void gpu_texture_cached(const void* t, int cached) {   // 
    hsd::image_texture_cached((const nvn_tex*)t, cached != 0);
}
// (the transient pool, docs/direct/transient-pool.md: games/oot/patches/rt_slots.c's calls)
extern "C" MKWVK_EXPORT int gpu_transient_pool(uint64_t lo, uint64_t hi) {
    return hsd::image_pool_range(lo, hi) ? 1 : 0;
}
extern "C" MKWVK_EXPORT void gpu_transient_slot(uint64_t gpu, uint64_t cap) { hsd::image_pool_slot(gpu, cap); }
extern "C" MKWVK_EXPORT uint64_t gpu_image_need(const void* desc, uint64_t* align) {
    return hsd::image_need((const nvn_texdesc*)desc, align);
}
extern "C" MKWVK_EXPORT void gpu_target_given(const void* t) { hsd::image_target_given((const nvn_tex*)t); }
extern "C" MKWVK_EXPORT void gpu_host_event(const char* reason, const char* what) {
    hsd::strict_event(reason, "%s", what ? what : "");
}
namespace hsd {
namespace {
void program_set(const nvn_prog* p) {
    count(R_PROG_SET);
    shader_program_set(p);
    compute_program_set(p);   // 
}
void program_released(const nvn_prog*) { count(R_PROG_GONE); }
void read_texels(const nvn_tex*, unsigned, unsigned, const NVNcopyRegion*, uint8_t*, uint64_t, uint64_t) { count(R_READ_TEXELS); }
void write_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src, uint64_t row,
                  uint64_t img) {
    count(R_WRITE_TEXELS);
    image_write_texels(t, level, layer0, r, src, row, img);
}
void invalidate(uint64_t, uint64_t) {}
void wait_writebacks_reading(const void*, uint64_t) {}
void pool_created(uint64_t gpu, uint64_t size) {
    count(R_POOL);
    layer0_pool_created(gpu, size);
}
void pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint32_t claim, uint64_t key, int on) {
    count(R_POOL_NATIVE);
    layer0_pool_native(gpu, size, flags, claim, key, on);
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

/* the settings' accessors (renderer/api/settings.h), the host's */
extern "C" {
int setting_bool(setting_id id) { return hsd::host->setting_int(id) != 0; }
int64_t setting_int(setting_id id) { return hsd::host->setting_int(id); }
double setting_float(setting_id id) { return hsd::host->setting_float(id); }
const char* setting_str(setting_id id) { return hsd::host->setting_str(id); }
int setting_given(setting_id id) { return hsd::host->setting_given(id); }
}

extern "C" MKWVK_EXPORT const MkwVkRenderer* mkwvk_init(const MkwVkHost* h) {
    using namespace hsd;
    if (!h || h->version != MKWVK_API_VERSION) return nullptr;
    host = h;
    {   // (the settings read once: the host's accessors are there from now on, not at the library's load)
        strict_enabled = dbg(SET_DBG_STRICT);
        deferred_check = dbg(SET_DBG_DEFERREDCHECK);
    }
    if (!load_global()) {
        logf("direct: %s not found", OS_LIB("libvulkan.so.1", "vulkan-1.dll"));
        return nullptr;
    }
    if (!create_instance() || !pick_device() || !create_device()) return nullptr;
    if (!vma_init()) {
        logf("direct: no memory allocator");
        return nullptr;
    }
    draw_device_created();   // (M2b: the pools' set, the draws' functions)
    query_device_created();  // (M5: queries, conditions, the resolve step)
    upscale_device_created();
    marks_init();            // (M6c: debug.marks; after the draws' functions are loaded)
    pstats_init(caps.mesh_queries);   // (M6h: debug.pstats=<present>)
    passtime_init();
    logf("direct: %s, Vulkan %u.%u, driver %#x: the direct back end (M2b: targets, passes, present, vertex and fragment draws)", dev.props.deviceName,
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
    table.nvn_texture_created = texture_created;
    table.nvn_program_set = program_set;
    table.nvn_pool_native = pool_native;
    sync_fill_table(table);   // (nvn_submit, _q, fence, fence_q, pushed, replayed, wait, finish, event_signal)
    return &table;
}
