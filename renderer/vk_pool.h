// NVN's texture and sampler pools as one descriptor set (descriptor indexing, update after bind), the game's texture and
// sampler ids as array indexes. Per Vulkan view type a binding of kPoolSlots sampled images and one of storage images, then
// the sampler pool twice, plain and depth compare (the translator's emit_spirv.h kPool*). A registration
// (nvnTexturePoolRegisterTexture / RegisterSampler, on the game's thread) writes the entries of every view type the
// texture's target allows, from views cached on its image, and the samplers from a cache by their create info; one that
// holds what the id already holds writes nothing. Every other entry is null (robustness2's null descriptors); slot
// kPoolNullSlot always is, and the shaders clamp image ids to it. An image that goes takes its entries with it (nulled).
// Uniform and storage buffers are read by device address: the set holds images and samplers only. Beside each texture id
// its answer byte (the translator's texture type and integer bit), for the draws' texture checks.
//
// Also the device part of M2b: the extensions and features the draws need (shader objects, descriptor indexing, null
// descriptors, dynamic state 3), and the device functions they call.
#pragma once

#include <cstdint>
#include <vector>

#include "vk_internal.h"

namespace hsd {

#define GPU_VK_M2B(X)                                                                                                  \
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
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets)     \
    X(vkUpdateDescriptorSets) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCreateSampler)                      \
    X(vkBindBufferMemory) X(vkMapMemory)
/// indirect and mesh draws; the mesh ones are null without the mesh stage (caps.mesh)
#define GPU_VK_M4B(X)                                                                                                  \
    X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount)        \
    X(vkCmdDrawMeshTasksEXT) X(vkCmdDrawMeshTasksIndirectEXT) X(vkCmdDrawMeshTasksIndirectCountEXT)
#define GPU_VK_DECLARE(f) extern PFN_##f f;
namespace vkfn {
GPU_VK_M2B(GPU_VK_DECLARE)
GPU_VK_M4B(GPU_VK_DECLARE)
}
#undef GPU_VK_DECLARE

/// What the device gives M2b's draws (set at device creation)
struct DrawCaps {
    bool ok = false;               // (shader objects, the pools' set, dynamic state 3: the draws can be made)
    const char* why = "not asked";
    bool provoking_vertex = false, index_uint8 = false, vertex_divisor = false, alpha_to_one = false, logic_op = false;
    VkShaderStageFlags sg32_stages = 0;   // (stages the GPU runs at 32 lanes when asked: the translator's profile follows)
    uint32_t subgroup_size = 64, subgroup_max = 64;
    uint32_t max_push = 0;
    VkDriverId driver_id{};
    bool mesh = false;   // (M4b: VK_EXT_mesh_shader's mesh stage (never the task stage))
    bool maximal_reconvergence = false;   // (VK_KHR_shader_maximal_reconvergence, enabled when the device has it)
    bool mesh_queries = false;   // (M6h: mesh shader invocations counted, for debug.pstats only)
};
extern DrawCaps caps;

/// (vk_main.cpp create_device) the extensions M2b needs, its feature structures chained at *tail
void draw_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail);
/// (after vkGetPhysicalDeviceFeatures2) what the device has; features not to enable turned off; caps.why names the first
/// requirement it misses
void draw_features_resolved(const VkPhysicalDeviceFeatures& f10, VkPhysicalDeviceVulkan12Features& f12,
                            const VkPhysicalDeviceVulkan14Features* f14);
/// (after vkCreateDevice) the functions, the pools' set, the window table
void draw_device_created();

// ---- the pools' set ----
bool pool_on();
/// The set bound on a command buffer at a bind point (before its first draw or dispatch there)
void pool_bind(VkCommandBuffer cb, VkPipelineBindPoint point);
void pool_push(VkCommandBuffer cb, const void* data, uint32_t bytes);
/// A shader object's layout: the set and the push constant range every shader object shares
void pool_shader_layout(VkShaderCreateInfoEXT& ci);

/// (the host's registrations, any thread)
void pool_texture_registered(int id, const nvn_tex* t, const nvn_view* v);
void pool_sampler_registered(int id, const nvn_samp_par* p);
struct Image;
/// (vk_image.cpp, an image destroyed once the GPU is done with it) the entries that hold it nulled
void pool_image_gone(const Image* im);
/// The answer byte of texture id: the translator's texture type | 0x80 for an integer format (0xFF: none registered)
uint8_t pool_answer(uint32_t id);
/// (logs) what texture id's entries hold
void pool_describe(uint32_t id, char* out, size_t cap);
/// (logs) what sampler id was registered with
void pool_sampler_describe(uint32_t id, char* out, size_t cap);
/// (debug.phlog) 1: id is registered with the placeholder image, 2: registered with no image; 0 otherwise (locked)
int pool_placeholder(uint32_t id);

/// (the transient pool) 1: id names an image in the pool that is not its range's owner (another target took its memory:
/// its content is that target's); the ids that do, for the 600-present block
int pool_stale(uint32_t id);
void pool_stale_census(char* out, size_t cap);

void pool_stats(char* out, size_t cap);

}  // namespace hsd
