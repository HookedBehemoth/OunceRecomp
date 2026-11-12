// Images of NVN textures (direct_image.h). Lifted from gpu/vk/vk_texture.cpp (texture_key, create's usage and flags,
// format_class, linear_twin, widened) and gpu/vk/vk_xfer.cpp (resolve), without its cache's uploads, decoders, eviction,
// sparse images and pools. An image made is initialized at once on the making thread's upload: moved to GENERAL and,
// for an uncompressed one, cleared to zero (an asset's content is not this stage's: docs/direct/m2-interface.md, A-rt).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "direct_assets.h"
#include "direct_cmd.h"
#include "direct_image.h"

namespace hsd {
namespace {

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}
bool is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
VkFormat linear_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    default: return f;
    }
}
/// 3-component formats are no image formats here: the image holds 4
VkFormat widened(VkFormat f, bool* was) {
    *was = true;
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: *was = false; return f;
    }
}
/// Depth formats this GPU cannot render to: the 32-bit float ones (RADV has no D24 attachments)
VkFormat depth_twin(VkFormat f) {
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, f, &p);
    if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return f;
    switch (f) {
    case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D16_UNORM: return VK_FORMAT_D32_SFLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default: return f;
    }
}

struct Key {
    uint64_t gpu;
    uint32_t target, width, height, depth, levels, samples, layout, cls;
    bool operator==(const Key& o) const { return !std::memcmp(this, &o, sizeof o); }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        uint64_t h = mix(k.gpu, k.target);
        h = mix(h, (uint64_t)k.width << 32 | k.height);
        h = mix(h, (uint64_t)k.depth << 32 | k.levels);
        return (size_t)mix(h, (uint64_t)k.samples << 40 | (uint64_t)k.layout << 20 | k.cls);
    }
};

std::shared_mutex images_mu;   // (the table, each image's refs and death)
std::unordered_map<Key, std::unique_ptr<Image>, KeyHash> images;
std::vector<Image*> dying;   // (refs 0: destroyed by image_sweep)
Image placeholder;   // (one 1x1 RGBA8 image, for every texture that has none: copies and attachments skip it)
std::once_flag placeholder_once;
uint32_t next_id = 1;

// (counts: made, destroyed and placeholders since the last line; per reason since the start)
std::atomic<int64_t> st_made{0}, st_destroyed{0}, st_revived{0}, st_ph{0}, st_ph_astc{0}, st_ph_format{0},
    st_ph_refused{0}, st_buffer_tex{0}, st_registered_new{0}, st_texels{0}, st_texels_skipped{0}, st_bytes{0},
    st_made_total{0}, st_asset_images{0}, st_asset_bytes{0}, st_asset_shared{0}, st_asset_fill_waits{0}, st_asset_fail{0},
    st_ph_norec{0};

enum { K_NONE, K_PLAIN, K_ASSET, K_ASSET_MISS };
constexpr uint32_t kAssetClass = 0x80000u;
/// (A-rt) The key of an asset texture's image: its record's content and descriptor, so every texture of that content
/// (moved by compaction, or the same texture in another file) has the one image
void asset_key(const AssetDesc& d, Key& k) {
    k = Key{};
    k.gpu = d.key;
    k.target = (uint32_t)d.view;
    k.width = d.width, k.height = d.height, k.depth = d.layers, k.levels = d.mips, k.samples = 1;
    k.layout = (uint32_t)d.format;
    k.cls = kAssetClass;
}

/// The table's key of an NVN texture (as vk_texture.cpp texture_key): K_NONE a buffer texture or no storage; K_ASSET an
/// asset texture (d: its record's image); K_ASSET_MISS an asset texture with no record (the placeholder)
int key_of(const nvn_tex* t, Key& k, FormatInfo& fi, int& bw, int& bh, int& bpb, AssetRef* ar = nullptr, AssetDesc* ad = nullptr) {
    if (!t || !t->gpu || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return K_NONE;
    AssetRef a;
    if (assets_of(t, a)) {
        if (ar) *ar = a;
        AssetDesc d;
        if (!a.rec || !assets_desc(a.rec, d)) return K_ASSET_MISS;
        asset_key(d, k);
        fi = FormatInfo{d.format, false, (int)d.bw, (int)d.bh};
        bw = (int)d.bw, bh = (int)d.bh, bpb = (int)d.bpb;
        if (ad) *ad = d;
        return K_ASSET;
    }
    fi = vk_format(t->d.format);
    bw = 1, bh = 1;
    bpb = nvn_block_size(t->d.format, &bw, &bh);
    k = Key{};
    k.gpu = t->gpu;
    k.target = (uint32_t)t->d.target;
    if (k.target == NVN_TEXTURE_TARGET_CUBEMAP || k.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) k.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    if (k.target == NVN_TEXTURE_TARGET_RECTANGLE) k.target = NVN_TEXTURE_TARGET_2D;
    k.width = t->d.width, k.height = t->d.height;
    k.depth = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : t->d.depth;
    if (k.target == NVN_TEXTURE_TARGET_2D) k.depth = 1;
    k.levels = std::max(1u, t->d.levels);
    k.samples = (uint32_t)std::max(1, t->d.samples);
    k.layout = nvn_tex_is_linear(&t->d) ? 0x80000000u | t->d.stride : t->L.tileH | t->L.tileD << 4;
    // (the class an image is shared by: a depth format; a compressed one; else the bytes a texel)
    k.cls = is_depth_format(fi.format) ? 0x10000u | (uint32_t)fi.format
          : is_bc(fi.format) ? 0x20000u | (uint32_t)linear_twin(fi.format)
                             : 0x40000u | (uint32_t)bpb;
    return K_PLAIN;
}

/// A new image initialized on this thread's upload: GENERAL, and zero where it can be cleared
void init_image(Image& im) {
    Upload u = upload_begin(0);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    b.subresourceRange = {im.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(u.cb, &d);
    if (im.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        const VkClearDepthStencilValue z{0.0f, 0};
        vkCmdClearDepthStencilImage(u.cb, im.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    } else if (!im.compressed) {
        const VkClearColorValue z{};
        vkCmdClearColorImage(u.cb, im.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    }
    upload_end(u);
}

void make_placeholder() {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {1, 1, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    placeholder.placeholder = true;
    placeholder.format = ci.format;
    if (!mem_image(ci, placeholder.image, placeholder.mem)) logf("direct: the placeholder image not made");
    else init_image(placeholder);
}

void name_placeholder(const nvn_tex* t, const char* why, std::atomic<int64_t>& counter) {
    st_ph.fetch_add(1, std::memory_order_relaxed);
    if (counter.fetch_add(1, std::memory_order_relaxed) < 6)
        logf("direct: texture %#llx %ux%ux%u, %u levels, NVN format %#x, target %d: the placeholder (%s)",
             (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.format, t->d.target, why);
}

/// The image of t, made if there is none (images_mu held exclusively); null: no image (counted)
Image* make(const nvn_tex* t, const Key& k, const FormatInfo& fi, int bw, int bh, int bpb) {
    std::call_once(placeholder_once, make_placeholder);
    if (fi.format == VK_FORMAT_UNDEFINED || bpb <= 0) return name_placeholder(t, "no Vulkan format", st_ph_format), &placeholder;
    if (fi.astc) return name_placeholder(t, "ASTC, which this GPU does not sample", st_ph_astc), &placeholder;
    auto x = std::make_unique<Image>();
    Image& im = *x;
    bool wide = false;
    VkFormat vf = widened(fi.format, &wide);
    const bool depth = is_depth_format(vf);
    if (depth) vf = depth_twin(vf);
    im.format = depth ? vf : linear_twin(vf);
    im.aspect = !depth ? VK_IMAGE_ASPECT_COLOR_BIT
              : vf == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
              : has_stencil(vf) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                : VK_IMAGE_ASPECT_DEPTH_BIT;
    im.compressed = is_bc(vf);
    im.gpu = t->gpu, im.nvn_format = t->d.format, im.bpb = bpb, im.bw = bw, im.bh = bh;
    const nvn_texdesc& d = t->d;
    im.width = std::max(1u, d.width), im.height = std::max(1u, d.height);
    switch (d.target) {
    case NVN_TEXTURE_TARGET_1D: im.type = VK_IMAGE_TYPE_1D, im.height = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: im.type = VK_IMAGE_TYPE_1D, im.layers = std::max(1u, d.height), im.height = 1; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: im.layers = std::max(1u, d.depth); break;
    case NVN_TEXTURE_TARGET_CUBEMAP: im.layers = 6; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: im.layers = std::max(6u, d.depth); break;
    case NVN_TEXTURE_TARGET_3D: im.type = VK_IMAGE_TYPE_3D, im.depth = std::max(1u, d.depth); break;
    default: break;
    }
    const uint32_t max_levels = 1 + (uint32_t)std::floor(std::log2((double)std::max({im.width, im.height, im.depth})));
    im.levels = nvn_tex_is_linear(&d) ? 1 : std::clamp<uint32_t>(d.levels, 1, max_levels);
    im.samples = (VkSampleCountFlagBits)std::clamp(d.samples, 1, 16);
    VkImageCreateFlags flags = 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (depth) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (!im.compressed) {
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (im.samples == VK_SAMPLE_COUNT_1_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    } else {
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // (sRGB and UNORM views)
    }
    if (im.type == VK_IMAGE_TYPE_2D && im.layers >= 6 && im.width == im.height && im.samples == VK_SAMPLE_COUNT_1_BIT)
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (im.type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // (what the driver takes: storage, then attachments, left out until it does)
    VkImageFormatProperties lim{};
    VkResult r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (VkImageUsageFlags drop : {0u, (uint32_t)VK_IMAGE_USAGE_STORAGE_BIT,
                                   (uint32_t)(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)}) {
        r = vkGetPhysicalDeviceImageFormatProperties(dev.physical, im.format, im.type, VK_IMAGE_TILING_OPTIMAL, usage & ~drop,
                                                     flags, &lim);
        if (r == VK_SUCCESS) {
            usage &= ~drop;
            break;
        }
    }
    if (r != VK_SUCCESS || im.width > lim.maxExtent.width || im.height > lim.maxExtent.height || im.depth > lim.maxExtent.depth ||
        im.layers > lim.maxArrayLayers || im.levels > lim.maxMipLevels || !(lim.sampleCounts & im.samples))
        return name_placeholder(t, "the driver refuses the image", st_ph_refused), &placeholder;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = im.type;
    ci.format = im.format;
    ci.extent = {im.width, im.height, im.depth};
    ci.mipLevels = im.levels;
    ci.arrayLayers = im.layers;
    ci.samples = im.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (!mem_image(ci, im.image, im.mem)) return name_placeholder(t, "no memory for it", st_ph_refused), &placeholder;
    init_image(im);
    im.id = next_id++;
    st_made.fetch_add(1, std::memory_order_relaxed);
    st_made_total.fetch_add(1, std::memory_order_relaxed);
    st_bytes.fetch_add((int64_t)mem_size(im.mem), std::memory_order_relaxed);
    static std::atomic<int> late{0};   // (images made in play, after the first 1200 presents: named)
    if (present_count() > 1200 && late++ < 20)
        logf("direct: image #%u made at present %llu: %#llx %ux%ux%u x%u, %u levels, NVN format %#x, target %d (%u live textures before)",
             im.id, (unsigned long long)present_count(), (unsigned long long)im.gpu, im.width, im.height, im.depth, im.layers,
             im.levels, im.nvn_format, d.target, (unsigned)images.size());
    Image* out = x.get();
    images.emplace(k, std::move(x));
    return out;
}

/// (A-rt) An asset texture's image, in its record's format (images_mu held exclusively); filled by the first texture
/// that takes it (fill 0 -> 1 there)
Image* make_asset(const nvn_tex* t, const Key& k, const AssetDesc& d, uint32_t rec) {
    std::call_once(placeholder_once, make_placeholder);
    auto x = std::make_unique<Image>();
    Image& im = *x;
    im.format = d.format;
    im.compressed = is_bc(d.format);
    im.gpu = t->gpu, im.nvn_format = t->d.format, im.bpb = (int)d.bpb, im.bw = (int)d.bw, im.bh = (int)d.bh;
    im.width = d.width, im.height = d.height, im.levels = d.mips, im.layers = d.layers;
    im.asset = rec, im.asset_view = d.view;
    std::memcpy(im.chan_map, d.map, 4);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // (sRGB and UNORM views)
    if ((d.view == VK_IMAGE_VIEW_TYPE_CUBE || d.view == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && d.layers % 6 == 0 && d.width == d.height)
        ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = d.format;
    ci.extent = {d.width, d.height, 1};
    ci.mipLevels = d.mips;
    ci.arrayLayers = d.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!mem_image(ci, im.image, im.mem)) return name_placeholder(t, "no memory for its prepared image", st_ph_refused), &placeholder;
    im.id = next_id++;
    st_made.fetch_add(1, std::memory_order_relaxed);
    st_made_total.fetch_add(1, std::memory_order_relaxed);
    st_bytes.fetch_add((int64_t)mem_size(im.mem), std::memory_order_relaxed);
    st_asset_images.fetch_add(1, std::memory_order_relaxed);
    st_asset_bytes.fetch_add((int64_t)mem_size(im.mem), std::memory_order_relaxed);
    Image* out = x.get();
    images.emplace(k, std::move(x));
    return out;
}

/// Finds or makes t's image (images_mu held exclusively)
Image* get(const nvn_tex* t) {
    Key k;
    FormatInfo fi;
    int bw, bh, bpb;
    AssetRef a;
    AssetDesc d;
    const int kind = key_of(t, k, fi, bw, bh, bpb, &a, &d);
    if (kind == K_NONE) return nullptr;
    if (kind == K_ASSET_MISS) return std::call_once(placeholder_once, make_placeholder), &placeholder;
    auto it = images.find(k);
    if (it != images.end()) {
        if (kind == K_ASSET) st_asset_shared.fetch_add(1, std::memory_order_relaxed);
        return it->second.get();
    }
    if (kind == K_ASSET) return make_asset(t, k, d, a.rec);
    return make(t, k, fi, bw, bh, bpb);
}

void destroy(Image* im) {
    if (im->asset) {
        st_asset_images.fetch_sub(1, std::memory_order_relaxed);
        st_asset_bytes.fetch_sub((int64_t)mem_size(im->mem), std::memory_order_relaxed);
    }
    for (auto& [key, v] : im->views)
        if (v) vkDestroyImageView(dev.device, v, nullptr);
    st_bytes.fetch_sub((int64_t)mem_size(im->mem), std::memory_order_relaxed);
    mem_image_free(im->image, im->mem);
    st_destroyed.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void image_texture_created(const nvn_tex* t) {
    if (t && t->d.target == NVN_TEXTURE_TARGET_BUFFER) return void(st_buffer_tex.fetch_add(1, std::memory_order_relaxed));
    AssetRef a;
    const bool asset = assets_created(t, a);   // (A-rt: the record its load named, kept for its serial)
    if (asset && !a.rec) {
        st_ph.fetch_add(1, std::memory_order_relaxed);
        st_ph_norec.fetch_add(1, std::memory_order_relaxed);
        assets_note_placeholder(t, a.why);
    }
    std::unique_lock lk(images_mu);
    Image* im = get(t);
    if (!im || im->placeholder) return;
    if (im->refs++ == 0 && im->dead_present) im->dead_present = im->dead_value = 0, st_revived.fetch_add(1, std::memory_order_relaxed);
    if (!im->asset) return;
    // (A-rt) the first texture of an asset image fills it, here, before the game can register it; another one that comes
    // while it is being filled waits for that (the same content in another file, on another loader thread)
    uint8_t s = 0;
    if (im->fill.compare_exchange_strong(s, 1)) {
        lk.unlock();
        const bool ok = assets_fill(*im, im->asset);
        if (ok) im->written.fetch_or(W_STORE, std::memory_order_relaxed);
        else st_asset_fail.fetch_add(1, std::memory_order_relaxed);
        im->fill.store(ok ? 2 : 3, std::memory_order_release);
        im->fill.notify_all();
        return;
    }
    if (s == 1) {
        lk.unlock();
        st_asset_fill_waits.fetch_add(1, std::memory_order_relaxed);
        while (im->fill.load(std::memory_order_acquire) == 1) im->fill.wait(1, std::memory_order_acquire);
    }
}

void image_texture_released(const nvn_tex* t) {
    Key k;
    FormatInfo fi;
    int bw, bh, bpb;
    const int kind = key_of(t, k, fi, bw, bh, bpb);
    if (kind == K_PLAIN || kind == K_ASSET) {
        std::unique_lock lk(images_mu);
        auto it = images.find(k);
        if (it != images.end()) {
            Image* im = it->second.get();
            if (im->refs > 0 && --im->refs == 0) {
                im->dead_present = present_count() + 1;
                dying.push_back(im);
            }
        }
    }
    if (kind == K_ASSET || kind == K_ASSET_MISS) assets_released(t);
}

void image_hold(Image* im) {
    if (!im || im->placeholder) return;
    std::unique_lock lk(images_mu);
    if (im->refs++ == 0 && im->dead_present) im->dead_present = im->dead_value = 0;
}
void image_unhold(Image* im) {
    if (!im || im->placeholder) return;
    std::unique_lock lk(images_mu);
    if (im->refs > 0 && --im->refs == 0) {
        im->dead_present = present_count() + 1;
        dying.push_back(im);
    }
}

VkComponentMapping image_swizzle(const Image* im, VkComponentMapping g) {
    if (!im || !im->asset) return g;
    auto one = [&](VkComponentSwizzle c, VkComponentSwizzle self) {
        if (c == VK_COMPONENT_SWIZZLE_IDENTITY) c = self;
        if (c >= VK_COMPONENT_SWIZZLE_R && c <= VK_COMPONENT_SWIZZLE_A) return (VkComponentSwizzle)im->chan_map[c - VK_COMPONENT_SWIZZLE_R];
        return c;
    };
    return {one(g.r, VK_COMPONENT_SWIZZLE_R), one(g.g, VK_COMPONENT_SWIZZLE_G), one(g.b, VK_COMPONENT_SWIZZLE_B),
            one(g.a, VK_COMPONENT_SWIZZLE_A)};
}

void image_texture_registered(const nvn_tex* t) {
    if (!t || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return;
    if (image_find(t)) return;
    std::unique_lock lk(images_mu);   // (registered without its initialization seen: made now)
    if (get(t)) st_registered_new.fetch_add(1, std::memory_order_relaxed);
}

Image* image_find(const nvn_tex* t) {
    Key k;
    FormatInfo fi;
    int bw, bh, bpb;
    const int kind = key_of(t, k, fi, bw, bh, bpb);
    if (kind == K_NONE) return nullptr;
    if (kind == K_ASSET_MISS) return &placeholder;
    std::shared_lock lk(images_mu);
    auto it = images.find(k);
    if (it != images.end()) return it->second.get();
    if (fi.format == VK_FORMAT_UNDEFINED || fi.astc || bpb <= 0) return &placeholder;
    return nullptr;
}

int image_view_format(const nvn_tex* t, const nvn_view* v) { return v && v->set_format ? v->format : t->d.format; }

VkImageView image_attachment_view(Image* im, int nvn_format, uint32_t level, uint32_t layer, VkFormat* format_out) {
    if (!im || im->placeholder || im->compressed || level >= im->levels) return VK_NULL_HANDLE;
    const bool depth = (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0;
    VkFormat f = im->format;
    if (!depth) {
        bool wide = false;
        const VkFormat vf = widened(vk_format(nvn_format).format, &wide);
        if (vf != VK_FORMAT_UNDEFINED && !is_depth_format(vf) && !is_bc(vf)) f = vf;
    }
    const uint32_t slices = im->type == VK_IMAGE_TYPE_3D ? std::max(1u, im->depth >> level) : im->layers;
    if (layer >= slices) return VK_NULL_HANDLE;
    if (format_out) *format_out = f;
    const uint64_t key = (uint64_t)f << 40 | (uint64_t)level << 32 | layer;
    std::lock_guard lk(im->views_mu);
    auto it = im->views.find(key);
    if (it != im->views.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageViewUsageCreateInfo use{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    use.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    vi.pNext = &use;
    vi.image = im->image;
    vi.viewType = im->type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {im->aspect, level, 1, layer, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vi, nullptr, &v) != VK_SUCCESS) {
        static std::atomic<int> told{0};
        if (told++ < 8)
            logf("direct: no attachment view of image #%u (vk %d) as vk %d level %u layer %u", im->id, (int)im->format, (int)f,
                 level, layer);
        v = VK_NULL_HANDLE;
    } else {
        st_objects.fetch_add(1, std::memory_order_relaxed);
    }
    im->views[key] = v;
    return v;
}

bool image_sub(const nvn_tex* t, const nvn_view* v, const NVNcopyRegion& r, Sub& s) {
    s.im = image_find(t);
    if (!s.im || s.im->placeholder) return false;
    Image& im = *s.im;
    s.aspect = im.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : im.aspect;
    s.level = v && v->set_levels ? v->base_level : 0;
    if (s.level >= im.levels) return false;
    const uint32_t lw = std::max(1u, im.width >> s.level), lh = std::max(1u, im.height >> s.level);
    const uint32_t ld = im.type == VK_IMAGE_TYPE_3D ? std::max(1u, im.depth >> s.level) : 1;
    s.bpb = (uint32_t)im.bpb, s.bw = im.bw, s.bh = im.bh;
    s.x = r.xoffset, s.y = r.yoffset;
    s.w = r.width > 0 ? (uint32_t)r.width : (s.x >= 0 && (uint32_t)s.x < lw ? lw - (uint32_t)s.x : 0);
    s.h = r.height > 0 ? (uint32_t)r.height : (s.y >= 0 && (uint32_t)s.y < lh ? lh - (uint32_t)s.y : 0);
    const uint64_t first = (uint64_t)(v && v->set_layers ? v->min_layer : 0) + (uint64_t)std::max(0, r.zoffset);
    const uint32_t count = (uint32_t)std::max(1, r.depth);
    if (im.type == VK_IMAGE_TYPE_3D) {
        s.z = (int32_t)std::max(0, r.zoffset), s.d = std::min(count, ld - std::min<uint32_t>(ld, (uint32_t)s.z));
        s.layer = 0, s.layers = 1;
    } else {
        s.z = 0, s.d = 1;
        s.layer = first < im.layers ? (uint32_t)first : 0;
        s.layers = first < im.layers ? (uint32_t)std::min<uint64_t>(count, im.layers - first) : 0;
    }
    s.w = std::min<uint32_t>(s.w, lw > (uint32_t)std::max(0, s.x) ? lw - (uint32_t)std::max(0, s.x) : 0);
    s.h = std::min<uint32_t>(s.h, lh > (uint32_t)std::max(0, s.y) ? lh - (uint32_t)std::max(0, s.y) : 0);
    return s.bpb && s.x >= 0 && s.y >= 0 && s.w && s.h && s.layers && s.d;
}

void image_write_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src,
                        uint64_t row, uint64_t img) {
    if (image_upload_region(t, level, layer0, r, src, row, img, W_TEXELS)) st_texels.fetch_add(1, std::memory_order_relaxed);
    else st_texels_skipped.fetch_add(1, std::memory_order_relaxed);
}

bool image_upload_region(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src,
                         uint64_t row, uint64_t img, uint8_t source) {
    const int64_t t0 = host->now_ns();
    nvn_view v{};
    v.set_levels = 1, v.base_level = (uint8_t)level, v.nlevels = 1;
    v.set_layers = 1, v.min_layer = (uint16_t)layer0, v.nlayers = 1;
    Sub s;
    bool wide = false;
    widened(vk_format(t->d.format).format, &wide);
    if (!r || !src || wide || !image_sub(t, &v, *r, s)) return false;
    const uint64_t wb = (s.w + s.bw - 1) / s.bw * (uint64_t)s.bpb, hb = (s.h + s.bh - 1) / s.bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    const uint64_t slices = (uint64_t)s.layers * s.d, packed = wb * hb * slices;
    if (row < wb || img < row * hb) return false;
    Upload u = upload_begin(packed);
    if (!u.map) {
        upload_end(u);
        return false;
    }
    {   // (the GPU's earlier reads of the image (the frames sent before) done before the copy writes it: a tile or glyph cell
        // written again)
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(u.cb, &d);
    }
    for (uint64_t k = 0; k < slices; k++)   // (packed rows: the staging copy is the region's blocks only)
        for (uint64_t y = 0; y < hb; y++) std::memcpy(u.map + (k * hb + y) * wb, src + k * img + y * row, wb);
    VkBufferImageCopy c{};
    c.bufferRowLength = (uint32_t)(wb / s.bpb) * (uint32_t)s.bw;
    c.bufferImageHeight = (uint32_t)hb * (uint32_t)s.bh;
    c.imageSubresource = {s.aspect, s.level, s.layer, s.layers};
    c.imageOffset = {s.x, s.y, s.z};
    c.imageExtent = {s.w, s.h, s.d};
    vkCmdCopyBufferToImage(u.cb, u.staging, s.im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    if (source != W_TEXELS) assets_check_region(s.im, source, s.level, s.layer, s.layers, s.x, s.y, s.w, s.h, u.map, packed);
    upload_end(u);
    s.im->written.fetch_or(source, std::memory_order_relaxed);
    assets_note_upload(source, packed, host->now_ns() - t0);
    return true;
}

void image_sweep(uint64_t present) {
    std::unique_lock lk(images_mu, std::try_to_lock);   // (a texture being made: the next present)
    if (!lk.owns_lock()) return;
    for (size_t i = 0; i < dying.size();) {
        Image* im = dying[i];
        bool gone = false;
        if (im->refs > 0 || !im->dead_present) {
            dying[i] = dying.back(), dying.pop_back();   // (revived)
            continue;
        }
        if (present >= im->dead_present + 8) {   // (8 presents for the lists that name it to be sent, then the GPU done)
            if (!im->dead_value) im->dead_value = sync_pushed();
            else if (sync_completed() >= im->dead_value) gone = true;
        }
        if (!gone) {
            i++;
            continue;
        }
        dying[i] = dying.back(), dying.pop_back();
        for (auto it = images.begin(); it != images.end(); ++it)
            if (it->second.get() == im) {
                destroy(im);
                images.erase(it);
                break;
            }
    }
}

void image_stats(char* out, size_t cap) {
    int64_t live = 0, color = 0, depth = 0, bc = 0, rendered = 0, copied = 0, texels = 0, store = 0, terrain = 0, glyph = 0,
            untouched = 0, assets = 0, unfilled = 0, no_content = 0;
    {
        std::shared_lock lk(images_mu);
        for (const auto& [k, im] : images) {
            live++;
            (im->compressed ? bc : (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? color : depth)++;
            if (im->asset) assets++, unfilled += im->fill.load(std::memory_order_relaxed) != 2;
            no_content += im->no_content;
            const uint8_t w = im->written.load(std::memory_order_relaxed);
            if (w & W_ATTACHMENT) rendered++;
            else if (w & (W_COPY | W_CLEAR)) copied++;
            else if (w & W_STORE) store++;
            else if (w & W_TERRAIN) terrain++;
            else if (w & W_GLYPH) glyph++;
            else if (w & W_TEXELS) texels++;
            else untouched++;
        }
    }
    std::snprintf(out, cap, "images: %lld made since the last line (%lld since the start), %lld destroyed, %lld revived; %lld "
                  "live (%lld colour, %lld depth, %lld compressed; by their first source: %lld rendered, %lld copied or cleared, %lld "
                  "from the store, %lld terrain uploads, %lld glyph uploads, %lld texel writes, %lld never written), %.0f MB (assets: "
                  "%lld images, %.0f MB, %lld not filled; %lld textures found their content's image made, %lld waited for its fill, "
                  "%lld fills failed); placeholders %lld (no prepared record %lld, ASTC %lld, no format %lld, refused %lld; texels "
                  "not given %lld), buffer textures %lld; made at registration %lld; texel writes %lld (%lld skipped)",
                  (long long)st_made.exchange(0), (long long)st_made_total.load(), (long long)st_destroyed.exchange(0),
                  (long long)st_revived.exchange(0), (long long)live, (long long)color, (long long)depth, (long long)bc,
                  (long long)rendered, (long long)copied, (long long)store, (long long)terrain, (long long)glyph, (long long)texels,
                  (long long)untouched, st_bytes.load() / 1048576.0, (long long)assets, st_asset_bytes.load() / 1048576.0,
                  (long long)unfilled, (long long)st_asset_shared.load(), (long long)st_asset_fill_waits.load(),
                  (long long)st_asset_fail.load(), (long long)st_ph.load(), (long long)st_ph_norec.load(), (long long)st_ph_astc.load(),
                  (long long)st_ph_format.load(), (long long)st_ph_refused.load(), (long long)no_content, (long long)st_buffer_tex.load(),
                  (long long)st_registered_new.load(), (long long)st_texels.load(), (long long)st_texels_skipped.load());
}

}  // namespace hsd
