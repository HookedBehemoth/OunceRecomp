// The descriptor heap (docs/direct/m2-interface.md, direct_heap; m2-shader-contract.md sections 2 and 6, step A 2):
// VK_EXT_descriptor_heap, the game's texture and sampler ids as heap indices. The resource heap is one region of 65,536
// image descriptors a VkImageViewType (a texture id is its index there), then a zero tail (null descriptors: RADV reads
// zeros as none). The sampler heap: two regions of 4,096 (plain, depth compare). Entries are written when the game
// registers the id (nvnTexturePoolRegisterTexture / RegisterSampler), on its thread, in every region the texture's target
// allows: the sampled views are encoded from their create info (the heap needs no VkImageView). An image that goes takes
// its entries with it (nulled). Beside each texture id its answer byte (the translator's texture type and integer bit),
// for the program variants' checks.
//
// Also the device part of M2b: the extensions and features the draws need (shader objects, the heap, dynamic state 3), and
// the device functions they call.
#pragma once

#include <cstdint>
#include <vector>

#include "direct_internal.h"

namespace hsd {

#define HSD_VK_M2B(X)                                                                                                  \
    X(vkCreateShadersEXT) X(vkDestroyShaderEXT) X(vkCmdBindShadersEXT) X(vkCmdSetVertexInputEXT)                       \
    X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference)    \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBias) X(vkCmdSetDepthBiasEnable)          \
    X(vkCmdSetLineWidth) X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetCullMode) X(vkCmdSetFrontFace)                     \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetDepthTestEnable)                          \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp)            \
    X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetPolygonModeEXT) X(vkCmdSetDepthClampEnableEXT)                        \
    X(vkCmdSetAlphaToCoverageEnableEXT) X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT)                \
    X(vkCmdSetColorWriteMaskEXT) X(vkCmdSetRasterizationSamplesEXT) X(vkCmdSetSampleMaskEXT)                           \
    X(vkCmdSetAlphaToOneEnableEXT) X(vkCmdSetLogicOpEnableEXT) X(vkCmdSetProvokingVertexModeEXT)                       \
    X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed)                                \
    X(vkWriteResourceDescriptorsEXT) X(vkWriteSamplerDescriptorsEXT) X(vkCmdBindResourceHeapEXT)                       \
    X(vkCmdBindSamplerHeapEXT) X(vkCmdPushDataEXT) X(vkBindBufferMemory) X(vkMapMemory)
/// (M4b) indirect and mesh draws; the mesh ones are null without the mesh stage (caps.mesh)
#define HSD_VK_M4B(X)                                                                                                  \
    X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount)        \
    X(vkCmdDrawMeshTasksEXT) X(vkCmdDrawMeshTasksIndirectEXT) X(vkCmdDrawMeshTasksIndirectCountEXT)
#define HSD_VK_DECLARE(f) extern PFN_##f f;
namespace vkfn {
HSD_VK_M2B(HSD_VK_DECLARE)
HSD_VK_M4B(HSD_VK_DECLARE)
}
#undef HSD_VK_DECLARE

/// What the device gives M2b's draws (set at device creation)
struct DrawCaps {
    bool ok = false;               // (shader objects, the heap, dynamic state 3: the draws can be made)
    const char* why = "not asked";
    bool provoking_vertex = false, index_uint8 = false, vertex_divisor = false, alpha_to_one = false, logic_op = false;
    VkShaderStageFlags sg32_stages = 0;   // (stages the GPU runs at 32 lanes when asked: the translator's profile follows)
    uint32_t subgroup_size = 64, subgroup_max = 64;
    uint32_t max_push = 0;
    VkDriverId driver_id{};
    bool mesh = false;   // (M4b: VK_EXT_mesh_shader's mesh stage (never the task stage))
};
extern DrawCaps caps;

/// (direct_main.cpp create_device) the extensions M2b needs, its feature structures chained at *tail
void draw_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail);
/// (after vkGetPhysicalDeviceFeatures2) what the device has; features not to enable turned off
void draw_features_resolved(const VkPhysicalDeviceVulkan14Features* f14);
/// (after vkCreateDevice) the functions, the heaps, the window table
void draw_device_created();

// ---- the heap ----
constexpr uint32_t kHeapSlots = 1u << 16;   // (texture ids a view type region holds)
constexpr uint32_t kHeapRegions = 7;        // (VkImageViewType 0..6: sampled views; then as many of storage views, M3b)
constexpr uint32_t kHeapSamplers = 4096;    // (12-bit sampler ids)
constexpr uint32_t kImageDesc = 64, kSamplerDesc = 16;

bool heap_on();
/// Byte offsets of a region's first descriptor, of the zero tail, of a sampler region
uint32_t heap_region_offset(uint32_t region);
/// (M3b) the storage-image region of a view type (images bound by draws read it, indexed by the handle's image id)
uint32_t heap_storage_offset(uint32_t region);
uint32_t heap_tail_offset();
uint32_t heap_sampler_offset(bool compare);
/// The heaps bound on a command buffer (before its first heap draw)
void heap_bind(VkCommandBuffer cb);
void heap_push(VkCommandBuffer cb, const void* data, uint32_t bytes);

/// (the host's registrations, any thread)
void heap_texture_registered(int id, const nvn_tex* t, const nvn_view* v);
void heap_sampler_registered(int id, const nvn_samp_par* p);
struct Image;
/// (direct_image.cpp, an image destroyed) the entries that hold it nulled
void heap_image_gone(const Image* im);
/// The answer byte of texture id: the translator's texture type | 0x80 for an integer format (0xFF: none registered)
uint8_t heap_answer(uint32_t id);
/// (logs) what texture id's entries hold
void heap_describe(uint32_t id, char* out, size_t cap);
/// (M4h, logs) what sampler id was registered with
void heap_sampler_describe(uint32_t id, char* out, size_t cap);
/// (M4d, HOS_DBG=phlog) 1: id is registered with the placeholder image, 2: registered with no image; 0 otherwise (locked)
int heap_placeholder(uint32_t id);

void heap_stats(char* out, size_t cap);

}  // namespace hsd
