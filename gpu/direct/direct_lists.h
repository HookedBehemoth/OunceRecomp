// A recorded NVN list as the direct back end keeps it (M1: counts, and the ops the frame level needs, in order).
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "direct_api.h"
#include "direct_internal.h"
#include "../render/rx_nvn.h"

namespace hsd {

struct List;
/// One op the frame level needs: SIGNAL_EVENT / WAIT_EVENT (addr, mode, value), REPORT_COUNTER / RESET_COUNTER (counter),
/// CALL (callee)
struct FrameOp {
    uint32_t op = 0, mode = 0, value = 0;
    uint64_t addr = 0;
    nvn_p_counter counter{};
    List* callee = nullptr;
};
struct Image;
struct CmdSlot;
struct DumpItem;
/// A pass's attachment (direct_pass.cpp)
struct Target {
    Image* im = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t level = 0, layer = 0, w = 0, h = 0;
};
/// A list's recording state (direct_pass.cpp): the targets SetRenderTargets noted, whether the rendering is begun, the
/// clears waiting to be its load ops, and the NVN state clears and copies read
struct PassRec {
    bool noted = false, begun = false, rendered = false;
    int n = 0;
    Target color[8], depth;
    uint32_t w = 0, h = 0;
    uint32_t load = 0;   // (bits 0-7 colour, 8 depth, 9 stencil: cleared by the load op at the begin)
    VkClearValue clear[10]{};
    int32_t scissor[4]{};
    bool render_enabled = true;
    int64_t copy_stride = 0;
    uint64_t set_rt_at = 0;   // (its SET_RT's number in the list, for the dump's names)
};
/// A list's commands in call order: its own command buffers and the callees (that have commands) between them
struct Seg {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    const List* callee = nullptr;
};
struct List {
    std::atomic<int32_t> refs{1};   // (its handle, each caller that calls it, each submission not run yet)
    hsd_list_info info{};
    int32_t site = -1;   // (its return address's entry in the table of the 600-present line)
    bool ended = false;
    uint32_t counts[NOP_COUNT_]{};   // (records by op; the recording thread's alone until it is ended)
    std::vector<FrameOp> ops;
    // (M2a: real command buffers; the recording thread's alone until it is ended)
    std::vector<Seg> segs;
    bool has_cmds = false;   // (it or a callee has a command buffer)
    CmdSlot* slot = nullptr;
    VkCommandBuffer cur = VK_NULL_HANDLE;
    uint64_t thread = 0;   // (the thread its command buffers come from)
    PassRec pass;
    uint64_t set_rts = 0;
    std::vector<DumpItem*> dumps;   // (HOS_DBG=dump: its passes' copies, in order)
};

void list_ref(List* l);
void list_unref(List* l);   // (any thread; the last one frees it and lets go of its callees)
/// The frame-level ops a submission of l runs, its callees' in place (l ended)
void list_flatten(const List* l, std::vector<const FrameOp*>& out);
/// The command buffers a submission of l runs, its callees' in place (l ended)
void list_buffers(const List* l, std::vector<VkCommandBuffer>& out, std::vector<CmdSlot*>& slots, std::vector<DumpItem*>* dumps);
/// l handed to hsd_submit (counted by its return address)
void list_submitted(const List* l);

}  // namespace hsd
