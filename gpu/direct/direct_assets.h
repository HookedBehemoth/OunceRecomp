// Prepared assets: the texture store of
// nx2/prepare_textures.py (index.bin and pack_NNN.bin, mapped; the format is that script's docstring) and the links from
// the host's loader hooks (hsd_asset_load: a texture object -> its record) to the images direct_image makes. An asset
// texture's image is made in the record's format when the texture is initialized, keyed by the record's content, and
// filled from the packs on the initializing thread (its uploads, sent at once with their own timeline values): so before
// the texture is registered, and so before any list can draw it. A texture whose load found no record gets the
// placeholder, counted and named. No conversion, no ASTC, no block-linear code: the store holds the Vulkan layout.
#pragma once

#include <cstdint>

#include "direct_internal.h"

namespace hsd {

struct Image;

/// What an asset texture is: rec = its texture record + 1 (0: no record: the placeholder, why says)
struct AssetRef {
    uint32_t rec = 0;
    const char* why = nullptr;
    uint64_t plain = 0;   // (no record and no data at its load: the GPU fills it; a plain image that follows its object)
};
/// What an image of a record needs
struct AssetDesc {
    uint64_t key = 0;   // (the content: the record's content hash and descriptor)
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType view = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t width = 1, height = 1, layers = 1, mips = 1, bpb = 0, bw = 1, bh = 1;
    uint8_t map[4] = {};
};

/// At t's initialization: the asset its object was linked to by the last load (true), kept for t's serial; false: t is no
/// asset texture
bool assets_created(const nvn_tex* t, AssetRef& a);
/// After: the asset of t, by its serial; false: none
bool assets_of(const nvn_tex* t, AssetRef& a);
/// t is gone (after its image was let go)
void assets_released(const nvn_tex* t);
/// The image description of a record (rec as AssetRef's)
bool assets_desc(uint32_t rec, AssetDesc& d);
/// The record's texels into im (made by the caller in its format, layout undefined): moved to GENERAL and filled by this
/// thread's uploads; false: the store could not be read (the image stays empty, counted)
bool assets_fill(Image& im, uint32_t rec);
/// [gpu, gpu + size) is a texture data pool (hsd_texture_data_pool: its texels come from the store, nothing reads it)
bool assets_pool_is(uint64_t gpu, uint64_t size);
/// The record's name (logs)
const char* assets_name(uint32_t rec, uint32_t* len);

/// An upload was sent (direct_image: the region uploads; source W_* bit), for the counters a frame and by thread
void assets_note_upload(uint8_t source, uint64_t bytes, int64_t ns);
/// (HOS_DBG=assetcheck / assetdump) a region upload's bytes, sampled for a read back after the load
void assets_check_region(Image* im, uint8_t source, uint32_t level, uint32_t layer, uint32_t layers, int32_t x, int32_t y, uint32_t w,
                         uint32_t h, const uint8_t* bytes, uint64_t n, const uint8_t* src, uint64_t row);
/// A placeholder named (direct_image: why)
void assets_note_placeholder(const nvn_tex* t, const char* why);
/// A present (the Presentation Thread): the load's end found, the debug checks run
void assets_present(uint64_t present);
/// A job list began (the first frame in play is the first with many)
void assets_job_list();
void assets_stats(char* out, size_t cap, double frames);

}  // namespace hsd
