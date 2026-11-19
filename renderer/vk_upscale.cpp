// The host's upscale (vk_upscale.h).
#include "vk_upscale.h"

#include <atomic>
#include <cstring>

#include "vk_barrier.h"
#include "vk_check.h"
#include "vk_compute.h"
#include "vk_draw.h"
#include "vk_pass.h"
#include "vk_passtime.h"
#include "vk_pool.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include <shaders/upscale_spv.h>

namespace hsd {

namespace {

using namespace Shader::Backend::SPIRV;   // (the set's layout: kPool*)
static_assert(kPoolSampledBinding + VK_IMAGE_VIEW_TYPE_2D == 1 && kPoolStorageBinding + VK_IMAGE_VIEW_TYPE_2D == 8,
              "upscale.comp's bindings");

VkShaderEXT shader = VK_NULL_HANDLE;
PFN_vkCmdDispatch pDispatch = nullptr;
std::atomic<int> told{0};

struct Push {   // (upscale.comp's)
    uint32_t src, dst;
    uint32_t src_size[2], dst_size[2];
    uint32_t kind;
};

/// The colour's writes, by a rendering or a dispatch, and the reads and writes of the output before, ahead of the dispatch
constexpr Dep kBeforeUpscale{VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0};

void refuse(const char* why, const gpu_upscale_args* a) {
    if (told++ < 8)
        logf("direct: upscale %ux%u (texture %u) to %ux%u (image %u), filter %u: not recorded, %s", a->src_w, a->src_h, a->src_id,
             a->dst_w, a->dst_h, a->dst_id, a->filter, why);
    strict_event("upscale not recorded", "%s", why);
}

}  // namespace

void upscale_device_created() {
    pDispatch = (PFN_vkCmdDispatch)vkGetDeviceProcAddr(dev.device, "vkCmdDispatch");
    if (!pool_on()) return;
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    pool_shader_layout(ci);
    ci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    ci.codeSize = sizeof upscale_spv;
    ci.pCode = upscale_spv;
    ci.pName = "main";
    const VkResult r = vkCreateShadersEXT(dev.device, 1, &ci, nullptr, &shader);
    if (r == VK_SUCCESS) object_made(1, "the upscale shader");
    else logf("direct: the upscale shader was not made (%d)", (int)r), shader = VK_NULL_HANDLE;
}

}  // namespace hsd

using namespace hsd;

extern "C" MKWVK_EXPORT void gpu_upscale(void* list, const gpu_upscale_args* a) {
    List* l = (List*)list;
    if (!shader) return refuse("no shader", a);
    if (!l || !l->rec || l->secondary) return refuse("not in a list that records outside a rendering", a);
    if (a->filter != GPU_UPSCALE_BILINEAR && a->filter != GPU_UPSCALE_CATMULL_ROM) return refuse("no such filter", a);
    if (a->src_id >= kPoolNullSlot || a->dst_id >= kPoolNullSlot || !a->src_w || !a->src_h || !a->dst_w || !a->dst_h)
        return refuse("an id or a size out of range", a);
    const VkCommandBuffer cb = pass_dispatch(l, kBeforeUpscale);
    const Push push{a->src_id, a->dst_id, {a->src_w, a->src_h}, {a->dst_w, a->dst_h}, a->filter};
    pool_bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE);
    const VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT;
    vkCmdBindShadersEXT(cb, 1, &st, &shader);
    pool_push(cb, &push, sizeof push);
    const uint32_t gx = (a->dst_w + 7) / 8, gy = (a->dst_h + 7) / 8;
    pDispatch(cb, gx, gy, 1);
    l->after_dispatch = true;
    compute_invalidate(l);   // (the bound compute program and the push data are this dispatch's now)
    draw_push_invalidate(l);
    if (passtime_on()) {
        PassLabel pl;
        pl.what = PT_DISPATCH, pl.kind = l->info.kind, pl.lr = l->info.lr, pl.w = gx, pl.h = gy, pl.targets = 1;
        pl.prog = 0x7570736361000000ull | a->filter;   // ("upsca" and the filter: the profile's name for the pass)
        passtime_end(cb, pl);
    }
}
