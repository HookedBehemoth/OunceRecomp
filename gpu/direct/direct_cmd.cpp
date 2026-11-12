// Per-thread command pools by frame slot, and the per-thread upload ring (direct_cmd.h).
#include <chrono>
#include <algorithm>
#include <cstdio>

#include "direct_cmd.h"
#include "direct_lists.h"

namespace hsd {
namespace {

using clk = std::chrono::steady_clock;
constexpr int kSlots = 3, kUploads = 4;

struct UploadSlot {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    Mem* mem = nullptr;
    uint8_t* map = nullptr;
    uint64_t cap = 0, value = 0;
};
struct Thread {
    CmdSlot slots[kSlots];
    VkCommandPool upload_pool = VK_NULL_HANDLE;
    UploadSlot uploads[kUploads];
    int next = 0;
    UploadSlot* open = nullptr;
};
thread_local Thread* self = nullptr;

std::atomic<int64_t> st_resets{0}, st_kept{0}, st_reset_waits{0}, st_reset_wait_ns{0}, st_buffers{0}, st_uploads{0},
    st_upload_bytes{0}, st_upload_waits{0}, st_threads{0};

VkCommandPool make_pool(VkCommandPoolCreateFlags flags) {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = flags;
    pi.queueFamilyIndex = dev.queue_family;
    VkCommandPool p = VK_NULL_HANDLE;
    if (vkCreateCommandPool(dev.device, &pi, nullptr, &p) != VK_SUCCESS) {
        logf("direct: vkCreateCommandPool failed");
        host->log_flush();
        abort();
    }
    st_objects.fetch_add(1, std::memory_order_relaxed);
    return p;
}
Thread& me() {
    if (!self) {
        self = new Thread;   // (kept for the process: a thread that records again finds its pools)
        for (CmdSlot& s : self->slots) s.pool = make_pool(0);
        self->upload_pool = make_pool(VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
        st_threads.fetch_add(1, std::memory_order_relaxed);
    }
    return *self;
}
VkCommandBuffer alloc_buffer(VkCommandPool pool, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool, ai.level = level, ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev.device, &ai, &cb) != VK_SUCCESS) {
        logf("direct: vkAllocateCommandBuffers failed");
        host->log_flush();
        abort();
    }
    st_objects.fetch_add(1, std::memory_order_relaxed);
    st_buffers.fetch_add(1, std::memory_order_relaxed);
    return cb;
}

}  // namespace

CmdSlot* cmd_slot_take() {
    Thread& t = me();
    const uint64_t frame = present_count();
    CmdSlot& s = t.slots[frame % kSlots];
    if (s.frame != frame) {
        if (s.live.load(std::memory_order_acquire) > 0) {
            st_kept.fetch_add(1, std::memory_order_relaxed);   // (a list of three frames ago still alive: not reset)
            static std::atomic<int> told{0};
            if (present_count() > 1200 && told++ < 30)
                logf("direct: slot %p not reset at present %llu: %d lists alive on it (frame %llu, %zu buffers used)", (void*)&s,
                     (unsigned long long)present_count(), s.live.load(), (unsigned long long)s.frame, s.used);
        } else {
            const uint64_t v = s.value.load(std::memory_order_acquire);
            if (v && sync_completed() < v) {
                const auto t0 = clk::now();
                sync_wait_value(v, UINT64_MAX);
                st_reset_waits.fetch_add(1, std::memory_order_relaxed);
                st_reset_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(),
                                           std::memory_order_relaxed);
            }
            vkResetCommandPool(dev.device, s.pool, 0);
            arena_reset(s.arena);
            s.used = 0, s.secs_used = 0;
            st_resets.fetch_add(1, std::memory_order_relaxed);
        }
        s.frame = frame;
    }
    s.live.fetch_add(1, std::memory_order_acq_rel);
    return &s;
}

VkCommandBuffer cmd_buffer(CmdSlot* s) {
    if (s->used == s->bufs.size()) {
        // (a slot's buffers made in batches: 16 at its first use, then as many again. One at a time, a thread's peak (the job
        // system hands it a few more lists in some frame) grew over minutes of play)
        const size_t more = std::max<size_t>(16, s->bufs.size());
        for (size_t i = 0; i < more; i++) s->bufs.push_back(alloc_buffer(s->pool));
        static std::atomic<int> told{0};   // (made in play: named, with what holds the slot)
        if (present_count() > 1200 && told++ < 30)
            logf("direct: command buffer made at present %llu: thread slot %p now %zu buffers, frame %llu, %d lists alive on it",
                 (unsigned long long)present_count(), (void*)s, s->bufs.size(), (unsigned long long)s->frame, s->live.load());
    }
    VkCommandBuffer cb = s->bufs[s->used++];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

VkCommandBuffer cmd_secondary(CmdSlot* s, const PassFormats& f, VkRenderingFlags flags) {
    if (s->secs_used == s->secs.size()) {
        const size_t more = std::max<size_t>(16, s->secs.size());
        for (size_t i = 0; i < more; i++) s->secs.push_back(alloc_buffer(s->pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY));
        static std::atomic<int> told{0};
        if (present_count() > 1200 && told++ < 30)
            logf("direct: secondary command buffers made at present %llu: thread slot %p now %zu", (unsigned long long)present_count(),
                 (void*)s, s->secs.size());
    }
    VkCommandBuffer cb = s->secs[s->secs_used++];
    VkCommandBufferInheritanceRenderingInfo ri{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    ri.flags = flags;
    ri.colorAttachmentCount = f.n;
    ri.pColorAttachmentFormats = f.color;
    ri.depthAttachmentFormat = f.depth;
    ri.stencilAttachmentFormat = f.stencil;
    ri.rasterizationSamples = f.samples;
    VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    ii.pNext = &ri;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    bi.pInheritanceInfo = &ii;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

void cmd_slot_drop(CmdSlot* s) { s->live.fetch_sub(1, std::memory_order_acq_rel); }

void cmd_slot_sent(CmdSlot* s, uint64_t v) {
    for (uint64_t m = s->value.load(std::memory_order_relaxed); v > m && !s->value.compare_exchange_weak(m, v);) {}
}

Upload upload_begin(uint64_t bytes) {
    Thread& t = me();
    UploadSlot& u = t.uploads[t.next];
    t.next = (t.next + 1) % kUploads;
    if (u.value && sync_completed() < u.value) {
        st_upload_waits.fetch_add(1, std::memory_order_relaxed);
        sync_wait_value(u.value, UINT64_MAX);
    }
    if (!u.cb) u.cb = alloc_buffer(t.upload_pool);
    if (bytes > u.cap) {
        if (u.staging) mem_buffer_free(u.staging, u.mem), u.staging = VK_NULL_HANDLE, u.map = nullptr, u.cap = 0;
        const uint64_t cap = std::max<uint64_t>(bytes, 1ull << 20);
        void* map = nullptr;
        if (!mem_buffer(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, u.staging, u.mem, &map) || !map) {
            logf("direct: an upload's staging buffer of %llu bytes not made", (unsigned long long)cap);
            u.staging = VK_NULL_HANDLE;
        } else {
            u.map = (uint8_t*)map, u.cap = cap;
        }
    }
    vkResetCommandBuffer(u.cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(u.cb, &bi);
    t.open = &u;
    st_upload_bytes.fetch_add((int64_t)bytes, std::memory_order_relaxed);
    return {u.cb, bytes <= u.cap ? u.staging : VK_NULL_HANDLE, bytes <= u.cap ? u.map : nullptr};
}

std::atomic<uint64_t> newest_upload{0};
uint64_t upload_value() { return newest_upload.load(std::memory_order_acquire); }

uint64_t upload_end(Upload& u) {
    UploadSlot* s = self ? self->open : nullptr;
    if (!s || s->cb != u.cb) return 0;
    self->open = nullptr;
    if (s->mem) mem_flush(s->mem);
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(u.cb, &d);
    vkEndCommandBuffer(u.cb);
    s->value = sync_send(0, &u.cb, 1);
    st_uploads.fetch_add(1, std::memory_order_relaxed);
    for (uint64_t m = newest_upload.load(std::memory_order_relaxed); s->value > m && !newest_upload.compare_exchange_weak(m, s->value);) {}
    return s->value;
}

void cmd_stats(char* out, size_t cap, double frames) {
    std::snprintf(out, cap, "command buffers: %lld threads, %lld made since the start, slot resets %.1f a frame, kept (a list "
                  "alive) %lld, reset waits %lld (%.2f ms); uploads %.1f a frame (%.1f KB a frame), upload waits %lld; Vulkan "
                  "objects made since the start %lld", (long long)st_threads.load(), (long long)st_buffers.load(),
                  st_resets.exchange(0) / frames, (long long)st_kept.exchange(0), (long long)st_reset_waits.exchange(0),
                  st_reset_wait_ns.exchange(0) / 1e6, st_uploads.exchange(0) / frames, st_upload_bytes.exchange(0) / 1024.0 / frames,
                  (long long)st_upload_waits.exchange(0), (long long)st_objects.load());
}

}  // namespace hsd
