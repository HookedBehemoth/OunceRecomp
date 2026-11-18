// What each draw and dispatch of one frame made the GPU run (direct_pstats.h).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "direct_internal.h"
#include "direct_pstats.h"

namespace hsd {

bool pstats_enabled = false;

namespace {

constexpr uint32_t kQueries = 1u << 15;
constexpr uint32_t kAfter = 10;   // (presents after the measured one when the results are read: long finished)

int64_t at = -1;   // (the present measured)
bool mesh = false;
VkQueryPool pool = VK_NULL_HANDLE;
VkQueryPool times = VK_NULL_HANDLE;   // (two timestamps a command: all earlier work done before it, and after it)
double ns_tick = 1.0;
PFN_vkCmdWriteTimestamp2 pStamp = nullptr;
uint32_t values = 0;   // (statistics a query)
std::atomic<uint32_t> next{0};
std::atomic<bool> written{false};
std::vector<PstatWhat> what;
std::vector<uint64_t> present_of;
PFN_vkCreateQueryPool pCreate = nullptr;
PFN_vkResetQueryPool pReset = nullptr;
PFN_vkCmdBeginQuery pBegin = nullptr;
PFN_vkCmdEndQuery pEnd = nullptr;
PFN_vkGetQueryPoolResults pResults = nullptr;

}  // namespace

bool pstats_wants_mesh_queries() {
    const char* v = dbg_value("pstats");
    return v != nullptr;
}

void pstats_init(bool mesh_queries) {
    const char* v = dbg_value("pstats");
    if (!v) return;
    at = std::atoll(v);
    mesh = mesh_queries;
    pCreate = (PFN_vkCreateQueryPool)vkGetDeviceProcAddr(dev.device, "vkCreateQueryPool");
    pReset = (PFN_vkResetQueryPool)vkGetDeviceProcAddr(dev.device, "vkResetQueryPool");
    pBegin = (PFN_vkCmdBeginQuery)vkGetDeviceProcAddr(dev.device, "vkCmdBeginQuery");
    pEnd = (PFN_vkCmdEndQuery)vkGetDeviceProcAddr(dev.device, "vkCmdEndQuery");
    pResults = (PFN_vkGetQueryPoolResults)vkGetDeviceProcAddr(dev.device, "vkGetQueryPoolResults");
    pStamp = (PFN_vkCmdWriteTimestamp2)vkGetDeviceProcAddr(dev.device, "vkCmdWriteTimestamp2");
    if (!dev.features.pipelineStatisticsQuery || !pCreate || !pReset || !pBegin || !pEnd || !pResults) {
        logf("direct: pstats: no pipeline statistics queries on this device");
        return;
    }
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    ci.queryCount = kQueries;
    ci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
    if (mesh) ci.pipelineStatistics |= VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT;
    values = (uint32_t)__builtin_popcount(ci.pipelineStatistics);
    if (pCreate(dev.device, &ci, nullptr, &pool) != VK_SUCCESS) {
        logf("direct: pstats: the query pool was not made");
        return;
    }
    pReset(dev.device, pool, 0, kQueries);
    {
        VkQueryPoolCreateInfo ti{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ti.queryType = VK_QUERY_TYPE_TIMESTAMP, ti.queryCount = 2 * kQueries;
        if (pStamp && pCreate(dev.device, &ti, nullptr, &times) == VK_SUCCESS) pReset(dev.device, times, 0, 2 * kQueries);
        else times = VK_NULL_HANDLE;
        ns_tick = dev.props.limits.timestampPeriod;
    }
    what.resize(kQueries);
    present_of.resize(kQueries);
    pstats_enabled = true;
    logf("direct: pstats (HOS_DBG=pstats:%lld): every draw and dispatch recorded at present %lld bracketed by a pipeline statistics "
         "query (%u statistics, mesh %d); direct_pstats.csv written %u presents later", (long long)at, (long long)at, values, (int)mesh, kAfter);
}

uint32_t pstat_before(VkCommandBuffer cb, const PstatWhat& w) {
    if ((int64_t)present_count() != at) return ~0u;
    const uint32_t i = next.fetch_add(1, std::memory_order_relaxed);
    if (i >= kQueries) return ~0u;
    what[i] = w;
    present_of[i] = present_count();
    if (times) pStamp(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, times, 2 * i);
    pBegin(cb, pool, i, 0);
    return i;
}

void pstat_after(VkCommandBuffer cb, uint32_t i) {
    if (i == ~0u) return;
    pEnd(cb, pool, i);
    if (times) pStamp(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, times, 2 * i + 1);
}

void pstats_present(uint64_t present) {
    if (!pstats_enabled || (int64_t)present != at + kAfter || written.exchange(true)) return;
    const uint32_t n = std::min(next.load(), kQueries);
    std::vector<uint64_t> r((size_t)n * (values + 1));
    VkResult res = n ? pResults(dev.device, pool, 0, n, r.size() * 8, r.data(), (values + 1) * 8,
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
                     : VK_SUCCESS;
    std::vector<uint64_t> ts((size_t)n * 4);   // (begin, its availability, end, its availability)
    if (times && n) pResults(dev.device, times, 0, 2 * n, ts.size() * 8, ts.data(), 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    FILE* f = std::fopen("direct_pstats.csv", "w");
    if (!f) return logf("direct: pstats: direct_pstats.csv not written");
    std::fprintf(f, "index,what,key,code0,code1,code2,kind,lr,w,h,format,targets,count,instances,extra,available,ia_vertices,ia_primitives,"
                    "vs_invocations,clip_invocations,clip_primitives,fs_invocations,cs_invocations,ms_invocations,t_begin_ns,t_end_ns\n");
    uint32_t avail = 0;
    uint64_t fs = 0, vs = 0, cs = 0, ms = 0;
    for (uint32_t i = 0; i < n; i++) {
        const PstatWhat& w = what[i];
        const uint64_t* v = &r[(size_t)i * (values + 1)];
        const bool ok = v[values] != 0;
        avail += ok;
        // (the statistics in bit order: IA vertices, IA primitives, VS, (GS x2 not asked), clipping x2, FS, (tess x2 not asked), CS, then mesh)
        const uint64_t mv = mesh ? v[7] : 0;
        const bool tok = times && ts[4ull * i + 1] && ts[4ull * i + 3];
        const double tb = tok ? (double)ts[4ull * i] * ns_tick : -1, te = tok ? (double)ts[4ull * i + 2] * ns_tick : -1;
        std::fprintf(f, "%u,%s,%016llx,%016llx,%016llx,%016llx,%u,%#llx,%u,%u,%d,%u,%u,%u,%u,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.0f,%.0f\n", i, w.what,
                     (unsigned long long)w.key, (unsigned long long)w.codes[0], (unsigned long long)w.codes[1], (unsigned long long)w.codes[2],
                     w.kind, (unsigned long long)w.lr, w.w, w.h, w.format, w.targets, w.count, w.instances, w.extra, (int)ok,
                     (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3],
                     (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6], (unsigned long long)mv, tb, te);
        if (ok) vs += v[2], fs += v[5], cs += v[6], ms += mv;
    }
    std::fclose(f);
    logf("direct: pstats: present %lld: %u commands measured, %u results available (%d); vertex invocations %llu, fragment %llu, "
         "compute %llu, mesh %llu; direct_pstats.csv written", (long long)at, n, avail, (int)res, (unsigned long long)vs,
         (unsigned long long)fs, (unsigned long long)cs, (unsigned long long)ms);
}

}  // namespace hsd
