// A recorded NVN list as the direct back end keeps it (M1: counts, and the ops the frame level needs, in order).
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "direct_api.h"
#include "direct_internal.h"
#include "../api/rx_nvn.h"

namespace hsd {

struct List;
/// One op the frame level needs: SIGNAL_EVENT / WAIT_EVENT (addr, mode, value), REPORT_COUNTER / RESET_COUNTER (counter),
/// CALL (callee)
struct FrameOp {
    uint32_t op = 0, mode = 0, value = 0;
    uint32_t seg = 0;   // (M4a: its list's segments before it: where it is among the list's commands)
    uint64_t addr = 0;
    nvn_p_counter counter{};
    List* callee = nullptr;
};
struct Image;
struct CmdSlot;
struct DumpItem;
struct ListDraw;   // (direct_draw.cpp: its state and draw caches)
struct ListComp;   // (direct_compute.cpp: its compute state)
struct ListQuery;  // (direct_query.cpp: its counter and condition)
/// A pass's attachment (direct_pass.cpp)
struct Target {
    Image* im = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;   // (UNDEFINED: no attachment there)
    VkImageAspectFlags aspect = 0;
    uint32_t level = 0, layer = 0, w = 0, h = 0;
};
/// A pass's formats, what a job's rendering secondary inherits (direct_pass.cpp's format table keeps them by pass key)
struct PassFormats {
    uint32_t n = 0;
    VkFormat color[8]{};
    VkFormat depth = VK_FORMAT_UNDEFINED, stencil = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags depth_aspect = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t w = 0, h = 0;   // (the size the job's draws assume: not part of the match)
    bool same(const PassFormats& o) const;
};
/// A list's recording state (direct_pass.cpp): the targets SetRenderTargets noted, whether the rendering is begun, the
/// clears waiting to be its load ops, and the NVN state clears and copies read
struct PassRec {
    bool noted = false, begun = false, rendered = false;
    int n = 0;
    Target color[8], depth;
    uint32_t w = 0, h = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool inherited = false;   // (M3a: a job's secondary: the rendering is its caller's)
    uint32_t load = 0;   // (bits 0-7 colour, 8 depth, 9 stencil: cleared by the load op at the begin)
    VkClearValue clear[10]{};
    int32_t scissor[4]{};
    float viewport[4]{};   // (M6b: viewport 0, kept as the scissor is: a pass with no target is drawn over its area)
    bool bare = false;     // (M6b: SetRenderTargets set no target: a rendering with no attachments, the viewport's area, as NVN draws it)
    bool render_enabled = true;
    int64_t copy_stride = 0;
    uint64_t set_rt_at = 0;   // (its SET_RT's number in the list, for the dump's names)
    uint64_t last_prog = 0;   // (M6b: the program of its rendering's last draw, a job's executed in it included: the dump's
                              // pass identity, as the frozen back end names its passes; 0 after each rendering's end)
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
    bool draws = false;      // (it or a callee draws: a call of it begins its caller's rendering, as a draw does)
    CmdSlot* slot = nullptr;
    VkCommandBuffer cur = VK_NULL_HANDLE;
    uint64_t thread = 0;   // (the thread its command buffers come from)
    PassRec pass;
    uint64_t set_rts = 0;
    uint32_t last_op = 0, last_op2 = 0;   // (the ops before the one being handled, for logs)
    std::vector<DumpItem*> dumps;   // (HOS_DBG=dump: its passes' copies, in order)
    ListDraw* dctx = nullptr;   // (M2b: made at its first state op)
    // 
    bool rec = false;         // (it records Vulkan commands: layer, gate, compute, head lists, and jobs with their pass's formats)
    bool secondary = false;   // (a job: one rendering secondary, cur, executed by its caller inside the caller's rendering)
    PassFormats fmt;          // (a secondary's: what it was begun with)
    VkCommandBuffer sec = VK_NULL_HANDLE;   // (a secondary, ended)
    bool packet = false;      // (load-time and unclassified lists: a packet, expanded into each caller at its call)
    std::vector<uint8_t> pkt; // (the packet: per op {op, size} then the payload, 8-byte aligned; callees in place)
    uint32_t pkt_ops = 0;
    bool cap_drawn = false;   // (HOS_DBG=capture: it renders into a 543x306 target)
    uint64_t counted = 0;     // (its bytes as counted to MO_M_LISTS)
    std::vector<const List*> executed;   // (secondaries executed in its renderings: their slots go with its submissions)
    uint32_t sec_draws = 0;   // (a secondary's draw ops so far: its first is timed apart)
    uint64_t last_prog = 0;   // (M6b: its last drawn program, a job's: its caller's pass takes it at the call)
    uint32_t n_drawn = 0, n_mesh = 0, n_step_a = 0;   // (its draws drawn, skipped as mesh draws, skipped by step A: per pass in the log)
    bool pending_barrier = false;   // (M2b: a barrier waiting for the list's next command: recorded with it)
    ListComp* cctx = nullptr;   // (M4a: made at its first compute state op)
    ListQuery* qctx = nullptr;  // (M5: made at its first counter or condition op)
    // (M4c: a job's task step, two plain secondaries of its slot holding the compute work of its task+mesh draws: the
    // headers' reset and the indirect draws' header making (task_pre), then the task dispatches (task_cb). Its caller executes
    // them before the job, outside its rendering, a barrier after each. Open until the list ends; the times it was executed)
    VkCommandBuffer task_pre = VK_NULL_HANDLE, task_cb = VK_NULL_HANDLE;
    bool task_open = false;
    mutable std::atomic<int32_t> task_runs{0};
};

void list_ref(List* l);
void list_unref(List* l);   // (any thread; the last one frees it and lets go of its callees)
/// The frame-level ops a submission of l runs, its callees' in place (l ended)
void list_flatten(const List* l, std::vector<const FrameOp*>& out);
/// The command buffers a submission of l runs, its callees' in place (l ended)
void list_buffers(const List* l, std::vector<VkCommandBuffer>& out, std::vector<CmdSlot*>& slots, std::vector<DumpItem*>* dumps);
/// A submission's work in its order: each item a command buffer or a frame-level op
struct SubItem {
    const FrameOp* op = nullptr;
    VkCommandBuffer cb = VK_NULL_HANDLE;
};
/// The command buffers and frame-level ops a submission of l runs, in their order, its callees' in place (l ended)
void list_items(const List* l, std::vector<SubItem>& out, std::vector<CmdSlot*>& slots, std::vector<DumpItem*>* dumps);
/// l handed to hsd_submit (counted by its return address)
void list_submitted(const List* l);

}  // namespace hsd
