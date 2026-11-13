// The descriptor heap and M2b's device part (direct_heap.h). The heaps and their encodings from gpu/vk/vk_desc_heap.cpp
// (make_heap, the device half, encode image / sampler), cut to what registration-time entries need: no ring, no lazy
// resolves, no classic descriptor commands. The sampled view rules from gpu/vk/vk_texture.cpp texture_view and the sampler
// from gpu/vk/vk_draw.cpp sampler_make, without their switches and without the shadow filter's PCF (a picture change, not
// Vulkan's need: plan rule 8).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "direct_marks.h"
#include "direct_pstats.h"
#include "direct_check.h"
#include "shader_recompiler/shader_info.h"

#include "direct_heap.h"
#include "direct_image.h"
#include "direct_assets.h"

#define HSD_VK_DEFINE(f) PFN_##f f;
namespace hsd::vkfn {
HSD_VK_M2B(HSD_VK_DEFINE)
HSD_VK_M4B(HSD_VK_DEFINE)
}
#undef HSD_VK_DEFINE

namespace hsd {

DrawCaps caps;
void draw_window_table_init();   // (direct_draw.cpp)

namespace {

// ---- the device ----
VkPhysicalDeviceShaderObjectFeaturesEXT shobj{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
VkPhysicalDeviceDescriptorHeapFeaturesEXT heap_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
VkPhysicalDeviceProvokingVertexFeaturesEXT pv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
// (M4b: mesh shading with the mesh stage only. The task stage is never enabled: native task-stage draws lose the device on this
// GPU (plan, "Task and mesh shaders"); with the feature off no task shader object can be made at all)
VkPhysicalDeviceMeshShaderFeaturesEXT mesh_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
bool has_ext[8]{};
enum { E_SHOBJ, E_HEAP, E_EDS3, E_PV, E_STENCIL, E_ATOMIC_FLOAT, E_M5, E_MESH };

VkPhysicalDeviceDescriptorHeapPropertiesEXT hprops{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};

// ---- the heaps ----
struct HeapBuf {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0, reserved_at = 0, reserved = 0;
};
HeapBuf res_heap, smp_heap;
bool ready = false;
uint64_t tail_at = 0;

/// A buffer in device-local, host-visible memory (the BAR) where there is one, mapped, with its device address
bool make_mapped(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& map,
                 VkDeviceAddress& address) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev.device, buffer, &mr);
    const VkMemoryPropertyFlags want[2] = {
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    uint32_t type = UINT32_MAX;
    for (VkMemoryPropertyFlags w : want) {
        if (marks_on() && (w & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;   // (M6c: HOS_DBG=marks reads the arena after a reset, which loses VRAM)
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)
            if ((mr.memoryTypeBits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & w) == w) type = i;
        if (type != UINT32_MAX) break;
    }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fi;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev.device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(dev.device, buffer, memory, 0) != VK_SUCCESS) return false;
    void* p = nullptr;
    if (vkMapMemory(dev.device, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
    map = (uint8_t*)p;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = buffer;
    address = vkGetBufferDeviceAddress(dev.device, &da);
    object_made(2, "a mapped buffer");
    return address != 0;
}

bool make_heap(HeapBuf& h, VkDeviceSize used, VkDeviceSize min_reserved) {
    const VkDeviceSize align = 256;
    h.reserved_at = (used + align - 1) & ~(align - 1);
    h.reserved = (min_reserved + align - 1) & ~(align - 1);
    h.size = h.reserved_at + h.reserved;
    if (!make_mapped(h.size, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT, h.buffer, h.memory, h.map, h.address)) return false;
    std::memset(h.map, 0, h.reserved_at);
    return true;
}

// ---- entries ----
std::mutex heap_mu;   // (registrations: the entries, slot_image)
std::unique_ptr<const Image*[]> slot_image;   // (texture id -> the image its entries hold)
struct SlotTex {
    uint64_t gpu;
    uint32_t w, h;
    int format, target;
    uint32_t depth = 0, levels = 0;   // (M4h, logs: the texture's depth or layers and levels, and its view's)
    nvn_view view{};
    bool has_view = false;
};
std::unique_ptr<SlotTex[]> slot_tex;   // (logs: what was registered there)
std::unique_ptr<std::atomic<uint8_t>[]> answers;   // (texture id -> answer byte)

std::atomic<int64_t> st_storage{0}, st_tex{0}, st_entries{0}, st_nulls{0}, st_big_id{0}, st_samp{0}, st_border{0}, st_gone{0}, st_no_image{0},
    st_failed{0}, st_remapped{0};

VkComponentSwizzle component(unsigned s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}
bool is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
VkFormat widened(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return f;
    }
}

/// A sampled view of t's image as view type `type` (texture_view's rules); false: Vulkan allows no such view of it
bool sampled_view(const Image* im, const nvn_tex* t, const nvn_view* v, VkImageViewType type, VkImageViewCreateInfo& vi) {
    const int nf = v && v->set_format ? v->format : t->d.format;
    VkFormat f = im->placeholder ? VK_FORMAT_R8G8B8A8_UNORM : widened(vk_format(nf).format);
    const bool depth = !(im->aspect & VK_IMAGE_ASPECT_COLOR_BIT);
    if (depth || f == VK_FORMAT_UNDEFINED || im->placeholder) f = im->format;
    else if (f != im->format) {   // (a reinterpreting view: only within the image's size class, which MUTABLE_FORMAT allows)
        int bw = 1, bh = 1;
        const int bpb = nvn_block_size(nf, &bw, &bh);
        if (bpb != im->bpb || bw != im->bw || bh != im->bh || is_bc(f) != im->compressed) f = im->format;
    }
    const uint32_t vl = v && v->set_levels ? v->base_level : 0;
    if (vl >= im->levels) return false;
    uint32_t nlevels = v && v->set_levels && v->nlevels ? v->nlevels : im->levels - vl;
    nlevels = std::clamp<uint32_t>(nlevels, 1, im->levels - vl);
    const uint32_t total = im->type == VK_IMAGE_TYPE_3D ? 1 : im->layers;
    uint32_t layer0 = std::min<uint32_t>(v && v->set_layers ? v->min_layer : 0, total - 1);
    uint32_t nlayers = v && v->set_layers && v->nlayers ? v->nlayers : total - layer0;
    nlayers = std::clamp<uint32_t>(nlayers, 1, total - layer0);
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        if (im->type != VK_IMAGE_TYPE_1D) return false;
        if (type == VK_IMAGE_VIEW_TYPE_1D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_2D: case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        if (im->type != VK_IMAGE_TYPE_2D) return false;   // (a 2D view of a 3D image needs VK_EXT_image_2d_view_of_3d: not yet)
        if (type == VK_IMAGE_VIEW_TYPE_2D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_3D:
        if (im->type != VK_IMAGE_TYPE_3D) return false;
        layer0 = 0, nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE: case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        if (im->type != VK_IMAGE_TYPE_2D || im->layers < 6 || im->width != im->height || im->samples != VK_SAMPLE_COUNT_1_BIT)
            return false;
        nlayers = type == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : std::max(6u, nlayers / 6 * 6);
        if (layer0 + nlayers > im->layers) return false;
        break;
    default: return false;
    }
    unsigned sw[4] = {t->d.swizzle_r, t->d.swizzle_g, t->d.swizzle_b, t->d.swizzle_a};
    if (v && v->set_swizzle)
        for (int i = 0; i < 4; i++) sw[i] = v->swz[i];
    switch (nf) {   // (X formats read alpha as one)
    case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX8I:
    case NVN_FORMAT_RGBX16F: case NVN_FORMAT_RGBX16: case NVN_FORMAT_RGBX16SN: case NVN_FORMAT_RGBX16UI:
    case NVN_FORMAT_RGBX16I: case NVN_FORMAT_RGBX32F: case NVN_FORMAT_RGBX32UI: case NVN_FORMAT_RGBX32I:
    case NVN_FORMAT_RGBX8_SRGB: case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRX8_SRGB:
        for (unsigned& s : sw)
            if (s == NVN_TEXTURE_SWIZZLE_A) s = NVN_TEXTURE_SWIZZLE_ONE;
        break;
    default: break;
    }
    vi = VkImageViewCreateInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im->image;
    vi.viewType = type;
    vi.format = f;
    // (M4h: the game's swizzle names the source texture's channels; a prepared image keeps them where its record's channel map
    // says (BC4 / BC5 / RG8 move them: an ASTC normal map read R,A is stored R,G), so the view reads through that map too)
    if (im->placeholder) {
        vi.components = VkComponentMapping{};
    } else {
        const VkComponentMapping game{component(sw[0]), component(sw[1]), component(sw[2]), component(sw[3])};
        static const bool unmapped = dbg("nochanmap");   // (M4h, a diagnosis: the game's swizzle alone, as before M4h)
        vi.components = unmapped ? game : image_swizzle(im, game);
        if (std::memcmp(&vi.components, &game, sizeof game)) st_remapped.fetch_add(1, std::memory_order_relaxed);
    }
    VkImageAspectFlags aspect = im->aspect;
    if (aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange = {aspect, vl, nlevels, layer0, nlayers};
    return true;
}

bool encode_image(const VkImageViewCreateInfo* vi, uint8_t out[kImageDesc], bool storage = false) {
    std::memset(out, 0, kImageDesc);
    if (!vi) return true;   // (zeros: a null descriptor on RDNA)
    VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    uci.usage = storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewCreateInfo ci = *vi;
    ci.pNext = &uci;
    VkImageDescriptorInfoEXT ii{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
    ii.pView = &ci;
    ii.layout = VK_IMAGE_LAYOUT_GENERAL;
    VkResourceDescriptorInfoEXT ri{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    ri.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    ri.data.pImage = &ii;
    VkHostAddressRangeEXT dst{out, (size_t)kImageDesc};
    return vkWriteResourceDescriptorsEXT(dev.device, 1, &ri, &dst) == VK_SUCCESS;
}

/// (M3b) A storage view of t's image as view type `type` from its sampled view: one level, no swizzle, a format the device
/// stores to (the view's, else the image's own); false: none
bool storage_view(const Image* im, VkImageViewCreateInfo& vi) {
    if (!im || !im->storage || im->placeholder || (im->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (vi.viewType == VK_IMAGE_VIEW_TYPE_CUBE || vi.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) return false;
    static std::once_flag once;
    static uint8_t can[256];   // (VkFormat < 256: optimal tiling has STORAGE_IMAGE)
    std::call_once(once, [] {
        for (int f = 0; f < 256; f++) {
            VkFormatProperties p{};
            vkGetPhysicalDeviceFormatProperties(dev.physical, (VkFormat)f, &p);
            can[f] = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        }
    });
    auto ok = [&](VkFormat f) { return (uint32_t)f < 256 && can[f]; };
    if (!ok(vi.format)) {
        if (ok(im->format)) {
            vi.format = im->format;
        } else {   // (M4a: a format the device cannot store (B10G11R11, E5B9G9R9, BGRA8): the unsigned one of its size, which
                   // the game's raw stores (SUST.B) write packed as Maxwell has them; the image is made MUTABLE)
            const VkFormat raw = im->compressed ? VK_FORMAT_UNDEFINED : im->bpb == 4 ? VK_FORMAT_R32_UINT : im->bpb == 8 ? VK_FORMAT_R32G32_UINT
                               : im->bpb == 16 ? VK_FORMAT_R32G32B32A32_UINT : im->bpb == 2 ? VK_FORMAT_R16_UINT : im->bpb == 1 ? VK_FORMAT_R8_UINT
                               : VK_FORMAT_UNDEFINED;
            if (!ok(raw)) return false;
            vi.format = raw;
        }
    }
    vi.components = VkComponentMapping{};
    vi.subresourceRange.levelCount = 1;
    return true;
}

/// The view types a texture of this NVN target may be sampled as (the shader's declared type picks the region)
uint32_t regions_of(int target) {
    auto b = [](VkImageViewType t) { return 1u << t; };
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: case NVN_TEXTURE_TARGET_1D_ARRAY: return b(VK_IMAGE_VIEW_TYPE_1D) | b(VK_IMAGE_VIEW_TYPE_1D_ARRAY);
    case NVN_TEXTURE_TARGET_3D: return b(VK_IMAGE_VIEW_TYPE_3D);
    case NVN_TEXTURE_TARGET_CUBEMAP: case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:
        return b(VK_IMAGE_VIEW_TYPE_CUBE) | b(VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) | b(VK_IMAGE_VIEW_TYPE_2D_ARRAY) | b(VK_IMAGE_VIEW_TYPE_2D);
    case NVN_TEXTURE_TARGET_BUFFER: return 0;
    default: return b(VK_IMAGE_VIEW_TYPE_2D) | b(VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    }
}

uint8_t texture_type(int target) {
    using T = Shader::TextureType;
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return (uint8_t)T::Color1D;
    case NVN_TEXTURE_TARGET_3D: return (uint8_t)T::Color3D;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return (uint8_t)T::ColorArray1D;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return (uint8_t)T::ColorArray2D;
    case NVN_TEXTURE_TARGET_RECTANGLE: return (uint8_t)T::Color2DRect;
    case NVN_TEXTURE_TARGET_CUBEMAP: return (uint8_t)T::ColorCube;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return (uint8_t)T::ColorArrayCube;
    case NVN_TEXTURE_TARGET_BUFFER: return (uint8_t)T::Buffer;
    default: return (uint8_t)T::Color2D;
    }
}
bool integer_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_S8_UINT: return true;
    default: return false;
    }
}

VkCompareOp compare_op(uint32_t f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
VkSamplerAddressMode wrap(uint8_t w) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

/// A sampler's descriptor (sampler_make's create info; compare: the depth-compare region's, which a shader's depth-reference
/// sample needs whatever the game set: Vulkan leaves such a sample through a sampler that does not compare undefined)
bool encode_sampler(const nvn_samp_par& p, bool compare, uint8_t out[kSamplerDesc]) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = p.magf == NVN_MAG_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = (p.minf == NVN_MIN_FILTER_NEAREST || p.minf == NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST ||
                    p.minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = (p.minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p.minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR)
                        ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap(p.ws), ci.addressModeV = wrap(p.wt), ci.addressModeW = wrap(p.wr);
    ci.mipLodBias = std::clamp(p.lod_bias, -dev.props.limits.maxSamplerLodBias, dev.props.limits.maxSamplerLodBias);
    ci.anisotropyEnable = p.aniso > 1.0f && dev.features.samplerAnisotropy;
    ci.maxAnisotropy = std::clamp(p.aniso, 1.0f, dev.props.limits.maxSamplerAnisotropy);
    ci.compareEnable = compare;
    ci.compareOp = compare ? (p.cmp_mode && p.cmp_func >= 1 && p.cmp_func <= 8 ? compare_op(p.cmp_func) : VK_COMPARE_OP_LESS_OR_EQUAL)
                           : VK_COMPARE_OP_ALWAYS;
    ci.minLod = std::max(0.0f, p.lod_min);
    ci.maxLod = (p.minf == NVN_MIN_FILTER_NEAREST || p.minf == NVN_MIN_FILTER_LINEAR) ? ci.minLod + 0.25f : std::max(ci.minLod, p.lod_max);
    const bool black = !p.border[0] && !p.border[1] && !p.border[2];
    const bool white = p.border[0] == 1.0f && p.border[1] == 1.0f && p.border[2] == 1.0f;
    if (black && !p.border[3]) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (black && p.border[3] == 1.0f) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if (white && p.border[3] == 1.0f) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else {   // (a custom colour needs an index registered with the heap: not yet; counted)
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (wrap(p.ws) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || wrap(p.wt) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)
            st_border.fetch_add(1, std::memory_order_relaxed);
    }
    VkSamplerReductionModeCreateInfo rm{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    if (p.reduction && !compare) {
        rm.reductionMode = p.reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        ci.pNext = &rm;
    }
    std::memset(out, 0, kSamplerDesc);
    VkHostAddressRangeEXT dst{out, (size_t)kSamplerDesc};
    return vkWriteSamplerDescriptorsEXT(dev.device, 1, &ci, &dst) == VK_SUCCESS;
}

bool has(const std::vector<VkExtensionProperties>& exts, const char* n) {
    for (const auto& e : exts)
        if (!std::strcmp(e.extensionName, n)) return true;
    return false;
}

}  // namespace

bool mapped_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& map,
                   VkDeviceAddress& address) {
    return make_mapped(size, usage, buffer, memory, map, address);
}

// ---- the device ----
void draw_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail) {
    const struct { int k; const char* name; } list[] = {
        {E_SHOBJ, VK_EXT_SHADER_OBJECT_EXTENSION_NAME}, {E_HEAP, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME},
        {E_EDS3, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME}, {E_PV, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME},
        {E_STENCIL, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME}, {E_ATOMIC_FLOAT, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME},
        {E_M5, VK_KHR_MAINTENANCE_5_EXTENSION_NAME}, {E_MESH, VK_EXT_MESH_SHADER_EXTENSION_NAME}};
    for (const auto& e : list)
        if ((has_ext[e.k] = has(exts, e.name))) want.push_back(e.name);
    if (has_ext[E_SHOBJ]) *tail = &shobj, tail = &shobj.pNext;
    if (has_ext[E_HEAP]) *tail = &heap_feat, tail = &heap_feat.pNext;
    if (has_ext[E_EDS3]) *tail = &eds3, tail = &eds3.pNext;
    if (has_ext[E_PV]) *tail = &pv, tail = &pv.pNext;
    if (has_ext[E_ATOMIC_FLOAT]) *tail = &af, tail = &af.pNext;
    if (has_ext[E_MESH]) *tail = &mesh_feat, tail = &mesh_feat.pNext;
}

void draw_features_resolved(const VkPhysicalDeviceVulkan14Features* f14) {
    heap_feat.descriptorHeapCaptureReplay = VK_FALSE;
    mesh_feat.taskShader = VK_FALSE;   // (never: see mesh_feat)
    // (M6h: HOS_DBG=pstats counts mesh invocations too, when the device can)
    const VkBool32 mesh_queries = mesh_feat.meshShaderQueries && pstats_wants_mesh_queries();
    mesh_feat.multiviewMeshShader = mesh_feat.primitiveFragmentShadingRateMeshShader = mesh_feat.meshShaderQueries = VK_FALSE;
    mesh_feat.meshShaderQueries = mesh_queries;
    caps.mesh_queries = mesh_queries;
    caps.mesh = has_ext[E_MESH] && mesh_feat.meshShader;
    if (!caps.mesh) mesh_feat.meshShader = VK_FALSE;
    caps.index_uint8 = f14 && f14->indexTypeUint8;
    caps.vertex_divisor = f14 && f14->vertexAttributeInstanceRateDivisor;
    pv.transformFeedbackPreservesProvokingVertex = VK_FALSE;
    caps.provoking_vertex = has_ext[E_PV] && pv.provokingVertexLast;
    const bool eds3_ok = has_ext[E_EDS3] && eds3.extendedDynamicState3PolygonMode && eds3.extendedDynamicState3DepthClampEnable &&
                         eds3.extendedDynamicState3ColorBlendEnable && eds3.extendedDynamicState3ColorBlendEquation &&
                         eds3.extendedDynamicState3ColorWriteMask && eds3.extendedDynamicState3AlphaToCoverageEnable &&
                         eds3.extendedDynamicState3RasterizationSamples && eds3.extendedDynamicState3SampleMask &&
                         eds3.extendedDynamicState3AlphaToOneEnable && eds3.extendedDynamicState3LogicOpEnable &&
                         (!caps.provoking_vertex || eds3.extendedDynamicState3ProvokingVertexMode);
    if (!has_ext[E_SHOBJ] || !shobj.shaderObject) caps.why = "no shader objects";
    else if (!has_ext[E_HEAP] || !heap_feat.descriptorHeap) caps.why = "no VK_EXT_descriptor_heap";
    else if (!eds3_ok) caps.why = "not all of extended dynamic state 3";
    else caps.ok = true, caps.why = "";
    if (!caps.ok) heap_feat.descriptorHeap = VK_FALSE;
}

void draw_device_created() {
    auto fail = [](const char* why) {
        caps.ok = false, caps.why = why;
        logf("direct: no draws: %s", why);
    };
    if (!caps.ok) return void(logf("direct: no draws on this device: %s", caps.why));
    caps.alpha_to_one = dev.features.alphaToOne, caps.logic_op = dev.features.logicOp;   // (enabled: their state must be set)
#define HSD_VK_LOAD(f) f = (PFN_##f)vkGetDeviceProcAddr(dev.device, #f);
    HSD_VK_M2B(HSD_VK_LOAD)
    HSD_VK_M4B(HSD_VK_LOAD)
#undef HSD_VK_LOAD
    bool all = true;
#define HSD_VK_CHECK(f) all = all && f != nullptr;
    HSD_VK_M2B(HSD_VK_CHECK)
#undef HSD_VK_CHECK
    if (!all) return fail("a device function is missing");
    {   // (NVN's programs are written for 32 lanes: the stages the GPU can run at 32 are pinned there, gpu/vk/vk_main.cpp)
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceSubgroupSizeControlProperties sc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        sc.pNext = &drv;
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        sg.pNext = &sc;
        hprops.pNext = &sg;
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &hprops;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        hprops.pNext = nullptr;
        caps.sg32_stages = sc.minSubgroupSize <= 32 && sc.maxSubgroupSize >= 32 ? sc.requiredSubgroupSizeStages : 0;
        caps.subgroup_size = sg.subgroupSize;
        caps.subgroup_max = std::max(sc.maxSubgroupSize, sg.subgroupSize);
        caps.driver_id = drv.driverID;
        caps.max_push = (uint32_t)hprops.maxPushDataSize;
    }
    if (std::max<uint64_t>(hprops.imageDescriptorSize, hprops.imageDescriptorAlignment) > kImageDesc ||
        std::max<uint64_t>(hprops.samplerDescriptorSize, hprops.samplerDescriptorAlignment) > kSamplerDesc)
        return fail("descriptor sizes past the heap's layout");
    // (the resource heap: 7 regions, then the zero tail, so that an id up to 0xFFFFF from the last region's base stays inside)
    tail_at = 2ull * kHeapRegions * kHeapSlots * kImageDesc;   // (M3b: sampled regions, then storage regions)
    const uint64_t res_used = ((uint64_t)(2 * kHeapRegions - 1) * kHeapSlots + (1u << 20)) * kImageDesc;
    if (!make_heap(res_heap, res_used, hprops.minResourceHeapReservedRange) ||
        !make_heap(smp_heap, 2ull * kHeapSamplers * kSamplerDesc, hprops.minSamplerHeapReservedRange) ||
        res_heap.size > hprops.maxResourceHeapSize || smp_heap.size > hprops.maxSamplerHeapSize)
        return fail("the heaps were not made");
    {   // (sampler entries before the game registers them: linear, clamped)
        nvn_samp_par p{};
        p.minf = NVN_MIN_FILTER_LINEAR, p.magf = NVN_MAG_FILTER_LINEAR, p.lod_max = 1000.0f;
        uint8_t d[2][kSamplerDesc];
        if (encode_sampler(p, false, d[0]) && encode_sampler(p, true, d[1]))
            for (uint32_t i = 0; i < kHeapSamplers; i++)
                std::memcpy(smp_heap.map + i * kSamplerDesc, d[0], kSamplerDesc),
                    std::memcpy(smp_heap.map + (kHeapSamplers + i) * kSamplerDesc, d[1], kSamplerDesc);
    }
    slot_image.reset(new const Image*[kHeapSlots]());
    slot_tex.reset(new SlotTex[kHeapSlots]());
    answers.reset(new std::atomic<uint8_t>[kHeapSlots]);
    for (uint32_t i = 0; i < kHeapSlots; i++) answers[i].store(0xFF, std::memory_order_relaxed);
    ready = true;
    draw_window_table_init();
    logf("direct: draws on: shader objects, descriptor heap (%u ids x %u view types, %u samplers x 2; resource heap %llu MB, "
         "sampler heap %llu KB, push data %u B), subgroups %u (up to %u, 32 on stages %#x)", kHeapSlots, kHeapRegions, kHeapSamplers,
         (unsigned long long)(res_heap.size >> 20), (unsigned long long)(smp_heap.size >> 10), caps.max_push, caps.subgroup_size,
         caps.subgroup_max, (unsigned)caps.sg32_stages);
}

// ---- the heap ----
bool heap_on() { return ready; }
uint32_t heap_region_offset(uint32_t region) { return region * kHeapSlots * kImageDesc; }
uint32_t heap_storage_offset(uint32_t region) { return (kHeapRegions + region) * kHeapSlots * kImageDesc; }
uint32_t heap_tail_offset() { return (uint32_t)tail_at; }
uint32_t heap_sampler_offset(bool compare) { return compare ? kHeapSamplers * kSamplerDesc : 0; }

void heap_bind(VkCommandBuffer cb) {
    VkBindHeapInfoEXT rb{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    rb.heapRange = {res_heap.address, res_heap.size}, rb.reservedRangeOffset = res_heap.reserved_at, rb.reservedRangeSize = res_heap.reserved;
    VkBindHeapInfoEXT sb{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    sb.heapRange = {smp_heap.address, smp_heap.size}, sb.reservedRangeOffset = smp_heap.reserved_at, sb.reservedRangeSize = smp_heap.reserved;
    vkCmdBindResourceHeapEXT(cb, &rb);
    vkCmdBindSamplerHeapEXT(cb, &sb);
}
void heap_push(VkCommandBuffer cb, const void* data, uint32_t bytes) {
    VkPushDataInfoEXT pi{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
    pi.offset = 0;
    pi.data = {data, bytes};
    vkCmdPushDataEXT(cb, &pi);
}

void heap_texture_registered(int id, const nvn_tex* t, const nvn_view* v) {
    if (!ready || !t) return;
    if (id < 0 || (uint32_t)id >= kHeapSlots) return void(st_big_id.fetch_add(1, std::memory_order_relaxed));
    st_tex.fetch_add(1, std::memory_order_relaxed);
    const int target = v && v->set_target ? v->target : t->d.target;
    const int nf = v && v->set_format ? v->format : t->d.format;
    answers[id].store((uint8_t)(texture_type(target) | (integer_format(vk_format(nf).format) ? 0x80 : 0)), std::memory_order_release);
    const Image* im = t->d.target == NVN_TEXTURE_TARGET_BUFFER ? nullptr : image_find(t);
    if (!im && t->d.target != NVN_TEXTURE_TARGET_BUFFER) st_no_image.fetch_add(1, std::memory_order_relaxed);
    if (im && im->placeholder) {   // (named: a registration that samples the placeholder)
        static std::atomic<int> told{0};
        if (told++ < 40) {
            char why[256];
            image_key_describe(t, why, sizeof why);
            logf("direct: heap: id %d registered with the placeholder: texture %#llx %ux%u NVN format %#x target %d; %s", id,
                 (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.format, t->d.target, why);
        }
    }
    const uint32_t allowed = im ? regions_of(target) : 0;
    std::lock_guard lk(heap_mu);
    for (uint32_t r = 0; r < kHeapRegions; r++) {
        VkImageViewCreateInfo vi;
        const bool ok = (allowed >> r & 1) && sampled_view(im, t, v, (VkImageViewType)r, vi);
        uint8_t d[kImageDesc];
        if (!encode_image(ok ? &vi : nullptr, d)) {
            st_failed.fetch_add(1, std::memory_order_relaxed);
            encode_image(nullptr, d);
        }
        std::memcpy(res_heap.map + heap_region_offset(r) + (uint64_t)id * kImageDesc, d, kImageDesc);
        (ok ? st_entries : st_nulls).fetch_add(1, std::memory_order_relaxed);
        // (M3b: the storage entry of the same view, where the image can be stored to; zeros otherwise)
        const bool sok = ok && storage_view(im, vi);
        if (!encode_image(sok ? &vi : nullptr, d, true)) {
            st_failed.fetch_add(1, std::memory_order_relaxed);
            encode_image(nullptr, d);
        }
        std::memcpy(res_heap.map + heap_storage_offset(r) + (uint64_t)id * kImageDesc, d, kImageDesc);
        if (sok) st_storage.fetch_add(1, std::memory_order_relaxed);
    }
    if (slot_image[id] != im) {   // (M6f: the heap's holds of an image, for one its texture let go of: image_sweep)
        if (slot_image[id]) slot_image[id]->heap_refs.fetch_sub(1, std::memory_order_relaxed);
        if (im) im->heap_refs.fetch_add(1, std::memory_order_relaxed);
    }
    slot_image[id] = im;
    slot_tex[id] = {t->gpu, t->d.width, t->d.height, t->d.format, t->d.target, t->d.depth, t->d.levels, v ? *v : nvn_view{}, v != nullptr};
}

nvn_samp_par slot_samp[kHeapSamplers];   // (M4h, logs: what each sampler id was registered with)
void heap_sampler_registered(int id, const nvn_samp_par* p) {
    if (!ready || !p) return;
    if (id < 0 || (uint32_t)id >= kHeapSamplers) return void(st_big_id.fetch_add(1, std::memory_order_relaxed));
    {
        std::lock_guard lk(heap_mu);
        slot_samp[id] = *p;
    }
    uint8_t d[2][kSamplerDesc];
    if (!encode_sampler(*p, false, d[0]) || !encode_sampler(*p, true, d[1])) return void(st_failed.fetch_add(1, std::memory_order_relaxed));
    std::lock_guard lk(heap_mu);
    std::memcpy(smp_heap.map + heap_sampler_offset(false) + (uint64_t)id * kSamplerDesc, d[0], kSamplerDesc);
    std::memcpy(smp_heap.map + heap_sampler_offset(true) + (uint64_t)id * kSamplerDesc, d[1], kSamplerDesc);
    st_samp.fetch_add(1, std::memory_order_relaxed);
}

void heap_image_gone(const Image* im) {
    if (!ready || !im) return;
    std::lock_guard lk(heap_mu);
    for (uint32_t id = 0; id < kHeapSlots; id++) {
        if (slot_image[id] != im) continue;
        for (uint32_t r = 0; r < kHeapRegions; r++) {
            std::memset(res_heap.map + heap_region_offset(r) + (uint64_t)id * kImageDesc, 0, kImageDesc);
            std::memset(res_heap.map + heap_storage_offset(r) + (uint64_t)id * kImageDesc, 0, kImageDesc);
        }
        slot_image[id] = nullptr;
        im->heap_refs.fetch_sub(1, std::memory_order_relaxed);
        st_gone.fetch_add(1, std::memory_order_relaxed);
    }
}

uint8_t heap_answer(uint32_t id) {
    return ready && id < kHeapSlots ? answers[id].load(std::memory_order_acquire) : 0xFF;
}

void heap_describe(uint32_t id, char* out, size_t cap) {
    if (!ready || id >= kHeapSlots) return void(std::snprintf(out, cap, "-"));
    std::lock_guard lk(heap_mu);
    const Image* im = slot_image[id];
    const SlotTex& st = slot_tex[id];
    int k = std::snprintf(out, cap, "id %u: texture %#llx %ux%ux%u L%u NVN format %#x target %d", id, (unsigned long long)st.gpu, st.w, st.h,
                          st.depth, st.levels, st.format, st.target);
    if (st.has_view)   // (M4h: the view's levels, layers, format and target where it sets them)
        k += std::snprintf(out + k, cap - k, " view%s%u+%u%s%u+%u%s%#x%s%d", st.view.set_levels ? " levels " : " -", st.view.base_level, st.view.nlevels,
                           st.view.set_layers ? " layers " : " -", st.view.min_layer, st.view.nlayers, st.view.set_format ? " format " : " -",
                           st.view.format, st.view.set_target ? " target " : " -", st.view.target);
    k += std::snprintf(out + k, cap - k, ", ");
    const unsigned w = im ? im->written.load(std::memory_order_relaxed) : 0;   // (M3b: what has written it: W_* bits)
    k += std::snprintf(out + k, cap - k, "image #%u %ux%u x%u L%u vk %d%s, written %s%s%s%s%s%s%s%s, entries", im ? im->id : 0, im ? im->width : 0,
                       im ? im->height : 0, im ? std::max(im->layers, im->depth) : 0, im ? im->levels : 0, im ? (int)im->format : 0, im && im->placeholder ? " (placeholder)" : "", w ? "" : "never",
                       w & W_ATTACHMENT ? "A" : "", w & W_COPY ? "C" : "", w & W_TEXELS ? "T" : "", w & W_CLEAR ? "L" : "",
                       w & W_STORE ? "S" : "", w & W_TERRAIN ? "R" : "", w & W_GLYPH ? "G" : "");
    if (im && im->asset) {   // (M4f: a prepared texture's name, for the per-draw trace)
        uint32_t n = 0;
        const char* name = assets_name(im->asset, &n);
        k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " '%.*s' map %u%u%u%u", (int)std::min<uint32_t>(n, 60), name,
                           im->chan_map[0], im->chan_map[1], im->chan_map[2], im->chan_map[3]);
    }
    for (uint32_t r = 0; r < kHeapRegions; r++) {
        const uint8_t* e = res_heap.map + heap_region_offset(r) + (uint64_t)id * kImageDesc;
        bool any = false;
        for (uint32_t i = 0; i < kImageDesc; i++) any |= e[i] != 0;
        k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %u:%s", r, any ? "set" : "0");
    }
}

void heap_sampler_describe(uint32_t id, char* out, size_t cap) {
    if (!ready || id >= kHeapSamplers) return void(std::snprintf(out, cap, "-"));
    nvn_samp_par p;
    {
        std::lock_guard lk(heap_mu);
        p = slot_samp[id];
    }
    std::snprintf(out, cap, "sampler %u: min %u mag %u wrap %u/%u/%u cmp %u/%u red %u lod %.2f..%.2f bias %.2f aniso %.0f border %.2f,%.2f,%.2f,%.2f",
                  id, p.minf, p.magf, p.ws, p.wt, p.wr, p.cmp_mode, p.cmp_func, p.reduction, p.lod_min, p.lod_max, p.lod_bias, p.aniso,
                  p.border[0], p.border[1], p.border[2], p.border[3]);
}

int heap_placeholder(uint32_t id) {
    if (!ready || id >= kHeapSlots) return 0;
    std::lock_guard lk(heap_mu);
    const Image* im = slot_image[id];
    if (im) return im->placeholder ? 1 : 0;
    return slot_tex[id].gpu && slot_tex[id].target != NVN_TEXTURE_TARGET_BUFFER ? 2 : 0;
}

void heap_stats(char* out, size_t cap) {
    std::snprintf(out, cap, "heap (since the start): %lld texture registrations (%lld entries written, %lld null, %lld storage), %lld with no image, "
                  "%lld ids past the heap, %lld sampler registrations, %lld custom border colours as black, %lld ids nulled with their "
                  "image, %lld descriptor writes failed, %lld views read through a prepared image's channel map", (long long)st_tex.load(), (long long)st_entries.load(), (long long)st_nulls.load(), (long long)st_storage.load(),
                  (long long)st_no_image.load(), (long long)st_big_id.load(), (long long)st_samp.load(), (long long)st_border.load(),
                  (long long)st_gone.load(), (long long)st_failed.load(), (long long)st_remapped.load());
}

}  // namespace hsd
