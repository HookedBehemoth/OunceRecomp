// The GPU's time by pass over a window of presents (vk_passtime.h).
#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "vk_internal.h"
#include "vk_passtime.h"
#include "vk_pstats.h"

namespace hsd {

std::atomic<bool> passtime_rec{false};

namespace {

constexpr uint32_t kStamps = 1u << 18;
constexpr uint64_t kAfter = 4;   // (presents after the window when the results are read: the GPU is past it)
constexpr uint8_t kTop = 0xff;   // (a start stamp's entry in what_of)

VkQueryPool pool = VK_NULL_HANDLE;
PFN_vkCmdWriteTimestamp2 pStamp = nullptr;
PFN_vkResetQueryPool pReset = nullptr;
PFN_vkGetQueryPoolResults pResults = nullptr;
double ns_tick = 1.0;
std::atomic<uint32_t> next{0};
std::atomic<uint64_t> start_at{0}, frames{0};   // (the window: the present it was asked at, and its presents; 0 none asked)
std::vector<PassLabel> labels;
std::vector<uint8_t> what_of;
bool read_done = false;

uint64_t label_key(const PassLabel& l) {
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
    mix(l.what), mix(l.kind), mix(l.lr), mix(l.rt), mix(l.w), mix(l.h), mix(l.targets), mix((uint32_t)l.format), mix((uint32_t)l.depth);
    mix(l.pass_key);
    if (l.what == PT_DISPATCH) mix(l.prog);
    return h;
}

uint32_t take() {
    const uint32_t i = next.fetch_add(1, std::memory_order_relaxed);
    return i < kStamps ? i : ~0u;
}

void read_results(uint64_t presents) {
    const uint32_t n = std::min(next.load(), kStamps);
    const uint32_t lost = next.load() > kStamps ? next.load() - kStamps : 0;
    std::vector<uint64_t> r((size_t)n * 2);
    if (n) pResults(dev.device, pool, 0, n, r.size() * 8, r.data(), 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    std::vector<std::pair<uint64_t, uint32_t>> order;
    order.reserve(n);
    uint32_t unavailable = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!r[2ull * i + 1]) unavailable++;
        else order.push_back({r[2ull * i], i});
    }
    std::sort(order.begin(), order.end());
    struct Agg {
        PassLabel l;
        uint64_t count = 0, draws = 0, jobs = 0;
        double ns = 0;
    };
    std::unordered_map<uint64_t, Agg> agg;
    double busy = 0, idle = 0;
    uint64_t cursor = order.empty() ? 0 : order.front().first;
    for (const auto& [t, i] : order) {
        if (what_of[i] == kTop) {
            if (t > cursor) idle += (double)(t - cursor) * ns_tick, cursor = t;
            continue;
        }
        const double d = t > cursor ? (double)(t - cursor) * ns_tick : 0;
        cursor = std::max(cursor, t);
        const PassLabel& l = labels[i];
        Agg& a = agg[label_key(l)];
        if (!a.count) a.l = l;
        a.l.prog = l.prog ? l.prog : a.l.prog;
        a.count++, a.draws += l.draws, a.jobs += l.jobs, a.ns += d;
        busy += d;
    }
    std::vector<const Agg*> rows;
    for (const auto& [k, a] : agg) rows.push_back(&a);
    std::sort(rows.begin(), rows.end(), [](const Agg* x, const Agg* y) { return x->ns > y->ns; });
    FILE* f = std::fopen("passtime.csv", "w");
    if (!f) return logf("direct: passtime: passtime.csv not written");
    std::fprintf(f, "# presents=%llu busy_ns=%.0f idle_ns=%.0f stamps=%u lost=%u unavailable=%u\n", (unsigned long long)presents, busy, idle, n,
                 lost, unavailable);
    std::fprintf(f, "what,kind,lr,rt,w,h,targets,format,depth,pass_key,prog,count,draws,jobs,ns\n");
    static const char* const kWhat[PT_KINDS] = {"render", "dispatch", "task", "copy", "clear", "present"};
    for (const Agg* a : rows) {
        const PassLabel& l = a->l;
        std::fprintf(f, "%s,%u,%#llx,%u,%u,%u,%u,%d,%d,%#llx,%016llx,%llu,%llu,%llu,%.0f\n", kWhat[l.what], l.kind, (unsigned long long)l.lr, l.rt,
                     l.w, l.h, l.targets, l.format, l.depth, (unsigned long long)l.pass_key, (unsigned long long)l.prog,
                     (unsigned long long)a->count, (unsigned long long)a->draws, (unsigned long long)a->jobs, a->ns);
    }
    std::fclose(f);
    const double fr = presents ? (double)presents : 1;
    logf("direct: passtime: %llu presents: GPU %.3f ms a frame in %zu labels, idle %.3f ms a frame; %u stamps (%u lost, %u not available); "
         "passtime.csv written", (unsigned long long)presents, busy / 1e6 / fr, rows.size(), idle / 1e6 / fr, n, lost, unavailable);
}

}  // namespace

void passtime_init() {
    pStamp = (PFN_vkCmdWriteTimestamp2)vkGetDeviceProcAddr(dev.device, "vkCmdWriteTimestamp2");
    pReset = (PFN_vkResetQueryPool)vkGetDeviceProcAddr(dev.device, "vkResetQueryPool");
    pResults = (PFN_vkGetQueryPoolResults)vkGetDeviceProcAddr(dev.device, "vkGetQueryPoolResults");
    ns_tick = dev.props.limits.timestampPeriod;
}

void passtime_top(VkCommandBuffer cb) {
    const uint32_t i = take();
    if (i == ~0u) return;
    what_of[i] = kTop;
    pStamp(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool, i);
}

void passtime_end(VkCommandBuffer cb, const PassLabel& l) {
    const uint32_t i = take();
    if (i == ~0u) return;
    labels[i] = l;
    what_of[i] = l.what;
    pStamp(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, i);
}

void passtime_present(uint64_t present) {
    const uint64_t s = start_at.load(std::memory_order_acquire), n = frames.load(std::memory_order_relaxed);
    if (!s || read_done) return;
    if (present == s + n) passtime_rec.store(false, std::memory_order_release);
    else if (present == s + n + kAfter) read_done = true, read_results(n);
}

}  // namespace hsd

/// The next `presents` presents measured by pass (the host's bench, at its measured frames' start); debug.pstats=bench
/// measures the next present's draws
extern "C" __attribute__((visibility("default"))) void gpu_measure(uint32_t presents) {
    using namespace hsd;
    pstats_arm(present_count() + 2);
    if (!presents || start_at.load() || !pStamp || !pReset || !pResults) return;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP, ci.queryCount = kStamps;
    auto create = (PFN_vkCreateQueryPool)vkGetDeviceProcAddr(dev.device, "vkCreateQueryPool");
    if (!create || create(dev.device, &ci, nullptr, &pool) != VK_SUCCESS) return logf("direct: passtime: no query pool");
    pReset(dev.device, pool, 0, kStamps);
    labels.resize(kStamps);
    what_of.resize(kStamps);
    const uint64_t at = present_count();
    frames.store(presents);
    start_at.store(at, std::memory_order_release);
    passtime_rec.store(true, std::memory_order_release);   // (the lists recorded from now on: the next presents')
    logf("direct: passtime: presents %llu-%llu measured by pass", (unsigned long long)at + 1, (unsigned long long)(at + presents));
}
