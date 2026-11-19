// The pools' set and M2b's device part (vk_pool.h). The sampled view rules from gpu/legacy/vk/vk_texture.cpp texture_view
// and the sampler from gpu/legacy/vk/vk_draw.cpp sampler_make, without their switches and without the shadow filter's PCF
// (a picture change, not Vulkan's need: plan rule 8).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "vk_marks.h"
#include "vk_pstats.h"
#include "vk_check.h"
#include "shader_recompiler/shader_info.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"

#include "vk_pool.h"
#include "vk_image.h"
#include "vk_assets.h"

#define GPU_VK_DEFINE(f) PFN_##f f;
namespace hsd::vkfn {
GPU_VK_M2B(GPU_VK_DEFINE)
GPU_VK_M4B(GPU_VK_DEFINE)
}
#undef GPU_VK_DEFINE

namespace hsd {

DrawCaps caps;
void draw_window_table_init();   // (vk_draw.cpp)

namespace {

using namespace Shader::Backend::SPIRV;   // (the set's layout: kPool*)
constexpr uint32_t kSlots = kPoolSlots, kSamplers = kPoolSamplers, kViewTypes = kPoolViewTypes;
constexpr uint32_t kPushBytes = UNIFORM_TABLE_PUSH_OFFSET + 8 * UNIFORM_TABLE_SLOTS;   // (the push data a draw writes, at most)

// ---- the device ----
VkPhysicalDeviceShaderObjectFeaturesEXT shobj{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
VkPhysicalDeviceRobustness2FeaturesKHR robust2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR};
VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
VkPhysicalDeviceProvokingVertexFeaturesEXT pv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
// (M4b: mesh shading with the mesh stage only. The task stage is never enabled: native task-stage draws lose the device on this
// GPU (docs/direct/task-mesh.md); with the feature off no task shader object can be made at all)
VkPhysicalDeviceMeshShaderFeaturesEXT mesh_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
// (the translator's MaximallyReconvergesKHR, for the stages whose front asks for it)
VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR reconv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR};
bool has_ext[9]{};
enum { E_SHOBJ, E_ROBUST2, E_EDS3, E_PV, E_STENCIL, E_ATOMIC_FLOAT, E_M5, E_MESH, E_RECONV };

// ---- the set ----
VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
VkDescriptorSet set = VK_NULL_HANDLE;
VkPushConstantRange push_range{VK_SHADER_STAGE_ALL, 0, 0};
bool ready = false;

/// (UINT32_MAX: none)
uint32_t mapped_type(const VkMemoryRequirements& mr) {
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
    return type;
}

/// A buffer in device-local, host-visible memory (the BAR) where there is one, mapped, with its device address
bool make_mapped(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& map,
                 VkDeviceAddress& address, MemOwner owner) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev.device, buffer, &mr);
    const uint32_t type = mapped_type(mr);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fi;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev.device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    mem_account(owner, type, (int64_t)mr.size);
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


// ---- entries ----
std::mutex pool_mu;   // (registrations: the slots, the set's writes)
/// What a texture id's entries hold: its image, and per view type the sampled and the storage view written there
struct Slot {
    const Image* im = nullptr;
    VkImageView view[2][kPoolViewTypes]{};
};
std::unique_ptr<Slot[]> slots;
struct SlotTex {
    uint64_t gpu;
    uint32_t w, h;
    int format, target;
    uint32_t depth = 0, levels = 0;   // (logs: the texture's depth or layers and levels, and its view's)
    nvn_view view{};
    bool has_view = false;
};
std::unique_ptr<SlotTex[]> slot_tex;   // (logs: what was registered there)
std::unique_ptr<std::atomic<uint8_t>[]> answers;   // (texture id -> answer byte)

std::atomic<int64_t> st_storage{0}, st_tex{0}, st_same{0}, st_entries{0}, st_nulls{0}, st_big_id{0}, st_samp{0}, st_border{0},
    st_gone{0}, st_no_image{0}, st_failed{0}, st_remapped{0}, st_samplers_over{0};

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
    // (an ASTC texture's image is in the store's format for it, BC7 or BC5: vk_format's BC7 would read a BC5 image's blocks
    // as BC7, which the size check below lets through)
    else if (nvn_astc(nf)) f = image_astc_view_format(im, nf);
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
        static const bool unmapped = dbg("nochanmap");   // (a diagnosis: the game's swizzle alone, as before M4h)
        vi.components = unmapped ? game : image_swizzle(im, game);
        if (std::memcmp(&vi.components, &game, sizeof game)) st_remapped.fetch_add(1, std::memory_order_relaxed);
    }
    VkImageAspectFlags aspect = im->aspect;
    if (aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange = {aspect, vl, nlevels, layer0, nlayers};
    return true;
}

/// A storage view of t's image as view type `type` from its sampled view: one level, no swizzle, a format the device
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


// ---- samplers: one object per distinct create info, never destroyed ----
struct SamplerKey {
    VkSamplerCreateInfo ci;   // (pNext null: the reduction mode beside it)
    VkSamplerReductionMode reduction;
};
std::mutex sampler_mu;
std::vector<std::pair<SamplerKey, VkSampler>> samplers;
VkSampler default_sampler[2] = {};   // (linear, clamped: plain and compare; ids the game has not registered)

/// A sampler's create info (sampler_make's; compare: the depth-compare binding's, which a shader's depth-reference sample
/// needs whatever the game set: Vulkan leaves such a sample through a sampler that does not compare undefined)
SamplerKey sampler_key(const nvn_samp_par& p, bool compare) {
    SamplerKey k{};
    VkSamplerCreateInfo& ci = k.ci;
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
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
    else {   // (a custom colour needs VK_EXT_custom_border_color: not yet; counted)
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (wrap(p.ws) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || wrap(p.wt) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)
            st_border.fetch_add(1, std::memory_order_relaxed);
    }
    k.reduction = p.reduction && !compare ? (p.reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX)
                                          : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
    return k;
}


/// The sampler of key (made the first time; the default one past the device's count of samplers)
VkSampler sampler_of(const SamplerKey& k, bool compare) {
    std::lock_guard lk(sampler_mu);
    for (const auto& [key, s] : samplers)
        if (!std::memcmp(&key, &k, sizeof k)) return s;
    if (samplers.size() + 8 >= dev.props.limits.maxSamplerAllocationCount) {
        st_samplers_over.fetch_add(1, std::memory_order_relaxed);
        return default_sampler[compare];
    }
    VkSamplerCreateInfo ci = k.ci;
    VkSamplerReductionModeCreateInfo rm{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    if (k.reduction != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE) rm.reductionMode = k.reduction, ci.pNext = &rm;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(dev.device, &ci, nullptr, &s) != VK_SUCCESS) {
        st_failed.fetch_add(1, std::memory_order_relaxed);
        return default_sampler[compare];
    }
    object_made(1, "a sampler");
    samplers.push_back({k, s});
    return s;
}
size_t sampler_count() {
    std::lock_guard lk(sampler_mu);
    return samplers.size();
}

/// One descriptor written: an image view (null: a null descriptor) at binding, array element id
struct Writes {
    VkDescriptorImageInfo info[2 * kPoolViewTypes + 2];
    VkWriteDescriptorSet w[2 * kPoolViewTypes + 2];
    uint32_t n = 0;
    void add(uint32_t binding, uint32_t id, VkDescriptorType type, VkImageView view, VkSampler sampler = VK_NULL_HANDLE) {
        info[n] = {sampler, view, VK_IMAGE_LAYOUT_GENERAL};
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, id, 1, type, &info[n], nullptr, nullptr};
        n++;
    }
    void flush() {
        if (n) vkUpdateDescriptorSets(dev.device, n, w, 0, nullptr);
        n = 0;
    }
};

bool has(const std::vector<VkExtensionProperties>& exts, const char* n) {
    for (const auto& e : exts)
        if (!std::strcmp(e.extensionName, n)) return true;
    return false;
}

/// The set: its layout, the pipeline layout every shader object shares, the pool and the set, every entry null and every
/// sampler the default
const char* make_set() {
    VkDescriptorSetLayoutBinding b[kPoolBindings];
    VkDescriptorBindingFlags fl[kPoolBindings];
    for (uint32_t i = 0; i < kPoolBindings; i++) {
        const VkDescriptorType type = i < kPoolStorageBinding ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                    : i < kPoolSamplerBinding ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[i] = {i, type, type == VK_DESCRIPTOR_TYPE_SAMPLER ? kSamplers : kSlots, VK_SHADER_STAGE_ALL, nullptr};
        // (written while command buffers that bind the set are recorded and run: see pool_texture_registered)
        fl[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = kPoolBindings, bfi.pBindingFlags = fl;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bfi, li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = kPoolBindings, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set_layout) != VK_SUCCESS) return "the set's layout was not made";
    push_range.size = caps.max_push;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &set_layout, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(dev.device, &pli, nullptr, &pipe_layout) != VK_SUCCESS) return "the pipeline layout was not made";
    const VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kViewTypes * kSlots},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kViewTypes * kSlots},
                                        {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * kSamplers}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, pi.maxSets = 1, pi.poolSizeCount = 3, pi.pPoolSizes = ps;
    VkDescriptorPool pool;
    if (vkCreateDescriptorPool(dev.device, &pi, nullptr, &pool) != VK_SUCCESS) return "the descriptor pool was not made";
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &set_layout};
    if (vkAllocateDescriptorSets(dev.device, &ai, &set) != VK_SUCCESS) return "the set was not allocated";
    nvn_samp_par p{};   // (the default sampler: linear, clamped)
    p.minf = NVN_MIN_FILTER_LINEAR, p.magf = NVN_MAG_FILTER_LINEAR, p.lod_max = 1000.0f;
    for (const bool cmp : {false, true}) {
        const SamplerKey k = sampler_key(p, cmp);
        if (vkCreateSampler(dev.device, &k.ci, nullptr, &default_sampler[cmp]) != VK_SUCCESS) return "the default sampler was not made";
    }
    std::vector<VkDescriptorImageInfo> infos(kSlots, VkDescriptorImageInfo{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL});
    std::vector<VkDescriptorImageInfo> sinfos(2 * kSamplers);
    for (uint32_t i = 0; i < 2 * kSamplers; i++) sinfos[i].sampler = default_sampler[i >= kSamplers];
    std::vector<VkWriteDescriptorSet> w(kPoolBindings);
    for (uint32_t i = 0; i < kPoolBindings; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, b[i].descriptorCount, b[i].descriptorType, infos.data(), nullptr, nullptr};
        if (b[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) w[i].pImageInfo = sinfos.data() + (i - kPoolSamplerBinding) * kSamplers;
    }
    vkUpdateDescriptorSets(dev.device, kPoolBindings, w.data(), 0, nullptr);
    return nullptr;
}

}  // namespace

bool mapped_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& map,
                   VkDeviceAddress& address, MemOwner owner) {
    return make_mapped(size, usage, buffer, memory, map, address, owner);
}
void mapped_buffer_free(VkBuffer buffer, VkDeviceMemory memory, MemOwner owner) {
    VkMemoryRequirements mr;   // (what make_mapped counted: the same question gives the same size and type)
    vkGetBufferMemoryRequirements(dev.device, buffer, &mr);
    mem_account(owner, mapped_type(mr), -(int64_t)mr.size);
    vkDestroyBuffer(dev.device, buffer, nullptr), vkFreeMemory(dev.device, memory, nullptr);
}

// ---- the device ----
void draw_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail) {
    const struct { int k; const char* name; } list[] = {
        {E_SHOBJ, VK_EXT_SHADER_OBJECT_EXTENSION_NAME}, {E_ROBUST2, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME},
        {E_EDS3, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME}, {E_PV, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME},
        {E_STENCIL, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME}, {E_ATOMIC_FLOAT, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME},
        {E_M5, VK_KHR_MAINTENANCE_5_EXTENSION_NAME}, {E_MESH, VK_EXT_MESH_SHADER_EXTENSION_NAME},
        {E_RECONV, VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME}};
    for (const auto& e : list)
        if ((has_ext[e.k] = has(exts, e.name))) want.push_back(e.name);
    if (!has_ext[E_ROBUST2] && has(exts, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME))   // (the same structure, before KHR)
        has_ext[E_ROBUST2] = true, want.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    if (has_ext[E_SHOBJ]) *tail = &shobj, tail = &shobj.pNext;
    if (has_ext[E_ROBUST2]) *tail = &robust2, tail = &robust2.pNext;
    if (has_ext[E_EDS3]) *tail = &eds3, tail = &eds3.pNext;
    if (has_ext[E_PV]) *tail = &pv, tail = &pv.pNext;
    if (has_ext[E_ATOMIC_FLOAT]) *tail = &af, tail = &af.pNext;
    if (has_ext[E_MESH]) *tail = &mesh_feat, tail = &mesh_feat.pNext;
    if (has_ext[E_RECONV]) *tail = &reconv, tail = &reconv.pNext;
}

void draw_features_resolved(const VkPhysicalDeviceFeatures& f10, VkPhysicalDeviceVulkan12Features& f12,
                            const VkPhysicalDeviceVulkan14Features* f14) {
    mesh_feat.taskShader = VK_FALSE;   // (never: see mesh_feat)
    // (M6h: HOS_DBG=pstats counts mesh invocations too, when the device can)
    const VkBool32 mesh_queries = mesh_feat.meshShaderQueries && pstats_wants_mesh_queries();
    mesh_feat.multiviewMeshShader = mesh_feat.primitiveFragmentShadingRateMeshShader = mesh_feat.meshShaderQueries = VK_FALSE;
    mesh_feat.meshShaderQueries = mesh_queries;
    caps.mesh_queries = mesh_queries;
    caps.mesh = has_ext[E_MESH] && mesh_feat.meshShader;
    if (!caps.mesh) mesh_feat.meshShader = VK_FALSE;
    caps.maximal_reconvergence = has_ext[E_RECONV] && reconv.shaderMaximalReconvergence;
    caps.index_uint8 = f14 && f14->indexTypeUint8;
    caps.vertex_divisor = f14 && f14->vertexAttributeInstanceRateDivisor;
    pv.transformFeedbackPreservesProvokingVertex = VK_FALSE;
    caps.provoking_vertex = has_ext[E_PV] && pv.provokingVertexLast;
    robust2.robustBufferAccess2 = robust2.robustImageAccess2 = VK_FALSE;   // (null descriptors only: robust access costs)
    const bool eds3_ok = has_ext[E_EDS3] && eds3.extendedDynamicState3PolygonMode && eds3.extendedDynamicState3DepthClampEnable &&
                         eds3.extendedDynamicState3ColorBlendEnable && eds3.extendedDynamicState3ColorBlendEquation &&
                         eds3.extendedDynamicState3ColorWriteMask && eds3.extendedDynamicState3AlphaToCoverageEnable &&
                         eds3.extendedDynamicState3RasterizationSamples && eds3.extendedDynamicState3SampleMask &&
                         eds3.extendedDynamicState3AlphaToOneEnable && eds3.extendedDynamicState3LogicOpEnable &&
                         (!caps.provoking_vertex || eds3.extendedDynamicState3ProvokingVertexMode);
    // (the pools' set: descriptor indexing with update after bind, and its sizes)
    const bool indexing = f12.runtimeDescriptorArray && f12.shaderSampledImageArrayNonUniformIndexing &&
                          f12.shaderStorageImageArrayNonUniformIndexing && f12.descriptorBindingSampledImageUpdateAfterBind &&
                          f12.descriptorBindingStorageImageUpdateAfterBind && f12.descriptorBindingUpdateUnusedWhilePending;
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &p12;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    const uint32_t images = kViewTypes * kSlots;
    const bool sizes = p12.maxDescriptorSetUpdateAfterBindSampledImages >= images && p12.maxDescriptorSetUpdateAfterBindStorageImages >= images &&
                       p12.maxDescriptorSetUpdateAfterBindSamplers >= 2 * kSamplers &&
                       p12.maxPerStageDescriptorUpdateAfterBindSampledImages >= images &&
                       p12.maxPerStageDescriptorUpdateAfterBindStorageImages >= images &&
                       p12.maxPerStageDescriptorUpdateAfterBindSamplers >= 2 * kSamplers &&
                       p12.maxPerStageUpdateAfterBindResources >= 2 * images + 2 * kSamplers &&
                       p12.maxUpdateAfterBindDescriptorsInAllPools >= 2 * images + 2 * kSamplers;
    caps.max_push = std::min<uint32_t>(dev.props.limits.maxPushConstantsSize, 256);
    if (!has_ext[E_SHOBJ] || !shobj.shaderObject) caps.why = "no VK_EXT_shader_object";
    else if (!indexing) caps.why = "no descriptor indexing with update after bind (runtime arrays, non-uniform image indexing, sampled "
                                   "and storage images updated after bind and while pending)";
    else if (!sizes) caps.why = "update-after-bind limits below the pools' set (7 x 65536 sampled and storage images, 8192 samplers)";
    else if (!has_ext[E_ROBUST2] || !robust2.nullDescriptor) caps.why = "no null descriptors (VK_KHR_robustness2 / VK_EXT_robustness2)";
    else if (!f12.bufferDeviceAddress || !f10.shaderInt64) caps.why = "no buffer device addresses or 64-bit integers in shaders";
    else if (caps.max_push < kPushBytes) caps.why = "push constants below 144 bytes";
    else if (!eds3_ok) caps.why = "not all of extended dynamic state 3";
    else caps.ok = true, caps.why = "";
}

void draw_device_created() {
    auto fail = [](const char* why) {
        caps.ok = false, caps.why = why;
        logf("direct: no draws: %s", why);
    };
    caps.alpha_to_one = dev.features.alphaToOne, caps.logic_op = dev.features.logicOp;   // (enabled: their state must be set)
#define GPU_VK_LOAD(f) f = (PFN_##f)vkGetDeviceProcAddr(dev.device, #f);
    GPU_VK_M2B(GPU_VK_LOAD)
    GPU_VK_M4B(GPU_VK_LOAD)
#undef GPU_VK_LOAD
    bool all = true;
#define GPU_VK_CHECK(f) all = all && f != nullptr;
    GPU_VK_M2B(GPU_VK_CHECK)
#undef GPU_VK_CHECK
    if (!all) return fail("a device function is missing");
    {   // (NVN's programs are written for 32 lanes: the stages the GPU can run at 32 are pinned there, gpu/legacy/vk/vk_main.cpp)
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceSubgroupSizeControlProperties sc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        sc.pNext = &drv;
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        sg.pNext = &sc;
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sg;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        caps.sg32_stages = sc.minSubgroupSize <= 32 && sc.maxSubgroupSize >= 32 ? sc.requiredSubgroupSizeStages : 0;
        caps.subgroup_size = sg.subgroupSize;
        caps.subgroup_max = std::max(sc.maxSubgroupSize, sg.subgroupSize);
        caps.driver_id = drv.driverID;
    }
    if (const char* why = make_set()) return fail(why);
    slots.reset(new Slot[kSlots]());
    slot_tex.reset(new SlotTex[kSlots]());
    answers.reset(new std::atomic<uint8_t>[kSlots]);
    for (uint32_t i = 0; i < kSlots; i++) answers[i].store(0xFF, std::memory_order_relaxed);
    ready = true;
    draw_window_table_init();
    logf("direct: draws on: shader objects, the pools' set (%u ids x %u view types, sampled and storage; %u samplers x 2; update "
         "after bind), push constants %u B, subgroups %u (up to %u, 32 on stages %#x)", kSlots, kViewTypes, kSamplers, caps.max_push,
         caps.subgroup_size, caps.subgroup_max, (unsigned)caps.sg32_stages);
}

// ---- the set ----
bool pool_on() { return ready; }

void pool_bind(VkCommandBuffer cb, VkPipelineBindPoint point) {
    vkCmdBindDescriptorSets(cb, point, pipe_layout, 0, 1, &set, 0, nullptr);
}
void pool_push(VkCommandBuffer cb, const void* data, uint32_t bytes) {
    vkCmdPushConstants(cb, pipe_layout, VK_SHADER_STAGE_ALL, 0, bytes, data);
}
void pool_shader_layout(VkShaderCreateInfoEXT& ci) {
    ci.setLayoutCount = 1, ci.pSetLayouts = &set_layout;
    ci.pushConstantRangeCount = 1, ci.pPushConstantRanges = &push_range;
}

void pool_texture_registered(int id, const nvn_tex* t, const nvn_view* v) {
    if (!ready || !t) return;
    if (id < 0 || (uint32_t)id >= kPoolNullSlot) return void(st_big_id.fetch_add(1, std::memory_order_relaxed));
    st_tex.fetch_add(1, std::memory_order_relaxed);
    const int target = v && v->set_target ? v->target : t->d.target;
    const int nf = v && v->set_format ? v->format : t->d.format;
    answers[id].store((uint8_t)(texture_type(target) | (integer_format(vk_format(nf).format) ? 0x80 : 0)), std::memory_order_release);
    Image* im = t->d.target == NVN_TEXTURE_TARGET_BUFFER ? nullptr : image_find(t);
    if (!im && t->d.target != NVN_TEXTURE_TARGET_BUFFER) st_no_image.fetch_add(1, std::memory_order_relaxed);
    if (im && im->placeholder) {   // (named: a registration that samples the placeholder)
        static std::atomic<int> told{0};
        if (told++ < 40) {
            char why[256];
            image_key_describe(t, why, sizeof why);
            logf("direct: pools: id %d registered with the placeholder: texture %#llx %ux%u NVN format %#x target %d; %s", id,
                 (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.format, t->d.target, why);
        }
    }
    // (the views each view type's entries take, from the image's cache: made once per image and view)
    const uint32_t allowed = im ? regions_of(target) : 0;
    Slot want{im, {}};
    for (uint32_t r = 0; r < kViewTypes; r++) {
        VkImageViewCreateInfo vi;
        if (!(allowed >> r & 1) || !sampled_view(im, t, v, (VkImageViewType)r, vi)) continue;
        if (!(want.view[0][r] = image_pool_view(im, vi, VK_IMAGE_USAGE_SAMPLED_BIT))) st_failed.fetch_add(1, std::memory_order_relaxed);
        if (storage_view(im, vi) && !(want.view[1][r] = image_pool_view(im, vi, VK_IMAGE_USAGE_STORAGE_BIT)))
            st_failed.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard lk(pool_mu);
    slot_tex[id] = {t->gpu, t->d.width, t->d.height, t->d.format, t->d.target, t->d.depth, t->d.levels, v ? *v : nvn_view{}, v != nullptr};
    Slot& s = slots[id];
    if (!std::memcmp(&s, &want, sizeof s)) return void(st_same.fetch_add(1, std::memory_order_relaxed));   // (what it holds)
    // (Update after bind: the set is bound in command buffers being recorded and running while these entries change. Vulkan
    // allows it for an entry no pending work reads (UPDATE_UNUSED_WHILE_PENDING); one that pending work does read is
    // undefined while it changes. NVN's pool has that contract too: the game registers another texture at an id once its
    // work no longer reads the id, and a registration of what an id holds writes nothing)
    Writes w;
    for (uint32_t r = 0; r < kViewTypes; r++)
        for (int k = 0; k < 2; k++) {
            if (s.view[k][r] == want.view[k][r]) continue;
            w.add((k ? kPoolStorageBinding : kPoolSampledBinding) + r, (uint32_t)id,
                  k ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, want.view[k][r]);
            (want.view[k][r] ? k ? st_storage : st_entries : st_nulls).fetch_add(1, std::memory_order_relaxed);
        }
    w.flush();
    s = want;
}

nvn_samp_par slot_samp[kPoolSamplers];   // (logs: what each sampler id was registered with)
void pool_sampler_registered(int id, const nvn_samp_par* p) {
    if (!ready || !p) return;
    if (id < 0 || (uint32_t)id >= kSamplers) return void(st_big_id.fetch_add(1, std::memory_order_relaxed));
    const VkSampler plain = sampler_of(sampler_key(*p, false), false), compare = sampler_of(sampler_key(*p, true), true);
    std::lock_guard lk(pool_mu);   // (update after bind, as a texture's: pool_texture_registered)
    slot_samp[id] = *p;
    Writes w;
    w.add(kPoolSamplerBinding, (uint32_t)id, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, plain);
    w.add(kPoolCompareSamplerBinding, (uint32_t)id, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, compare);
    w.flush();
    st_samp.fetch_add(1, std::memory_order_relaxed);
}

void pool_image_gone(const Image* im) {
    if (!ready || !im) return;
    std::lock_guard lk(pool_mu);
    for (uint32_t id = 0; id < kSlots; id++) {
        Slot& s = slots[id];
        if (s.im != im) continue;
        Writes w;
        for (uint32_t r = 0; r < kViewTypes; r++)
            for (int k = 0; k < 2; k++)
                if (s.view[k][r]) w.add((k ? kPoolStorageBinding : kPoolSampledBinding) + r, id,
                                        k ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_NULL_HANDLE);
        w.flush();
        s = Slot{};
        st_gone.fetch_add(1, std::memory_order_relaxed);
    }
}

uint8_t pool_answer(uint32_t id) {
    return ready && id < kSlots ? answers[id].load(std::memory_order_acquire) : 0xFF;
}

void pool_describe(uint32_t id, char* out, size_t cap) {
    if (!ready || id >= kSlots) return void(std::snprintf(out, cap, "-"));
    std::lock_guard lk(pool_mu);
    const Image* im = slots[id].im;
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
    for (uint32_t r = 0; r < kViewTypes; r++)
        k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %u:%s%s", r, slots[id].view[0][r] ? "set" : "0", slots[id].view[1][r] ? "+st" : "");
}

void pool_sampler_describe(uint32_t id, char* out, size_t cap) {
    if (!ready || id >= kSamplers) return void(std::snprintf(out, cap, "-"));
    nvn_samp_par p;
    {
        std::lock_guard lk(pool_mu);
        p = slot_samp[id];
    }
    std::snprintf(out, cap, "sampler %u: min %u mag %u wrap %u/%u/%u cmp %u/%u red %u lod %.2f..%.2f bias %.2f aniso %.0f border %.2f,%.2f,%.2f,%.2f",
                  id, p.minf, p.magf, p.ws, p.wt, p.wr, p.cmp_mode, p.cmp_func, p.reduction, p.lod_min, p.lod_max, p.lod_bias, p.aniso,
                  p.border[0], p.border[1], p.border[2], p.border[3]);
}

int pool_placeholder(uint32_t id) {
    if (!ready || id >= kSlots) return 0;
    std::lock_guard lk(pool_mu);
    const Image* im = slots[id].im;
    if (im) return im->placeholder ? 1 : 0;
    return slot_tex[id].gpu && slot_tex[id].target != NVN_TEXTURE_TARGET_BUFFER ? 2 : 0;
}

int pool_stale(uint32_t id) {
    if (!ready || id >= kSlots) return 0;
    std::lock_guard lk(pool_mu);
    return image_stale(slots[id].im) ? 1 : 0;
}

void pool_stale_census(char* out, size_t cap) {
    uint64_t ids = 0, shadow = 0;
    if (ready) {
        std::lock_guard lk(pool_mu);
        for (uint32_t id = 0; id < kSlots; id++)
            if (const Image* im = slots[id].im; image_stale(im)) ids++, shadow += im->width == im->height && is_depth_format(im->format);
    }
    std::snprintf(out, cap, "descriptor ids naming a transient target that is not its range's owner: %llu (%llu square depth maps); "
                  "they hold an image object each and no memory", (unsigned long long)ids, (unsigned long long)shadow);
}

void pool_stats(char* out, size_t cap) {
    std::snprintf(out, cap, "pools (since the start): %lld texture registrations (%lld holding what the id held: nothing written; %lld entries "
                  "written, %lld nulled, %lld storage), %lld with no image, %lld ids past the pool, %lld sampler registrations (%zu "
                  "distinct samplers, %lld past the device's count as linear), %lld custom border colours as black, %lld ids nulled "
                  "with their image, %lld views or samplers not made, %lld views read through a prepared image's channel map",
                  (long long)st_tex.load(), (long long)st_same.load(), (long long)st_entries.load(), (long long)st_nulls.load(),
                  (long long)st_storage.load(), (long long)st_no_image.load(), (long long)st_big_id.load(), (long long)st_samp.load(),
                  sampler_count(), (long long)st_samplers_over.load(), (long long)st_border.load(), (long long)st_gone.load(),
                  (long long)st_failed.load(), (long long)st_remapped.load());
}

}  // namespace hsd
