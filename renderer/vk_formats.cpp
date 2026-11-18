// NVN texture formats (renderer/api/nvn_api.h) as Vulkan formats, after nvn_pixel_format of the old renderer
// (gpu/legacy/render/rx_nvn.cpp): the same NVN -> suyu pixel format table, each suyu format as its Vulkan one. An ASTC
// format is the texture store's BC7 for it (ounce/tex/src/plan.rs astc_target; sRGB for sRGB), for a texture the game
// loads and one it makes alike, so copies between them fit texel for texel.
#include "vk_image.h"

namespace hsd {

bool nvn_astc(int nvn_format) { return nvn_format >= 0x79 && nvn_format <= 0x94; }

FormatInfo vk_format(int nvn_format) {
    if (nvn_astc(nvn_format)) return {nvn_format >= 0x87 ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, 4, 4};
    switch (nvn_format) {
    case NVN_FORMAT_R8: return {VK_FORMAT_R8_UNORM, 1, 1};
    case NVN_FORMAT_R8SN: return {VK_FORMAT_R8_SNORM, 1, 1};
    case NVN_FORMAT_R8UI: return {VK_FORMAT_R8_UINT, 1, 1};
    case NVN_FORMAT_R8I: return {VK_FORMAT_R8_SINT, 1, 1};
    case NVN_FORMAT_R16F: return {VK_FORMAT_R16_SFLOAT, 1, 1};
    case NVN_FORMAT_R16: return {VK_FORMAT_R16_UNORM, 1, 1};
    case NVN_FORMAT_R16SN: return {VK_FORMAT_R16_SNORM, 1, 1};
    case NVN_FORMAT_R16UI: return {VK_FORMAT_R16_UINT, 1, 1};
    case NVN_FORMAT_R16I: return {VK_FORMAT_R16_SINT, 1, 1};
    case NVN_FORMAT_R32F: return {VK_FORMAT_R32_SFLOAT, 1, 1};
    case NVN_FORMAT_R32UI: return {VK_FORMAT_R32_UINT, 1, 1};
    case NVN_FORMAT_R32I: return {VK_FORMAT_R32_SINT, 1, 1};
    case NVN_FORMAT_RG8: return {VK_FORMAT_R8G8_UNORM, 1, 1};
    case NVN_FORMAT_RG8SN: return {VK_FORMAT_R8G8_SNORM, 1, 1};
    case NVN_FORMAT_RG8UI: return {VK_FORMAT_R8G8_UINT, 1, 1};
    case NVN_FORMAT_RG8I: return {VK_FORMAT_R8G8_SINT, 1, 1};
    case NVN_FORMAT_RG16F: return {VK_FORMAT_R16G16_SFLOAT, 1, 1};
    case NVN_FORMAT_RG16: return {VK_FORMAT_R16G16_UNORM, 1, 1};
    case NVN_FORMAT_RG16SN: return {VK_FORMAT_R16G16_SNORM, 1, 1};
    case NVN_FORMAT_RG16UI: return {VK_FORMAT_R16G16_UINT, 1, 1};
    case NVN_FORMAT_RG16I: return {VK_FORMAT_R16G16_SINT, 1, 1};
    case NVN_FORMAT_RG32F: return {VK_FORMAT_R32G32_SFLOAT, 1, 1};
    case NVN_FORMAT_RG32UI: return {VK_FORMAT_R32G32_UINT, 1, 1};
    case NVN_FORMAT_RG32I: return {VK_FORMAT_R32G32_SINT, 1, 1};
    case NVN_FORMAT_RGB32F: return {VK_FORMAT_R32G32B32_SFLOAT, 1, 1};
    case NVN_FORMAT_RGBX8: return {VK_FORMAT_R8G8B8A8_UNORM, 1, 1};
    case NVN_FORMAT_RGBX8SN: return {VK_FORMAT_R8G8B8A8_SNORM, 1, 1};
    case NVN_FORMAT_RGBX8UI: return {VK_FORMAT_R8G8B8A8_UINT, 1, 1};
    case NVN_FORMAT_RGBX8I: return {VK_FORMAT_R8G8B8A8_SINT, 1, 1};
    case NVN_FORMAT_RGBA16F: return {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1};
    case NVN_FORMAT_RGBX16F: return {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1};
    case NVN_FORMAT_RGBX16: return {VK_FORMAT_R16G16B16A16_UNORM, 1, 1};
    case NVN_FORMAT_RGBX16SN: return {VK_FORMAT_R16G16B16A16_SNORM, 1, 1};
    case NVN_FORMAT_RGBX16UI: return {VK_FORMAT_R16G16B16A16_UINT, 1, 1};
    case NVN_FORMAT_RGBX16I: return {VK_FORMAT_R16G16B16A16_SINT, 1, 1};
    case NVN_FORMAT_RGBX32F: return {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1};
    case NVN_FORMAT_RGBX32UI: return {VK_FORMAT_R32G32B32A32_UINT, 1, 1};
    case NVN_FORMAT_RGBX32I: return {VK_FORMAT_R32G32B32A32_SINT, 1, 1};
    case NVN_FORMAT_RGB8: return {VK_FORMAT_R8G8B8_UNORM, 1, 1};
    case NVN_FORMAT_RGB8SN: return {VK_FORMAT_R8G8B8_SNORM, 1, 1};
    case NVN_FORMAT_RGB8UI: return {VK_FORMAT_R8G8B8_UINT, 1, 1};
    case NVN_FORMAT_RGB8I: return {VK_FORMAT_R8G8B8_SINT, 1, 1};
    case NVN_FORMAT_RGBA8: return {VK_FORMAT_R8G8B8A8_UNORM, 1, 1};
    case NVN_FORMAT_RGBA8SN: return {VK_FORMAT_R8G8B8A8_SNORM, 1, 1};
    case NVN_FORMAT_RGBA8UI: return {VK_FORMAT_R8G8B8A8_UINT, 1, 1};
    case NVN_FORMAT_RGBA8I: return {VK_FORMAT_R8G8B8A8_SINT, 1, 1};
    case NVN_FORMAT_RGBX8_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, 1, 1};
    case NVN_FORMAT_RGBA8_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, 1, 1};
    case NVN_FORMAT_RGB5: return {VK_FORMAT_B5G5R5A1_UNORM_PACK16, 1, 1};
    case NVN_FORMAT_RGBA16: return {VK_FORMAT_R16G16B16A16_UNORM, 1, 1};
    case NVN_FORMAT_RGBA16SN: return {VK_FORMAT_R16G16B16A16_SNORM, 1, 1};
    case NVN_FORMAT_RGBA16UI: return {VK_FORMAT_R16G16B16A16_UINT, 1, 1};
    case NVN_FORMAT_RGBA16I: return {VK_FORMAT_R16G16B16A16_SINT, 1, 1};
    case NVN_FORMAT_RGBA32F: return {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1};
    case NVN_FORMAT_RGBA32UI: return {VK_FORMAT_R32G32B32A32_UINT, 1, 1};
    case NVN_FORMAT_RGBA32I: return {VK_FORMAT_R32G32B32A32_SINT, 1, 1};
    case NVN_FORMAT_STENCIL8: return {VK_FORMAT_S8_UINT, 1, 1};
    case NVN_FORMAT_DEPTH16: return {VK_FORMAT_D16_UNORM, 1, 1};
    case NVN_FORMAT_DEPTH24: return {VK_FORMAT_X8_D24_UNORM_PACK32, 1, 1};
    case NVN_FORMAT_DEPTH32F: return {VK_FORMAT_D32_SFLOAT, 1, 1};
    case NVN_FORMAT_DEPTH24_STENCIL8: return {VK_FORMAT_D24_UNORM_S8_UINT, 1, 1};
    case NVN_FORMAT_DEPTH32F_STENCIL8: return {VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 1};
    case NVN_FORMAT_RGBA4: return {VK_FORMAT_A4B4G4R4_UNORM_PACK16, 1, 1};
    case NVN_FORMAT_RGB5A1: return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 1, 1};
    case NVN_FORMAT_RGB565: return {VK_FORMAT_B5G6R5_UNORM_PACK16, 1, 1};
    case NVN_FORMAT_BGR565: return {VK_FORMAT_R5G6B5_UNORM_PACK16, 1, 1};
    case NVN_FORMAT_RGB10A2: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 1};
    case NVN_FORMAT_RGB10A2UI: return {VK_FORMAT_A2B10G10R10_UINT_PACK32, 1, 1};
    case NVN_FORMAT_R11G11B10F: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 1, 1};
    case NVN_FORMAT_RGB9E5F: return {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 1, 1};
    case NVN_FORMAT_RGBA_DXT1: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 1, 1};
    case NVN_FORMAT_RGBA_DXT1_SRGB: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 1, 1};
    case NVN_FORMAT_RGBA_DXT3: return {VK_FORMAT_BC2_UNORM_BLOCK, 1, 1};
    case NVN_FORMAT_RGBA_DXT3_SRGB: return {VK_FORMAT_BC2_SRGB_BLOCK, 1, 1};
    case NVN_FORMAT_RGBA_DXT5: return {VK_FORMAT_BC3_UNORM_BLOCK, 1, 1};
    case NVN_FORMAT_RGBA_DXT5_SRGB: return {VK_FORMAT_BC3_SRGB_BLOCK, 1, 1};
    case NVN_FORMAT_RGTC1_UNORM: return {VK_FORMAT_BC4_UNORM_BLOCK, 1, 1};
    case NVN_FORMAT_RGTC1_SNORM: return {VK_FORMAT_BC4_SNORM_BLOCK, 1, 1};
    case NVN_FORMAT_RGTC2_UNORM: return {VK_FORMAT_BC5_UNORM_BLOCK, 1, 1};
    case NVN_FORMAT_RGTC2_SNORM: return {VK_FORMAT_BC5_SNORM_BLOCK, 1, 1};
    case NVN_FORMAT_BPTC_UNORM: return {VK_FORMAT_BC7_UNORM_BLOCK, 1, 1};
    case NVN_FORMAT_BPTC_UNORM_SRGB: return {VK_FORMAT_BC7_SRGB_BLOCK, 1, 1};
    case NVN_FORMAT_BPTC_SFLOAT: return {VK_FORMAT_BC6H_SFLOAT_BLOCK, 1, 1};
    case NVN_FORMAT_BPTC_UFLOAT: return {VK_FORMAT_BC6H_UFLOAT_BLOCK, 1, 1};
    case NVN_FORMAT_BGRX8: return {VK_FORMAT_B8G8R8A8_UNORM, 1, 1};
    case NVN_FORMAT_BGRX8_SRGB: return {VK_FORMAT_B8G8R8A8_SRGB, 1, 1};
    default: return {VK_FORMAT_UNDEFINED, 1, 1};
    }
}

bool is_depth_format(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_S8_UINT;
}
bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_S8_UINT;
}

// Bytes per block and block size of an NVN format's image: the texture code's table; ASTC's image is BC7
int nvn_block_size(int format, int* bw, int* bh) {
    int w = 1, h = 1;
    int bpb = host->fmt_bpb(format, &w, &h);
    if (nvn_astc(format)) bpb = 16, w = h = 4;
    if (bw) *bw = w;
    if (bh) *bh = h;
    return bpb;
}

} // namespace hsd
