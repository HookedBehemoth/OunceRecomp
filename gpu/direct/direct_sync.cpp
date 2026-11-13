// The guest's submits, its fences and its events (docs/direct/m1-interface.md; plan, Submission). One Vulkan queue with one
// timeline semaphore. (M4a) An NVN submission's command buffers and frame-level ops are run in their order (the lists cut
// their command buffers at each event signal and wait: direct_pass.cpp pass_cut), and its command buffers are sent in
// chunks: the ones before an event signal are sent before the signal is written, so another queue's work after its wait for
// that signal is sent after them, as the console's GPU runs it; a chunk after an event wait waits on the GPU for everything
// sent before it (the signal's work and its writes). Each chunk is one vkQueueSubmit2 that signals the next value.
//
// An NVN queue's submissions run in order. A submission's frame-level ops (direct_lists.cpp) are applied when it runs: an
// event signal writes the event's word in guest memory (the value the game polls), a report writes its counter, and a wait
// whose event does not hold the value yet stops that queue there (held on the host, as the console's GPU would stall the
// queue) until another queue's signal or the CPU's (nvnEventSignal) meets it. nvnQueueWaitSync holds the queue's next
// submission until the fence's submission has been sent. An NVN fence names its queue and the submission (its sequence
// number on that queue); a CPU wait for it waits until that submission is sent and then for its timeline value on the GPU.
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <unordered_map>

#include "direct_cmd.h"
#include "direct_compute.h"
#include "direct_draw.h"
#include "direct_dump.h"
#include "direct_internal.h"
#include "direct_lists.h"
#include "direct_marks.h"
#include "direct_pass.h"

namespace hsd {
std::atomic<int64_t> st_send_ns{0}, st_send_n{0};
std::unique_lock<std::mutex> lock_mu();   // (M6a: below)
namespace {

using clk = std::chrono::steady_clock;
constexpr int kFenceQueueShift = 40;   // (a fence: (queue index + 1) << 40 | its sequence number on that queue)

VkSemaphore timeline = VK_NULL_HANDLE;
std::atomic<uint64_t> sent{0};   // (the newest value submitted)

struct Sub {
    std::vector<List*> lists;   // (referenced until it has run)
    std::vector<SubItem> items;   // (M4a: its command buffers and frame-level ops in their order)
    size_t at = 0;
    std::vector<VkCommandBuffer> pending;   // (M4a: the command buffers run past and not sent yet: the next chunk)
    uint32_t chunks = 0;   // (sent so far)
    bool wait_next = false;   // (the next chunk follows an event wait)
    uint64_t last_value = 0;
    uint64_t seq = 0;
    std::vector<uint64_t> wait_fences;   // (nvnQueueWaitSync before it: fences of other queues)
    clk::time_point held_since{};
    bool held = false;
    // (M2a: the slots its command buffers come from, the dump's copies in them)
    std::vector<CmdSlot*> slots;
    std::vector<DumpItem*> dumps;
    uint64_t frame = 0;   // (presents before its hand-in)
    // (a present: its GPU work runs here, in its queue's order)
    bool present = false;
    nvn_tex tex{};
    int crop[4]{};
};
struct Queue {
    uint64_t id = 0;
    uint32_t index = 0;
    uint64_t seq = 0;        // (submissions handed in)
    uint64_t seq_sent = 0;   // (submissions run and sent)
    std::deque<Sub> subs;    // (not run yet: the first may be held at a wait)
    std::vector<uint64_t> wait_fences;   // (for the next submission)
    std::deque<std::pair<uint64_t, uint64_t>> values;   // (seq -> timeline value of the recent ones, oldest first)
};
thread_local uint64_t present_queue = 0;   // (hsd_present_queue's, for the present that follows on this thread)
std::mutex mu;   // (the queues, their submissions, the event and report writes; queue_mu is taken inside it)
std::condition_variable sent_cv;   // (a submission sent: CPU waits for a fence not sent yet)
std::vector<Queue*> queues;   // (by index; never freed)
std::unordered_map<uint64_t, Queue*> by_id;

// counts since the last line (mu held, or atomic)
int64_t st_chunks = 0, st_chunk_waits = 0, st_upload_waits_gpu = 0, st_cbs = 0, st_submits = 0, st_held = 0, st_waits_met = 0, st_signals = 0, st_reports = 0, st_resets = 0, st_fence_held = 0;
double st_hold_max_ms = 0, st_hold_ms = 0;
int64_t st_freed_cpu = 0, st_freed_queue = 0;   // (M5: holds ended by the CPU's nvnEventSignal / by another queue's signal)
bool in_cpu_signal = false;   // (progress() runs for nvnEventSignal; mu held)
std::atomic<int64_t> st_submit_ns{0}, st_submit_max_ns{0}, st_cpu_waits{0}, st_cpu_wait_ns{0}, st_cpu_signals{0};
// (the transient pool's rule, host/rt_slots.c REUSE_AFTER: how late a list reaches the queue. Presents between a list's
// first command buffer and its hsd_submit, and between the hsd_submit and its chunks' vkQueueSubmit2: 0, 1, 2, more)
std::atomic<int64_t> st_age_submit[4], st_age_sent[4];

Queue* queue_of(uint64_t id) {   // (mu held)
    auto it = by_id.find(id);
    if (it != by_id.end()) return it->second;
    Queue* q = new Queue;
    q->id = id;
    q->index = (uint32_t)queues.size();
    queues.push_back(q);
    by_id[id] = q;
    logf("direct: NVN queue %#llx is queue %u", (unsigned long long)id, q->index);
    return q;
}
uint64_t fence_of(const Queue* q, uint64_t seq) { return (uint64_t)(q->index + 1) << kFenceQueueShift | seq; }

/// The timeline value of a fence's submission once it is sent (mu held): true and the value, false while it is not sent.
/// A sequence number older than the ones kept: its queue's oldest kept value bounds it
bool fence_value(uint64_t fence, uint64_t& value) {
    const uint64_t qi = (fence >> kFenceQueueShift) - 1, seq = fence & ((1ull << kFenceQueueShift) - 1);
    if (!fence || qi >= queues.size() || !seq) return value = 0, true;
    const Queue* q = queues[qi];
    if (seq > q->seq_sent) return false;
    value = 0;
    for (const auto& [s, v] : q->values)
        if (s >= seq) return value = v, true;
    return true;   // (none kept: long done)
}

bool event_wait_met(uint32_t have, uint32_t mode, uint32_t want) {
    return mode == 0 ? have == want : (int32_t)(have - want) >= 0;   // (NVN: EQUAL, GEQUAL_WRAP)
}

// (M4a) The chunks a pass of progress() sends, in one vkQueueSubmit2 at its end (one call costs ~0.1 ms on RADV: 34 chunks a
// frame sent one by one took 4.2 ms). Each chunk's value is given when it is added, queue_mu held from the first until the
// call, so no other submission (uploads, presents) comes between their values (mu held)
struct Batch {
    std::unique_lock<std::mutex> lk;
    struct Chunk {
        uint64_t wait = 0, value = 0;
        size_t first = 0, n = 0;
    };
    std::vector<Chunk> chunks;
    std::vector<VkCommandBuffer> cbs;
};
Batch batch;
std::atomic<int64_t> st_batch_calls{0};

// (M6a, HOS_DBG=gputime: measurement) The GPU's own time: each vkQueueSubmit2 of the batch and each present's command
// buffer is bracketed by two timestamps (top of pipe before its first command, all commands after its last). In the
// queue's order these give the time the queue had work (the union of the brackets) and its idle gaps: the GPU waiting
// for the CPU. Read once available, at the presents; every 600 presents a line, and with HOS_DBG=frames each present's
// row in direct_frames.csv (direct_present.cpp)
struct GpuTime {
    bool on = false;
    VkQueryPool pool = VK_NULL_HANDLE;
    static constexpr uint32_t kSlots = 2048;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkCommandBuffer start[kSlots]{}, end[kSlots]{};
    uint32_t next = 0;
    std::deque<std::pair<uint32_t, int>> fifo;   // (slots sent, in queue order: kind 0 a batch, 1 a present)
    std::mutex mu;
    PFN_vkCmdWriteTimestamp2 write = nullptr;
    PFN_vkCmdResetQueryPool reset = nullptr;
    PFN_vkGetQueryPoolResults results = nullptr;
    double period_ns = 10;
    uint64_t busy_end = 0;
    double busy_ns = 0, idle_ns = 0, frame_busy = 0, frame_idle = 0, last_present_end = 0;
    int64_t idle_gaps = 0, presents = 0;
    double longest_gap = 0;
    // (per present, for direct_present.cpp's rows: GPU frame time, busy, idle)
    std::deque<std::array<double, 3>> rows;
};
GpuTime gt;
void gputime_init() {
    gt.on = dbg("gputime");
    if (!gt.on) return;
    gt.write = (PFN_vkCmdWriteTimestamp2)vkGetDeviceProcAddr(dev.device, "vkCmdWriteTimestamp2");
    gt.reset = (PFN_vkCmdResetQueryPool)vkGetDeviceProcAddr(dev.device, "vkCmdResetQueryPool");
    gt.results = (PFN_vkGetQueryPoolResults)vkGetDeviceProcAddr(dev.device, "vkGetQueryPoolResults");
    auto create = (PFN_vkCreateQueryPool)vkGetDeviceProcAddr(dev.device, "vkCreateQueryPool");
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP, qi.queryCount = 2 * GpuTime::kSlots;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = dev.queue_family;
    if (!gt.write || !gt.reset || !gt.results || !create || create(dev.device, &qi, nullptr, &gt.pool) != VK_SUCCESS ||
        vkCreateCommandPool(dev.device, &pi, nullptr, &gt.cpool) != VK_SUCCESS) {
        gt.on = false;
        logf("direct: gputime: no query pool");
        return;
    }
    gt.period_ns = dev.props.limits.timestampPeriod;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = gt.cpool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = GpuTime::kSlots;
    vkAllocateCommandBuffers(dev.device, &ai, gt.start);
    vkAllocateCommandBuffers(dev.device, &ai, gt.end);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    for (uint32_t i = 0; i < GpuTime::kSlots; i++) {
        vkBeginCommandBuffer(gt.start[i], &bi);
        gt.reset(gt.start[i], gt.pool, 2 * i, 2);
        gt.write(gt.start[i], VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, gt.pool, 2 * i);
        vkEndCommandBuffer(gt.start[i]);
        vkBeginCommandBuffer(gt.end[i], &bi);
        gt.write(gt.end[i], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gt.pool, 2 * i + 1);
        vkEndCommandBuffer(gt.end[i]);
    }
    logf("direct: gputime: every submit and present bracketed by timestamps (HOS_DBG=gputime/frames, measurement)");
}
/// a slot for the next submit (queue_mu held by the caller, so slots are taken in queue order)
uint32_t gputime_slot(int kind) {
    std::lock_guard lk(gt.mu);
    const uint32_t s = gt.next;
    gt.next = (gt.next + 1) % GpuTime::kSlots;
    gt.fifo.push_back({s, kind});
    return s;
}
/// the slots whose two timestamps are in, in queue order
void gputime_poll() {
    std::lock_guard lk(gt.mu);
    while (!gt.fifo.empty()) {
        const auto [s, kind] = gt.fifo.front();
        uint64_t v[4];   // (start, its availability, end, its availability)
        if (gt.results(dev.device, gt.pool, 2 * s, 2, sizeof v, v, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS || !v[1] || !v[3])
            break;
        gt.fifo.pop_front();
        const uint64_t a = v[0], b = v[2];
        if (gt.busy_end && a > gt.busy_end) {
            const double gap = (a - gt.busy_end) * gt.period_ns;
            gt.idle_ns += gap, gt.frame_idle += gap, gt.idle_gaps++;
            gt.longest_gap = std::max(gt.longest_gap, gap);
        }
        if (b > gt.busy_end) {
            const double d = (b - std::max(a, gt.busy_end)) * gt.period_ns;
            gt.busy_ns += d, gt.frame_busy += d;
            gt.busy_end = b;
        }
        if (kind == 1) {   // (a present: a GPU frame ends)
            const double end = b * gt.period_ns;
            if (gt.last_present_end > 0) gt.rows.push_back({(end - gt.last_present_end) / 1e6, gt.frame_busy / 1e6, gt.frame_idle / 1e6});
            while (gt.rows.size() > 4096) gt.rows.pop_front();
            gt.last_present_end = end, gt.frame_busy = gt.frame_idle = 0, gt.presents++;
        }
    }
}
/// A chunk into the batch: with the chunk before it in one submit info (its value theirs), unless it waits for a value (a
/// fence, an upload); after an event wait a full barrier first (its first scope: all work before it on the queue)
uint64_t batch_add(uint64_t wait_value, bool barrier, const VkCommandBuffer* cbs, uint32_t n) {
    if (!batch.lk.owns_lock()) batch.lk = std::unique_lock<std::mutex>(dev.queue_mu);
    if (batch.chunks.empty() || wait_value) {
        const uint64_t v = sent.load(std::memory_order_relaxed) + 1;
        batch.chunks.push_back({wait_value, v, batch.cbs.size(), 0});
        sent.store(v, std::memory_order_release);
    }
    Batch::Chunk& c = batch.chunks.back();
    if (barrier) batch.cbs.push_back(pass_barrier_buffer()), c.n++;
    batch.cbs.insert(batch.cbs.end(), cbs, cbs + n), c.n += n;
    return c.value;
}
void batch_flush() {
    if (!batch.lk.owns_lock()) return;
    const size_t k = batch.chunks.size();
    std::vector<VkSubmitInfo2> si(k, VkSubmitInfo2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2});
    std::vector<VkSemaphoreSubmitInfo> sem(2 * k, VkSemaphoreSubmitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO});
    if (gt.on && k) {   // (M6a: the batch's two timestamps, first and last)
        const uint32_t slot = gputime_slot(0);
        batch.cbs.insert(batch.cbs.begin() + (long)batch.chunks[0].first, gt.start[slot]);
        batch.chunks[0].n++;
        for (size_t i = 1; i < k; i++) batch.chunks[i].first++;
        batch.cbs.insert(batch.cbs.begin() + (long)(batch.chunks[k - 1].first + batch.chunks[k - 1].n), gt.end[slot]);
        batch.chunks[k - 1].n++;
    }
    std::vector<VkCommandBufferSubmitInfo> cb(batch.cbs.size(), VkCommandBufferSubmitInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO});
    for (size_t i = 0; i < cb.size(); i++) cb[i].commandBuffer = batch.cbs[i];
    for (size_t i = 0; i < k; i++) {
        const Batch::Chunk& c = batch.chunks[i];
        VkSemaphoreSubmitInfo& sig = sem[2 * i];
        sig.semaphore = timeline, sig.value = c.value, sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si[i].commandBufferInfoCount = (uint32_t)c.n, si[i].pCommandBufferInfos = c.n ? cb.data() + c.first : nullptr;
        si[i].signalSemaphoreInfoCount = 1, si[i].pSignalSemaphoreInfos = &sig;
        if (c.wait) {   // (a value given before this one: never a wait before its signal in submission order)
            VkSemaphoreSubmitInfo& w = sem[2 * i + 1];
            w = sig, w.value = c.wait;
            si[i].waitSemaphoreInfoCount = 1, si[i].pWaitSemaphoreInfos = &w;
        }
    }
    const auto t0 = clk::now();
    const VkResult r = vkQueueSubmit2(dev.queue, (uint32_t)k, si.data(), VK_NULL_HANDLE);
    st_send_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed);
    st_send_n.fetch_add(1, std::memory_order_relaxed), st_batch_calls.fetch_add((int64_t)k, std::memory_order_relaxed);
    if (r != VK_SUCCESS) {
        logf("direct: vkQueueSubmit2 of %zu chunks failed (VkResult %d)", k, (int)r);
        if (r == VK_ERROR_DEVICE_LOST) device_lost("a batch submit");
    }
    batch.chunks.clear(), batch.cbs.clear();
    batch.lk.unlock();
}

/// (M4a) The command buffers run past and not sent yet, sent as one chunk (mu held). The first chunk of a submission waits
/// for its fences (nvnQueueWaitSync) and the texture uploads; a chunk after an event wait for everything sent before it
void send_chunk(Queue& q, Sub& s, bool last) {
    if (s.pending.empty() && !last) return;
    uint64_t wait_value = 0;   // (one timeline: the newest of the values orders them all)
    if (!s.chunks)
        for (uint64_t f : s.wait_fences) {
            uint64_t v = 0;
            fence_value(f, v);
            wait_value = std::max(wait_value, v);
        }
    if (s.wait_next) st_chunk_waits++;
    {   // (A-rt: the uploads of textures registered before it; one wait per new upload, mu held)
        static uint64_t waited = 0;
        const uint64_t up = upload_value();
        if (up > waited) wait_value = std::max(wait_value, up), waited = up, st_upload_waits_gpu++;
    }
    {   // (M4a, HOS_DBG=sublog:<present>: the chunks sent while that present is the newest, in their order)
        static const int64_t at = [] {
            const char* v = dbg_value("sublog");
            return v ? atoll(v) : -1;
        }();
        if (at >= 0 && (int64_t)present_count() == at) {
            char b[600];
            int k = std::snprintf(b, sizeof b, "direct: sublog present %lld: queue %u seq %llu chunk %u%s%s: %zu command buffers (%zu of %zu items), lists:",
                                  (long long)at, q.index, (unsigned long long)s.seq, s.chunks, s.wait_next ? " after a wait" : "", last ? " last" : "",
                                  s.pending.size(), s.at, s.items.size());
            for (const List* l : s.lists)
                if (k < 540) k += std::snprintf(b + k, sizeof b - k, " %u/%#llx", l->info.kind, (unsigned long long)l->info.lr);
            logf("%s", b);
        }
    }
    st_age_sent[std::min<uint64_t>(present_count() - std::min(present_count(), s.frame), 3)].fetch_add(1, std::memory_order_relaxed);
    s.last_value = batch_add(wait_value, s.wait_next && !s.pending.empty(), s.pending.data(), (uint32_t)s.pending.size());
    if (s.wait_next && s.pending.empty()) return;   // (nothing after the wait yet: the barrier goes with the next chunk)
    st_cbs += (int64_t)s.pending.size(), st_chunks++;
    s.pending.clear();
    s.chunks++;
    s.wait_next = false;
}

/// A submission's command buffers and ops from where it stopped; false: held at a wait not met (mu held). moved: it got on
bool run(Queue& q, Sub& s, bool& moved) {
    for (uint64_t f : s.wait_fences) {
        uint64_t v;
        if (!s.chunks && !fence_value(f, v)) {
            if (!s.held) st_fence_held++;
            return false;
        }
    }
    for (; s.at < s.items.size(); s.at++, moved = true) {
        const SubItem& it = s.items[s.at];
        if (it.cb) {
            s.pending.push_back(it.cb);
            continue;
        }
        const FrameOp& o = *it.op;
        switch (o.op) {
        case NOP_SIGNAL_EVENT: {
            send_chunk(q, s, false);   // (the work before it on the GPU's queue before any work after its wait)
            uint32_t v = o.value;
            if (o.mode == NVN_EVENT_SIGNAL_MODE_ADD) {
                uint32_t old = 0;
                host->gpu_read(o.addr, &old, 4);
                v += old;
            }
            host->gpu_write(o.addr, &v, 4);
            st_signals++;
            break;
        }
        case NOP_WAIT_EVENT: {
            uint32_t have = 0;
            host->gpu_read(o.addr, &have, 4);
            if (!event_wait_met(have, o.mode, o.value)) {
                send_chunk(q, s, false);   // (what came before it may run meanwhile)
                return false;
            }
            st_waits_met++;
            send_chunk(q, s, false);
            s.wait_next = true;
            break;
        }
        default: break;
        }
    }
    return true;
}

/// Every queue's first submissions run as far as they go, until none moves (a signal may let another queue on); each one
/// that ran to its end sent (mu held)
void progress() {
    for (bool moved = true; moved;) {
        moved = false;
        for (Queue* q : queues) {
            while (!q->subs.empty()) {
                Sub& s = q->subs.front();
                if (s.present) {
                    batch_flush();   // (the present submits on its own)
                    present_run(s.tex, s.crop);
                    q->subs.pop_front();
                    moved = true;
                    continue;
                }
                if (!run(*q, s, moved)) {
                    if (!s.held) s.held = true, s.held_since = clk::now(), st_held++;
                    break;
                }
                if (s.held) {
                    const double ms = std::chrono::duration<double, std::milli>(clk::now() - s.held_since).count();
                    if (ms > st_hold_max_ms) st_hold_max_ms = ms;
                    st_hold_ms += ms;
                    (in_cpu_signal ? st_freed_cpu : st_freed_queue)++;
                }
                const bool dumped = dump_claim(s.dumps, s.frame);
                send_chunk(*q, s, true);   // (the last, even with no command buffer: its value is the submission's fence)
                const uint64_t v = s.last_value;
                for (CmdSlot* c : s.slots) cmd_slot_sent(c, v);
                if (dumped) dump_sent(v);
                q->seq_sent = s.seq;
                q->values.push_back({s.seq, v});
                while (q->values.size() > 64) q->values.pop_front();
                for (List* l : s.lists) list_unref(l);
                q->subs.pop_front();
                st_submits++;
                moved = true;
            }
        }
    }
    batch_flush();
    sent_cv.notify_all();
}

// ---- the table's sync entries ----
void nvn_submit(const uint64_t*, int) {   // (the host's held submits: never in direct mode)
    static int told;
    if (!told++) logf("direct: nvn_submit called: the host holds submits (HOS_NVN_HOLD?), which direct mode does not use");
}
void nvn_submit_q(const uint64_t*, int, uint64_t) {
    static int told;
    if (!told++) logf("direct: nvn_submit_q called: the host should call hsd_submit in direct mode");
}
uint64_t nvn_fence() { return sent.load(std::memory_order_acquire); }
uint64_t nvn_fence_q(uint64_t queue) {
    std::lock_guard lk(mu);
    const Queue* q = queue_of(queue);
    return fence_of(q, q->seq);
}
uint64_t nvn_pushed() { return sync_pushed(); }
uint64_t nvn_replayed() { return sync_completed(); }
int nvn_wait(uint64_t fence, uint64_t timeout_ns) {
    const auto t0 = clk::now();
    uint64_t value = 0;
    {
        std::unique_lock lk(mu);
        if (!fence_value(fence, value)) {
            if (!timeout_ns) return 0;
            const auto until = t0 + std::chrono::nanoseconds(std::min<uint64_t>(timeout_ns, 3600ull * 1000000000ull));
            if (!sent_cv.wait_until(lk, until, [&] { return fence_value(fence, value); })) return 0;
        }
    }
    if (!value || sync_completed() >= value) return 1;
    if (!timeout_ns) return 0;
    const uint64_t spent = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
    const bool ok = sync_wait_value(value, timeout_ns > spent ? timeout_ns - spent : 0);
    st_cpu_waits.fetch_add(1, std::memory_order_relaxed);
    st_cpu_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed);
    return ok ? 1 : 0;
}
void nvn_finish() {   // (what is sent done; a submission held at a wait is not: it waits for another queue or the CPU)
    const uint64_t v = sent.load(std::memory_order_acquire);
    if (v) sync_wait_value(v, UINT64_MAX);
}
void nvn_event_signal(uint64_t, uint32_t) {   // (the host wrote the value: the queues held at waits may go on)
    st_cpu_signals.fetch_add(1, std::memory_order_relaxed);
    auto lk = lock_mu();
    in_cpu_signal = true;
    progress();
    in_cpu_signal = false;
}

}  // namespace

// (M6a) for direct_present.cpp
bool gputime_on() { return gt.on; }
void gputime_present_begin(VkCommandBuffer cb, uint32_t& slot) {
    slot = gputime_slot(1);
    gt.reset(cb, gt.pool, 2 * slot, 2);
    gt.write(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, gt.pool, 2 * slot);
}
void gputime_present_end(VkCommandBuffer cb, uint32_t slot) { gt.write(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gt.pool, 2 * slot + 1); }
void gputime_read() { gputime_poll(); }
bool gputime_row(double out[3]) {
    std::lock_guard lk(gt.mu);
    if (gt.rows.empty()) return false;
    const auto r = gt.rows.front();
    gt.rows.pop_front();
    out[0] = r[0], out[1] = r[1], out[2] = r[2];
    return true;
}
void gputime_stats(char* out, size_t cap) {
    std::lock_guard lk(gt.mu);
    const double f = gt.presents ? (double)gt.presents : 1;
    std::snprintf(out, cap, "gputime: %lld GPU frames: busy %.3f ms a frame, idle %.3f ms a frame (%.1f gaps a frame, longest %.3f ms)",
                  (long long)gt.presents, gt.busy_ns / 1e6 / f, gt.idle_ns / 1e6 / f, gt.idle_gaps / f, gt.longest_gap / 1e6);
    gt.busy_ns = gt.idle_ns = 0, gt.idle_gaps = 0, gt.presents = 0, gt.longest_gap = 0;
}
std::atomic<int64_t> st_mu_wait_ns{0}, st_mu_waits{0}, st_fence_wait_ns{0};
/// (M6a) mu taken, its wait counted when it was held
std::unique_lock<std::mutex> lock_mu() {
    std::unique_lock lk(mu, std::try_to_lock);
    if (lk.owns_lock()) return lk;
    const auto t0 = clk::now();
    lk.lock();
    st_mu_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed);
    st_mu_waits.fetch_add(1, std::memory_order_relaxed);
    return lk;
}

bool sync_init() {
    gputime_init();
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    si.pNext = &ti;
    if (vkCreateSemaphore(dev.device, &si, nullptr, &timeline) != VK_SUCCESS) {
        logf("direct: no timeline semaphore");
        return false;
    }
    return true;
}
uint64_t sync_pushed() { return sent.load(std::memory_order_acquire); }
uint64_t sync_completed() {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev.device, timeline, &v);
    return v;
}
bool sync_wait_value(uint64_t value, uint64_t timeout_ns) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &value;
    return vkWaitSemaphores(dev.device, &wi, timeout_ns) == VK_SUCCESS;
}
uint64_t sync_send(uint64_t wait_value, const VkCommandBuffer* cbs, uint32_t n) {
    const auto t0 = clk::now();
    struct Timed {
        clk::time_point t0;
        ~Timed() { st_send_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed), st_send_n.fetch_add(1, std::memory_order_relaxed); }
    } timed{t0};
    std::lock_guard qk(dev.queue_mu);
    const uint64_t v = sent.load(std::memory_order_relaxed) + 1;
    VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = timeline, sig.value = v, sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo wait = sig;
    wait.value = wait_value;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    std::vector<VkCommandBufferSubmitInfo> cb(n, VkCommandBufferSubmitInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO});
    for (uint32_t i = 0; i < n; i++) cb[i].commandBuffer = cbs[i];
    si.commandBufferInfoCount = n;
    si.pCommandBufferInfos = cb.data();
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    if (wait_value) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &wait;   // (sent already: never a wait before its signal)
    const VkResult r = vkQueueSubmit2(dev.queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        logf("direct: vkQueueSubmit2 failed (VkResult %d)", (int)r);
        if (r == VK_ERROR_DEVICE_LOST) {
            device_lost("a submit");
        }
        return 0;
    }
    sent.store(v, std::memory_order_release);
    return v;
}

void sync_age_stats(char* out, size_t cap) {
    std::snprintf(out, cap, "lists handed to the queue 0, 1, 2, 3+ presents after they were begun: %lld %lld %lld %lld; chunks sent to "
                  "the device 0, 1, 2, 3+ presents after their submit: %lld %lld %lld %lld (since the start; a slot's memory goes to "
                  "another target only after more presents than these)", (long long)st_age_submit[0].load(), (long long)st_age_submit[1].load(),
                  (long long)st_age_submit[2].load(), (long long)st_age_submit[3].load(), (long long)st_age_sent[0].load(),
                  (long long)st_age_sent[1].load(), (long long)st_age_sent[2].load(), (long long)st_age_sent[3].load());
}
void sync_stats(char* out, size_t cap, double frames) {
    std::lock_guard lk(mu);
    size_t held_now = 0;
    for (const Queue* q : queues) held_now += q->subs.size();
    const int64_t ns = st_submit_ns.exchange(0), mx = st_submit_max_ns.exchange(0);
    std::snprintf(out, cap, "submits %.1f a frame (%.1f chunks, %.1f of them after an event wait; %.1f command buffers; %.3f ms a frame in hsd_submit on the submitting threads, longest %.3f ms); "
                  "event signals %.1f, waits met %.1f, held at a wait %.1f (longest %.2f ms, %.2f ms a frame in all; ended by the CPU's signal %.1f, by another queue's %.1f; %zu held now), fence waits held %lld, "
                  "reports written by the CPU %.1f, resets on the CPU %.1f, CPU signals %.1f; CPU fence waits %.1f (%.3f ms a frame); submissions waiting for uploads %lld; "
                  "timeline %llu sent, %llu done; vkQueueSubmit2 %.1f a frame, %.3f ms a frame (all threads); waits for the submit lock %.1f a frame (%.3f ms a frame); "
                  "the present's fence wait %.3f ms a frame",
                  st_submits / frames, st_chunks / frames, st_chunk_waits / frames, st_cbs / frames, ns / 1e6 / frames, mx / 1e6, st_signals / frames, st_waits_met / frames, st_held / frames,
                  st_hold_max_ms, st_hold_ms / frames, st_freed_cpu / frames, st_freed_queue / frames, held_now, (long long)st_fence_held, st_reports / frames, st_resets / frames,
                  st_cpu_signals.exchange(0) / frames, st_cpu_waits.exchange(0) / frames, st_cpu_wait_ns.exchange(0) / 1e6 / frames,
                  (long long)st_upload_waits_gpu, (unsigned long long)sent.load(), (unsigned long long)sync_completed(), st_send_n.exchange(0) / frames, st_send_ns.exchange(0) / 1e6 / frames,
                  st_mu_waits.exchange(0) / frames, st_mu_wait_ns.exchange(0) / 1e6 / frames, st_fence_wait_ns.exchange(0) / 1e6 / frames);
    st_chunks = st_chunk_waits = st_upload_waits_gpu = st_cbs = st_submits = st_held = st_waits_met = st_signals = st_reports = st_resets = st_fence_held = 0;
    st_hold_max_ms = st_hold_ms = 0, st_freed_cpu = st_freed_queue = 0;
}

void sync_fill_table(MkwVkRenderer& t) {
    t.nvn_submit = nvn_submit;
    t.nvn_submit_q = nvn_submit_q;
    t.nvn_fence = nvn_fence;
    t.nvn_fence_q = nvn_fence_q;
    t.nvn_pushed = nvn_pushed;
    t.nvn_replayed = nvn_replayed;
    t.nvn_wait = nvn_wait;
    t.nvn_finish = nvn_finish;
    t.nvn_event_signal = nvn_event_signal;
}

}  // namespace hsd

using namespace hsd;

extern "C" __attribute__((visibility("default"))) void hsd_submit(uint64_t queue, void* const* lists, uint32_t n) {
    const auto t0 = clk::now();
    Sub s;
    s.lists.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        List* l = (List*)lists[i];
        if (!l) continue;
        list_ref(l);
        if (l->slot && l->slot->frame != UINT64_MAX)
            st_age_submit[std::min<uint64_t>(present_count() - std::min(present_count(), l->slot->frame), 3)].fetch_add(1, std::memory_order_relaxed);
        list_submitted(l);
        draw_list_submitted(l);   // (M2b: the deferred binds' handles as the game's arrays hold them now)
        compute_list_submitted(l);   // (M4a: and the dispatches')
        s.lists.push_back(l);
        list_items(l, s.items, s.slots, dump_recording() ? &s.dumps : nullptr);
    }
    s.frame = present_count();
    {
        auto lk = lock_mu();
        Queue* q = queue_of(queue);
        s.seq = ++q->seq;
        s.wait_fences.swap(q->wait_fences);
        q->subs.push_back(std::move(s));
        progress();
    }
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
    st_submit_ns.fetch_add(ns, std::memory_order_relaxed);
    for (int64_t m = st_submit_max_ns.load(); ns > m && !st_submit_max_ns.compare_exchange_weak(m, ns);) {}
}

namespace hsd {
/// A present in its queue's order (the queue hsd_present_queue named on this thread; none: at once)
void sync_present(const nvn_tex* t, const int crop[4]) {
    Sub s;
    s.present = true;
    if (t) s.tex = *t;
    else s.tex.gpu = 0;
    for (int i = 0; i < 4; i++) s.crop[i] = crop ? crop[i] : 0;
    auto lk = lock_mu();
    if (!present_queue) return present_run(s.tex, s.crop);
    Queue* q = queue_of(present_queue);
    present_queue = 0;
    q->subs.push_back(std::move(s));
    progress();
}
}  // namespace hsd

extern "C" __attribute__((visibility("default"))) void hsd_present_queue(uint64_t queue) { present_queue = queue; }

extern "C" __attribute__((visibility("default"))) void hsd_queue_wait_fence(uint64_t queue, uint64_t fence) {
    std::lock_guard lk(mu);
    Queue* q = queue_of(queue);
    q->wait_fences.push_back(fence);
}
