// Per-thread command pools by frame slot, and the per-thread upload ring (direct_cmd.h).
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <memory>

#include "direct_check.h"
#include "direct_cmd.h"
#include "direct_draw.h"
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
    // (its slots, at fixed addresses: kSlots of them in rotation, one more made whenever none can be reset because lists of
    // more than kSlots frames ago are still alive on all the others)
    std::vector<std::unique_ptr<CmdSlot>> slots;
    CmdSlot* cur = nullptr;   // (the slot of the frame it recorded in last)
    VkCommandPool upload_pool = VK_NULL_HANDLE;
    UploadSlot uploads[kUploads];
    int next = 0;
    UploadSlot* open = nullptr;
    int64_t aside = 0;   // (its part of st_aside_now)
};
thread_local Thread* self = nullptr;

std::atomic<int64_t> st_resets{0}, st_kept{0}, st_reset_waits{0}, st_reset_wait_ns{0}, st_buffers{0}, st_uploads{0},
    st_upload_bytes{0}, st_upload_waits{0}, st_threads{0}, st_slots{0}, st_aside{0};
// (a command pool has no byte size to ask the driver for: counts stand in for it. st_aside_now sums each thread's last take)
std::atomic<int64_t> st_aside_now{0}, st_primaries{0}, st_secondaries{0}, st_upload_bufs{0};

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
    object_made(1, "a command pool");
    return p;
}
Thread& me() {
    if (!self) {
        self = new Thread;   // (kept for the process: a thread that records again finds its pools)
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
    object_made(1, level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ? "a command buffer" : "a secondary command buffer");
    st_buffers.fetch_add(1, std::memory_order_relaxed);
    (level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ? st_primaries : st_secondaries).fetch_add(1, std::memory_order_relaxed);
    return cb;
}

// The command buffers all threads took in a frame, by frame (mod 4), and the most of any frame so far: a thread can be
// handed at most a whole frame's lists, so a slot that records is grown to that at its reset, by its own thread. Its buffers
// then grow only when a frame needs more than any frame before it (an area loads), never because the job system handed this
// thread a larger share than it had before (M6b: that grew 80 secondaries over minutes of steady Kokiri)
std::atomic<int64_t> frame_bufs[4], frame_secs[4];
std::atomic<size_t> most_bufs{0}, most_secs{0};
void grow(CmdSlot& s, std::vector<VkCommandBuffer>& v, size_t to, VkCommandBufferLevel level) {
    to = (to + 15) & ~size_t(15);
    while (v.size() < to) v.push_back(alloc_buffer(s.pool, level));
}

}  // namespace

void cmd_frame_presented(uint64_t n) {
    if (n < 2) return;
    for (auto [f, most] : {std::pair{frame_bufs, &most_bufs}, std::pair{frame_secs, &most_secs}}) {
        const size_t done = (size_t)f[(n - 2) & 3].load(std::memory_order_relaxed);
        for (size_t m = most->load(std::memory_order_relaxed); done > m && !most->compare_exchange_weak(m, done);) {}
        f[(n + 1) & 3].store(0, std::memory_order_relaxed);
    }
}

CmdSlot* cmd_slot_take() {
    Thread& t = me();
    const uint64_t frame = present_count();
    if (!t.cur || t.cur->frame != frame) {
        // A slot can be reset once it was last recorded in kSlots frames ago or more (a list can reach the device two
        // presents after it was begun) and no list holding its buffers is alive. The one used last among those is taken, so
        // kSlots of them rotate. A slot with a list still alive is left as it is, with its buffers, until the list is gone:
        // recording on in it without a reset grew its pool by a frame's buffers every frame, for as long as the game kept
        // one list (a pause: 188,000 command buffers and 20 GB of the driver's memory in 30,000 presents).
        CmdSlot* s = nullptr;
        size_t aside = 0;
        for (auto& c : t.slots) {
            if (c->frame != UINT64_MAX && c->frame + kSlots > frame) continue;
            if (c->live.load(std::memory_order_acquire) > 0) {
                aside++;
                continue;
            }
            if (!s || (c->frame != UINT64_MAX && (s->frame == UINT64_MAX || c->frame > s->frame))) s = c.get();
        }
        if (aside) st_kept.fetch_add((int64_t)aside, std::memory_order_relaxed);
        if ((int64_t)aside != t.aside) st_aside_now.fetch_add((int64_t)aside - t.aside, std::memory_order_relaxed), t.aside = (int64_t)aside;
        if (!s) {
            t.slots.push_back(std::make_unique<CmdSlot>());
            s = t.slots.back().get();
            s->pool = make_pool(0);
            st_slots.fetch_add(1, std::memory_order_relaxed);
            if (t.slots.size() > kSlots) {
                st_aside.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<int> told{0};
                if (told++ < 30)
                    logf("direct: a command slot made at present %llu: this thread now has %zu, %zu of them set aside with a list "
                         "of %d frames ago or more still alive", (unsigned long long)frame, t.slots.size(), aside, kSlots);
            }
        } else if (s->frame != UINT64_MAX) {
            const uint64_t v = s->value.load(std::memory_order_acquire);
            if (v && sync_completed() < v) {
                const auto t0 = clk::now();
                sync_wait_value(v, UINT64_MAX);
                st_reset_waits.fetch_add(1, std::memory_order_relaxed);
                st_reset_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(),
                                           std::memory_order_relaxed);
            }
            vkResetCommandPool(dev.device, s->pool, 0);
            arena_reset(s->arena);
            query_slot_reset(s->q);
            task_pool_reset(s->task);
            s->used = 0, s->secs_used = 0;
            st_resets.fetch_add(1, std::memory_order_relaxed);
            if (!s->bufs.empty()) grow(*s, s->bufs, most_bufs.load(std::memory_order_relaxed), VK_COMMAND_BUFFER_LEVEL_PRIMARY);
            if (!s->secs.empty()) grow(*s, s->secs, most_secs.load(std::memory_order_relaxed), VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        }
        s->frame = frame;
        t.cur = s;
    }
    t.cur->live.fetch_add(1, std::memory_order_acq_rel);
    return t.cur;
}

VkCommandBuffer cmd_buffer(CmdSlot* s) {
    if (s->used == s->bufs.size()) {
        // (a slot's buffers made in batches: 16 at its first use, then as many again. One at a time, a thread's peak (the job
        // system hands it a few more lists in some frame) grew over minutes of play)
        const size_t more = std::max<size_t>({16, s->bufs.size(), most_bufs.load(std::memory_order_relaxed)});
        for (size_t i = 0; i < more; i++) s->bufs.push_back(alloc_buffer(s->pool));
        static std::atomic<int> told{0};   // (made in play: named, with what holds the slot)
        if (present_count() > 1200 && told++ < 30)
            logf("direct: command buffer made at present %llu: thread slot %p now %zu buffers, frame %llu, %d lists alive on it",
                 (unsigned long long)present_count(), (void*)s, s->bufs.size(), (unsigned long long)s->frame, s->live.load());
    }
    VkCommandBuffer cb = s->bufs[s->used++];
    frame_bufs[s->frame & 3].fetch_add(1, std::memory_order_relaxed);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

VkCommandBuffer cmd_secondary(CmdSlot* s, const PassFormats& f, VkRenderingFlags flags) {
    if (s->secs_used == s->secs.size()) {
        const size_t more = std::max<size_t>({16, s->secs.size(), most_secs.load(std::memory_order_relaxed)});
        for (size_t i = 0; i < more; i++) s->secs.push_back(alloc_buffer(s->pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY));
        static std::atomic<int> told{0};
        if (present_count() > 1200 && told++ < 30)
            logf("direct: secondary command buffers made at present %llu: thread slot %p now %zu", (unsigned long long)present_count(),
                 (void*)s, s->secs.size());
    }
    VkCommandBuffer cb = s->secs[s->secs_used++];
    frame_secs[s->frame & 3].fetch_add(1, std::memory_order_relaxed);
    VkCommandBufferInheritanceRenderingInfo ri{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    ri.flags = flags;
    ri.colorAttachmentCount = f.n;
    ri.pColorAttachmentFormats = f.color;
    ri.depthAttachmentFormat = f.depth;
    ri.stencilAttachmentFormat = f.stencil;
    ri.rasterizationSamples = f.samples;
    VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    ii.pNext = &ri;
    ii.occlusionQueryEnable = VK_TRUE;   // (M5: a job may run inside its caller's occlusion query)
    ii.queryFlags = VK_QUERY_CONTROL_PRECISE_BIT;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    bi.pInheritanceInfo = &ii;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

VkCommandBuffer cmd_secondary_plain(CmdSlot* s) {
    if (s->secs_used == s->secs.size()) {
        const size_t more = std::max<size_t>({16, s->secs.size(), most_secs.load(std::memory_order_relaxed)});
        for (size_t i = 0; i < more; i++) s->secs.push_back(alloc_buffer(s->pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY));
        static std::atomic<int> told{0};
        if (present_count() > 1200 && told++ < 30)
            logf("direct: secondary command buffers made at present %llu: thread slot %p now %zu", (unsigned long long)present_count(),
                 (void*)s, s->secs.size());
    }
    VkCommandBuffer cb = s->secs[s->secs_used++];
    frame_secs[s->frame & 3].fetch_add(1, std::memory_order_relaxed);
    VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
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
    if (!u.cb) u.cb = alloc_buffer(t.upload_pool), st_upload_bufs.fetch_add(1, std::memory_order_relaxed);
    if (bytes > u.cap) {
        if (u.staging) mem_buffer_free(u.staging, u.mem), u.staging = VK_NULL_HANDLE, u.map = nullptr, u.cap = 0;
        const uint64_t cap = std::max<uint64_t>(bytes, 1ull << 20);
        void* map = nullptr;
        if (!mem_buffer(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, u.staging, u.mem, &map, MO_UPLOAD) || !map) {
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

size_t cmd_held_stats(char* out, size_t cap) {
    const int64_t up = st_upload_bufs.load(std::memory_order_relaxed);
    const int n = std::snprintf(out, cap, "command slots: %lld slots (%lld set aside now), %lld primaries, %lld secondaries, and %lld upload "
                                "buffers in %lld upload pools (no byte figure: the driver's)", (long long)st_slots.load(),
                                (long long)st_aside_now.load(), (long long)(st_primaries.load() - up), (long long)st_secondaries.load(),
                                (long long)up, (long long)st_threads.load());
    return n < 0 ? 0 : std::min((size_t)n, cap ? cap - 1 : 0);
}

void cmd_stats(char* out, size_t cap, double frames) {
    std::snprintf(out, cap, "command buffers: %lld threads, %lld slots (%lld made past a thread's %d because "
                  "the others had a list alive), %lld made since the start, slot resets %.1f a frame, slots passed over (a list "
                  "alive) %lld, reset waits %lld (%.2f ms); uploads %.1f a frame (%.1f KB a frame), upload waits %lld; Vulkan "
                  "objects made since the start %lld", (long long)st_threads.load(), (long long)st_slots.load(),
                  (long long)st_aside.load(), kSlots, (long long)st_buffers.load(),
                  st_resets.exchange(0) / frames, (long long)st_kept.exchange(0), (long long)st_reset_waits.exchange(0),
                  st_reset_wait_ns.exchange(0) / 1e6, st_uploads.exchange(0) / frames, st_upload_bytes.exchange(0) / 1024.0 / frames,
                  (long long)st_upload_waits.exchange(0), (long long)st_objects.load());
}

}  // namespace hsd
