// Images of NVN textures (vk_image.h). Lifted from gpu/legacy/vk/vk_texture.cpp (texture_key, create's usage and flags,
// format_class, linear_twin, widened) and gpu/legacy/vk/vk_xfer.cpp (resolve), without its cache's uploads, decoders, eviction,
// sparse images and pools. An image made is initialized at once on the making thread's upload: moved to GENERAL and,
// for an uncompressed one, cleared to zero (an asset's content is not set here).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

#include "vk_check.h"
#include "vk_assets.h"
#include "vk_cmd.h"
#include "vk_pool.h"
#include "vk_image.h"

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
// A dead image of its own memory whose content only the GPU wrote (rendered or copied) is held in `dying` instead of
// destroyed: the game initializes textures again over storage it does not write again (a terrain tile's normal map,
// drawn once, its textures made anew after a save loads), and the storage still holds that content. It goes when a new
// image overlaps its storage or is over kHeldBytes (images_mu).
constexpr int64_t kHeldBytes = 256ll << 20;
int64_t held_bytes = 0;
// The transient pool (vk_image.h, docs/direct/transient-pool.md): the range of storage addresses the host named
// (games/oot/patches/rt_slots.c's slots), its images, and the slot sizes the host told (images_mu; on, lo and hi are set once before
// the first texture in the range is initialized).
struct Pool {
    std::atomic<bool> on{false};
    uint64_t lo = 0, hi = 0;
    std::vector<Image*> images;
    std::unordered_map<uint64_t, uint64_t> caps;   // (storage address -> the bytes of its slot)
} pool;
constexpr size_t kPoolImages = 1024;   // (image objects kept: those no texture is initialized over go, the longest unclaimed first, past it)
constexpr uint64_t kPoolInUse = 2;     // (presents: a target claimed this recently may still be drawn or sampled by lists not sent)
std::atomic<int64_t> st_pool_made{0}, st_pool_claims{0}, st_pool_pruned{0}, st_pool_took_live{0}, st_pool_nofit{0},
    st_pool_given{0}, st_pool_given_claimed{0}, st_pool_stale_bound{0}, st_pool_made_total{0}, st_pool_claims_total{0};
void claim(Image* im, const char* how);   // (below)
Image placeholder;   // (one 1x1 RGBA8 image, for every texture that has none: copies and attachments skip it)
std::once_flag placeholder_once;
uint32_t next_id = 1;

// (counts: made, destroyed and placeholders since the last line; per reason since the start)
std::atomic<int64_t> st_made{0}, st_destroyed{0}, st_revived{0}, st_ph{0}, st_ph_format{0},
    st_ph_refused{0}, st_buffer_tex{0}, st_registered_new{0}, st_texels{0}, st_texels_skipped{0}, st_bytes{0},
    st_made_total{0}, st_asset_images{0}, st_asset_bytes{0}, st_asset_shared{0}, st_asset_fill_waits{0}, st_asset_fail{0},
    st_ph_norec{0};

enum { K_NONE, K_PLAIN, K_ASSET, K_ASSET_MISS, K_LINKED_PLAIN };
constexpr uint32_t kAssetClass = 0x80000u;
/// The key of an asset texture's image: its record's content and descriptor, so every texture of that content
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
    const bool linked = assets_of(t, a);
    if (linked && !a.plain) {
        if (ar) *ar = a;
        AssetDesc d;
        if (!a.rec || !assets_desc(a.rec, d)) return K_ASSET_MISS;
        asset_key(d, k);
        fi = FormatInfo{d.format, (int)d.bw, (int)d.bh};
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
    if (linked) {   // (A-rt: a texture loaded with no data: its own image wherever its storage moves)
        k.gpu = 0xC000000000000000ull | a.plain;
        if (ar) *ar = a;
        return K_LINKED_PLAIN;
    }
    return K_PLAIN;
}

/// An image initialized on this thread's upload: GENERAL from UNDEFINED (its content discarded), and zero where it can be cleared
void init_image(Image& im) {
    Upload u = upload_begin(0);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    // (an image in the transient pool: its memory was another image's; everything sent before is done with it first)
    b.srcStageMask = im.pooled ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE;
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

/// What image a texture of description d is: im's fields and its create info. 0, or why there is none (1 the driver
/// refuses it)
int shape(const nvn_texdesc& d, const FormatInfo& fi, Image& im, VkImageCreateInfo& ci) {
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
        // (sRGB and UNORM views; M4g: a compressed image not from the store has only what the GPU writes into it, and NVN renders
        // into it through a view of an uncompressed format of its block size (the game BC4-encodes its noise volumes so at the
        // load: f_809eb720's textures, program 53509044401d78d2): those views are attachments of its blocks)
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
        return 1;
    im.storage = (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    im.block_views = im.compressed && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ci = VkImageCreateInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = im.type;
    ci.format = im.format;
    ci.extent = {im.width, im.height, im.depth};
    ci.mipLevels = im.levels;
    ci.arrayLayers = im.layers;
    ci.samples = im.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    return 0;
}

/// An image of the transient pool: created and bound at its texture's storage offset (images_mu held exclusively).
/// false: it does not fit there (its slot's bytes as the host told them, the pool, the alignment): a strict event
bool pool_image(const VkImageCreateInfo& ci, Image& im, const nvn_tex* t) {
    if (vkCreateImage(dev.device, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev.device, im.image, &req);
    const uint64_t off = t->gpu - pool.lo;
    auto cap = pool.caps.find(t->gpu);
    const uint64_t room = cap != pool.caps.end() ? cap->second : 0;
    if (req.size > room || !mem_pool_bind(im.image, off, req)) {
        st_pool_nofit.fetch_add(1, std::memory_order_relaxed);
        strict_event("a transient target does not fit its slot", "texture %#llx %ux%ux%u, %u levels, NVN format %#x: the image "
                     "needs %llu bytes aligned to %llu, its slot at +%#llx has %llu (0: the host named none), the pool %llu",
                     (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.format,
                     (unsigned long long)req.size, (unsigned long long)req.alignment, (unsigned long long)off,
                     (unsigned long long)room, (unsigned long long)mem_pool_size());
        logf("direct: transient pool: texture %#llx %ux%u NVN format %#x needs %llu bytes (alignment %llu); its slot at +%#llx has "
             "%llu: not bound, the placeholder", (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.format,
             (unsigned long long)req.size, (unsigned long long)req.alignment, (unsigned long long)off, (unsigned long long)room);
        vkDestroyImage(dev.device, im.image, nullptr);
        im.image = VK_NULL_HANDLE;
        return false;
    }
    im.pooled = true, im.pool_off = off, im.pool_size = req.size;
    object_made(1, "an image");
    return true;
}

void destroy(Image* im);
/// a dead image's end (images_mu held exclusively; the GPU done with it): out of `dying` and the table, destroyed
void drop_dead(Image* im) {
    dying.erase(std::remove(dying.begin(), dying.end(), im), dying.end());
    if (im->held) held_bytes -= (int64_t)mem_size(im->mem), im->held = false;
    for (auto it = images.begin(); it != images.end(); ++it)
        if (it->second.get() == im) {
            destroy(im);
            images.erase(it);
            return;
        }
}
/// the held images whose storage overlaps lo..hi go
void drop_held_over(uint64_t lo, uint64_t hi) {
    for (size_t i = 0; i < dying.size();) {
        Image* o = dying[i];
        if (o->held && o->gpu < hi && lo < o->gpu + std::max<uint64_t>(o->span, 1)) drop_dead(o);
        else i++;
    }
}

/// The image of t, made if there is none (images_mu held exclusively); null: no image (counted)
Image* make(const nvn_tex* t, const Key& k, const FormatInfo& fi, int bw, int bh, int bpb) {
    std::call_once(placeholder_once, make_placeholder);
    if (fi.format == VK_FORMAT_UNDEFINED || bpb <= 0) return name_placeholder(t, "no Vulkan format", st_ph_format), &placeholder;
    auto x = std::make_unique<Image>();
    Image& im = *x;
    const nvn_texdesc& d = t->d;
    VkImageCreateInfo ci{};
    if (shape(d, fi, im, ci)) return name_placeholder(t, "the driver refuses the image", st_ph_refused), &placeholder;
    im.gpu = t->gpu, im.nvn_format = t->d.format, im.bpb = bpb, im.bw = bw, im.bh = bh;
    const VkImageUsageFlags usage = ci.usage;
    // (M6b: strict names an image the game's texture needs as made for a texture of the game (in play, a texture the game made in
    // play: a render target of a new size, a movie's frames))
    const ObjectsFor objects_for(usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
                                     ? "an image for a texture the game made (renderable)" : "an image for a texture the game made");
    // (a transient target: bound in the pool at its storage offset, no memory of its own; initialized by its claim)
    const bool pooled = pool.on.load(std::memory_order_acquire) && t->gpu >= pool.lo && t->gpu < pool.hi;
    if (pooled) {
        if (!pool_image(ci, im, t)) return name_placeholder(t, "no room at its offset in the transient pool", st_ph_refused), &placeholder;
        st_pool_made.fetch_add(1, std::memory_order_relaxed), st_pool_made_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (!mem_image(ci, im.image, im.mem, MO_IMAGE)) return name_placeholder(t, "no memory for it", st_ph_refused), &placeholder;
        init_image(im);
        im.span = t->L.size;
        drop_held_over(im.gpu, im.gpu + std::max<uint64_t>(im.span, 1));   // (that storage is this texture's now)
    }
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
    if (pooled) pool.images.push_back(out), claim(out, "made");
    return out;
}

/// An asset texture's image, in its record's format (images_mu held exclusively); filled by the first texture
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
    // (the store's ASTC: BC7 by the NVN format, or BC5 for a linear normal map; anything else is a store of another rule)
    if (nvn_astc(t->d.format) && linear_twin(d.format) != linear_twin(vk_format(t->d.format).format) &&
        !(d.format == VK_FORMAT_BC5_UNORM_BLOCK && t->d.format < 0x87))
        strict_event("a prepared ASTC texture in neither BC7 nor BC5", "texture %#llx %ux%u NVN format %#x: record %u is vk %d",
                     (unsigned long long)t->gpu, d.width, d.height, t->d.format, rec - 1, (int)d.format);
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
    const ObjectsFor objects_for("an image of a texture the game loaded (its prepared content)");   // 
    if (!mem_image(ci, im.image, im.mem, MO_IMAGE_STORE)) return name_placeholder(t, "no memory for its prepared image", st_ph_refused), &placeholder;
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
    Image* im = make(t, k, fi, bw, bh, bpb);
    if (kind == K_LINKED_PLAIN && im && !im->placeholder) im->no_data = true;
    return im;
}

void destroy(Image* im) {
    pool_image_gone(im);   // (its texture ids' entries nulled)
    if (im->asset) {
        st_asset_images.fetch_sub(1, std::memory_order_relaxed);
        st_asset_bytes.fetch_sub((int64_t)mem_size(im->mem), std::memory_order_relaxed);
    }
    for (auto& [key, v] : im->views)
        if (v) vkDestroyImageView(dev.device, v, nullptr);
    for (const Image::PoolView& v : im->pool_views)
        if (v.view) vkDestroyImageView(dev.device, v.view, nullptr);
    st_bytes.fetch_sub((int64_t)mem_size(im->mem), std::memory_order_relaxed);
    if (im->pooled) {   // (the pool keeps its memory)
        pool.images.erase(std::remove(pool.images.begin(), pool.images.end(), im), pool.images.end());
        vkDestroyImage(dev.device, im->image, nullptr);
        st_pool_pruned.fetch_add(1, std::memory_order_relaxed);
    } else {
        mem_image_free(im->image, im->mem);
    }
    st_destroyed.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void image_texture_created(const nvn_tex* t) {
    if (t && t->d.target == NVN_TEXTURE_TARGET_BUFFER) return void(st_buffer_tex.fetch_add(1, std::memory_order_relaxed));
    AssetRef a;
    const bool asset = assets_created(t, a);   // (A-rt: the record its load named, kept for its serial)
    if (asset && !a.rec && !a.plain) {
        st_ph.fetch_add(1, std::memory_order_relaxed);
        st_ph_norec.fetch_add(1, std::memory_order_relaxed);
        assets_note_placeholder(t, a.why);
    }
    std::unique_lock lk(images_mu);
    Image* im = get(t);
    if (!im || im->placeholder) return;
    if (im->refs++ == 0 && im->dead_present) im->dead_present = im->dead_value = 0, st_revived.fetch_add(1, std::memory_order_relaxed);
    if (im->pooled) claim(im, "initialized");   // (a texture over that storage now: the range is its image's)
    if (!im->asset) return;
    // the first texture of an asset image fills it, here, before the game can register it; another one that comes
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

namespace {
/// im takes its range of the pool (images_mu held exclusively): nothing if it is the owner; else every owner of memory it
/// overlaps stops being one (its content is undefined from here), and im is discarded and cleared on this thread's
/// upload, which is sent at once: after every list sent before, before every list sent later. The host's slots never
/// give a range to another target while lists that use the last one may be unsent (games/oot/patches/rt_slots.c); an owner claimed
/// within kPoolInUse presents that loses its range is a strict event
void claim(Image* im, const char* how) {
    const uint64_t now = present_count();
    im->claimed.store(now, std::memory_order_relaxed);
    if (im->owner.load(std::memory_order_relaxed)) return;
    for (Image* o : pool.images) {
        if (o == im || !o->owner.load(std::memory_order_relaxed) || o->pool_off >= im->pool_off + im->pool_size ||
            im->pool_off >= o->pool_off + o->pool_size)
            continue;
        o->owner.store(false, std::memory_order_relaxed);
        if (o->claimed.load(std::memory_order_relaxed) + kPoolInUse > now) {
            st_pool_took_live.fetch_add(1, std::memory_order_relaxed);
            strict_event("a transient target's memory taken while it may be in use", "image #%u %ux%u vk %d at +%#llx (%llu "
                         "bytes, %s at present %llu) over image #%u %ux%u vk %d at +%#llx (%llu bytes, claimed at present %llu)",
                         im->id, im->width, im->height, (int)im->format, (unsigned long long)im->pool_off,
                         (unsigned long long)im->pool_size, how, (unsigned long long)now, o->id, o->width, o->height,
                         (int)o->format, (unsigned long long)o->pool_off, (unsigned long long)o->pool_size,
                         (unsigned long long)o->claimed.load(std::memory_order_relaxed));
        }
    }
    im->owner.store(true, std::memory_order_relaxed);
    im->written.store(0, std::memory_order_relaxed);
    init_image(*im);
    st_pool_claims.fetch_add(1, std::memory_order_relaxed), st_pool_claims_total.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

bool image_pool_range(uint64_t lo, uint64_t hi) {
    std::unique_lock lk(images_mu);
    if (pool.on.load(std::memory_order_relaxed)) return pool.lo == lo && pool.hi == hi;
    // (the memory types an attachment image takes: a depth image's, as the pool's largest tenants are)
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_D32_SFLOAT;
    ci.extent = {64, 64, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage probe = VK_NULL_HANDLE;
    VkMemoryRequirements req{};
    req.memoryTypeBits = ~0u;
    if (vkCreateImage(dev.device, &ci, nullptr, &probe) == VK_SUCCESS) {
        vkGetImageMemoryRequirements(dev.device, probe, &req);
        vkDestroyImage(dev.device, probe, nullptr);
    }
    if (hi <= lo || !mem_pool_make(hi - lo, req.memoryTypeBits)) {
        logf("direct: transient pool: %llu MB of device memory not allocated: the transient targets get images of their own",
             (unsigned long long)((hi - lo) >> 20));
        strict_event("the transient pool was not made", "%llu bytes", (unsigned long long)(hi - lo));
        return false;
    }
    pool.lo = lo, pool.hi = hi;
    pool.on.store(true, std::memory_order_release);
    logf("direct: transient pool: %llu MB of device memory for the game's transient targets at %#llx..%#llx, allocated once; "
         "their images are bound in it at their storage offsets", (unsigned long long)((hi - lo) >> 20), (unsigned long long)lo,
         (unsigned long long)hi);
    return true;
}

void image_pool_slot(uint64_t gpu, uint64_t cap) {
    if (!pool.on.load(std::memory_order_acquire) || gpu < pool.lo || gpu >= pool.hi) return;
    std::unique_lock lk(images_mu);
    pool.caps[gpu] = cap;
}

uint64_t image_need(const nvn_texdesc* d, uint64_t* align) {
    if (align) *align = 0;
    if (!d || !dev.device || d->target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    const FormatInfo fi = vk_format(d->format);
    if (fi.format == VK_FORMAT_UNDEFINED) return 0;
    Image im;
    VkImageCreateInfo ci{};
    if (shape(*d, fi, im, ci)) return 0;
    // (by what the driver's answer depends on; a shape asked once is not asked again)
    uint64_t h = mix((uint64_t)ci.format << 32 | ci.flags, (uint64_t)ci.usage << 32 | (uint32_t)ci.imageType);
    h = mix(h, (uint64_t)ci.extent.width << 32 | ci.extent.height);
    h = mix(h, (uint64_t)ci.extent.depth << 32 | ci.mipLevels);
    h = mix(h, (uint64_t)ci.arrayLayers << 32 | (uint32_t)ci.samples);
    static std::mutex mu;
    static std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> known;
    std::lock_guard lk(mu);
    auto it = known.find(h);
    if (it == known.end()) {
        const DebugObjects not_an_event;   // (a question to the driver: made and destroyed here)
        VkImage probe = VK_NULL_HANDLE;
        VkMemoryRequirements req{};
        if (vkCreateImage(dev.device, &ci, nullptr, &probe) == VK_SUCCESS) {
            vkGetImageMemoryRequirements(dev.device, probe, &req);
            vkDestroyImage(dev.device, probe, nullptr);
        }
        it = known.emplace(h, std::make_pair((uint64_t)req.size, (uint64_t)req.alignment)).first;
    }
    if (align) *align = it->second.second;
    return it->second.first;
}

void image_target_given(const nvn_tex* t) {
    if (!t || !t->serial || !pool.on.load(std::memory_order_acquire) || t->gpu < pool.lo || t->gpu >= pool.hi) return;
    Key k;
    FormatInfo fi;
    int bw, bh, bpb;
    if (key_of(t, k, fi, bw, bh, bpb) != K_PLAIN) return;
    st_pool_given.fetch_add(1, std::memory_order_relaxed);
    {
        std::shared_lock lk(images_mu);
        auto it = images.find(k);
        if (it != images.end() && it->second->owner.load(std::memory_order_relaxed)) {
            it->second->claimed.store(present_count(), std::memory_order_relaxed);
            return;
        }
    }
    std::unique_lock lk(images_mu);
    Image* im = get(t);
    if (!im || !im->pooled) return;
    if (!im->owner.load(std::memory_order_relaxed)) st_pool_given_claimed.fetch_add(1, std::memory_order_relaxed);
    claim(im, "given out by the allocator");
}

bool image_stale(const Image* im) { return im && im->pooled && !im->owner.load(std::memory_order_relaxed); }
void image_stale_bound() { st_pool_stale_bound.fetch_add(1, std::memory_order_relaxed); }

void image_texture_released(const nvn_tex* t) {
    Key k;
    FormatInfo fi;
    int bw, bh, bpb;
    const int kind = key_of(t, k, fi, bw, bh, bpb);
    if (kind == K_PLAIN || kind == K_ASSET || kind == K_LINKED_PLAIN) {
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
    if (kind == K_ASSET || kind == K_ASSET_MISS || kind == K_LINKED_PLAIN) assets_released(t);
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

void image_texture_cached(const nvn_tex* t, bool cached) {
    if (!t || !t->serial) return;
    Key k;
    FormatInfo fi;
    int bw, bh, bpb;
    if (key_of(t, k, fi, bw, bh, bpb) == K_NONE) return;
    std::unique_lock lk(images_mu);
    auto it = images.find(k);
    if (it == images.end()) return;
    Image* im = it->second.get();
    im->cached_refs = std::max(0, im->cached_refs + (cached ? 1 : -1));
}

void image_texture_registered(const nvn_tex* t) {
    if (!t || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return;
    if (image_find(t)) return;
    std::unique_lock lk(images_mu);   // (registered without its initialization seen: made now)
    if (get(t)) st_registered_new.fetch_add(1, std::memory_order_relaxed);
}

void image_key_describe(const nvn_tex* t, char* out, size_t cap) {   // (M3b: why a texture's image is what it is, for logs)
    Key k;
    FormatInfo fi;
    int bw, bh, bpb;
    AssetRef a;
    const int kind = key_of(t, k, fi, bw, bh, bpb, &a);
    AssetRef now;
    const bool linked = assets_of(t, now);
    static const char* const kinds[] = {"none", "plain", "asset", "asset with no record", "linked plain"};
    std::snprintf(out, cap, "serial %llu key %s, linked %d (record %u, plain %llu, why '%s')", t ? (unsigned long long)t->serial : 0ull,
                  kinds[kind], (int)linked, now.rec, (unsigned long long)now.plain, now.why ? now.why : "");
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
    if (fi.format == VK_FORMAT_UNDEFINED || bpb <= 0) return &placeholder;
    return nullptr;
}

int image_view_format(const nvn_tex* t, const nvn_view* v) { return v && v->set_format ? v->format : t->d.format; }

VkFormat image_astc_view_format(const Image* im, int nvn_format) {
    const VkFormat lin = linear_twin(im->format);
    if (nvn_format < 0x87) return lin;
    switch (lin) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return im->format;
    }
}

VkImageView image_attachment_view(Image* im, int nvn_format, uint32_t level, uint32_t layer, VkFormat* format_out) {
    if (!im || im->placeholder || (im->compressed && !im->block_views) || level >= im->levels) return VK_NULL_HANDLE;
    const ObjectsFor objects_for("an attachment view of a game's texture");   // 
    const bool depth = (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0;
    VkFormat f = im->format;
    if (!depth) {
        bool wide = false;
        const FormatInfo vi = vk_format(nvn_format);
        const VkFormat vf = widened(vi.format, &wide);
        if (vf != VK_FORMAT_UNDEFINED && !is_depth_format(vf) && !is_bc(vf)) f = vf;
        // (M4g: a compressed image's blocks, through a format of the block's size)
        if (im->compressed && (f == im->format || wide || nvn_block_size(nvn_format, nullptr, nullptr) != im->bpb)) return VK_NULL_HANDLE;
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
        object_made(1, "an attachment view");
    }
    im->views[key] = v;
    return v;
}

VkImageView image_pool_view(Image* im, const VkImageViewCreateInfo& vi, VkImageUsageFlags usage) {
    Image::PoolView k{};
    k.type = vi.viewType, k.format = vi.format, k.components = vi.components, k.range = vi.subresourceRange, k.usage = usage;
    std::lock_guard lk(im->views_mu);
    for (const Image::PoolView& v : im->pool_views)
        if (!std::memcmp(&v, &k, offsetof(Image::PoolView, view))) return v.view;
    const ObjectsFor objects_for("a view of a game's texture for the pools' set");   // 
    VkImageViewUsageCreateInfo use{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    use.usage = usage;
    VkImageViewCreateInfo ci = vi;
    ci.pNext = &use;
    if (vkCreateImageView(dev.device, &ci, nullptr, &k.view) != VK_SUCCESS) k.view = VK_NULL_HANDLE;
    else object_made(1, "a pool view");
    im->pool_views.push_back(k);
    return k.view;
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
    if (nvn_astc(s.im->nvn_format)) {   // (the game's bytes are ASTC blocks, the image BC7)
        strict_event("texels written into an ASTC texture", "texture %#llx %ux%u NVN format %#x level %u: its image is the store's "
                     "BC7", (unsigned long long)t->gpu, s.w, s.h, s.im->nvn_format, level);
        return false;
    }
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
    if (source != W_TEXELS) assets_check_region(s.im, source, s.level, s.layer, s.layers, s.x, s.y, s.w, s.h, u.map, packed, src, row);
    upload_end(u);
    s.im->written.fetch_or(source, std::memory_order_relaxed);
    assets_note_upload(source, packed, host->now_ns() - t0);
    return true;
}

std::atomic<int64_t> dying_bytes{0};
int64_t image_dying_bytes() { return dying_bytes.load(std::memory_order_relaxed); }
void image_sweep(uint64_t present) {
    std::unique_lock lk(images_mu, std::try_to_lock);   // (a texture being made: the next present)
    if (!lk.owns_lock()) return;
    {   // (M6f: what the images waiting to die hold: VRAM already given back, for the free VRAM the host reads)
        int64_t b = 0;
        for (const Image* im : dying)
            if (!im->refs && im->dead_present && !im->held) b += (int64_t)mem_size(im->mem);
        dying_bytes.store(b, std::memory_order_relaxed);
    }
    // (the transient pool's images hold no memory of their own: one no texture is initialized over is kept as an object,
    // for the next texture of its storage and shape, until the pool has more than kPoolImages; then the longest unclaimed go)
    if (pool.images.size() > kPoolImages) {
        size_t over = pool.images.size() - kPoolImages;
        std::vector<Image*> idle;
        for (Image* im : dying)
            if (im->pooled && !im->refs && im->dead_present) {
                if (im->prune) over = over ? over - 1 : 0;
                else idle.push_back(im);
            }
        std::sort(idle.begin(), idle.end(), [](const Image* a, const Image* b) {
            return a->claimed.load(std::memory_order_relaxed) < b->claimed.load(std::memory_order_relaxed);
        });
        for (size_t i = 0; i < idle.size() && i < over; i++) idle[i]->prune = true;
    }
    for (size_t i = 0; i < dying.size();) {
        Image* im = dying[i];
        bool gone = false;
        if (im->refs > 0 || !im->dead_present) {
            dying[i] = dying.back(), dying.pop_back();   // (revived)
            im->prune = false;
            if (im->held) held_bytes -= (int64_t)mem_size(im->mem), im->held = false;
            continue;
        }
        if ((im->pooled && !im->prune) || im->held) {   // (kept)
            i++;
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
        const int64_t size = (int64_t)mem_size(im->mem);
        if (!im->pooled && !im->asset && !im->no_data && im->span &&
            (im->written.load(std::memory_order_relaxed) & (W_ATTACHMENT | W_COPY)) && held_bytes + size <= kHeldBytes) {
            im->held = true, held_bytes += size;
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

void image_mem_split(ImageSplit& s) {
    std::shared_lock lk(images_mu);
    for (const auto& [k, im] : images) {
        s.images++;
        const bool colour = im->compressed || (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT);
        if (im->pooled) {   // (no memory of its own: its range of the pool counts while it owns it)
            if (im->owner.load(std::memory_order_relaxed)) s.pool_use[colour ? 2 : im->width == im->height ? 0 : 1] += im->pool_size;
            continue;
        }
        const uint64_t b = mem_size(im->mem);
        const uint8_t w = im->written.load(std::memory_order_relaxed);
        if (im->asset) s.kind[ImageSplit::STORE] += b;   // (image_stats' order of sources)
        else if (w & W_ATTACHMENT) s.kind[colour ? ImageSplit::TARGET_COLOUR : ImageSplit::TARGET_DEPTH] += b;
        else if (w & (W_COPY | W_CLEAR)) s.kind[ImageSplit::COPIED] += b;
        else if (w & W_STORE) s.kind[ImageSplit::STORE] += b;
        else if (w & W_TERRAIN) s.kind[ImageSplit::TERRAIN] += b;
        else if (w & W_GLYPH) s.kind[ImageSplit::GLYPH] += b;
        else if (w & W_TEXELS) s.kind[ImageSplit::TEXELS] += b;
        else s.kind[ImageSplit::NEVER] += b;
    }
    if (placeholder.mem) s.placeholder = mem_size(placeholder.mem);
    s.table = images.size() * (sizeof(Image) + sizeof(Key) + 32);   // (a record, its key and the map's node, about)
}

void image_stats(char* out, size_t cap) {
    int64_t live = 0, color = 0, depth = 0, bc = 0, rendered = 0, copied = 0, texels = 0, store = 0, terrain = 0, glyph = 0,
            untouched = 0, assets = 0, unfilled = 0, no_content = 0, nd = 0, nd_bits[8] = {}, nd_never = 0;
    uint64_t own_live[3] = {}, own_dying[3] = {}, own_n[3] = {}, refs_hist[4] = {}, many_refs_bytes = 0;
    uint64_t cache_n = 0, cache_bytes = 0, cache_part = 0;   // (M6f: images every texture of which is in agl's LRU cache)
    std::unordered_map<uint64_t, uint32_t> gpus;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> shapes;   // (w, h, layers, format -> bytes, images)
    {
        std::shared_lock lk(images_mu);
        for (const auto& [k, im] : images) {
            live++;
            (im->compressed ? bc : (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? color : depth)++;
            if (im->asset) assets++, unfilled += im->fill.load(std::memory_order_relaxed) != 2;
            no_content += im->no_content;
            const uint8_t w = im->written.load(std::memory_order_relaxed);
            if (im->no_data) {
                nd++, nd_never += !w;
                for (int b = 0; b < 8; b++) nd_bits[b] += w >> b & 1;
            }
            if (im->cached_refs > 0) {
                if (im->cached_refs >= im->refs) cache_n++, cache_bytes += mem_size(im->mem);
                else cache_part++;
            }
            if (!im->asset && !im->pooled) {   // (M6f: the images not from the store, by kind, held or waiting to die, and the oldest ones)
                const int c = im->compressed ? 2 : (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? 0 : 1;
                const uint64_t b = mem_size(im->mem);
                (im->refs > 0 ? own_live : own_dying)[c] += b;
                own_n[c]++;
                if (im->refs > 0) refs_hist[std::min(im->refs, 4) - 1]++;
                gpus[im->gpu]++;
                auto& sh = shapes[(uint64_t)im->width << 48 | (uint64_t)im->height << 32 | (uint64_t)im->layers << 24 | (uint32_t)im->format];
                sh.first += b, sh.second++;
                if (im->refs > 1) many_refs_bytes += b;
            }
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
                  "%lld fills failed); placeholders %lld (no prepared record %lld, no format %lld, refused %lld; texels "
                  "not given %lld), buffer textures %lld; made at registration %lld; texel writes %lld (%lld skipped)",
                  (long long)st_made.exchange(0), (long long)st_made_total.load(), (long long)st_destroyed.exchange(0),
                  (long long)st_revived.exchange(0), (long long)live, (long long)color, (long long)depth, (long long)bc,
                  (long long)rendered, (long long)copied, (long long)store, (long long)terrain, (long long)glyph, (long long)texels,
                  (long long)untouched, st_bytes.load() / 1048576.0, (long long)assets, st_asset_bytes.load() / 1048576.0,
                  (long long)unfilled, (long long)st_asset_shared.load(), (long long)st_asset_fill_waits.load(),
                  (long long)st_asset_fail.load(), (long long)st_ph.load(), (long long)st_ph_norec.load(),
                  (long long)st_ph_format.load(), (long long)st_ph_refused.load(), (long long)no_content, (long long)st_buffer_tex.load(),
                  (long long)st_registered_new.load(), (long long)st_texels.load(), (long long)st_texels_skipped.load());
    const size_t n = std::strlen(out);
    std::snprintf(out + n, cap > n ? cap - n : 0, "; textures loaded with no data: %lld images, written by rendering %lld, copies %lld, "
                  "texel writes %lld, clears %lld, never %lld", (long long)nd, (long long)nd_bits[0], (long long)nd_bits[1],
                  (long long)nd_bits[2], (long long)nd_bits[3], (long long)nd_never);
    uint64_t shared_addr = 0;
    for (const auto& [g, n] : gpus) shared_addr += n > 1 ? n : 0;
    const size_t n2 = std::strlen(out);
    std::snprintf(out + n2, cap > n2 ? cap - n2 : 0, "; not from the store (MB held by a texture / waiting to die): colour %llu / %llu "
                  "(%llu images), depth %llu / %llu (%llu), compressed %llu / %llu (%llu); held by 1, 2, 3, 4+ textures %llu %llu %llu "
                  "%llu (%llu MB by more than one); images sharing a storage address with another %llu",
                  (unsigned long long)(own_live[0] >> 20), (unsigned long long)(own_dying[0] >> 20), (unsigned long long)own_n[0],
                  (unsigned long long)(own_live[1] >> 20), (unsigned long long)(own_dying[1] >> 20), (unsigned long long)own_n[1],
                  (unsigned long long)(own_live[2] >> 20), (unsigned long long)(own_dying[2] >> 20), (unsigned long long)own_n[2],
                  (unsigned long long)refs_hist[0], (unsigned long long)refs_hist[1], (unsigned long long)refs_hist[2],
                  (unsigned long long)refs_hist[3], (unsigned long long)(many_refs_bytes >> 20), (unsigned long long)shared_addr);
    {
        const size_t n5 = std::strlen(out);
        std::snprintf(out + n5, cap > n5 ? cap - n5 : 0, "; held only by textures in agl's LRU cache: %llu images, %llu MB (%llu "
                      "more images held by a cached texture and a live one)", (unsigned long long)cache_n,
                      (unsigned long long)(cache_bytes >> 20), (unsigned long long)cache_part);
    }
    std::vector<std::pair<uint64_t, std::pair<uint64_t, uint32_t>>> top(shapes.begin(), shapes.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    for (size_t i = 0; i < top.size() && i < 8; i++) {
        const size_t n3 = std::strlen(out);
        const uint64_t k = top[i].first;
        std::snprintf(out + n3, cap > n3 ? cap - n3 : 0, "%s %llux%llu x%llu vk %llu: %u images %llu MB", i ? ";" : "; the largest shapes:",
                      (unsigned long long)(k >> 48), (unsigned long long)(k >> 32 & 0xffff), (unsigned long long)(k >> 24 & 0xff),
                      (unsigned long long)(k & 0xffffff), top[i].second.second, (unsigned long long)(top[i].second.first >> 20));
    }
}

/// The transient pool's line: its bytes are a constant; what moves is image objects
void image_pool_stats(char* out, size_t cap) {
        size_t n_pool = 0, owners = 0, held = 0;
        {
            std::shared_lock lk(images_mu);
            n_pool = pool.images.size();
            for (const Image* im : pool.images) owners += im->owner.load(std::memory_order_relaxed), held += im->refs > 0;
        }
        std::snprintf(out, cap, "transient pool: %llu MB of device memory (constant), %llu image objects "
                      "kept (%llu own their range, %llu with a texture initialized over them; bound %llu), since the last line %lld "
                      "made (%lld since the start), %lld destroyed over the bound, %lld claims with a discard and clear (%lld since "
                      "the start), %lld targets given out by the allocator (%lld of them not their range's owner: claimed); since "
                      "the start: %lld taken while in use, %lld that did not fit their slot, %lld draws binding an image that is "
                      "not its range's owner", (unsigned long long)(mem_pool_size() >> 20), (unsigned long long)n_pool,
                      (unsigned long long)owners, (unsigned long long)held, (unsigned long long)kPoolImages,
                      (long long)st_pool_made.exchange(0), (long long)st_pool_made_total.load(), (long long)st_pool_pruned.exchange(0),
                      (long long)st_pool_claims.exchange(0), (long long)st_pool_claims_total.load(), (long long)st_pool_given.exchange(0),
                      (long long)st_pool_given_claimed.exchange(0), (long long)st_pool_took_live.load(), (long long)st_pool_nofit.load(),
                      (long long)st_pool_stale_bound.load());
}

}  // namespace hsd
