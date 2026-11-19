// NVN's barriers as Vulkan memory dependencies (vk_barrier.h has the table).
#include "vk_barrier.h"

#include "api/nvn_api.h"
#include "vk_internal.h"

namespace hsd {
namespace {

constexpr VkPipelineStageFlags2 kGraphics = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kCopies = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
constexpr VkPipelineStageFlags2 kFragments = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

constexpr VkAccessFlags2 kAttachmentWrites = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags2 kAttachments = kAttachmentWrites | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
constexpr VkAccessFlags2 kStorageWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr VkAccessFlags2 kTransfers = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

}  // namespace

const Dep kAfterDispatch{kCompute, kGraphics | kCopies, kStorageWrite, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, 0};
const Dep kAfterTransfer{kCopies, kGraphics | kCompute | kCopies, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, 0};

Dep nvn_barrier_dep(int32_t bits) {
    Dep d;
    if (bits & NVN_BARRIER_ORDER_PRIMITIVES_BIT) {
        d.src |= kGraphics | kCompute, d.dst |= kGraphics | kCompute | kCopies;
        d.src_access |= kAttachmentWrites | kStorageWrite;
        d.dst_access |= kAttachments | kStorageWrite | kTransfers | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
    }
    if (bits & (NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT)) {
        d.src |= kFragments, d.dst |= kFragments | kCompute | kCopies;
        d.src_access |= kAttachmentWrites | kStorageWrite;
        d.dst_access |= kAttachments | kStorageWrite | kTransfers;
    }
    if (bits & NVN_BARRIER_ORDER_INDIRECT_DATA_BIT) {
        d.src |= kGraphics | kCompute, d.dst |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        d.src_access |= kAttachmentWrites | kStorageWrite;
        d.dst_access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    }
    if (d.empty()) return d;
    VkAccessFlags2 reads = 0;
    if (bits & NVN_BARRIER_INVALIDATE_TEXTURE_BIT) reads |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    if (bits & NVN_BARRIER_INVALIDATE_SHADER_BIT)
        reads |= VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (reads && !(d.dst & (kGraphics | kCompute) & ~VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT)) d.dst |= kGraphics | kCompute;
    d.dst_access |= reads;
    // (by region: the tiled order alone)
    if ((bits & (NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_INDIRECT_DATA_BIT)) == 0)
        d.flags = VK_DEPENDENCY_BY_REGION_BIT;
    return d;
}

void record_dep(VkCommandBuffer cb, const Dep& d) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = d.src, b.srcAccessMask = d.src_access, b.dstStageMask = d.dst, b.dstAccessMask = d.dst_access;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.dependencyFlags = d.flags;
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

}  // namespace hsd
