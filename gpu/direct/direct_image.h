// One image per NVN texture (docs/direct/m2-interface.md, direct_image): made when the texture is initialized (or
// registered, if it was not seen first), never at a draw; kept by its storage and shape, so a texture the game
// initializes again every frame over the same storage (the transient targets, host/rt_slots.c) keeps its image.
// Every image lives in VK_IMAGE_LAYOUT_GENERAL. A texture with no image this GPU can hold (ASTC, a format with no Vulkan
// twin, a size the driver refuses) gets the placeholder, counted and named; a buffer texture gets nothing.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "direct_internal.h"

namespace hsd {

// ---- formats (direct_formats.cpp, a copy of gpu/vk/vk_formats.cpp) ----
struct FormatInfo {
    VkFormat format;
    bool astc;   // (ASTC on a GPU that does not sample it: the placeholder)
    int block_w, block_h;
};
FormatInfo vk_format(int nvn_format);
bool is_depth_format(VkFormat f);
bool has_stencil(VkFormat f);
int nvn_block_size(int format, int* bw, int* bh);

struct Image {
    VkImage image = VK_NULL_HANDLE;
    Mem* mem = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t width = 1, height = 1, depth = 1, levels = 1, layers = 1;
    int nvn_format = 0, bpb = 0, bw = 1, bh = 1;
    uint64_t gpu = 0;
    uint32_t id = 0;
    bool placeholder = false;
    bool compressed = false;
    std::atomic<uint8_t> written{0};   // (W_* bits: what has written it)
    // (lifetime: the live NVN textures over it; images_mu held)
    int refs = 0;
    uint64_t dead_present = 0, dead_value = 0;
    std::mutex views_mu;
    std::unordered_map<uint64_t, VkImageView> views;
};
enum : uint8_t { W_ATTACHMENT = 1, W_COPY = 2, W_TEXELS = 4, W_CLEAR = 8 };

void image_texture_created(const nvn_tex* t);
void image_texture_released(const nvn_tex* t);
void image_texture_registered(const nvn_tex* t);
/// The image of an NVN texture (the placeholder when it has none this GPU can hold; null: a buffer texture or none made)
Image* image_find(const nvn_tex* t);
/// A view of one level and layer (a 3D image's slice) as an attachment, in the view's format; null when none can be
/// made (a compressed format, a level or layer outside the image)
VkImageView image_attachment_view(Image* im, int nvn_format, uint32_t level, uint32_t layer, VkFormat* format_out);
/// The format of a view of t (its view's, or its own)
int image_view_format(const nvn_tex* t, const nvn_view* v);
/// WriteTexels: the linear texels copied into the image at the call (the calling thread's upload, sent at once)
void image_write_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src,
                        uint64_t row, uint64_t img);
/// Images whose textures are all gone for a while, and done on the GPU, destroyed (the Presentation Thread, at a present)
void image_sweep(uint64_t present);
void image_stats(char* out, size_t cap);

// ---- one side of a copy (direct_image.cpp): a level, its layers or slices, and a region in texels ----
struct Sub {
    Image* im = nullptr;
    VkImageAspectFlags aspect = 0;
    uint32_t level = 0, layer = 0, layers = 1;
    int32_t x = 0, y = 0, z = 0;
    uint32_t w = 0, h = 0, d = 1;
    uint32_t bpb = 0;
    int bw = 1, bh = 1;
};
/// false (counted by the caller): no image, the placeholder, or a region outside it
bool image_sub(const nvn_tex* t, const nvn_view* v, const NVNcopyRegion& r, Sub& s);

}  // namespace hsd
