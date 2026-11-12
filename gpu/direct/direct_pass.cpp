// A list's Vulkan commands at M2a (docs/direct/m2-interface.md, direct_pass): SetRenderTargets notes the targets; the
// rendering is begun at the first draw, job call or call of a list that draws (the load-time draw lists) after it, so the clears between become its load ops; a clear after
// that, or one with a scissor short of the whole area, is vkCmdClearAttachments. A barrier, a dispatch, a copy or a
// texture clear ends the rendering (the targets stay set: the next draw begins it again, loading); the next
// SetRenderTargets or the list's end ends the pass. A barrier is the game's NVN barrier as one pipeline barrier.
// Everything is in VK_IMAGE_LAYOUT_GENERAL. Draws, dispatches, mesh draws and conditions are counted and skipped.
//
// Lists of the layer, gate, compute and queue-head kinds record into command buffers of their thread's slot
// (direct_cmd.h). Job, load and unclassified lists record none at this stage (jobs as secondaries and packets are M3):
// what they would have recorded is counted. A call of a list that has command buffers cuts the caller's: the rendering
// is ended, the caller's buffer too, and the callee's buffers run between (direct_lists.cpp list_buffers).
#include <pthread.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "direct_cmd.h"
#include "direct_dump.h"
#include "direct_image.h"
#include "direct_pass.h"

namespace hsd {
namespace {

enum {
    S_SET_RT, S_EMPTY_RT, S_NO_IMAGE, S_BEGUN, S_LOAD_CLEARS, S_CLEARS_IN, S_MASKED, S_CLEAR_NO_TARGET, S_CLEAR_DISABLED,
    S_DRAWS, S_DISPATCHES, S_BARRIERS, S_COPIES, S_COPY_SKIPPED, S_CLEAR_TEX, S_CLEAR_TEX_SKIPPED, S_BUF_OPS, S_UNRECORDED,
    S_CUTS, S_OTHER_THREAD, S_COUNT
};
const char* const kNames[S_COUNT] = {
    "SetRenderTargets", "with no target", "targets with no image", "renderings begun", "clears as load ops",
    "clears in a rendering", "masked clears skipped", "clears with no target", "clears not rendered", "draws skipped",
    "dispatches skipped", "barriers", "copies", "copies skipped", "texture clears", "texture clears skipped",
    "buffer ops skipped", "ops of lists that record nothing (job, load, packet)", "calls that cut a rendering",
    "records on another thread"};
std::atomic<int64_t> st[S_COUNT];
std::atomic<int64_t> st_bits[16];   // (barriers by NVN bit)
void count(int k) { st[k].fetch_add(1, std::memory_order_relaxed); }

uint64_t thread_id() { return (uint64_t)pthread_self(); }

bool records(const List* l) {
    const uint32_t k = l->info.kind;
    return k == HSD_LIST_LAYER || k == HSD_LIST_GATE || k == HSD_LIST_COMPUTE || k == HSD_LIST_HEAD;
}

VkCommandBuffer cmd(List* l) {
    if (l->cur) {
        if (l->thread != thread_id()) count(S_OTHER_THREAD);
        return l->cur;
    }
    if (!l->slot) l->slot = cmd_slot_take(), l->thread = thread_id();
    else if (l->thread != thread_id()) count(S_OTHER_THREAD);
    l->cur = cmd_buffer(l->slot);
    l->segs.push_back({l->cur, nullptr});
    l->has_cmds = true;
    return l->cur;
}

void begin(List* l) {
    PassRec& p = l->pass;
    VkCommandBuffer cb = cmd(l);
    VkRenderingAttachmentInfo ca[8]{}, da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    for (int i = 0; i < p.n; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = p.color[i].view;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = p.load >> i & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca[i].clearValue = p.clear[i];
        if (p.color[i].im) p.color[i].im->written.fetch_or(W_ATTACHMENT, std::memory_order_relaxed);
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {p.w, p.h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = (uint32_t)p.n;
    ri.pColorAttachments = ca;
    VkRenderingAttachmentInfo sa = da;
    if (p.depth.view) {
        const VkImageAspectFlags a = p.depth.im->aspect;
        da.imageView = sa.imageView = p.depth.view;
        da.imageLayout = sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.storeOp = sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.loadOp = p.load >> 8 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        sa.loadOp = p.load >> 9 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        da.clearValue = sa.clearValue = p.clear[8];
        if (a & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &da;
        if (a & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &sa;
        p.depth.im->written.fetch_or(W_ATTACHMENT, std::memory_order_relaxed);
    }
    vkCmdBeginRendering(cb, &ri);
    p.begun = p.rendered = true;
    p.load = 0;
    count(S_BEGUN);
}

/// The rendering ended, its waiting clears done first; the targets stay noted
void suspend(List* l) {
    PassRec& p = l->pass;
    if (!p.noted) return;
    if (!p.begun && p.load) begin(l);
    if (!p.begun) return;
    vkCmdEndRendering(l->cur);
    p.begun = false;
    dump_pass(l, l->cur);
}
void end_pass(List* l) {
    suspend(l);
    l->pass.noted = false;
}

void set_rt(List* l, const nvn_p_set_rt& r) {
    end_pass(l);
    count(S_SET_RT);
    PassRec& p = l->pass;
    const int32_t scissor[4] = {p.scissor[0], p.scissor[1], p.scissor[2], p.scissor[3]};
    const bool enabled = p.render_enabled;
    const int64_t stride = p.copy_stride;
    p = PassRec{};
    std::memcpy(p.scissor, scissor, sizeof scissor);
    p.render_enabled = enabled, p.copy_stride = stride;
    p.set_rt_at = l->set_rts++;
    p.n = std::clamp(r.n, 0, 8);
    p.w = p.h = UINT32_MAX;
    bool any = false;
    auto target = [&](const nvn_tex* t, const nvn_view* v, Target& out) {
        if (!t) return;
        Image* im = image_find(t);
        const uint32_t level = v && v->set_levels ? v->base_level : 0, layer = v && v->set_layers ? v->min_layer : 0;
        out.view = im ? image_attachment_view(im, image_view_format(t, v), level, layer, &out.format) : VK_NULL_HANDLE;
        if (!out.view) return count(S_NO_IMAGE);
        out.im = im, out.level = level, out.layer = layer;
        out.w = std::max(1u, im->width >> level), out.h = std::max(1u, im->height >> level);
        p.w = std::min(p.w, out.w), p.h = std::min(p.h, out.h);
        any = true;
    };
    for (int i = 0; i < p.n; i++) target(r.color[i], r.has_cview[i] ? &r.cview[i] : nullptr, p.color[i]);
    target(r.depth, r.has_dview ? &r.dview : nullptr, p.depth);
    static const bool log_rt = dbg("rtlog");   // (HOS_DBG=rtlog: 200 SetRenderTargets from present 1500 on, named)
    static std::atomic<int> logged{0};
    if (log_rt && present_count() >= 1500 && logged++ < 200) {
        char b[1024];
        int k = std::snprintf(b, sizeof b, "direct: rtlog present %llu list %u lr %#llx rt %llu n %d:", (unsigned long long)present_count(),
                              l->info.kind, (unsigned long long)l->info.lr, (unsigned long long)p.set_rt_at, p.n);
        for (int i = 0; i < p.n && k < 900; i++)
            k += std::snprintf(b + k, sizeof b - k, " [%#llx L%u l%u %s]", r.color[i] ? (unsigned long long)r.color[i]->gpu : 0ull,
                               p.color[i].level, p.color[i].layer, p.color[i].view ? "ok" : "none");
        if (r.depth) std::snprintf(b + k, sizeof b - k, " depth %#llx %s", (unsigned long long)r.depth->gpu, p.depth.view ? "ok" : "none");
        logf("%s", b);
    }
    if (!any) return count(S_EMPTY_RT);
    p.noted = true;
}

/// The clear's rectangle: the scissor's, within the area (whole: no scissor or one past the area)
VkRect2D clear_rect(const PassRec& p, bool& whole) {
    VkRect2D r{{0, 0}, {p.w, p.h}};
    whole = true;
    if (p.scissor[2] > 0 && p.scissor[3] > 0) {
        const int32_t x0 = std::clamp(p.scissor[0], 0, (int32_t)p.w), y0 = std::clamp(p.scissor[1], 0, (int32_t)p.h);
        const int32_t x1 = std::clamp(p.scissor[0] + p.scissor[2], 0, (int32_t)p.w);
        const int32_t y1 = std::clamp(p.scissor[1] + p.scissor[3], 0, (int32_t)p.h);
        r = {{x0, y0}, {(uint32_t)std::max(0, x1 - x0), (uint32_t)std::max(0, y1 - y0)}};
        whole = !x0 && !y0 && r.extent.width == p.w && r.extent.height == p.h;
    }
    return r;
}

void clear(List* l, uint32_t bit_mask, VkImageAspectFlags aspect, uint32_t colour_index, const VkClearValue& v) {
    PassRec& p = l->pass;
    if (!p.render_enabled) return count(S_CLEAR_DISABLED);
    bool whole;
    const VkRect2D rect = clear_rect(p, whole);
    if (!rect.extent.width || !rect.extent.height) return;
    if (!p.begun && whole) {
        for (int b = 0; b < 10; b++)
            if (bit_mask >> b & 1) p.clear[b < 9 ? b : 8] = v;
        if (bit_mask & 0x300) p.clear[8] = v;
        p.load |= bit_mask;
        return count(S_LOAD_CLEARS);
    }
    if (!p.begun) begin(l);
    VkClearAttachment ca{};
    ca.aspectMask = aspect;
    ca.colorAttachment = colour_index;
    ca.clearValue = v;
    const VkClearRect cr{rect, 0, 1};
    vkCmdClearAttachments(l->cur, 1, &ca, 1, &cr);
    count(S_CLEARS_IN);
}

void barrier(List* l, int32_t bits) {
    suspend(l);
    for (int b = 0; b < 16; b++)
        if (bits >> b & 1) st_bits[b].fetch_add(1, std::memory_order_relaxed);
    // (every NVN barrier as one full memory barrier: the order bits wait for earlier work, the invalidate bits make its
    // writes visible; Vulkan has no invalidate without a wait, and images are all GENERAL, so no image barriers)
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd(l), &d);
    count(S_BARRIERS);
}

void copy_bt(List* l, const nvn_p_copy_bt& p, bool to_image) {
    Sub s;
    if (!image_sub(p.tex, p.has_view ? &p.view : nullptr, p.r, s) || (to_image && s.im->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)))
        return count(S_COPY_SKIPPED);
    const uint64_t wb = (s.w + s.bw - 1) / s.bw, hb = (s.h + s.bh - 1) / s.bh;
    const uint64_t row = l->pass.copy_stride > 0 ? (uint64_t)l->pass.copy_stride : wb * s.bpb;
    if (row % s.bpb || row < wb * s.bpb) return count(S_COPY_SKIPPED);
    const uint64_t size = row * (hb * s.layers * s.d - 1) + wb * s.bpb;
    VkBuffer buf;
    VkDeviceSize off;
    if (!layer0_span(p.buf, size, buf, off)) return count(S_COPY_SKIPPED);
    suspend(l);
    VkBufferImageCopy c{};
    c.bufferOffset = off;
    c.bufferRowLength = (uint32_t)(row / s.bpb) * (uint32_t)s.bw;
    c.bufferImageHeight = (uint32_t)hb * (uint32_t)s.bh;
    c.imageSubresource = {s.aspect, s.level, s.layer, s.layers};
    c.imageOffset = {s.x, s.y, s.z};
    c.imageExtent = {s.w, s.h, s.d};
    if (to_image) {
        vkCmdCopyBufferToImage(cmd(l), buf, s.im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        s.im->written.fetch_or(W_COPY, std::memory_order_relaxed);
    } else {
        vkCmdCopyImageToBuffer(cmd(l), s.im->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    }
    count(S_COPIES);
}

void copy_tt(List* l, const nvn_p_copy_tt& p) {
    Sub s, d;
    if (!image_sub(p.src, p.has_sv ? &p.sv : nullptr, p.sr, s) || !image_sub(p.dst, p.has_dv ? &p.dv : nullptr, p.dr, d) ||
        (s.im == d.im && s.level == d.level && s.layer == d.layer))
        return count(S_COPY_SKIPPED);
    const bool colour = s.aspect == VK_IMAGE_ASPECT_COLOR_BIT && d.aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    if (!colour && s.im->format != d.im->format) return count(S_COPY_SKIPPED);
    s.layers = d.layers = std::min(s.layers, d.layers);
    const bool same = s.w == d.w && s.h == d.h && s.d == d.d;
    const VkImageSubresourceLayers sl{s.aspect, s.level, s.layer, s.layers}, dl{d.aspect, d.level, d.layer, d.layers};
    suspend(l);
    VkCommandBuffer cb = cmd(l);
    if (s.im->samples != VK_SAMPLE_COUNT_1_BIT && d.im->samples == VK_SAMPLE_COUNT_1_BIT && same) {
        const VkImageResolve r{sl, {s.x, s.y, s.z}, dl, {d.x, d.y, d.z}, {s.w, s.h, s.d}};
        vkCmdResolveImage(cb, s.im->image, VK_IMAGE_LAYOUT_GENERAL, d.im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else if (same && s.bpb == d.bpb && s.bw == d.bw && s.bh == d.bh && s.im->samples == d.im->samples) {
        const VkImageCopy c{sl, {s.x, s.y, s.z}, dl, {d.x, d.y, d.z}, {s.w, s.h, s.d}};
        vkCmdCopyImage(cb, s.im->image, VK_IMAGE_LAYOUT_GENERAL, d.im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else if (colour && !s.im->compressed && !d.im->compressed && s.im->samples == d.im->samples) {
        VkImageBlit b{};
        b.srcSubresource = sl, b.dstSubresource = dl;
        b.srcOffsets[0] = {s.x, s.y, s.z}, b.srcOffsets[1] = {s.x + (int32_t)s.w, s.y + (int32_t)s.h, s.z + (int32_t)s.d};
        b.dstOffsets[0] = {d.x, d.y, d.z}, b.dstOffsets[1] = {d.x + (int32_t)d.w, d.y + (int32_t)d.h, d.z + (int32_t)d.d};
        vkCmdBlitImage(cb, s.im->image, VK_IMAGE_LAYOUT_GENERAL, d.im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       p.flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        return count(S_COPY_SKIPPED);
    }
    d.im->written.fetch_or(W_COPY, std::memory_order_relaxed);
    count(S_COPIES);
}

void clear_tex(List* l, const nvn_p_clear_tex& p) {
    Sub s;
    if (!image_sub(p.tex, p.has_view ? &p.view : nullptr, p.r, s) || !(s.im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ||
        s.im->compressed)
        return count(S_CLEAR_TEX_SKIPPED);
    const uint32_t lw = std::max(1u, s.im->width >> s.level), lh = std::max(1u, s.im->height >> s.level);
    if (s.x || s.y || s.w != lw || s.h != lh || s.im->type == VK_IMAGE_TYPE_3D) return count(S_CLEAR_TEX_SKIPPED);
    suspend(l);
    VkClearColorValue c;
    std::memcpy(&c, p.c.u, 16);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, s.level, 1, s.layer, s.layers};
    vkCmdClearColorImage(cmd(l), s.im->image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    s.im->written.fetch_or(W_CLEAR, std::memory_order_relaxed);
    count(S_CLEAR_TEX);
}

template <class T>
bool payload(const void* p, uint32_t size, T& out) {
    if (size < sizeof out) return false;
    std::memcpy(&out, p, sizeof out);
    return true;
}

}  // namespace

void pass_emit(List* l, uint32_t op, const void* data, uint32_t size) {
    PassRec& p = l->pass;
    switch (op) {   // (state that clears and copies read: kept in every list)
    case NOP_SCISSOR: {
        nvn_p_scissor s;
        if (payload(data, size, s)) p.scissor[0] = s.x, p.scissor[1] = s.y, p.scissor[2] = s.w, p.scissor[3] = s.h;
        return;
    }
    case NOP_RENDER_ENABLE: {
        nvn_p_flag f;
        if (payload(data, size, f)) p.render_enabled = f.value != 0;
        return;
    }
    case NOP_COPY_STRIDE: {
        nvn_p_copy_stride c;
        if (payload(data, size, c)) p.copy_stride = c.stride;
        return;
    }
    case NOP_DRAW: case NOP_DRAW_IDX: case NOP_DRAW_INDIRECT: case NOP_DRAW_MESH:
        count(S_DRAWS);
        l->draws = true;
        if (records(l) && p.noted && !p.begun) begin(l);
        return;
    case NOP_DISPATCH: case NOP_DISPATCH_INDIRECT:
        count(S_DISPATCHES);
        if (records(l)) suspend(l);
        return;
    case NOP_CLEAR_BUF: case NOP_COPY_B2B: count(S_BUF_OPS); return;
    case NOP_SET_RT: case NOP_CLEAR_COLOR: case NOP_CLEAR_DS: case NOP_BARRIER: case NOP_COPY_B2T: case NOP_COPY_T2B:
    case NOP_COPY_T2T: case NOP_CLEAR_TEX:
        if (!records(l)) return count(S_UNRECORDED);
        break;
    default: return;
    }
    switch (op) {
    case NOP_SET_RT: {
        nvn_p_set_rt r;
        if (payload(data, size, r)) set_rt(l, r);
        break;
    }
    case NOP_CLEAR_COLOR: {
        nvn_p_clear_color c;
        if (!payload(data, size, c) || !(c.mask & 0xF)) break;
        if (!p.noted || c.index < 0 || c.index >= p.n || !p.color[c.index].view) return count(S_CLEAR_NO_TARGET);
        if ((c.mask & 0xF) != 0xF) return count(S_MASKED);
        VkClearValue v;
        std::memcpy(&v.color, c.rgba, 16);
        clear(l, 1u << c.index, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c.index, v);
        break;
    }
    case NOP_CLEAR_DS: {
        nvn_p_clear_ds c;
        if (!payload(data, size, c)) break;
        if (!p.noted || !p.depth.view) return count(S_CLEAR_NO_TARGET);
        const VkImageAspectFlags a = p.depth.im->aspect;
        uint32_t bits = 0;
        VkImageAspectFlags aspect = 0;
        if (c.depth_mask && (a & VK_IMAGE_ASPECT_DEPTH_BIT)) bits |= 1u << 8, aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (c.stencil_mask && (a & VK_IMAGE_ASPECT_STENCIL_BIT)) bits |= 1u << 9, aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!bits) break;
        if (bits & (1u << 9) && (c.stencil_mask & 0xFF) != 0xFF) count(S_MASKED);   // (cleared whole, as the frozen back end)
        VkClearValue v;
        v.depthStencil = {std::clamp(c.depth, 0.0f, 1.0f), (uint32_t)c.stencil & 0xFF};
        clear(l, bits, aspect, 0, v);
        break;
    }
    case NOP_BARRIER: {
        int32_t bits;
        if (payload(data, size, bits)) barrier(l, bits);
        break;
    }
    case NOP_COPY_B2T: case NOP_COPY_T2B: {
        nvn_p_copy_bt c;
        if (payload(data, size, c)) copy_bt(l, c, op == NOP_COPY_B2T);
        break;
    }
    case NOP_COPY_T2T: {
        nvn_p_copy_tt c;
        if (payload(data, size, c)) copy_tt(l, c);
        break;
    }
    case NOP_CLEAR_TEX: {
        nvn_p_clear_tex c;
        if (payload(data, size, c)) clear_tex(l, c);
        break;
    }
    default: break;
    }
}

void pass_call(List* l, const List* callee) {
    if (callee->draws) l->draws = true;
    if ((callee->info.kind == HSD_LIST_JOB || callee->draws) && records(l) && l->pass.noted && !l->pass.begun) begin(l);
    if (!callee->has_cmds) return;
    if (l->pass.begun) count(S_CUTS);
    suspend(l);
    if (l->cur) vkEndCommandBuffer(l->cur), l->cur = VK_NULL_HANDLE;
    l->segs.push_back({VK_NULL_HANDLE, callee});
    l->has_cmds = true;
}

void pass_list_end(List* l) {
    if (records(l)) end_pass(l);
    if (l->cur) vkEndCommandBuffer(l->cur), l->cur = VK_NULL_HANDLE;
}

void pass_stats(char* out, size_t cap, double frames) {
    int k = std::snprintf(out, cap, "passes a frame:");
    for (int i = 0; i < S_COUNT; i++) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "%s %s %.1f", i ? "," : "", kNames[i], st[i].exchange(0) / frames);
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; barrier bits a frame:");
    for (int b = 0; b < 16; b++)
        if (const int64_t n = st_bits[b].exchange(0)) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %#x %.1f", 1 << b, n / frames);
}

}  // namespace hsd
