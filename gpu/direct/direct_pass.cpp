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

#include <chrono>
#include <mutex>
#include <thread>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "direct_cmd.h"
#include "direct_compute.h"
#include "direct_draw.h"
#include "direct_dump.h"
#include "direct_image.h"
#include "direct_pass.h"
#include "direct_query.h"
#include "direct_task.h"

namespace hsd {
namespace {

enum {
    S_SET_RT, S_EMPTY_RT, S_NO_IMAGE, S_BEGUN, S_LOAD_CLEARS, S_CLEARS_IN, S_MASKED, S_CLEAR_NO_TARGET, S_CLEAR_DISABLED,
    S_DRAWS, S_DISPATCHES, S_BARRIERS, S_COPIES, S_COPY_SKIPPED, S_CLEAR_TEX, S_CLEAR_TEX_SKIPPED, S_BUF_OPS, S_UNRECORDED,
    S_CUTS, S_OTHER_THREAD, S_JOB_OUTSIDE, S_EVENT_CUTS, S_TASK_JOB_STEPS, S_TASK_JOB_SPLITS, S_TASK_INLINE_SPLITS, S_TASK_JOB_AGAIN,
    S_COUNT
};
const char* const kNames[S_COUNT] = {
    "SetRenderTargets", "with no target", "targets with no image", "renderings begun", "clears as load ops",
    "clears in a rendering", "masked clears skipped", "clears with no target", "clears not rendered", "draws",
    "dispatches", "barriers", "copies", "copies skipped", "texture clears", "texture clears skipped",
    "buffer ops in lists that record nothing", "ops of lists that record nothing (job, load, packet)", "calls that cut a rendering",
    "records on another thread", "ops a job's rendering cannot hold (skipped)", "command buffers cut at an event op",
    "jobs' task steps executed (M4c)", "of them in a begun rendering (ended and begun again)", "renderings ended for an inline task step",
    "jobs' task steps run again (a job called again)"};
std::atomic<int64_t> st[S_COUNT];
std::atomic<int64_t> st_bits[16];   // (barriers by NVN bit)
void count(int k) { st[k].fetch_add(1, std::memory_order_relaxed); }

uint64_t thread_id() { return (uint64_t)pthread_self(); }

// ---- (M3a) jobs: the format table by pass key, the job's secondary, its execution at its call ----
enum {
    J_RECORDED, J_NO_FORMATS, J_NO_NESTING, J_EXECUTED, J_MISMATCH, J_UNENDED, J_NOT_SECONDARY, J_NO_PASS, J_SIZE, J_KNOWN,
    J_MISSED, J_INLINE, J_INLINE_NO_PASS, J_COUNT
};
const char* const kJobNames[J_COUNT] = {
    "recorded as secondaries", "recorded with no formats known (no secondary)", "recorded with no nested rendering",
    "executed", "not executed: formats differ from the pass at the call (key forgotten)", "not executed: not ended at the call",
    "called with no secondary", "called with no pass in the caller", "executed in a pass of another size",
    "recorder asked: key known", "recorder asked: key not known (the game's early return)", "passes drawn inline",
    "passes drawn inline with no targets set"};
std::atomic<int64_t> st_jobs[J_COUNT], st_first_ticks{0}, st_first_n{0}, st_other_ticks{0}, st_other_n{0};
void jcount(int k) { st_jobs[k].fetch_add(1, std::memory_order_relaxed); }

constexpr uint32_t kKeys = 1024, kSets = 256;
struct KeySlot {
    std::atomic<uint64_t> key{0};
    std::atomic<uint32_t> set{0};   // (1 + its formats' index in sets; 0: not known)
    std::atomic<int64_t> inl{0}, inl_all{0}, executed{0}, forgotten{0}, drawn{0}, mesh{0}, step_a{0};
    std::atomic<uint64_t> target{0};   // (the pass's first colour target, or its depth: the dump's name for it)
};
KeySlot keys[kKeys];
PassFormats sets[kSets];
uint32_t nsets = 0;
std::mutex sets_mu;   // (the formats seen: a few, written when a key's targets change, never per draw)

KeySlot* key_slot(uint64_t key, bool make) {
    if (!key) return nullptr;
    for (uint32_t k = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 54) & (kKeys - 1), tries = 0; tries < kKeys; k = (k + 1) & (kKeys - 1), tries++) {
        KeySlot& s = keys[k];
        uint64_t have = s.key.load(std::memory_order_acquire);
        if (have == key) return &s;
        if (!have) {
            if (!make) return nullptr;
            if (s.key.compare_exchange_strong(have, key, std::memory_order_acq_rel) || have == key) return &s;
        }
    }
    return nullptr;
}
bool exact(const PassFormats& a, const PassFormats& b) { return a.same(b) && a.w == b.w && a.h == b.h; }
uint32_t intern(const PassFormats& f) {
    std::lock_guard lk(sets_mu);
    for (uint32_t i = 0; i < nsets; i++)
        if (exact(sets[i], f)) return i + 1;
    if (nsets == kSets) return 0;
    sets[nsets] = f;
    return ++nsets;
}
PassFormats formats_of(const PassRec& p) {
    PassFormats f;
    f.n = (uint32_t)p.n;
    for (int i = 0; i < p.n; i++) f.color[i] = p.color[i].format;
    f.depth_aspect = p.depth.aspect;
    if (p.depth.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) f.depth = p.depth.format;
    if (p.depth.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) f.stencil = p.depth.format;
    f.samples = p.samples, f.w = p.w, f.h = p.h;
    return f;
}
/// The pass key's formats now: those of the targets the caller set
void key_put(uint64_t key, const PassFormats& f) {
    KeySlot* s = key_slot(key, true);
    if (!s) return;
    const uint32_t have = s->set.load(std::memory_order_acquire);
    if (have && exact(sets[have - 1], f)) return;
    if (const uint32_t i = intern(f)) s->set.store(i, std::memory_order_release);
}
void jobs_draw_timed(bool first, uint64_t ticks) {
    if (first) st_first_ticks.fetch_add((int64_t)ticks, std::memory_order_relaxed), st_first_n.fetch_add(1, std::memory_order_relaxed);
    else st_other_ticks.fetch_add((int64_t)ticks, std::memory_order_relaxed), st_other_n.fetch_add(1, std::memory_order_relaxed);
}
double ticks_per_us() {
    static const double r = [] {
        const auto a = std::chrono::steady_clock::now();
        const uint64_t t = __builtin_ia32_rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double us = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - a).count() / 1e3;
        return (double)(__builtin_ia32_rdtsc() - t) / us;
    }();
    return r;
}


bool records(const List* l) { return l->rec; }

void record_barrier(VkCommandBuffer cb);
VkCommandBuffer barrier_buffer();
VkCommandBuffer cmd_raw(List* l);
/// The list's command buffer, a waiting barrier recorded first
VkCommandBuffer cmd(List* l) {
    VkCommandBuffer cb = cmd_raw(l);
    if (l->pending_barrier) l->pending_barrier = false, record_barrier(cb);
    return cb;
}
VkCommandBuffer cmd_raw(List* l) {
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
    // (M3a: every rendering may execute job secondaries among its inline commands: VK_KHR_maintenance7)
    if (dev.nested_rendering) ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT | VK_RENDERING_CONTENTS_INLINE_BIT_KHR;
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
    if (!p.noted || p.inherited) return;
    if (!p.begun && p.load) begin(l);
    if (!p.begun) return;
    query_suspend(l, l->cur);   // (M5: its occlusion query and condition end inside it)
    vkCmdEndRendering(l->cur);
    p.begun = false;
    dump_pass(l, l->cur);
    query_flush(l, &cmd);   // (M5: its reports written, after it and before what follows)
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
        if (!out.view) return out.format = VK_FORMAT_UNDEFINED, count(S_NO_IMAGE);
        out.im = im, out.level = level, out.layer = layer, out.aspect = im->aspect;
        if (!any) p.samples = im->samples;
        out.w = std::max(1u, im->width >> level), out.h = std::max(1u, im->height >> level);
        if (im->compressed)   // (M4g: a view of its blocks: the attachment is its block grid)
            out.w = (out.w + im->bw - 1) / im->bw, out.h = (out.h + im->bh - 1) / im->bh;
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
    if (!p.begun && whole && !query_cond_wanted(l)) {   // (M5: a gated clear is no load op)
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
    query_draw(l, l->cur);
    vkCmdClearAttachments(l->cur, 1, &ca, 1, &cr);
    count(S_CLEARS_IN);
}

void barrier(List* l, int32_t bits) {
    suspend(l);
    for (int b = 0; b < 16; b++)
        if (bits >> b & 1) st_bits[b].fetch_add(1, std::memory_order_relaxed);
    count(S_BARRIERS);
    // (no command buffer yet: the barrier waits for the list's next command, or its end; a list of nothing but barriers then
    // makes none, M2b)
    if (!l->cur) return void(l->pending_barrier = true);
    record_barrier(l->cur);
}

/// Every NVN barrier as one full memory barrier: the order bits wait for earlier work, the invalidate bits make its writes
/// visible; Vulkan has no invalidate without a wait, and images are all GENERAL, so no image barriers
void record_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

/// One command buffer holding a full barrier, recorded once (a list's last barrier with no command after it in the list)
VkCommandBuffer barrier_buffer() {
    static VkCommandBuffer cb = [] {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = dev.queue_family;
        VkCommandPool pool;
        vkCreateCommandPool(dev.device, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
        VkCommandBuffer c = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(dev.device, &ai, &c);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        vkBeginCommandBuffer(c, &bi);
        record_barrier(c);
        vkEndCommandBuffer(c);
        st_objects.fetch_add(2, std::memory_order_relaxed);
        return c;
    }();
    return cb;
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

/// (M4f) a texture copy left out, named (the first 24): which images, why
void copy_skipped(const char* why, const nvn_p_copy_tt& p, const Sub& s, const Sub& d) {
    count(S_COPY_SKIPPED);
    static std::atomic<int> told{0};
    if (told++ >= 24) return;
    logf("direct: texture copy left out (%s): %#llx %ux%ux%u vk %d level %u layer %u (region %dx%dx%d) -> %#llx %ux%ux%u vk %d level %u "
         "layer %u (region %dx%dx%d) at present %llu", why, (unsigned long long)(p.src ? p.src->gpu : 0), s.w, s.h, s.d,
         s.im ? (int)s.im->format : -1, s.level, s.layer, p.sr.width, p.sr.height, p.sr.depth, (unsigned long long)(p.dst ? p.dst->gpu : 0),
         d.w, d.h, d.d, d.im ? (int)d.im->format : -1, d.level, d.layer, p.dr.width, p.dr.height, p.dr.depth,
         (unsigned long long)present_count());
}

void copy_tt(List* l, const nvn_p_copy_tt& p) {
    Sub s, d;
    if (!image_sub(p.src, p.has_sv ? &p.sv : nullptr, p.sr, s) || !image_sub(p.dst, p.has_dv ? &p.dv : nullptr, p.dr, d))
        return copy_skipped("no image, the placeholder or outside it", p, s, d);
    if (s.im == d.im && s.level == d.level && s.layer == d.layer) return copy_skipped("onto itself", p, s, d);
    const bool colour = s.aspect == VK_IMAGE_ASPECT_COLOR_BIT && d.aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    if (!colour && s.im->format != d.im->format) return copy_skipped("depth of another format", p, s, d);
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
        return copy_skipped("sizes or block shapes differ, a compressed side", p, s, d);
    }
    d.im->written.fetch_or(W_COPY, std::memory_order_relaxed);
    count(S_COPIES);
    static std::atomic<int> told{0};   // (named: the game's barriers around them are what synchronises them)
    if (present_count() > 1200 && told++ < 8)
        logf("direct: texture copy in list lr %#llx after its SetRenderTargets %llu (the ops before it %u, %u): #%u %#llx %ux%u vk %d "
             "level %u -> #%u %#llx vk %d level %u", (unsigned long long)l->info.lr, (unsigned long long)l->pass.set_rt_at, l->last_op2, l->last_op,
             s.im->id, (unsigned long long)s.im->gpu, s.w, s.h, (int)s.im->format, s.level, d.im->id, (unsigned long long)d.im->gpu,
             (int)d.im->format, d.level);
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
    // (NVN's texture clear is in the 3D engine's order, Vulkan's a transfer: the clear is made visible to what follows, M2b's
    // validation run found renderings loading it right after)
    record_barrier(l->cur);
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
    struct Last {   // (the op before this one, for the copies' names)
        List* l;
        uint32_t op;
        ~Last() { l->last_op2 = l->last_op, l->last_op = op; }
    } last{l, op};
    draw_state(l, op, data, size);   // (M2b: the NVN state the draws read)
    compute_state(l, op, data, size);   // (M4a: and the dispatches)
    if (l->info.kind == HSD_LIST_GATE && draw_log_frame())
        logf("direct: drawlog gate op %u (noted %d begun %d cur %p segs %zu)", op, (int)p.noted, (int)p.begun, (void*)l->cur, l->segs.size());
    switch (op) {   // (state that clears and copies read: kept in every list)
    case NOP_SCISSOR: {
        nvn_p_scissor s;
        if (payload(data, size, s)) p.scissor[0] = s.x, p.scissor[1] = s.y, p.scissor[2] = s.w, p.scissor[3] = s.h;
        return;
    }
    case NOP_REPORT_COUNTER: case NOP_RESET_COUNTER: case NOP_RENDER_COND:   // (M5: on the GPU)
        query_op(l, op, data, size, p.begun || l->secondary, &cmd);
        return;
    case NOP_RENDER_ENABLE: {
        query_op(l, op, data, size, p.begun || l->secondary, &cmd);
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
        if (records(l) && (p.begun || l->secondary)) query_draw(l, l->cur);   // (M5: its counter's query, its condition)
        if (l->secondary) {   // (M3a: a job's first draw sets all its state: timed apart)
            const uint64_t t0 = __builtin_ia32_rdtsc();
            draw_emit(l, op, data, size, true);
            jobs_draw_timed(l->sec_draws++ == 0, __builtin_ia32_rdtsc() - t0);
            return;
        }
        draw_emit(l, op, data, size, records(l) && p.begun);   // (M2b: drawn, or counted by why not)
        return;
    case NOP_DISPATCH: case NOP_DISPATCH_INDIRECT:   // (M4a: outside the rendering; the targets stay noted)
        count(S_DISPATCHES);
        if (l->secondary) return count(S_JOB_OUTSIDE);
        if (records(l)) suspend(l), compute_dispatch(l, op, data, size, &cmd);
        return;
    case NOP_CLEAR_BUF: case NOP_COPY_B2B:   // (M4a: the same)
        if (l->secondary) return count(S_JOB_OUTSIDE);
        if (!records(l)) return count(S_BUF_OPS);
        suspend(l), compute_buffer_op(l, op, data, size, &cmd);
        return;
    case NOP_SET_RT: case NOP_CLEAR_COLOR: case NOP_CLEAR_DS: case NOP_BARRIER: case NOP_COPY_B2T: case NOP_COPY_T2B:
    case NOP_COPY_T2T: case NOP_CLEAR_TEX:
        if (l->secondary && op != NOP_CLEAR_COLOR && op != NOP_CLEAR_DS) {   // (M3a: no pass change, barrier or copy inside a rendering)
            static std::atomic<int> told{0};
            if (told++ < 12)
                logf("direct: op %u in a job's rendering secondary (pass key %#llx): skipped, counted", op, (unsigned long long)l->info.pass_key);
            return count(S_JOB_OUTSIDE);
        }
        if (!records(l)) {
            static std::atomic<int> told{0};
            if (present_count() > 1200 && told++ < 12)
                logf("direct: op %u in a list of kind %u (lr %#llx) that records nothing at this stage: counted", op, l->info.kind,
                     (unsigned long long)l->info.lr);
            return count(S_UNRECORDED);
        }
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
        if (!p.noted || c.index < 0 || c.index >= p.n || p.color[c.index].format == VK_FORMAT_UNDEFINED) return count(S_CLEAR_NO_TARGET);
        if ((c.mask & 0xF) != 0xF) return count(S_MASKED);
        VkClearValue v;
        std::memcpy(&v.color, c.rgba, 16);
        clear(l, 1u << c.index, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c.index, v);
        break;
    }
    case NOP_CLEAR_DS: {
        nvn_p_clear_ds c;
        if (!payload(data, size, c)) break;
        if (!p.noted || !p.depth.aspect) return count(S_CLEAR_NO_TARGET);
        const VkImageAspectFlags a = p.depth.aspect;
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

/// (M4c) A job's task step (the compute work of its task+mesh draws, direct_task.cpp) executed in its caller before the job,
/// outside the caller's rendering: ended first when it is begun (counted); its headers reset and made, a barrier, its task
/// dispatches, a barrier that makes the headers, maps and payloads visible to the mesh draws (indirect arguments, mesh shader
/// reads). Each call of the job runs it (a job called again: counted)
void task_job_step(List* l, const List* callee) {
    if (!callee->task_cb || callee->task_open || !records(l) || !l->pass.noted || l->secondary || !callee->ended || !callee->sec) return;
    if (!callee->fmt.same(formats_of(l->pass))) return;   // (it will not be executed)
    if (callee->task_runs.fetch_add(1, std::memory_order_relaxed)) count(S_TASK_JOB_AGAIN);
    if (l->pass.begun) suspend(l), count(S_TASK_JOB_SPLITS);
    VkCommandBuffer cb = cmd(l);
    vkCmdExecuteCommands(cb, 1, &callee->task_pre);
    task_barrier(cb, false);
    vkCmdExecuteCommands(cb, 1, &callee->task_cb);
    task_barrier(cb, true);
    draw_invalidate(l);   // (the caller's state is undefined after a secondary)
    count(S_TASK_JOB_STEPS);
}

void pass_call(List* l, const List* callee) {
    if (l->info.kind == HSD_LIST_GATE && draw_log_frame())
        logf("direct: drawlog gate call kind %u draws %d cmds %d (noted %d begun %d)", callee->info.kind, (int)callee->draws, (int)callee->has_cmds,
             (int)l->pass.noted, (int)l->pass.begun);
    if (callee->info.kind == HSD_LIST_JOB) task_job_step(l, callee);
    if (callee->draws) l->draws = true;
    if ((callee->info.kind == HSD_LIST_JOB || callee->draws) && records(l) && l->pass.noted && !l->pass.begun) begin(l);
    if (callee->info.kind == HSD_LIST_JOB) {   // (M3a: executed inside the caller's rendering; the key's formats noted)
        if (!records(l) || !l->pass.noted || l->secondary) return jcount(J_NO_PASS);
        const PassFormats now = formats_of(l->pass);
        if (!callee->secondary) return jcount(J_NOT_SECONDARY), key_put(callee->info.pass_key, now);
        KeySlot* ks = key_slot(callee->info.pass_key, true);
        if (!callee->ended || !callee->sec) return jcount(J_UNENDED);
        if (!callee->fmt.same(now)) {
            jcount(J_MISMATCH);
            if (ks) ks->set.store(0, std::memory_order_release), ks->forgotten.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> told{0};
            if (told++ < 12)
                logf("direct: job of pass key %#llx not executed: recorded for %u colour targets (%d) and depth %d, the pass has %u (%d) and %d",
                     (unsigned long long)callee->info.pass_key, callee->fmt.n, (int)callee->fmt.color[0], (int)callee->fmt.depth, now.n,
                     (int)now.color[0], (int)now.depth);
            return;
        }
        if (!l->pass.begun) begin(l);
        query_execute(l, cmd(l), callee);   // (M5: no condition across it; the caller's query inherited, or ended)
        vkCmdExecuteCommands(cmd(l), 1, &callee->sec);
        query_executed(l, callee);
        draw_invalidate(l);   // (the caller's dynamic state, shaders, buffers and push data are undefined after it)
        l->executed.push_back(callee);
        jcount(J_EXECUTED);
        if (ks) {
            ks->executed.fetch_add(1, std::memory_order_relaxed);
            ks->drawn.fetch_add(callee->n_drawn, std::memory_order_relaxed), ks->mesh.fetch_add(callee->n_mesh, std::memory_order_relaxed);
            ks->step_a.fetch_add(callee->n_step_a, std::memory_order_relaxed);
            const Target& t = l->pass.n && l->pass.color[0].im ? l->pass.color[0] : l->pass.depth;
            if (t.im) ks->target.store(t.im->gpu, std::memory_order_relaxed);
        }
        if (callee->fmt.w != now.w || callee->fmt.h != now.h) jcount(J_SIZE);
        key_put(callee->info.pass_key, now);
        return;
    }
    if (!callee->has_cmds) return;
    if (l->pass.begun) count(S_CUTS);
    suspend(l);
    if (l->pending_barrier) {   // (a barrier before the call: before the callee's commands)
        l->pending_barrier = false;
        if (l->cur) record_barrier(l->cur);
        else l->segs.push_back({barrier_buffer(), nullptr});
    }
    if (l->cur) vkEndCommandBuffer(l->cur), l->cur = VK_NULL_HANDLE;
    l->segs.push_back({VK_NULL_HANDLE, callee});
    l->has_cmds = true;
}

VkCommandBuffer pass_barrier_buffer() { return barrier_buffer(); }

void pass_cut(List* l) {
    if (!records(l)) return;
    if (l->secondary) {
        static std::atomic<int> told{0};
        if (told++ < 12) logf("direct: an event op in a job's rendering secondary (pass key %#llx): not cut", (unsigned long long)l->info.pass_key);
        return count(S_JOB_OUTSIDE);
    }
    suspend(l);
    if (l->cur) vkEndCommandBuffer(l->cur), l->cur = VK_NULL_HANDLE, count(S_EVENT_CUTS);   // (a waiting barrier goes with the next command)
}

VkCommandBuffer pass_outside(List* l) {
    if (l->pass.begun) suspend(l), count(S_TASK_INLINE_SPLITS);
    return cmd(l);
}
void pass_resume(List* l) {
    PassRec& p = l->pass;
    if (!records(l) || !p.noted || p.begun || p.inherited) return;
    begin(l);
    query_draw(l, l->cur);   // (M5: the draw's query and condition, as before the rendering ended)
}

void pass_list_end(List* l) {
    task_list_end(l);   // (M4c: its task step, if it has one)
    if (l->secondary) {   // (M3a: its caller executes it)
        query_suspend(l, l->cur);   // (M5: a secondary's query and condition end in it)
        if (l->cur) vkEndCommandBuffer(l->cur), l->sec = l->cur, l->cur = VK_NULL_HANDLE;
        return;
    }
    if (records(l)) end_pass(l), query_flush(l, &cmd);
    if (l->pending_barrier) {   // (its last barrier: in its buffer, or the shared one between its segments)
        l->pending_barrier = false;
        if (l->cur) record_barrier(l->cur);
        else l->segs.push_back({barrier_buffer(), nullptr}), l->has_cmds = true;
    }
    if (l->cur) vkEndCommandBuffer(l->cur), l->cur = VK_NULL_HANDLE;
}

bool PassFormats::same(const PassFormats& o) const {
    if (n != o.n || depth != o.depth || stencil != o.stencil || samples != o.samples) return false;
    for (uint32_t i = 0; i < n; i++)
        if (color[i] != o.color[i]) return false;
    return true;
}

void pass_job_begin(List* l) {
    const KeySlot* ks = key_slot(l->info.pass_key, false);
    const uint32_t set = ks ? ks->set.load(std::memory_order_acquire) : 0;
    if (!set) return jcount(J_NO_FORMATS);
    if (!dev.nested_rendering) return jcount(J_NO_NESTING);
    const PassFormats& f = sets[set - 1];
    l->fmt = f;
    l->secondary = l->rec = true;
    l->slot = cmd_slot_take(), l->thread = thread_id();
    l->cur = cmd_secondary(l->slot, f, 0);
    PassRec& p = l->pass;
    p.noted = p.begun = p.inherited = true;
    p.n = (int)f.n;
    for (uint32_t i = 0; i < f.n; i++) p.color[i].format = f.color[i];
    p.depth.format = f.depth ? f.depth : f.stencil, p.depth.aspect = f.depth_aspect;
    p.w = f.w, p.h = f.h, p.samples = f.samples;
    jcount(J_RECORDED);
}

void jobs_stats(char* out, size_t cap, double frames) {
    int k = std::snprintf(out, cap, "jobs a frame:");
    for (int i = 0; i < J_COUNT; i++) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "%s %s %.2f", i ? "," : "", kJobNames[i], st_jobs[i].exchange(0) / frames);
    const int64_t fn = st_first_n.exchange(0), on = st_other_n.exchange(0);
    const double ft = st_first_ticks.exchange(0) / ticks_per_us(), ot = st_other_ticks.exchange(0) / ticks_per_us();
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; a secondary's first draw %.2f us (%.1f a frame), its others %.2f us (%.1f a frame)",
                       fn ? ft / fn : 0.0, fn / frames, on ? ot / on : 0.0, on / frames);
    uint32_t known = 0, seen = 0;
    for (KeySlot& s : keys) {
        if (!s.key.load(std::memory_order_acquire)) continue;
        seen++;
        if (s.set.load(std::memory_order_acquire)) known++;
    }
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; format table: %u keys, %u known, %u format sets; keys drawn inline (since the last line):",
                       seen, known, nsets);
    for (KeySlot& s : keys)
        if (const int64_t n = s.inl.exchange(0)) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %#llx %lld", (unsigned long long)s.key.load(), (long long)n);
    for (KeySlot& s : keys)   // (one line each: the line's length is bounded)
        if (const int64_t e = s.executed.exchange(0))
            logf("direct: pass key %#llx (target %#llx): a frame %.2f jobs executed, their draws: drawn %.1f, mesh (M4) %.1f, step A %.1f",
                 (unsigned long long)s.key.load(), (unsigned long long)s.target.load(), e / frames, s.drawn.exchange(0) / frames,
                 s.mesh.exchange(0) / frames, s.step_a.exchange(0) / frames);
    for (KeySlot& s : keys)
        if (const int64_t n = s.forgotten.exchange(0)) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; forgotten %#llx %lld", (unsigned long long)s.key.load(), (long long)n);
}

void pass_stats(char* out, size_t cap, double frames) {
    int k = std::snprintf(out, cap, "passes a frame:");
    for (int i = 0; i < S_COUNT; i++) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "%s %s %.1f", i ? "," : "", kNames[i], st[i].exchange(0) / frames);
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; barrier bits a frame:");
    for (int b = 0; b < 16; b++)
        if (const int64_t n = st_bits[b].exchange(0)) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %#x %.1f", 1 << b, n / frames);
}

}  // namespace hsd

using namespace hsd;

/// (M3a) The job recorder's entry (host/direct_passes.c): 1 the pass key's formats are known (the job records a secondary),
/// 0 not (the recorder takes the game's own early return; the layer list draws the pass inline)
extern "C" __attribute__((visibility("default"))) int hsd_pass_known(uint64_t key) {
    const KeySlot* ks = key_slot(key, false);
    const bool known = ks && ks->set.load(std::memory_order_acquire);
    jcount(known ? J_KNOWN : J_MISSED);
    return known ? 1 : 0;
}
/// (M3a) The game's inline path (f_80681320) draws the pass of that key into the list now, after its SetRenderTargets: the
/// key's formats noted from them
extern "C" __attribute__((visibility("default"))) void hsd_pass_inline(void* list, uint64_t key) {
    List* l = (List*)list;
    jcount(J_INLINE);
    if (KeySlot* ks = key_slot(key, true)) ks->inl.fetch_add(1, std::memory_order_relaxed), ks->inl_all.fetch_add(1, std::memory_order_relaxed);
    if (!l || !l->rec || !l->pass.noted || l->secondary) return jcount(J_INLINE_NO_PASS);
    key_put(key, formats_of(l->pass));
}
