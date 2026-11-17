// Reports and render conditions on the GPU (direct_query.h has the rules).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

#include "direct_check.h"
#include "os.h"
#include "direct_cmd.h"
#include "direct_compute.h"
#include "direct_counters.h"
#include "direct_draw.h"
#include "direct_heap.h"
#include "direct_query.h"
#include <shaders/report_resolve_spv.h>

namespace hsd {

constexpr uint32_t kRecs = 4096, kOcc = 4096, kSegs = 4;
struct Rec {   // (report_resolve.comp's)
    uint64_t addr;
    uint32_t kind, n;
    uint32_t occ[kSegs];
};
static_assert(sizeof(Rec) == 32);
struct QSlot {
    VkQueryPool occ = VK_NULL_HANDLE, ts = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    Mem* mem = nullptr;
    uint8_t* map = nullptr;
    VkDeviceAddress addr = 0;
    uint32_t recs = 0, occs = 0;   // (used since the reset: the owner thread's alone)
    Rec* rec() const { return (Rec*)map; }
    static constexpr VkDeviceSize kOccAt = sizeof(Rec) * kRecs, kTsAt = kOccAt + 8 * kOcc, kBytes = kTsAt + 8 * kRecs;
};
/// A report waiting to be written: its record, and the queries it sums
struct Pend {
    QSlot* q;
    uint32_t rec, n;
    uint32_t occ[kSegs];
};
struct ListQuery {
    bool counting = false;   // (a samples-passed reset seen: the counter runs)
    bool has_ops = false;    // (it begins queries of its own: a job like that is not executed inside its caller's query)
    int32_t open = -1;       // (the occlusion query active in the command buffer now)
    QSlot* open_q = nullptr;
    uint32_t segs[kSegs]{}, nsegs = 0;   // (the queries since the reset, ended)
    bool over = false;                   // (more stretches than kSegs since the reset)
    bool clobbered = false;              // (a job with counters of its own ran since the reset: NVN's counter is the job's then)
    QSlot* segs_q = nullptr;
    std::vector<Pend> pend;
    bool cond_want = false, cond_on = false;   // (a condition set; begun in the command buffer)
    uint64_t cond_addr = 0;
    int32_t cond_mode = 0;
};

namespace {

PFN_vkCreateQueryPool pCreateQueryPool;
PFN_vkResetQueryPool pResetQueryPool;
PFN_vkCmdBeginQuery pBeginQuery;
PFN_vkCmdEndQuery pEndQuery;
PFN_vkCmdWriteTimestamp2 pWriteTimestamp2;
PFN_vkCmdCopyQueryPoolResults pCopyResults;
PFN_vkCmdDispatch pDispatch;
PFN_vkCmdBeginConditionalRenderingEXT pBeginCond;
PFN_vkCmdEndConditionalRenderingEXT pEndCond;
PFN_vkGetCalibratedTimestampsKHR pCalibrated;

VkPhysicalDeviceConditionalRenderingFeaturesEXT cond_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
bool has_cond = false, has_calibrated = false, ready = false;
VkShaderEXT resolve = VK_NULL_HANDLE;
std::atomic<uint64_t> cal_dev{0};
std::atomic<double> cal_host{0}, cal_scale{0};

enum {
    Q_REPORTS, Q_SAMPLES, Q_TIMESTAMP, Q_TOP, Q_OTHER, Q_RESETS, Q_RESETS_OTHER, Q_QUERIES, Q_STRETCHES, Q_NO_RESET,
    Q_SUMMED, Q_TOO_MANY, Q_NO_ROOM, Q_NOT_MEMORY, Q_UNRECORDED, Q_WRITTEN, Q_FLUSHES, Q_DISPATCHES, Q_IN_JOB,
    Q_CONDS, Q_COND_JOB, Q_COND_BEGUN, Q_COND_DRAWS, Q_COND_CLEARS, Q_COND_PENDING, Q_COND_Y, Q_COND_HI, Q_COND_NOT_MEMORY,
    Q_COND_AT_EXEC, Q_EXEC_COUNTING, Q_EXEC_OWN, Q_JOB_PENDING_LEFT, Q_CLOBBERED, Q_COUNT
};
const char* const kNames[Q_COUNT] = {
    "reports", "samples passed", "timestamp", "timestamp (top)", "other types", "resets of samples passed", "resets of other types",
    "occlusion queries", "stretches ended at a rendering's end", "samples reports with no reset in their list",
    "reports summing stretches", "reports past 4 stretches (summed in part)", "reports with no room in the slot (not written)",
    "reports not in layer 0 (not written)", "reports in lists that record nothing (not written)", "reports written by the GPU",
    "flushes", "resolve dispatches", "reports in job secondaries", "conditions", "of them in job secondaries",
    "conditions begun", "gated draws", "gated clears", "conditions on a report not written yet in their list",
    "conditions whose second word is not 0 (condcheck)", "conditions whose first word is past 32 bits (condcheck)",
    "conditions not in layer 0 (drawn)", "jobs executed under their caller's condition", "jobs executed in a counting rendering",
    "jobs with counters of their own executed in a counting rendering", "job reports never executed", "reports after a job with counters of its own since their reset (not NVN's value)"};
struct QueryStats;
ThreadCounters<QueryStats, Q_COUNT> st;
void count(int k, int64_t n = 1) {
    st.add(k, n);
    // (M6b: a report not written, summed in part, or not NVN's value, and a condition not evaluated: strict's events)
    if (strict_on() && (k == Q_TOO_MANY || k == Q_NO_ROOM || k == Q_NOT_MEMORY || k == Q_UNRECORDED || k == Q_COND_NOT_MEMORY ||
                        k == Q_JOB_PENDING_LEFT || k == Q_CLOBBERED || k == Q_COND_PENDING))
        strict_event(kNames[k], "%lld at present %llu", (long long)n, (unsigned long long)present_count());
}

ListQuery& qx(List* l) {
    if (!l->qctx) l->qctx = new ListQuery;
    return *l->qctx;
}

QSlot* slot_of(List* l) {
    if (!l->slot) return nullptr;
    QSlot*& q = l->slot->q;
    if (q) return q;
    QSlot* n = new QSlot;
    VkQueryPoolCreateInfo pi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    pi.queryType = VK_QUERY_TYPE_OCCLUSION, pi.queryCount = kOcc;
    VkResult r = pCreateQueryPool(dev.device, &pi, nullptr, &n->occ);
    pi.queryType = VK_QUERY_TYPE_TIMESTAMP, pi.queryCount = kRecs;
    if (r == VK_SUCCESS) r = pCreateQueryPool(dev.device, &pi, nullptr, &n->ts);
    void* map = nullptr;
    if (r != VK_SUCCESS || !mem_buffer(QSlot::kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, n->buf, n->mem, &map, MO_QUERY) || !map) {
        logf("direct: a command slot's query pools or report buffer not made");
        host->log_flush();
        abort();
    }
    object_made(2, "a command slot's query pools and report buffer");
    n->map = (uint8_t*)map;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = n->buf;
    n->addr = vkGetBufferDeviceAddress(dev.device, &ai);
    pResetQueryPool(dev.device, n->occ, 0, kOcc);
    pResetQueryPool(dev.device, n->ts, 0, kRecs);
    static std::atomic<int> told{0};
    if (present_count() > 1200 && told++ < 8) logf("direct: query pools made for a command slot at present %llu", (unsigned long long)present_count());
    return q = n;
}

void cond_end(ListQuery& x, VkCommandBuffer cb) {
    if (x.cond_on) pEndCond(cb), x.cond_on = false;
}
/// The open stretch ended (kept: true, it counts into the reports after it)
void stretch_end(ListQuery& x, VkCommandBuffer cb, bool keep) {
    if (x.open < 0) return;
    pEndQuery(cb, x.open_q->occ, (uint32_t)x.open);
    if (keep) {
        if (x.segs_q != x.open_q) x.nsegs = 0, x.segs_q = x.open_q;   // (the counter's stretches all of one slot: a list's)
        if (x.nsegs < kSegs) x.segs[x.nsegs++] = (uint32_t)x.open;
        else x.segs[kSegs - 1] = (uint32_t)x.open, x.over = true;   // (counted at a report that reads it)
    }
    x.open = -1, x.open_q = nullptr;
}

// ---- (HOS_DBG=replog) a frame's reports and conditions every 600 presents (at 300 past), read back 4 presents later ----
struct Seen {
    uint64_t addr;
    uint32_t kind;   // (a counter type, or 100 + a condition's mode)
};
std::mutex log_mu;
std::vector<Seen> log_seen;
std::atomic<uint64_t> log_frame{0};
std::atomic<bool> log_due{false};
void log_note(uint64_t addr, uint32_t kind) {
    static const bool on = dbg("replog");
    if (!on) return;
    const uint64_t f = present_count();
    if (f % 600 == 300 && f > 300) {
        std::lock_guard lk(log_mu);
        if (log_frame.load() != f) log_seen.clear(), log_frame.store(f), log_due.store(true);
        if (log_seen.size() < 2048) log_seen.push_back({addr, kind});
        return;
    }
    if (!log_due.load(std::memory_order_relaxed) || f < log_frame.load() + 4) return;
    std::vector<Seen> v;
    {
        std::lock_guard lk(log_mu);
        if (!log_due.exchange(false)) return;
        v.swap(log_seen);
    }
    std::vector<uint64_t> samples;
    char first[400];
    int k = 0;
    first[0] = 0;
    uint32_t conds = 0, on_now = 0, ts_n = 0;
    double ts_lag_ms = 0;
    const uint64_t now19 = host->ticks() * 32 * 12 / 625;
    std::vector<uint64_t> pools;
    for (const Seen& s : v) {
        uint64_t w[3]{};
        host->gpu_read(s.addr, w, s.kind >= 100 ? 24 : 16);
        if (s.kind == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
            samples.push_back(w[0]);
            if (k < 360) k += std::snprintf(first + k, sizeof first - k, " %llu", (unsigned long long)w[0]);
        } else if (s.kind == NVN_COUNTER_TYPE_TIMESTAMP || s.kind == NVN_COUNTER_TYPE_TIMESTAMP_TOP) {
            ts_n++, ts_lag_ms += (double)((int64_t)(now19 - w[1])) * 625 / 12 / 1e6;
        } else if (s.kind >= 100) {
            conds++;
            if ((w[0] != w[2]) == (s.kind == 101)) on_now++;
        }
        uint64_t base, size;
        uint32_t flags;
        bool in_vram;
        if (pools.size() < 6 && layer0_pool_of(s.addr, base, size, flags, in_vram) &&
            std::find(pools.begin(), pools.end(), base) == pools.end()) {
            pools.push_back(base);
            logf("direct: replog: reports at %#llx in pool %#llx+%#llx flags %#x, %s", (unsigned long long)s.addr, (unsigned long long)base,
                 (unsigned long long)size, flags, in_vram ? "in VRAM" : "host memory");
        }
    }
    std::vector<uint64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const size_t zeros = (size_t)(std::upper_bound(sorted.begin(), sorted.end(), 0ull) - sorted.begin());
    logf("direct: replog present %llu (read at %llu): %zu samples reports (%zu zero; median %llu, max %llu), first:%s; %u timestamps %.2f ms "
         "before now (mean); %u conditions, %u of them render now", (unsigned long long)log_frame.load(), (unsigned long long)f, samples.size(), zeros,
         sorted.empty() ? 0ull : (unsigned long long)sorted[sorted.size() / 2], sorted.empty() ? 0ull : (unsigned long long)sorted.back(), first,
         ts_n, ts_n ? ts_lag_ms / ts_n : 0.0, conds, on_now);
}

void report(List* l, ListQuery& x, const nvn_p_counter& c, bool rendering, CmdOf cmd) {
    const uint32_t type = (uint32_t)c.type;
    log_note(c.addr, type);
    count(Q_REPORTS);
    count(type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? Q_SAMPLES : type == NVN_COUNTER_TYPE_TIMESTAMP ? Q_TIMESTAMP
                                                  : type == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? Q_TOP : Q_OTHER);
    if (l->secondary) count(Q_IN_JOB);
    if (!l->rec) return count(Q_UNRECORDED);
    VkBuffer b;
    VkDeviceSize off;
    if ((c.addr & 7) || !layer0_span(c.addr, 16, b, off)) return count(Q_NOT_MEMORY);
    VkCommandBuffer cb = cmd(l);
    QSlot* q = slot_of(l);
    if (!q || q->recs == kRecs) return count(Q_NO_ROOM);
    const uint32_t ri = q->recs++;
    Rec& r = q->rec()[ri];
    r.addr = c.addr, r.kind = type, r.n = 0;
    Pend p{q, ri, 0, {}};
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
        stretch_end(x, cb, true);   // (the counter goes on: the next draw opens the next stretch)
        if (!x.counting) count(Q_NO_RESET);
        if (x.nsegs > 1) count(Q_SUMMED);
        if (x.over) count(Q_TOO_MANY);
        if (x.clobbered) count(Q_CLOBBERED);
        if (x.segs_q == q || !x.nsegs) {
            r.n = p.n = x.nsegs;
            for (uint32_t k = 0; k < x.nsegs; k++) r.occ[k] = p.occ[k] = x.segs[k];
        }
    }
    const VkPipelineStageFlags2 stage = type == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    pWriteTimestamp2(cb, stage, q->ts, ri);
    x.pend.push_back(p);
    if (!rendering && !l->secondary) query_flush(l, cmd);   // (outside a rendering: written here, in the list's order)
}

}  // namespace

void query_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail) {
    auto has = [&](const char* n) {
        for (const auto& e : exts)
            if (!std::strcmp(e.extensionName, n)) return true;
        return false;
    };
    if ((has_cond = has(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME)))
        want.push_back(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME), *tail = &cond_feat, tail = &cond_feat.pNext;
    if ((has_calibrated = has(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))) want.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
}

VkBufferUsageFlags query_window_usage() { return has_cond ? VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT : 0; }

namespace {
void calibrate() {
    if (!pCalibrated) return;
    VkCalibratedTimestampInfoKHR ti[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}, {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
    ti[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR, ti[1].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    uint64_t t[2], dev_max = 0;
    const int64_t a = os_now_ns();   // (CLOCK_MONOTONIC, the time domain asked for)
    const uint64_t h = host->ticks() * 32;   // (the guest's tick clock in ns, as nvnDeviceGetCurrentTimestampInNanoseconds)
    const int64_t z = os_now_ns();
    if (pCalibrated(dev.device, 2, ti, t, &dev_max) != VK_SUCCESS) return;
    const double mono_at_h = ((double)a + (double)z) / 2;
    const double host_ns = (double)h + ((double)t[1] - mono_at_h);   // (the device timestamp's moment on the guest clock)
    cal_host.store(host_ns * 12 / 625, std::memory_order_relaxed);   // (counter ticks: 19.2 MHz)
    cal_scale.store(dev.props.limits.timestampPeriod * 12.0 / 625, std::memory_order_relaxed);
    cal_dev.store(t[0], std::memory_order_release);
}
}  // namespace

void query_device_created() {
#define L(f, t, n) p##f = (t)vkGetDeviceProcAddr(dev.device, n)
    L(CreateQueryPool, PFN_vkCreateQueryPool, "vkCreateQueryPool");
    L(ResetQueryPool, PFN_vkResetQueryPool, "vkResetQueryPool");
    L(BeginQuery, PFN_vkCmdBeginQuery, "vkCmdBeginQuery");
    L(EndQuery, PFN_vkCmdEndQuery, "vkCmdEndQuery");
    L(WriteTimestamp2, PFN_vkCmdWriteTimestamp2, "vkCmdWriteTimestamp2");
    L(Dispatch, PFN_vkCmdDispatch, "vkCmdDispatch");
#undef L
    pCopyResults = (PFN_vkCmdCopyQueryPoolResults)vkGetDeviceProcAddr(dev.device, "vkCmdCopyQueryPoolResults");
    pBeginCond = (PFN_vkCmdBeginConditionalRenderingEXT)vkGetDeviceProcAddr(dev.device, "vkCmdBeginConditionalRenderingEXT");
    pEndCond = (PFN_vkCmdEndConditionalRenderingEXT)vkGetDeviceProcAddr(dev.device, "vkCmdEndConditionalRenderingEXT");
    if (has_calibrated) pCalibrated = (PFN_vkGetCalibratedTimestampsKHR)vkGetDeviceProcAddr(dev.device, "vkGetCalibratedTimestampsKHR");
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    ci.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
    ci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    ci.codeSize = sizeof report_resolve_spv;
    ci.pCode = report_resolve_spv;
    ci.pName = "main";
    const VkResult r = heap_on() ? vkCreateShadersEXT(dev.device, 1, &ci, nullptr, &resolve) : VK_ERROR_FEATURE_NOT_PRESENT;
    if (r == VK_SUCCESS) object_made(1, "the report resolve shader");
    calibrate();
    ready = r == VK_SUCCESS && has_cond && cond_feat.conditionalRendering && pCopyResults && pCalibrated && dev.features.occlusionQueryPrecise &&
            dev.features.inheritedQueries;
    logf("direct: reports and conditions on the GPU: %s (conditional rendering %d, inherited %d; calibrated timestamps %d; resolve program "
         "%d; timestamp period %.3f ns, %u valid bits)", ready ? "on" : "NOT AVAILABLE: reports are not written, conditions draw",
         (int)(has_cond && cond_feat.conditionalRendering), (int)cond_feat.inheritedConditionalRendering, (int)(pCalibrated != nullptr), (int)r,
         dev.props.limits.timestampPeriod, 64u);
}

void query_slot_reset(QSlot* q) {
    if (!q) return;
    if (q->occs) pResetQueryPool(dev.device, q->occ, 0, q->occs);
    if (q->recs) pResetQueryPool(dev.device, q->ts, 0, q->recs);
    q->occs = q->recs = 0;
}

void query_op(List* l, uint32_t op, const void* data, uint32_t size, bool rendering, CmdOf cmd) {
    if (!ready) return;
    ListQuery& x = qx(l);
    switch (op) {
    case NOP_RESET_COUNTER: {
        nvn_p_counter c;
        if (size < sizeof c) return;
        std::memcpy(&c, data, sizeof c);
        if ((uint32_t)c.type != NVN_COUNTER_TYPE_SAMPLES_PASSED) return count(Q_RESETS_OTHER);
        count(Q_RESETS);
        x.has_ops = true;
        if (x.open >= 0) stretch_end(x, l->cur, false);   // (its samples are not counted)
        x.counting = true, x.nsegs = 0, x.segs_q = nullptr, x.over = false, x.clobbered = false;
        return;
    }
    case NOP_REPORT_COUNTER: {
        nvn_p_counter c;
        if (size < sizeof c) return;
        std::memcpy(&c, data, sizeof c);
        if ((uint32_t)c.type == NVN_COUNTER_TYPE_SAMPLES_PASSED) x.has_ops = true;
        return report(l, x, c, rendering, cmd);
    }
    case NOP_RENDER_COND: {
        nvn_p_render_cond r;
        if (size < sizeof r) return;
        std::memcpy(&r, data, sizeof r);
        count(Q_CONDS);
        log_note(r.addr, 100 + (uint32_t)r.mode);
        if (l->secondary) count(Q_COND_JOB);
        if (x.cond_on && l->cur) cond_end(x, l->cur);
        x.cond_want = true, x.cond_addr = r.addr, x.cond_mode = r.mode;
        for (const Pend& p : x.pend)
            if (p.q->rec()[p.rec].addr == r.addr) {
                count(Q_COND_PENDING);
                break;
            }
        static const bool check = dbg("condcheck");
        if (check) {
            uint64_t a[3]{};
            host->gpu_read(r.addr, a, 24);
            if (a[2]) count(Q_COND_Y);
            if (a[0] >> 32) count(Q_COND_HI);
        }
        return;
    }
    case NOP_RENDER_ENABLE:
        if (x.cond_on && l->cur) cond_end(x, l->cur);
        x.cond_want = false;
        return;
    default: return;
    }
}

void query_draw(List* l, VkCommandBuffer cb) {
    if (!ready || !l->qctx) return;
    ListQuery& x = *l->qctx;
    if (x.counting && x.open < 0) {
        QSlot* q = slot_of(l);
        if (q && q->occs < kOcc) {
            x.open = (int32_t)q->occs++, x.open_q = q;
            pBeginQuery(cb, q->occ, (uint32_t)x.open, VK_QUERY_CONTROL_PRECISE_BIT);
            count(Q_QUERIES);
        } else {
            count(Q_NO_ROOM);
        }
    }
    static const bool nocond = dbg("qnocond");   // (A/B aid: conditions not evaluated, their draws drawn)
    if (x.cond_want && !x.cond_on && !nocond) {
        VkBuffer b;
        VkDeviceSize off;
        if (!layer0_span(x.cond_addr, 4, b, off)) return count(Q_COND_NOT_MEMORY);
        VkConditionalRenderingBeginInfoEXT bi{VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT};
        bi.buffer = b, bi.offset = off;
        bi.flags = x.cond_mode == 0 ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT : 0;   // (render when equal to 0 / not)
        pBeginCond(cb, &bi);
        x.cond_on = true;
        count(Q_COND_BEGUN);
    }
    if (x.cond_want) count(Q_COND_DRAWS);
}

bool query_cond_wanted(const List* l) { return ready && l->qctx && l->qctx->cond_want; }

void query_suspend(List* l, VkCommandBuffer cb) {
    if (!l->qctx || !cb) return;
    ListQuery& x = *l->qctx;
    cond_end(x, cb);
    if (x.open >= 0) stretch_end(x, cb, true), count(Q_STRETCHES);
}

void query_flush(List* l, CmdOf cmd) {
    if (!ready || !l->qctx || l->qctx->pend.empty() || l->secondary) return;
    ListQuery& x = *l->qctx;
    VkCommandBuffer cb = cmd(l);
    count(Q_FLUSHES);
    // (the queries' results into their slots' buffers: each slot's runs of consecutive indices)
    std::sort(x.pend.begin(), x.pend.end(), [](const Pend& a, const Pend& b) { return a.q != b.q ? a.q < b.q : a.rec < b.rec; });
    std::vector<uint32_t> occ;
    for (size_t i = 0; i < x.pend.size();) {
        size_t j = i;
        QSlot* q = x.pend[i].q;
        occ.clear();
        while (j < x.pend.size() && x.pend[j].q == q) {
            for (uint32_t k = 0; k < x.pend[j].n; k++) occ.push_back(x.pend[j].occ[k]);
            j++;
        }
        auto runs = [&](std::vector<uint32_t>& v, VkQueryPool pool, VkDeviceSize at) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            for (size_t a = 0; a < v.size();) {
                size_t b = a + 1;
                while (b < v.size() && v[b] == v[b - 1] + 1) b++;
                pCopyResults(cb, pool, v[a], (uint32_t)(b - a), q->buf, at + 8ull * v[a], 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                a = b;
            }
        };
        runs(occ, q->occ, QSlot::kOccAt);
        std::vector<uint32_t> ts;
        for (size_t k = i; k < j; k++) ts.push_back(x.pend[k].rec);
        runs(ts, q->ts, QSlot::kTsAt);
        i = j;
    }
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb, &di);
    heap_bind(cb);
    const VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    vkCmdBindShadersEXT(cb, 1, &stage, &resolve);
    struct Push {
        VkDeviceAddress recs, idx, occ, ts, windows;
        uint64_t dev0;
        double host0, scale;
        uint32_t n, pad;
    } push{};
    push.windows = draw_window_table();
    push.dev0 = cal_dev.load(std::memory_order_acquire), push.host0 = cal_host.load(std::memory_order_relaxed);
    push.scale = cal_scale.load(std::memory_order_relaxed);
    static const bool cpu_ts = dbg("qcputs"), fixed = dbg("qfixed");   // (A/B aids: timestamps of the recording time; samples 4096)
    if (cpu_ts) push.scale = 0, push.host0 = (double)(host->ticks() * 32 * 12 / 625);
    push.pad = fixed ? 4096 : 0;
    for (size_t i = 0; i < x.pend.size();) {
        size_t j = i;
        QSlot* q = x.pend[i].q;
        while (j < x.pend.size() && x.pend[j].q == q) j++;
        uint8_t* map;
        VkDeviceAddress a;
        const uint32_t n = (uint32_t)(j - i);
        if (!draw_arena(l->slot, 4 * n, map, a)) {
            count(Q_NO_ROOM, n);
            i = j;
            continue;
        }
        for (uint32_t k = 0; k < n; k++) std::memcpy(map + 4 * k, &x.pend[i + k].rec, 4);
        push.recs = q->addr, push.idx = a, push.occ = q->addr + QSlot::kOccAt, push.ts = q->addr + QSlot::kTsAt, push.n = n;
        heap_push(cb, &push, sizeof push);
        pDispatch(cb, (n + 63) / 64, 1, 1);
        count(Q_DISPATCHES), count(Q_WRITTEN, n);
        i = j;
    }
    // (the reports' words visible to all that follows: conditions, shaders, copies; the CPU reads them when it likes)
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier2(cb, &di);
    x.pend.clear();
    draw_push_invalidate(l);   // (the push data and the bound compute program are this step's now)
    compute_invalidate(l);
}

void query_execute(List* l, VkCommandBuffer cb, const List* job) {
    if (!ready) return;
    ListQuery& x = qx(l);
    if (x.cond_want) count(Q_COND_AT_EXEC);
    cond_end(x, cb);
    const bool own = job->qctx && job->qctx->has_ops;
    if (x.counting) {
        count(Q_EXEC_COUNTING);
        if (own) count(Q_EXEC_OWN), query_suspend(l, cb), x.clobbered = true;   // (its queries may not run inside the caller's)
        else query_draw(l, cb);                            // (the job's draws count: the secondary inherits the query)
    }
}
void query_executed(List* l, const List* job) {
    if (!ready || !job->qctx || job->qctx->pend.empty()) return;
    ListQuery& x = qx(l);
    x.pend.insert(x.pend.end(), job->qctx->pend.begin(), job->qctx->pend.end());
    job->qctx->pend.clear();
}

void query_list_free(List* l) {
    if (l->qctx && l->secondary && !l->qctx->pend.empty()) count(Q_JOB_PENDING_LEFT, (int64_t)l->qctx->pend.size());
    delete l->qctx;
    l->qctx = nullptr;
}

void query_stats(char* out, size_t cap, double frames) {
    calibrate();
    if (const char* v = dbg_value("poolof")) {   // (HOS_DBG=poolof:<hex>+<hex>...: the pools of those guest addresses, at each line)
        for (const char* s = v; *s;) {
            char* e;
            const uint64_t a = strtoull(s, &e, 16);
            if (e == s) break;
            uint64_t base = 0, size = 0;
            uint32_t flags = 0;
            bool in_vram = false;
            const bool found = layer0_pool_of(a, base, size, flags, in_vram);
            logf("direct: poolof %#llx: %s pool %#llx+%#llx flags %#x, %s", (unsigned long long)a, found ? "in" : "NOT in a", (unsigned long long)base,
                 (unsigned long long)size, flags, in_vram ? "in VRAM" : "host memory");
            s = *e == '+' ? e + 1 : e;
        }
    }
    int k = std::snprintf(out, cap, "reports a frame:");
    for (int i = 0; i < Q_COUNT; i++) {
        if (i == Q_CONDS) logf("direct: %s", out), k = std::snprintf(out, cap, "conditions a frame:");
        k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "%s %s %.1f", i == 0 || i == Q_CONDS ? "" : ",", kNames[i], st.take(i) / frames);
    }
    static const bool log_pass = dbg_log("logpass");   // (the caller prints nothing for an empty text)
    if (!log_pass) out[0] = 0;
}

}  // namespace hsd
