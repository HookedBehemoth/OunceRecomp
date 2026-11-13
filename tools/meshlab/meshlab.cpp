// meshlab: the game's mesh draws outside the game (docs/direct/m6e-report.md).
//
// Reads one frame captured by HOS_DBG=meshcap (gpu/direct/direct_meshcap.h) and draws its mesh draws on their own, with the
// stages the back end made (the translator's SPIR-V and heap mappings) or with replacement mesh stages:
//   meshlab <frame.bin> list                              the frame's commands, its mesh draws in detail
//   meshlab <frame.bin> compare <seq...|all|task> --repl <dir> [--quiet]
//                                                         each draw's emitted geometry from the translator's mesh stage and
//                                                         from the replacement (<dir>/<mesh code key>.spv), compared
//   meshlab <frame.bin> dump <seq> [--repl <dir>] [--prims N]   one draw's emitted geometry, printed
//   meshlab <frame.bin> run <seq...> | --job <seq> [--task-only] [--seconds T] [--reps R] [--barrier none|local|full|split]
//                          [--repl <dir>] [--mesh <spv>] [--fs simple|real] [--secondary] [--watchdog S] [--empty]
//                                                         the draws in one rendering, again and again for T seconds, with
//                                                         top- and bottom-of-pipe markers in host memory; a submission that
//                                                         has not finished after S seconds stops the run (a hang) and the
//                                                         markers say which draws had started and not finished
// Built by tools/meshlab/build.sh; links nothing of the game. The capture file holds game data and stays under work/.
#include <vulkan/vulkan.h>

#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "spv_dump.h"

using namespace meshlab;
using clk = std::chrono::steady_clock;

namespace {

// (direct_draw.cpp Dyn, field for field: the capture holds its bytes)
struct Dyn {
    VkViewport vp;
    VkRect2D sc;
    float blend[4];
    uint32_t sref[2], scmp[2], swm[2];
    float bias[3];
    VkBool32 bias_en;
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topo;
    VkBool32 restart, dt, dw;
    VkCompareOp dop;
    VkBool32 st;
    VkStencilOp sops[2][3];
    VkCompareOp scmpop[2];
    VkBool32 discard;
    VkPolygonMode poly;
    VkBool32 clamp, a2c;
    VkSampleCountFlagBits samples;
    uint32_t n;
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
};

[[noreturn]] void die(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
    std::fputc('\n', stderr);
    std::exit(2);
}
#define VK(x) do { const VkResult r_ = (x); if (r_ != VK_SUCCESS) die("%s: %d", #x, (int)r_); } while (0)

// ---- the device ----
struct Gpu {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE, sparse_queue = VK_NULL_HANDLE;
    uint32_t family = 0, sparse_family = UINT32_MAX;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem{};
    VkPhysicalDeviceDescriptorHeapPropertiesEXT hprops{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkShaderStageFlags sg32 = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    bool has_local_read = false;
#define FN(f) PFN_##f f = nullptr;
    FN(vkCreateShadersEXT) FN(vkCmdBindShadersEXT) FN(vkCmdBindResourceHeapEXT) FN(vkCmdBindSamplerHeapEXT) FN(vkCmdPushDataEXT)
    FN(vkCmdDrawMeshTasksEXT) FN(vkCmdDrawMeshTasksIndirectEXT) FN(vkCmdWriteBufferMarker2AMD) FN(vkCmdSetPolygonModeEXT)
    FN(vkCmdSetDepthClampEnableEXT) FN(vkCmdSetAlphaToCoverageEnableEXT) FN(vkCmdSetColorBlendEnableEXT) FN(vkCmdSetColorBlendEquationEXT)
    FN(vkCmdSetColorWriteMaskEXT) FN(vkCmdSetRasterizationSamplesEXT) FN(vkCmdSetSampleMaskEXT) FN(vkCmdSetAlphaToOneEnableEXT)
    FN(vkCmdSetLogicOpEnableEXT) FN(vkCmdSetProvokingVertexModeEXT) FN(vkCmdBeginConditionalRenderingEXT) FN(vkCmdEndConditionalRenderingEXT)
#undef FN
};
Gpu g;

bool has_ext(const std::vector<VkExtensionProperties>& e, const char* n) {
    for (const auto& x : e)
        if (!std::strcmp(x.extensionName, n)) return true;
    return false;
}

void gpu_init() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "meshlab";
    ai.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &ai;
    VK(vkCreateInstance(&ii, nullptr, &g.inst));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(g.inst, &n, pds.data());
    for (VkPhysicalDevice p : pds) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || !g.phys) g.phys = p, g.props = pr;
    }
    if (!g.phys) die("no GPU");
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
    vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, exts.data());
    // (as the game's device: every core feature but robust buffer access; the back end's extensions and their features as the
    // device has them, the task stage off)
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceShaderObjectFeaturesEXT so{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT hf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    VkPhysicalDeviceMeshShaderFeaturesEXT ms{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceMaintenance7FeaturesKHR m7{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13, f13.pNext = &f14;
    void** tail = &f14.pNext;
    std::vector<const char*> want;
    const struct { const char* name; void* feat; } list[] = {
        {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, &so}, {VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, &hf},
        {VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, &e3}, {VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, &pv},
        {VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME, nullptr}, {VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, &af},
        {VK_KHR_MAINTENANCE_5_EXTENSION_NAME, nullptr}, {VK_EXT_MESH_SHADER_EXTENSION_NAME, &ms},
        {VK_KHR_MAINTENANCE_7_EXTENSION_NAME, &m7}, {VK_AMD_BUFFER_MARKER_EXTENSION_NAME, nullptr},
        {VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, &cr}};
    for (const auto& e : list) {
        if (!has_ext(exts, e.name)) {
            if (e.feat == &so || e.feat == &hf || e.feat == &ms) die("the device lacks %s", e.name);
            continue;
        }
        want.push_back(e.name);
        if (e.feat) *tail = e.feat, tail = (void**)&((VkBaseOutStructure*)e.feat)->pNext;
    }
    vkGetPhysicalDeviceFeatures2(g.phys, &f);
    f.features.robustBufferAccess = VK_FALSE;
    f14.pipelineRobustness = VK_FALSE;
    hf.descriptorHeapCaptureReplay = VK_FALSE;
    ms.taskShader = VK_FALSE;
    ms.multiviewMeshShader = ms.primitiveFragmentShadingRateMeshShader = ms.meshShaderQueries = VK_FALSE;
    pv.transformFeedbackPreservesProvokingVertex = VK_FALSE;
    g.has_local_read = f14.dynamicRenderingLocalRead;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &n, fams.data());
    for (uint32_t i = 0; i < n; i++)
        if ((fams[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            g.family = i;
            break;
        }
    for (uint32_t i = 0; i < n; i++)   // (the sparse binds: a family of their own, as the game's layer 0)
        if ((fams[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && !(fams[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            g.sparse_family = i;
            break;
        }
    if (g.sparse_family == UINT32_MAX && (fams[g.family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) g.sparse_family = g.family;
    if (g.sparse_family == UINT32_MAX) die("no sparse binding queue");
    f.features.sparseBinding = VK_TRUE;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo q[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    q[0].queueFamilyIndex = g.family, q[0].queueCount = 1, q[0].pQueuePriorities = &prio;
    q[1].queueFamilyIndex = g.sparse_family, q[1].queueCount = 1, q[1].pQueuePriorities = &prio;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f;
    ci.queueCreateInfoCount = g.sparse_family == g.family ? 1 : 2, ci.pQueueCreateInfos = q;
    ci.enabledExtensionCount = (uint32_t)want.size(), ci.ppEnabledExtensionNames = want.data();
    VK(vkCreateDevice(g.phys, &ci, nullptr, &g.dev));
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
    vkGetDeviceQueue(g.dev, g.sparse_family, 0, &g.sparse_queue);
#define LOAD(f) g.f = (PFN_##f)vkGetDeviceProcAddr(g.dev, #f);
    LOAD(vkCreateShadersEXT) LOAD(vkCmdBindShadersEXT) LOAD(vkCmdBindResourceHeapEXT) LOAD(vkCmdBindSamplerHeapEXT) LOAD(vkCmdPushDataEXT)
    LOAD(vkCmdDrawMeshTasksEXT) LOAD(vkCmdDrawMeshTasksIndirectEXT) LOAD(vkCmdWriteBufferMarker2AMD) LOAD(vkCmdSetPolygonModeEXT)
    LOAD(vkCmdSetDepthClampEnableEXT) LOAD(vkCmdSetAlphaToCoverageEnableEXT) LOAD(vkCmdSetColorBlendEnableEXT) LOAD(vkCmdSetColorBlendEquationEXT)
    LOAD(vkCmdSetColorWriteMaskEXT) LOAD(vkCmdSetRasterizationSamplesEXT) LOAD(vkCmdSetSampleMaskEXT) LOAD(vkCmdSetAlphaToOneEnableEXT)
    LOAD(vkCmdSetLogicOpEnableEXT) LOAD(vkCmdSetProvokingVertexModeEXT) LOAD(vkCmdBeginConditionalRenderingEXT) LOAD(vkCmdEndConditionalRenderingEXT)
#undef LOAD
    {
        VkPhysicalDeviceSubgroupSizeControlProperties sc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        g.hprops.pNext = &sc;
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &g.hprops;
        vkGetPhysicalDeviceProperties2(g.phys, &p2);
        g.hprops.pNext = nullptr;
        g.sg32 = sc.minSubgroupSize <= 32 && sc.maxSubgroupSize >= 32 ? sc.requiredSubgroupSizeStages : 0;
    }
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = g.family;
    VK(vkCreateCommandPool(g.dev, &pc, nullptr, &g.pool));
}

// ---- memory ----
struct Buf {
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceAddress addr = 0;
    uint64_t size = 0;
};
uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) {
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (g.mem.memoryTypes[i].propertyFlags & want) == want && !(g.mem.memoryTypes[i].propertyFlags & avoid)) return i;
    return UINT32_MAX;
}
bool host_only = false;   // (--host: every buffer in host memory, as layer 0's guest memory is)
Buf make_buf(uint64_t size, VkBufferUsageFlags usage, bool host_mem = false) {
    Buf r;
    r.size = size = std::max<uint64_t>(size, 256);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK(vkCreateBuffer(g.dev, &bi, nullptr, &r.b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, r.b, &mr);
    uint32_t t = UINT32_MAX;
    if (!host_mem && !host_only) t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t == UINT32_MAX) t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fi, ai.allocationSize = mr.size, ai.memoryTypeIndex = t;
    VK(vkAllocateMemory(g.dev, &ai, nullptr, &r.m));
    VK(vkBindBufferMemory(g.dev, r.b, r.m, 0));
    void* p;
    VK(vkMapMemory(g.dev, r.m, 0, VK_WHOLE_SIZE, 0, &p));
    r.map = (uint8_t*)p;
    std::memset(r.map, 0, size);
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = r.b;
    r.addr = vkGetBufferDeviceAddress(g.dev, &da);
    return r;
}

struct Img {
    VkImage im = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
};
Img make_img(VkFormat f, uint32_t w, uint32_t h, VkSampleCountFlagBits s, bool depth) {
    Img r;
    r.format = f;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = f, ci.extent = {w, h, 1}, ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = s;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VK(vkCreateImage(g.dev, &ci, nullptr, &r.im));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.dev, r.im, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size, ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory m;
    VK(vkAllocateMemory(g.dev, &ai, nullptr, &m));
    VK(vkBindImageMemory(g.dev, r.im, m, 0));
    r.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT) r.aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == VK_FORMAT_S8_UINT) r.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r.im, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = f, vi.subresourceRange = {r.aspect, 0, 1, 0, 1};
    VK(vkCreateImageView(g.dev, &vi, nullptr, &r.view));
    return r;
}

// ---- the heaps (direct_heap.cpp's layout, every descriptor zero: RADV reads zeros as none) ----
struct Heaps {
    Buf res, smp;
    uint64_t res_reserved_at = 0, res_reserved = 0, smp_reserved_at = 0, smp_reserved = 0;
};
Heaps heaps;
void heaps_init() {
    constexpr uint64_t kSlots = 1u << 16, kRegions = 7, kImageDesc = 64, kSamplers = 4096, kSamplerDesc = 16;
    const uint64_t res_used = ((2 * kRegions - 1) * kSlots + (1u << 20)) * kImageDesc;
    auto mk = [](Buf& b, uint64_t used, uint64_t reserved, uint64_t& at, uint64_t& rs) {
        at = (used + 255) & ~255ull, rs = (reserved + 255) & ~255ull;
        b = make_buf(at + rs, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT);
    };
    mk(heaps.res, res_used, g.hprops.minResourceHeapReservedRange, heaps.res_reserved_at, heaps.res_reserved);
    mk(heaps.smp, 2 * kSamplers * kSamplerDesc, g.hprops.minSamplerHeapReservedRange, heaps.smp_reserved_at, heaps.smp_reserved);
}
void heaps_bind(VkCommandBuffer cb) {
    VkBindHeapInfoEXT rb{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    rb.heapRange = {heaps.res.addr, heaps.res.size}, rb.reservedRangeOffset = heaps.res_reserved_at, rb.reservedRangeSize = heaps.res_reserved;
    VkBindHeapInfoEXT sb{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    sb.heapRange = {heaps.smp.addr, heaps.smp.size}, sb.reservedRangeOffset = heaps.smp_reserved_at, sb.reservedRangeSize = heaps.smp_reserved;
    g.vkCmdBindResourceHeapEXT(cb, &rb);
    g.vkCmdBindSamplerHeapEXT(cb, &sb);
}

// ---- shader objects ----
VkShaderEXT make_shader(VkShaderStageFlagBits stage, const std::vector<uint32_t>& spirv, const void* maps, uint32_t nmaps, std::string* err = nullptr) {
    VkShaderDescriptorSetAndBindingMappingInfoEXT mi{VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
    mi.mappingCount = nmaps, mi.pMappings = (const VkDescriptorSetAndBindingMappingEXT*)maps;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sz{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    sz.requiredSubgroupSize = 32;
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    ci.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
    if (stage == VK_SHADER_STAGE_MESH_BIT_EXT) ci.flags |= VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
    ci.stage = stage;
    ci.nextStage = stage == VK_SHADER_STAGE_MESH_BIT_EXT ? VK_SHADER_STAGE_FRAGMENT_BIT : 0;
    ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    ci.codeSize = spirv.size() * 4, ci.pCode = spirv.data(), ci.pName = "main";
    if (g.sg32 & stage) sz.pNext = &mi, ci.pNext = &sz;
    else ci.pNext = &mi;
    VkShaderEXT o = VK_NULL_HANDLE;
    const VkResult r = g.vkCreateShadersEXT(g.dev, 1, &ci, nullptr, &o);
    if (r != VK_SUCCESS) {
        if (err) *err = "vkCreateShadersEXT " + std::to_string((int)r);
        return VK_NULL_HANDLE;
    }
    return o;
}
std::vector<uint32_t> read_spv(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> v((size_t)n / 4);
    if (std::fread(v.data(), 4, v.size(), f) != v.size()) v.clear();
    std::fclose(f);
    return v;
}
bool is_int_format(VkFormat f, bool& sint) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        return sint = false, true;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        return sint = true, true;
    default: return false;
    }
}
std::string self_dir;
/// A fragment stage of our own that writes a constant to each target (of its type): compiled once a signature by glslang
VkShaderEXT simple_fs(const mcap::McDraw& d) {
    static std::map<std::string, VkShaderEXT> made;
    std::string sig, src = "#version 460\n";
    for (uint32_t i = 0; i < d.n && i < 8; i++) {
        if (!d.color[i]) continue;
        bool sint = false;
        const bool integer = is_int_format((VkFormat)d.color[i], sint);
        const char* t = integer ? (sint ? "ivec4" : "uvec4") : "vec4";
        sig += std::to_string(i) + t;
        src += "layout(location = " + std::to_string(i) + ") out " + t + " o" + std::to_string(i) + ";\n";
    }
    src += "void main() {\n";
    for (uint32_t i = 0; i < d.n && i < 8; i++) {
        if (!d.color[i]) continue;
        bool sint = false;
        const bool integer = is_int_format((VkFormat)d.color[i], sint);
        src += "    o" + std::to_string(i) + " = " + (integer ? (sint ? "ivec4(1, 2, 3, 4)" : "uvec4(1, 2, 3, 4)") : "vec4(0.25, 0.5, 0.75, 1.0)") + ";\n";
    }
    src += "}\n";
    if (const auto it = made.find(sig); it != made.end()) return it->second;
    const std::string dir = "/tmp/meshlab_fs_" + std::to_string(getpid());
    std::string cmd = "mkdir -p " + dir;
    if (std::system(cmd.c_str())) die("mkdir");
    const std::string fs = dir + "/s.frag", spv = dir + "/s.spv";
    FILE* f = std::fopen(fs.c_str(), "w");
    std::fputs(src.c_str(), f);
    std::fclose(f);
    cmd = "glslangValidator -V --target-env vulkan1.3 -o " + spv + " " + fs + " > /dev/null";
    if (std::system(cmd.c_str())) die("glslang failed on the simple fragment stage:\n%s", src.c_str());
    const auto code = read_spv(spv);
    VkShaderEXT o = make_shader(VK_SHADER_STAGE_FRAGMENT_BIT, code, nullptr, 0);
    if (!o) die("the simple fragment stage was not made");
    return made[sig] = o;
}

// ---- a draw made ready: its memory placed, its push data written ----
// The guest's memory as the game's layer 0 shows it (gpu/direct/layer0_windows.h): window k a sparse buffer over guest
// [k * 2 GB, k * 2 GB + 4 GB - 64 KB), every captured range bound at its own guest address in both windows that hold it, nothing
// else bound (reads of it are zeros: residencyNonResidentStrict). The capture's addresses are used as they are: no relocation.
struct Guest {
    Buf windows;   // (the window table: 513 entries)
    uint64_t bytes = 0;
};
Guest guest;
constexpr uint64_t kStride = 2ull << 30, kWindow = (4ull << 30) - (64ull << 10);
void guest_init(const Frame& f, const std::vector<const Draw*>&) {
    // (the pages to bind, a 64 KB page each, with their bytes)
    std::map<uint64_t, std::vector<uint8_t>> pages;
    for (const Blob& b : f.blobs) {
        if (!b.guest) continue;
        for (uint64_t o = 0; o < b.bytes.size();) {
            const uint64_t ga = b.guest + o, page = ga & ~0xFFFFull, in = ga - page;
            const uint64_t n = std::min<uint64_t>(0x10000 - in, b.bytes.size() - o);
            auto& pg = pages[page];
            if (pg.empty()) pg.resize(0x10000);
            std::memcpy(pg.data() + in, b.bytes.data() + o, n);
            o += n;
        }
    }
    std::map<uint32_t, std::vector<uint64_t>> by_window;   // (window -> pages, each bound at its offset there)
    for (const auto& [page, bytes] : pages) {
        const uint32_t k = (uint32_t)(page >> 31);
        if (k < 512) by_window[k].push_back(page);
        if (k > 0 && k - 1 < 512 && (page & (kStride - 1)) + kStride + 0x10000 <= kWindow) by_window[k - 1].push_back(page);
    }
    VkQueue sq = g.sparse_queue;
    guest.windows = make_buf(8 * 520 + 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    uint64_t* table = (uint64_t*)guest.windows.map;
    auto make_window = [&](const std::vector<uint64_t>& list, uint32_t k) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
        bi.size = kWindow;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VkBuffer buf;
        VK(vkCreateBuffer(g.dev, &bi, nullptr, &buf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g.dev, buf, &mr);
        if (mr.alignment > 0x10000) die("sparse pages of %llu bytes", (unsigned long long)mr.alignment);
        if (!list.empty()) {
            const uint32_t t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (t == UINT32_MAX) die("no host-visible memory for the sparse windows");
            VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
            fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.pNext = &fi, ai.allocationSize = 0x10000ull * list.size(), ai.memoryTypeIndex = t;
            VkDeviceMemory m;
            VK(vkAllocateMemory(g.dev, &ai, nullptr, &m));
            void* map;
            VK(vkMapMemory(g.dev, m, 0, VK_WHOLE_SIZE, 0, &map));
            std::vector<VkSparseMemoryBind> binds;
            for (size_t i = 0; i < list.size(); i++) {
                std::memcpy((uint8_t*)map + 0x10000 * i, pages[list[i]].data(), 0x10000);
                const uint64_t off = list[i] - (uint64_t)k * kStride;
                binds.push_back({off, 0x10000, m, 0x10000 * i, 0});
            }
            guest.bytes += ai.allocationSize;
            VkSparseBufferMemoryBindInfo sb{buf, (uint32_t)binds.size(), binds.data()};
            VkBindSparseInfo si{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
            si.bufferBindCount = 1, si.pBufferBinds = &sb;
            VK(vkQueueBindSparse(sq, 1, &si, VK_NULL_HANDLE));
        }
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = buf;
        return vkGetBufferDeviceAddress(g.dev, &da);
    };
    const VkDeviceAddress null_window = make_window({}, 0);
    for (uint32_t k = 0; k <= 512; k++) table[k] = null_window;
    for (const auto& [k, list] : by_window) table[k] = make_window(list, k);
    VK(vkQueueWaitIdle(sq));
}

struct Ready {
    const Draw* d = nullptr;
    const Program* p = nullptr;
    std::vector<Buf> ubo;   // (in the program's table order)
    Buf table, header, record;
    uint8_t push[256]{};
    uint32_t push_bytes = 0;
    uint32_t groups = 0;    // (the mesh workgroups the draw starts)
    VkShaderEXT ms = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    OutLayout layout;       // (compare: the dump's layout)
};
std::string repl_dir, mesh_override, fs_mode = "simple";
std::map<std::string, VkShaderEXT> shader_cache;

VkShaderEXT mesh_shader_of(const Program& p, const std::vector<uint32_t>& spirv, const std::string& tag) {
    const std::string k = std::to_string(p.h.key) + "/" + tag;
    if (const auto it = shader_cache.find(k); it != shader_cache.end()) return it->second;
    const Stage* s = p.stage(VK_SHADER_STAGE_MESH_BIT_EXT);
    std::fprintf(stderr, "meshlab: making the mesh stage of %016llx (%s; mesh code %016llx, task code %016llx)\n", (unsigned long long)p.h.key, tag.c_str(),
                 (unsigned long long)p.h.codes[0], (unsigned long long)p.h.task_code);
    std::fflush(stderr);
    std::string err;
    VkShaderEXT o = make_shader(VK_SHADER_STAGE_MESH_BIT_EXT, spirv, s->maps.data(), s->h.maps, &err);
    if (!o) die("program %016llx: mesh stage (%s) not made: %s", (unsigned long long)p.h.key, tag.c_str(), err.c_str());
    return shader_cache[k] = o;
}
std::string repl_path(const Program& p) {
    char b[64];
    std::snprintf(b, sizeof b, "/%016llx.spv", (unsigned long long)p.h.codes[0]);
    return repl_dir + b;
}

Ready prepare(const Frame& f, const Draw& d) {
    Ready r;
    r.d = &d;
    const auto pit = f.programs.find(d.d.key);
    if (pit == f.programs.end()) die("draw %u: program %016llx not in the capture", d.d.seq, (unsigned long long)d.d.key);
    r.p = &pit->second;
    const Program& p = *r.p;
    if (p.ubos.size() != d.ubos.size()) die("draw %u: %zu uniform buffers, its program %zu", d.d.seq, d.ubos.size(), p.ubos.size());
    // (uniform buffers: each its own buffer, 64 KB at least and zero past the bytes)
    for (size_t i = 0; i < d.ubos.size(); i++) {
        const mcap::McDrawUbo& u = d.ubos[i];
        std::vector<uint8_t> bytes;
        if (u.blob != ~0u) bytes = f.blobs[u.blob].bytes;
        else if (u.index == 1)
            for (const Stage& s : p.stages)
                if (s.h.nvn_stage == u.stage) bytes = s.c1;
        Buf b = make_buf(std::max<uint64_t>(bytes.size(), 64 << 10) + 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(b.map, bytes.data(), bytes.size());
        r.ubo.push_back(b);
    }
    r.table = make_buf(8 * std::max<size_t>(1, r.ubo.size()), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for (size_t i = 0; i < r.ubo.size(); i++) std::memcpy(r.table.map + 8 * i, &r.ubo[i].addr, 8);
    // (push data: the game's, with our addresses)
    r.push_bytes = d.d.push_bytes;
    std::memcpy(r.push, d.d.push, sizeof r.push);
    std::memset(r.push + 64, 0, 24);
    std::memset(r.push + 96, 0, 24);
    std::memcpy(r.push + 88, &guest.windows.addr, 8);
    std::memcpy(r.push + 120, &r.table.addr, 8);
    for (const auto& q : p.pushed)
        for (size_t i = 0; i < p.ubos.size(); i++)
            if (p.ubos[i].stage == q.stage && p.ubos[i].index == q.index && q.push_off + 8 <= sizeof r.push) std::memcpy(r.push + q.push_off, &r.ubo[i].addr, 8);
    // (a task+mesh draw: its header, map and payload as the GPU left them, at our addresses)
    if (d.d.task) {
        r.header = make_buf(64 + 4 * (d.map.size() + d.payload.size()) + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        uint32_t h[16];
        std::memcpy(h, d.d.header, 64);
        const uint64_t ma = r.header.addr + 64, pa = ma + 4 * d.map.size();
        h[4] = (uint32_t)ma, h[5] = (uint32_t)(ma >> 32), h[6] = (uint32_t)pa, h[7] = (uint32_t)(pa >> 32);
        h[0] = std::min<uint32_t>(h[0], (uint32_t)d.map.size());
        std::memcpy(r.header.map, h, 64);
        std::memcpy(r.header.map + 64, d.map.data(), 4 * d.map.size());
        std::memcpy(r.header.map + 64 + 4 * d.map.size(), d.payload.data(), 4 * d.payload.size());
        std::memcpy(r.push + 64, &r.header.addr, 8), std::memcpy(r.push + 72, &r.header.addr, 8);
        r.groups = h[0];
    } else if (d.d.indirect) {
        r.record = make_buf(64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        const uint32_t w[4] = {d.d.record_words[0], 1, 1, 0};
        std::memcpy(r.record.map, w, 16);
        r.groups = w[0];
    } else {
        r.groups = (uint32_t)std::max(d.d.y, 0);
    }
    // (the stages: the translator's mesh stage, a replacement, or one given for every draw)
    const Stage* ms = p.stage(VK_SHADER_STAGE_MESH_BIT_EXT);
    if (!ms) die("program %016llx has no mesh stage", (unsigned long long)p.h.key);
    if (!mesh_override.empty()) {
        r.ms = mesh_shader_of(p, read_spv(mesh_override), "override");
    } else if (!repl_dir.empty()) {
        const auto code = read_spv(repl_path(p));
        if (code.empty()) die("no replacement %s", repl_path(p).c_str());
        r.ms = mesh_shader_of(p, code, "repl");
    } else {
        r.ms = mesh_shader_of(p, ms->spirv, "translator");
    }
    const Stage* fs = p.stage(VK_SHADER_STAGE_FRAGMENT_BIT);
    if (fs_mode == "real" && fs) {
        const std::string k = std::to_string(p.h.key) + "/fs";
        auto it = shader_cache.find(k);
        if (it == shader_cache.end()) {
            std::string err;
            VkShaderEXT o = make_shader(VK_SHADER_STAGE_FRAGMENT_BIT, fs->spirv, fs->maps.data(), fs->h.maps, &err);
            if (!o) die("program %016llx: fragment stage not made: %s", (unsigned long long)p.h.key, err.c_str());
            it = shader_cache.emplace(k, o).first;
        }
        r.fs = it->second;
    } else {
        r.fs = simple_fs(d.d);
    }
    return r;
}

// ---- recording ----
struct Target {
    std::vector<Img> color;
    Img depth;
    uint32_t w = 0, h = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkFormat> formats;
    VkFormat depth_format = VK_FORMAT_UNDEFINED, stencil_format = VK_FORMAT_UNDEFINED;
};
Target make_target(const mcap::McDraw& d) {
    Target t;
    t.w = std::max<uint32_t>(d.w, 1), t.h = std::max<uint32_t>(d.h, 1), t.samples = (VkSampleCountFlagBits)std::max<uint32_t>(d.samples, 1);
    for (uint32_t i = 0; i < d.n && i < 8; i++) {
        t.formats.push_back((VkFormat)d.color[i]);
        t.color.push_back(d.color[i] ? make_img((VkFormat)d.color[i], t.w, t.h, t.samples, false) : Img{});
    }
    if (d.depth) {
        t.depth = make_img((VkFormat)d.depth, t.w, t.h, t.samples, true);
        if (t.depth.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) t.depth_format = (VkFormat)d.depth;
        if (t.depth.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) t.stencil_format = (VkFormat)d.depth;
    }
    return t;
}
void to_attachment(VkCommandBuffer cb, const Target& t) {
    std::vector<VkImageMemoryBarrier2> bs;
    for (const Img& i : t.color)
        if (i.im) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
            b.image = i.im, b.subresourceRange = {i.aspect, 0, 1, 0, 1};
            bs.push_back(b);
        }
    if (t.depth.im) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        b.image = t.depth.im, b.subresourceRange = {t.depth.aspect, 0, 1, 0, 1};
        bs.push_back(b);
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = (uint32_t)bs.size(), di.pImageMemoryBarriers = bs.data();
    vkCmdPipelineBarrier2(cb, &di);
}
void begin_rendering(VkCommandBuffer cb, const Target& t, bool secondary, bool clear) {
    std::vector<VkRenderingAttachmentInfo> ca;
    for (const Img& i : t.color) {
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = i.view, a.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.push_back(a);
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = t.depth.view, da.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    da.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    if (secondary) ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    ri.renderArea = {{0, 0}, {t.w, t.h}}, ri.layerCount = 1;
    ri.colorAttachmentCount = (uint32_t)ca.size(), ri.pColorAttachments = ca.data();
    if (t.depth_format) ri.pDepthAttachment = &da;
    if (t.stencil_format) ri.pStencilAttachment = &da;
    vkCmdBeginRendering(cb, &ri);
}
/// The draw's state, all of it (direct_draw.cpp set_dynamic with nothing set before)
void set_state(VkCommandBuffer cb, const Dyn& d) {
    vkCmdSetLineWidth(cb, 1.0f);
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    g.vkCmdSetAlphaToOneEnableEXT(cb, VK_FALSE);
    g.vkCmdSetLogicOpEnableEXT(cb, VK_FALSE);
    if (g.vkCmdSetProvokingVertexModeEXT) g.vkCmdSetProvokingVertexModeEXT(cb, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
    vkCmdSetViewportWithCount(cb, 1, &d.vp);
    vkCmdSetScissorWithCount(cb, 1, &d.sc);
    vkCmdSetBlendConstants(cb, d.blend);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, d.sref[0]), vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, d.sref[1]);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, d.scmp[0]), vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, d.scmp[1]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, d.swm[0]), vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, d.swm[1]);
    vkCmdSetDepthBias(cb, d.bias[0], d.bias[1], d.bias[2]);
    vkCmdSetDepthBiasEnable(cb, d.bias_en);
    vkCmdSetCullMode(cb, d.cull);
    vkCmdSetFrontFace(cb, d.front);
    vkCmdSetPrimitiveTopology(cb, d.topo);
    vkCmdSetPrimitiveRestartEnable(cb, d.restart);
    vkCmdSetDepthTestEnable(cb, d.dt);
    vkCmdSetDepthWriteEnable(cb, d.dw);
    vkCmdSetDepthCompareOp(cb, d.dop);
    vkCmdSetStencilTestEnable(cb, d.st);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, d.sops[0][0], d.sops[0][1], d.sops[0][2], d.scmpop[0]);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, d.sops[1][0], d.sops[1][1], d.sops[1][2], d.scmpop[1]);
    vkCmdSetRasterizerDiscardEnable(cb, d.discard);
    g.vkCmdSetPolygonModeEXT(cb, d.poly);
    g.vkCmdSetDepthClampEnableEXT(cb, d.clamp);
    g.vkCmdSetAlphaToCoverageEnableEXT(cb, d.a2c);
    const VkSampleMask mask[2] = {~0u, ~0u};
    g.vkCmdSetRasterizationSamplesEXT(cb, d.samples ? d.samples : VK_SAMPLE_COUNT_1_BIT);
    g.vkCmdSetSampleMaskEXT(cb, d.samples ? d.samples : VK_SAMPLE_COUNT_1_BIT, mask);
    if (d.n) {
        g.vkCmdSetColorBlendEnableEXT(cb, 0, d.n, d.en);
        g.vkCmdSetColorBlendEquationEXT(cb, 0, d.n, d.eq);
        g.vkCmdSetColorWriteMaskEXT(cb, 0, d.n, d.wm);
    }
}
void bind_stages(VkCommandBuffer cb, VkShaderEXT ms, VkShaderEXT fs) {
    VkShaderStageFlagBits st[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_MESH_BIT_EXT};
    VkShaderEXT sh[6] = {VK_NULL_HANDLE, fs, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, ms};
    g.vkCmdBindShadersEXT(cb, 6, st, sh);
}
void push(VkCommandBuffer cb, const void* data, uint32_t bytes) {
    VkPushDataInfoEXT pi{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
    pi.data = {data, bytes};
    g.vkCmdPushDataEXT(cb, &pi);
}
void record_draw(VkCommandBuffer cb, const Ready& r, VkShaderEXT ms_override = VK_NULL_HANDLE) {
    Dyn dyn;
    std::memcpy(&dyn, r.d->d.dyn, sizeof dyn);
    set_state(cb, dyn);
    bind_stages(cb, ms_override ? ms_override : r.ms, r.fs);
    push(cb, r.push, r.push_bytes);
    if (r.d->d.task) g.vkCmdDrawMeshTasksIndirectEXT(cb, r.header.b, 0, 1, 16);
    else if (r.d->d.indirect) g.vkCmdDrawMeshTasksIndirectEXT(cb, r.record.b, 0, 1, 16);
    else g.vkCmdDrawMeshTasksEXT(cb, r.groups, 1, 1);
}
enum class Between { none, local, full, split };
void barrier_between(VkCommandBuffer cb, Between how) {
    if (how == Between::local) {   // (M6d's: the framebuffer-local barrier a rendering may hold)
        const VkPipelineStageFlags2 fb = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = fb;
        b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &d);
    } else if (how == Between::full) {   // (M6d's tmsync: a full barrier inside the rendering, which RADV records as the wait it is)
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &d);
    }
}

// ---- markers ----
struct Marks {
    Buf buf;   // (host memory: a reset loses VRAM)
    uint32_t n = 0;
    void init(uint32_t entries) {
        n = entries;
        buf = make_buf(8ull * entries + 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }
    void before(VkCommandBuffer cb, uint32_t i) { g.vkCmdWriteBufferMarker2AMD(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, buf.b, 8ull * i, 1); }
    void after(VkCommandBuffer cb, uint32_t i) { g.vkCmdWriteBufferMarker2AMD(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, buf.b, 8ull * i + 4, 1); }
    void clear() { std::memset(buf.map, 0, 8ull * n); }
    uint32_t top(uint32_t i) const { return ((volatile uint32_t*)buf.map)[2 * i]; }
    uint32_t bottom(uint32_t i) const { return ((volatile uint32_t*)buf.map)[2 * i + 1]; }
};

VkCommandBuffer new_cb(VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.pool, ai.level = level, ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK(vkAllocateCommandBuffers(g.dev, &ai, &cb));
    return cb;
}
VkFence new_fence() {
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    VK(vkCreateFence(g.dev, &fi, nullptr, &f));
    return f;
}
void submit(VkCommandBuffer cb, VkFence f) {
    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = cb;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &ci;
    VK(vkQueueSubmit2(g.queue, 1, &si, f));
}

// ---- the emitted geometry ----
constexpr uint32_t kFill = 0x7fbadbadu;   // (a word never stored: a NaN no shader makes)
struct Geometry {
    uint32_t vc = 0, pc = 0;
    std::map<std::string, std::vector<uint32_t>> vert, prim;   // (field -> words, element-major)
    std::map<std::string, uint32_t> vwords, pwords;
};
std::vector<Geometry> decode(const OutLayout& L, const uint8_t* base, uint32_t groups) {
    std::vector<Geometry> out(groups);
    for (uint32_t w = 0; w < groups; w++) {
        const uint32_t* r = (const uint32_t*)base + (uint64_t)w * L.record_words;
        Geometry& gm = out[w];
        gm.vc = r[0], gm.pc = r[1];
        for (const OutVar& v : L.vars)
            for (const OutField& f : v.fields) {
                auto& dst = (f.per_primitive ? gm.prim : gm.vert)[f.name];
                (f.per_primitive ? gm.pwords : gm.vwords)[f.name] = f.words;
                dst.resize((size_t)v.elements * f.words);
                for (uint32_t e = 0; e < v.elements; e++)
                    std::memcpy(&dst[(size_t)e * f.words], r + v.offset + e * v.elem_words + f.word, 4 * f.words);
            }
    }
    return out;
}
std::string words_str(const uint32_t* w, uint32_t n) {
    std::string s = "(";
    for (uint32_t i = 0; i < n; i++) {
        char b[48];
        float x;
        std::memcpy(&x, &w[i], 4);
        if (w[i] == kFill) std::snprintf(b, sizeof b, "%s-", i ? ", " : "");
        else std::snprintf(b, sizeof b, "%s%g", i ? ", " : "", x);
        s += b;
    }
    return s + ")";
}

/// One draw's dump: the draw drawn once with the module rewritten (spv_dump.h)
std::vector<Geometry> run_dump(const Frame& f, const Draw& d, bool replacement, std::string& err) {
    const std::string keep = repl_dir;
    if (!replacement) repl_dir.clear();
    Ready r = prepare(f, d);
    repl_dir = keep;
    const Stage* ms = r.p->stage(VK_SHADER_STAGE_MESH_BIT_EXT);
    std::vector<uint32_t> code = replacement ? read_spv(repl_path(*r.p)) : ms->spirv;
    if (code.empty()) return err = "no replacement " + repl_path(*r.p), std::vector<Geometry>{};
    OutLayout L;
    if (!output_layout(code, L, err)) return {};
    const uint32_t groups = std::max<uint32_t>(r.groups, 1);
    Buf dump = make_buf(4ull * L.record_words * groups + 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    for (uint64_t i = 0; i < dump.size / 4; i++) ((uint32_t*)dump.map)[i] = kFill;
    const auto patched = add_output_dump(code, dump.addr, L, err);
    if (patched.empty()) return {};
    std::string e2;
    VkShaderEXT ms_dump = make_shader(VK_SHADER_STAGE_MESH_BIT_EXT, patched, ms->maps.data(), ms->h.maps, &e2);
    if (!ms_dump) return err = "the dump's mesh stage not made: " + e2, std::vector<Geometry>{};
    Target t = make_target(d.d);
    VkCommandBuffer cb = new_cb();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cb, &bi));
    to_attachment(cb, t);
    heaps_bind(cb);
    begin_rendering(cb, t, false, true);
    record_draw(cb, r, ms_dump);
    vkCmdEndRendering(cb);
    VK(vkEndCommandBuffer(cb));
    VkFence fence = new_fence();
    submit(cb, fence);
    const VkResult w = vkWaitForFences(g.dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
    if (w != VK_SUCCESS) die("the dump draw did not finish (%d)", (int)w);
    auto geo = decode(L, dump.map, r.groups);
    return geo;
}

/// Two dumps compared by what they draw: each primitive's corners' attributes and its own, in order
struct CmpResult {
    bool equal = true;
    std::string first;
    uint64_t prims = 0, groups = 0, max_ulp = 0;
    std::set<std::string> only_a, only_b;
};
uint64_t ulps(uint32_t a, uint32_t b) {
    auto key = [](uint32_t x) -> int64_t { return (x & 0x80000000u) ? -(int64_t)(x & 0x7fffffff) : (int64_t)x; };
    return (uint64_t)std::llabs(key(a) - key(b));
}
CmpResult compare_geo(const std::vector<Geometry>& A, const std::vector<Geometry>& B) {
    CmpResult c;
    auto note = [&](const std::string& s) {
        if (c.equal) c.first = s;
        c.equal = false;
    };
    if (A.size() != B.size()) note("workgroups " + std::to_string(A.size()) + " against " + std::to_string(B.size()));
    for (size_t w = 0; w < std::min(A.size(), B.size()); w++) {
        const Geometry &a = A[w], &b = B[w];
        c.groups++;
        if (a.pc != b.pc) {
            note("workgroup " + std::to_string(w) + ": " + std::to_string(a.pc) + " primitives against " + std::to_string(b.pc));
            continue;
        }
        for (const auto& [k, v] : a.vert)
            if (!b.vert.count(k) && k != "psize") c.only_a.insert(k);
        for (const auto& [k, v] : b.vert)
            if (!a.vert.count(k) && k != "psize") c.only_b.insert(k);
        for (const auto& [k, v] : a.prim)
            if (!b.prim.count(k)) c.only_a.insert(k);
        for (const auto& [k, v] : b.prim)
            if (!a.prim.count(k)) c.only_b.insert(k);
        const auto& ia = a.prim.at("idx");
        const auto& ib = b.prim.at("idx");
        const uint32_t corners = a.pwords.at("idx");
        for (uint32_t p = 0; p < a.pc; p++) {
            c.prims++;
            for (uint32_t j = 0; j < corners; j++) {
                const uint32_t va = ia[p * corners + j], vb = ib[p * corners + j];
                for (const auto& [k, wa] : a.vert) {
                    const auto it = b.vert.find(k);
                    if (it == b.vert.end()) continue;
                    const uint32_t n = a.vwords.at(k);
                    const uint32_t* x = &wa[(size_t)va * n];
                    const uint32_t* y = &it->second[(size_t)vb * n];
                    if (va >= a.vc || vb >= b.vc || (size_t)(va + 1) * n > wa.size() || (size_t)(vb + 1) * n > it->second.size()) {
                        note("workgroup " + std::to_string(w) + " primitive " + std::to_string(p) + ": a corner past the vertices");
                        continue;
                    }
                    for (uint32_t q = 0; q < n; q++) {
                        if (x[q] == y[q]) continue;
                        c.max_ulp = std::max(c.max_ulp, ulps(x[q], y[q]));
                        note("workgroup " + std::to_string(w) + " primitive " + std::to_string(p) + " corner " + std::to_string(j) + " " + k +
                             ": " + words_str(x, n) + " against " + words_str(y, n));
                        break;
                    }
                }
            }
            for (const auto& [k, wa] : a.prim) {
                if (k == "idx") continue;
                const auto it = b.prim.find(k);
                if (it == b.prim.end()) continue;
                const uint32_t n = a.pwords.at(k);
                if (std::memcmp(&wa[(size_t)p * n], &it->second[(size_t)p * n], 4 * n))
                    note("workgroup " + std::to_string(w) + " primitive " + std::to_string(p) + " " + k + ": " + words_str(&wa[(size_t)p * n], n) +
                         " against " + words_str(&it->second[(size_t)p * n], n));
            }
        }
    }
    return c;
}

std::vector<const Draw*> select(const Frame& f, const std::vector<std::string>& args, bool task_only) {
    std::vector<const Draw*> v;
    for (const std::string& a : args) {
        if (a == "all" || a == "task") {
            for (const Draw& d : f.draws)
                if (d.mesh && (a == "all" || d.d.task)) v.push_back(&d);
            continue;
        }
        const Draw* d = f.by_seq((uint32_t)std::atoi(a.c_str()));
        if (!d || !d->mesh) die("no mesh draw %s", a.c_str());
        v.push_back(d);
    }
    if (task_only) v.erase(std::remove_if(v.begin(), v.end(), [](const Draw* d) { return !d->d.task; }), v.end());
    return v;
}

void list(const Frame& f) {
    std::printf("frame of present %llu: %zu programs, %zu blobs, %zu commands\n", (unsigned long long)f.h.present, f.programs.size(), f.blobs.size(), f.draws.size());
    std::map<uint64_t, std::vector<const Draw*>> by_cb;
    for (const Draw& d : f.draws) by_cb[d.cb()].push_back(&d);
    for (const auto& [cb, v] : by_cb) {
        uint32_t mesh = 0, task = 0;
        for (const Draw* d : v) mesh += d->mesh, task += d->mesh && d->d.task;
        if (!mesh) continue;
        std::printf("command buffer %#llx: %zu commands, %u mesh draws (%u task+mesh)\n", (unsigned long long)cb, v.size(), mesh, task);
        for (const Draw* d : v) {
            if (!d->mesh) {
                static const char* k[] = {"?", "draw", "draw indexed", "draw indirect", "task step", "barrier"};
                std::printf("  #%-5u %-14s %016llx count %u\n", d->o.seq, k[std::min<uint32_t>(d->o.kind, 5)], (unsigned long long)d->o.key, d->o.count);
                continue;
            }
            const auto pit = f.programs.find(d->d.key);
            const Program* p = pit == f.programs.end() ? nullptr : &pit->second;
            const mcap::McDraw& x = d->d;
            std::printf("  #%-5u %-14s %016llx mesh %016llx task %016llx groups %u (tasks %u) %ux%u n %u depth %u kind %u%s\n", x.seq,
                        x.task ? "task+mesh" : x.indirect ? "mesh indirect" : "mesh", (unsigned long long)x.key,
                        p ? (unsigned long long)p->h.codes[0] : 0ull, p ? (unsigned long long)p->h.task_code : 0ull,
                        x.task ? std::min(x.header[0], x.header[8]) : x.indirect ? x.record_words[0] : (uint32_t)x.y, x.task ? x.header[9] : 0, x.w, x.h,
                        x.n, x.depth, x.list_kind, x.gated ? " gated" : "");
        }
    }
}

}  // namespace

/// meshlab probe <spv> [--groups N] [--pipeline]: a mesh stage of our own (tools/meshlab/probes) drawn once: push data {an output
/// buffer's address, k = 3}; its first 32 words printed, and checked against the scratch probe's expectation
int probe(int argc, char** argv) {
    const auto code = read_spv(argv[2]);
    if (code.empty()) die("no %s", argv[2]);
    uint32_t groups = 1;
    for (int i = 3; i + 1 < argc; i++)
        if (!std::strcmp(argv[i], "--groups")) groups = (uint32_t)std::atoi(argv[i + 1]);
    gpu_init();
    heaps_init();
    Buf out = make_buf(4096 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    VkShaderEXT ms = make_shader(VK_SHADER_STAGE_MESH_BIT_EXT, code, nullptr, 0);
    if (!ms) die("the probe's mesh stage not made");
    for (int i = 3; i < argc; i++)
        if (!std::strcmp(argv[i], "--compile")) return std::printf("probe: made, not drawn\n"), 0;
    mcap::McDraw d{};
    d.w = d.h = 64, d.n = 1, d.color[0] = VK_FORMAT_R8G8B8A8_UNORM, d.samples = 1;
    VkShaderEXT fs = simple_fs(d);
    Target t = make_target(d);
    VkCommandBuffer cb = new_cb();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(cb, &bi));
    to_attachment(cb, t);
    heaps_bind(cb);
    begin_rendering(cb, t, false, true);
    Dyn dyn{};
    dyn.vp = {0, 0, 64, 64, 0, 1}, dyn.sc = {{0, 0}, {64, 64}}, dyn.samples = VK_SAMPLE_COUNT_1_BIT, dyn.n = 1, dyn.wm[0] = 0xF;
    dyn.front = VK_FRONT_FACE_COUNTER_CLOCKWISE, dyn.poly = VK_POLYGON_MODE_FILL, dyn.dop = VK_COMPARE_OP_ALWAYS;
    dyn.eq[0] = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
    set_state(cb, dyn);
    bind_stages(cb, ms, fs);
    struct { uint64_t a; uint32_t k, pad; } pd{out.addr, 3, 0};
    push(cb, &pd, sizeof pd);
    g.vkCmdDrawMeshTasksEXT(cb, groups, 1, 1);
    vkCmdEndRendering(cb);
    VK(vkEndCommandBuffer(cb));
    VkFence f = new_fence();
    submit(cb, f);
    const VkResult r = vkWaitForFences(g.dev, 1, &f, VK_TRUE, 3ull * 1000 * 1000 * 1000);
    if (r != VK_SUCCESS) {
        std::printf("PROBE HUNG (%d)\n", (int)r);
        std::fflush(stdout);
        std::_Exit(3);
    }
    const uint32_t* w = (const uint32_t*)out.map;
    int bad = 0;
    for (uint32_t i = 0; i < 32; i++) {
        const uint32_t idx = (i * 7u + 3u) & 255u;
        uint32_t want = 0;
        for (uint32_t j = 0; j < 256; j++) if (((j * 13u + 3u) & 255u) == idx) want = j * 3u + i;
        bad += w[i] != want;
        std::printf("%u%s ", w[i], w[i] != want ? "!" : "");
    }
    std::printf("\nprobe: %d of 32 words wrong (scratch probe's expectation)\n", bad);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "probe")) return probe(argc, argv);
    if (argc < 3) die("meshlab <frame.bin> list|compare|dump|run ...");
    Frame f;
    std::string err;
    if (!load(argv[1], f, err)) die("%s: %s", argv[1], err.c_str());
    const std::string mode = argv[2];
    std::vector<std::string> pos;
    double seconds = 10, watchdog = 2;
    int reps = 1, job = -1, prims_shown = 8;
    bool task_only = false, secondary = false, quiet = false, empty = false, query = false;
    std::string cond;   // (--cond once|draw: the draws under conditional rendering on a predicate that lets them draw, begun once or around each)
    Between between = Between::none;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) die("%s needs a value", a.c_str()); return argv[++i]; };
        if (a == "--repl") repl_dir = next();
        else if (a == "--mesh") mesh_override = next();
        else if (a == "--fs") fs_mode = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--watchdog") watchdog = std::atof(next().c_str());
        else if (a == "--reps") reps = std::atoi(next().c_str());
        else if (a == "--job") job = std::atoi(next().c_str());
        else if (a == "--prims") prims_shown = std::atoi(next().c_str());
        else if (a == "--task-only") task_only = true;
        else if (a == "--secondary") secondary = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--host") host_only = true;
        else if (a == "--empty") empty = true;
        else if (a == "--cond") cond = next();
        else if (a == "--query") query = true;
        else if (a == "--barrier") {
            const std::string b = next();
            between = b == "local" ? Between::local : b == "full" ? Between::full : b == "split" ? Between::split : Between::none;
        } else pos.push_back(a);
    }
    if (mode == "list") return list(f), 0;
    if (mode == "extract") {   // (each program's stages as the translator made them: <dir>/<mesh code>.spv, <key>_frag.spv)
        if (pos.empty()) die("extract <dir>");
        for (const auto& [k, p] : f.programs)
            for (const Stage& s : p.stages) {
                char b[600];
                if (s.h.vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT) std::snprintf(b, sizeof b, "%s/%016llx.spv", pos[0].c_str(), (unsigned long long)p.h.codes[0]);
                else std::snprintf(b, sizeof b, "%s/%016llx_%s.spv", pos[0].c_str(), (unsigned long long)k, s.h.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT ? "frag" : "task_cs");
                FILE* o = std::fopen(b, "wb");
                if (!o) die("cannot write %s", b);
                std::fwrite(s.spirv.data(), 4, s.spirv.size(), o), std::fclose(o);
            }
        return 0;
    }
    std::vector<const Draw*> draws;
    if (job >= 0) {
        const Draw* j = f.by_seq((uint32_t)job);
        if (!j) die("no command %d", job);
        for (const Draw& d : f.draws)
            if (d.mesh && d.cb() == j->cb()) draws.push_back(&d);
        if (task_only) draws.erase(std::remove_if(draws.begin(), draws.end(), [](const Draw* d) { return !d->d.task; }), draws.end());
    } else {
        draws = select(f, pos, task_only);
    }
    if (draws.empty()) die("no draws chosen");
    gpu_init();
    heaps_init();
    guest_init(f, draws);
    std::fprintf(stderr, "meshlab: %s, %zu draws; guest memory %llu MB\n", g.props.deviceName, draws.size(), (unsigned long long)(guest.bytes >> 20));

    if (mode == "dump") {
        const auto geo = run_dump(f, *draws[0], !repl_dir.empty(), err);
        if (geo.empty() && !err.empty()) die("%s", err.c_str());
        for (size_t w = 0; w < geo.size(); w++) {
            const Geometry& gm = geo[w];
            std::printf("workgroup %zu: %u vertices, %u primitives\n", w, gm.vc, gm.pc);
            const auto& idx = gm.prim.at("idx");
            const uint32_t corners = gm.pwords.at("idx");
            for (uint32_t p = 0; p < gm.pc && (int)p < prims_shown; p++) {
                std::printf("  prim %u:", p);
                for (uint32_t j = 0; j < corners; j++) std::printf(" %u", idx[p * corners + j]);
                for (const auto& [k, v] : gm.prim)
                    if (k != "idx") std::printf(" %s %s", k.c_str(), words_str(&v[(size_t)p * gm.pwords.at(k)], gm.pwords.at(k)).c_str());
                std::printf("\n");
                for (uint32_t j = 0; j < corners; j++) {
                    const uint32_t vi = idx[p * corners + j];
                    std::printf("    v%u:", vi);
                    for (const auto& [k, v] : gm.vert)
                        if ((size_t)(vi + 1) * gm.vwords.at(k) <= v.size()) std::printf(" %s %s", k.c_str(), words_str(&v[(size_t)vi * gm.vwords.at(k)], gm.vwords.at(k)).c_str());
                    std::printf("\n");
                }
            }
        }
        return 0;
    }
    if (mode == "compare") {
        if (repl_dir.empty()) die("compare needs --repl <dir>");
        int equal = 0, different = 0, missing = 0;
        std::map<uint64_t, std::pair<int, int>> by_code;
        for (const Draw* d : draws) {
            const Program& p = f.programs.at(d->d.key);
            if (read_spv(repl_path(p)).empty()) {
                missing++;
                continue;
            }
            std::string e1, e2;
            const auto a = run_dump(f, *d, false, e1);
            const auto b = run_dump(f, *d, true, e2);
            if (!e1.empty() || !e2.empty()) die("draw %u: %s %s", d->d.seq, e1.c_str(), e2.c_str());
            const CmpResult c = compare_geo(a, b);
            auto& bc = by_code[p.h.codes[0]];
            (c.equal ? bc.first : bc.second)++;
            (c.equal ? equal : different)++;
            if (!quiet || !c.equal) {
                std::printf("draw #%u %016llx (mesh %016llx): %s: %llu workgroups, %llu primitives", d->d.seq, (unsigned long long)d->d.key,
                            (unsigned long long)p.h.codes[0], c.equal ? "EQUAL" : "DIFFERENT", (unsigned long long)c.groups, (unsigned long long)c.prims);
                if (!c.equal) std::printf("; first: %s (max %llu ulp)", c.first.c_str(), (unsigned long long)c.max_ulp);
                for (const auto& k : c.only_a) std::printf("; only the translator writes %s", k.c_str());
                for (const auto& k : c.only_b) std::printf("; only the replacement writes %s", k.c_str());
                std::printf("\n");
            }
        }
        std::printf("compare: %d equal, %d different, %d without a replacement\n", equal, different, missing);
        for (const auto& [code, n] : by_code) std::printf("  mesh %016llx: %d equal, %d different\n", (unsigned long long)code, n.first, n.second);
        return different ? 1 : 0;
    }
    if (mode == "stats") {   // (the stages made and nothing drawn: RADV_DEBUG=shaderstats prints what the compiler made)
        for (const Draw* d : draws) prepare(f, *d);
        std::printf("stats: %zu shader objects made\n", shader_cache.size());
        return 0;
    }
    if (mode != "run") die("unknown mode %s", mode.c_str());

    // ---- the hang run: the draws in one rendering, again and again ----
    std::vector<Ready> ready;
    for (const Draw* d : draws) ready.push_back(prepare(f, *d));
    // (one target: the first draw's formats and size; draws of other formats are left out)
    Target t = make_target(draws[0]->d);
    std::vector<const Ready*> run;
    for (const Ready& r : ready) {
        const mcap::McDraw& x = r.d->d;
        bool same = x.n == draws[0]->d.n && x.depth == draws[0]->d.depth && x.samples == draws[0]->d.samples;
        for (uint32_t i = 0; i < x.n && same; i++) same = x.color[i] == draws[0]->d.color[i];
        if (!same) {
            std::fprintf(stderr, "meshlab: draw #%u left out: other targets\n", x.seq);
            continue;
        }
        run.push_back(&r);
    }
    std::printf("run: %zu draws x %d a submission, barrier %s, %s, fs %s, mesh %s, condition %s, query %d; %u x %u\n", run.size(), reps,
                between == Between::none ? "none" : between == Between::local ? "local" : between == Between::full ? "full" : "split (rendering ended)",
                secondary ? "in a secondary" : "inline", fs_mode.c_str(), !mesh_override.empty() ? mesh_override.c_str() : !repl_dir.empty() ? "replacement" : "translator",
                cond.empty() ? "none" : cond.c_str(), (int)query, t.w, t.h);
    for (const Ready* r : run)
        std::printf("  #%u %016llx %s groups %u\n", r->d->d.seq, (unsigned long long)r->d->d.key, r->d->d.task ? "task+mesh" : "mesh", r->groups);
    const uint32_t per = (uint32_t)run.size() * (uint32_t)reps;
    Buf predicate = make_buf(256, VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT, true);
    ((uint32_t*)predicate.map)[0] = 1;   // (non-zero: the draws draw, as the game's passed condition)
    auto cond_begin = [&](VkCommandBuffer c) {
        VkConditionalRenderingBeginInfoEXT bi{VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT};
        bi.buffer = predicate.b, bi.offset = 0;
        g.vkCmdBeginConditionalRenderingEXT(c, &bi);
    };
    VkQueryPool qpool = VK_NULL_HANDLE;
    if (query) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION, qi.queryCount = 4;
        VK(vkCreateQueryPool(g.dev, &qi, nullptr, &qpool));
    }
    Marks marks[2];
    VkCommandBuffer cbs[2];
    VkFence fences[2];
    for (int k = 0; k < 2; k++) {
        marks[k].init(per);
        cbs[k] = new_cb();
        fences[k] = new_fence();
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(cbs[k], &bi));
        to_attachment(cbs[k], t);
        heaps_bind(cbs[k]);
        if (query) vkCmdResetQueryPool(cbs[k], qpool, (uint32_t)k, 1);
        VkCommandBuffer sec = VK_NULL_HANDLE;
        auto open = [&]() {
            begin_rendering(cbs[k], t, secondary, true);
            if (query) vkCmdBeginQuery(cbs[k], qpool, (uint32_t)k, VK_QUERY_CONTROL_PRECISE_BIT);
            if (!secondary) {
                if (cond == "once") cond_begin(cbs[k]);
                return cbs[k];
            }
            sec = new_cb(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
            VkCommandBufferInheritanceRenderingInfo ri{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
            ri.colorAttachmentCount = (uint32_t)t.formats.size(), ri.pColorAttachmentFormats = t.formats.data();
            ri.depthAttachmentFormat = t.depth_format, ri.stencilAttachmentFormat = t.stencil_format, ri.rasterizationSamples = t.samples;
            VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
            ii.pNext = &ri;
            ii.occlusionQueryEnable = VK_TRUE, ii.queryFlags = VK_QUERY_CONTROL_PRECISE_BIT;   // (as the back end's jobs)
            VkCommandBufferBeginInfo sb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            sb.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
            sb.pInheritanceInfo = &ii;
            VK(vkBeginCommandBuffer(sec, &sb));
            heaps_bind(sec);
            if (cond == "once") cond_begin(sec);
            return sec;
        };
        auto close = [&]() {
            if (cond == "once") g.vkCmdEndConditionalRenderingEXT(secondary ? sec : cbs[k]);
            if (secondary) {
                VK(vkEndCommandBuffer(sec));
                vkCmdExecuteCommands(cbs[k], 1, &sec);
            }
            if (query) vkCmdEndQuery(cbs[k], qpool, (uint32_t)k);
            vkCmdEndRendering(cbs[k]);
        };
        VkCommandBuffer cb = open();
        uint32_t m = 0;
        for (int rep = 0; rep < reps; rep++)
            for (size_t i = 0; i < run.size(); i++, m++) {
                if (m) {
                    if (between == Between::split) {
                        close();
                        cb = open();
                    } else {
                        barrier_between(cb, between);
                    }
                }
                marks[k].before(cb, m);
                if (cond == "draw") cond_begin(cb);
                if (empty) g.vkCmdDrawMeshTasksEXT(cb, 0, 1, 1);
                else record_draw(cb, *run[i]);
                if (cond == "draw") g.vkCmdEndConditionalRenderingEXT(cb);
                marks[k].after(cb, m);
            }
        close();
        VK(vkEndCommandBuffer(cbs[k]));
    }
    const auto t0 = clk::now();
    uint64_t submits = 0;
    bool inflight[2] = {false, false};
    int cur = 0;
    bool hung = false;
    while (std::chrono::duration<double>(clk::now() - t0).count() < seconds) {
        if (inflight[cur]) {
            // (the watchdog: a submission not finished after S seconds stops the run)
            const auto w0 = clk::now();
            VkResult r;
            while ((r = vkWaitForFences(g.dev, 1, &fences[cur], VK_TRUE, 50ull * 1000 * 1000)) == VK_TIMEOUT) {
                if (std::chrono::duration<double>(clk::now() - w0).count() > watchdog) {
                    hung = true;
                    break;
                }
            }
            if (hung || r != VK_SUCCESS) {
                hung = true;
                std::printf("HANG: submission %llu not finished after %.1f s (%s)\n", (unsigned long long)submits, watchdog, r == VK_TIMEOUT ? "timeout" : "device lost");
                for (int k = 0; k < 2; k++) {
                    if (!inflight[k]) continue;
                    std::printf("  submission slot %d markers (started / finished):", k);
                    for (uint32_t i = 0; i < per; i++) {
                        const uint32_t a = marks[k].top(i), b = marks[k].bottom(i);
                        if (a && !b) std::printf(" open:%u(#%u)", i, run[i % run.size()]->d->d.seq);
                    }
                    uint32_t done = 0;
                    for (uint32_t i = 0; i < per; i++) done += marks[k].bottom(i) != 0;
                    std::printf("; %u of %u finished\n", done, per);
                }
                break;
            }
            vkResetFences(g.dev, 1, &fences[cur]);
            inflight[cur] = false;
        }
        marks[cur].clear();
        submit(cbs[cur], fences[cur]);
        inflight[cur] = true;
        submits++;
        cur ^= 1;
    }
    if (!hung) {
        for (int k = 0; k < 2; k++)
            if (inflight[k] && vkWaitForFences(g.dev, 1, &fences[k], VK_TRUE, (uint64_t)(watchdog * 1e9)) != VK_SUCCESS) hung = true;
    }
    const double s = std::chrono::duration<double>(clk::now() - t0).count();
    std::printf("%s: %llu submissions (%llu draws) in %.1f s\n", hung ? "HUNG" : "SURVIVED", (unsigned long long)submits,
                (unsigned long long)(submits * per), s);
    std::fflush(stdout);
    if (hung) std::_Exit(3);
    vkDeviceWaitIdle(g.dev);
    return 0;
}
