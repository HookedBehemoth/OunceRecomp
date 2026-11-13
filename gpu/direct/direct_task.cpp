// The task stage as a compute step (direct_task.h).
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "direct_check.h"
#include "direct_cmd.h"
#include "direct_draw.h"
#include "direct_heap.h"
#include "direct_marks.h"
#include "direct_pstats.h"
#include "direct_meshcap.h"
#include "direct_pass.h"
#include "direct_shader.h"
#include "direct_task.h"
#include "shaders/task_prep_spv.h"

namespace hsd {
bool layer0_address(uint64_t gpu, uint64_t size, VkDeviceAddress& a);   // (direct_pools.cpp)

/// A recording thread's task pool for one frame slot: the indirect draws' maps and payload areas, taken by the GPU (the prep
/// step's atomic on word 0); word 1 counts the draws that did not fit. Reset with its slot, when the GPU is past the slot's
/// last use: a pool that refused a draw is made twice as large then (a Vulkan object made, counted; never in steady play)
struct TaskPool {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    uint64_t bytes = 0;
};

namespace {

constexpr uint64_t kPoolFirst = 8ull << 20, kPoolMost = 512ull << 20;
constexpr uint32_t kHeader = 64;

enum {
    T_DIRECT, T_INDIRECT, T_INLINE, T_JOB, T_EMPTY, T_CLIPPED, T_POOLS, T_GROWN, T_REFUSED, T_COUNT
};
const char* const kNames[T_COUNT] = {"direct draws' task steps", "indirect draws' task steps (with a prep step)", "of them inline (a rendering ended)",
                                     "of them in a job's task step", "direct draws of no task workgroup", "draws with more task workgroups than a dispatch holds (held to it)",
                                     "task pools made", "task pools grown", "indirect draws refused by a full pool (no task workgroup)"};
std::atomic<int64_t> st[T_COUNT], st_task_groups{0}, st_pool_most{0};
void count(int k, int64_t n = 1) { st[k].fetch_add(n, std::memory_order_relaxed); }

std::once_flag once;
PFN_vkCmdDispatch pDispatch = nullptr;
PFN_vkCmdDispatchIndirect pDispatchIndirect = nullptr;
PFN_vkCmdUpdateBuffer pUpdateBuffer = nullptr;
VkShaderEXT prep = VK_NULL_HANDLE;
uint32_t max_tasks = 65535, max_groups = 65535;
bool ready = false;

void init() {
    std::call_once(once, [] {
        pDispatch = (PFN_vkCmdDispatch)vkGetDeviceProcAddr(dev.device, "vkCmdDispatch");
        pDispatchIndirect = (PFN_vkCmdDispatchIndirect)vkGetDeviceProcAddr(dev.device, "vkCmdDispatchIndirect");
        pUpdateBuffer = (PFN_vkCmdUpdateBuffer)vkGetDeviceProcAddr(dev.device, "vkCmdUpdateBuffer");
        max_tasks = dev.props.limits.maxComputeWorkGroupCount[0];
        VkPhysicalDeviceMeshShaderPropertiesEXT mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &mp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        max_groups = std::min(mp.maxMeshWorkGroupCount[0], mp.maxMeshWorkGroupTotalCount);
        VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
        ci.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
        ci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        ci.codeSize = sizeof task_prep_spv;
        ci.pCode = task_prep_spv;
        ci.pName = "main";
        const VkResult r = vkCreateShadersEXT(dev.device, 1, &ci, nullptr, &prep);
        if (r == VK_SUCCESS) object_made(1, "the task prep shader");
        ready = r == VK_SUCCESS && pDispatch && pDispatchIndirect && pUpdateBuffer;
        logf("direct: task steps : the task stage of task+mesh programs as a compute step, the mesh stage native; prep step %s (%d); "
             "a dispatch holds %u task workgroups, a mesh draw %u mesh workgroups", ready ? "made" : "NOT MADE: indirect task+mesh draws are skipped",
             (int)r, max_tasks, max_groups);
    });
}

bool pool_make(TaskPool& p, uint64_t bytes) {
    if (!mapped_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, p.buffer, p.memory, p.map, p.address, MO_TASK)) return false;
    object_made(2, "a task pool buffer");
    p.bytes = bytes;
    std::memset(p.map, 0, kHeader);
    count(T_POOLS);
    logf("direct: task pool of %llu MB made at present %llu (a recording thread's frame slot)", (unsigned long long)(bytes >> 20),
         (unsigned long long)present_count());
    return true;
}

void pipeline_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src, b.srcAccessMask = src_access, b.dstStageMask = dst, b.dstAccessMask = dst_access;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

}  // namespace

void task_pool_reset(TaskPool* p) {
    if (!p || !p->map) return;
    uint32_t w[2];
    std::memcpy(w, p->map, sizeof w);   // (the GPU is past the slot's last use: its counters, for the line and the size)
    const int64_t used = (int64_t)w[0] * 4;
    for (int64_t m = st_pool_most.load(std::memory_order_relaxed); used > m && !st_pool_most.compare_exchange_weak(m, used);) {}
    if (w[1]) strict_event(kNames[T_REFUSED], "%u draws refused by a pool of %llu MB (its use %lld bytes)", w[1], (unsigned long long)(p->bytes >> 20),
                           (long long)used);   // 
    if (w[1] && p->bytes < kPoolMost) {
        count(T_REFUSED, w[1]);
        mapped_buffer_free(p->buffer, p->memory, MO_TASK);
        const uint64_t bytes = std::min(p->bytes * 2, kPoolMost);
        *p = TaskPool{};
        if (pool_make(*p, bytes)) count(T_GROWN);
        return;
    }
    if (w[1]) count(T_REFUSED, w[1]);
    std::memset(p->map, 0, kHeader);
}

void task_barrier(VkCommandBuffer cb, bool to_draw) {
    if (to_draw)   // (the headers' groups, the maps and payloads: the mesh draw's indirect arguments and its mesh stage's reads)
        pipeline_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
                         VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    else   // (the headers' reset (an update: the clear stage, in ALL_TRANSFER) and the prep steps' words: the task dispatches' indirect arguments and their reads, atomics)
        pipeline_barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

bool task_step(List* l, const DrawProg& p, bool indirect, uint64_t rec, uint32_t first, uint32_t count_, uint8_t* push, VkBuffer& hb,
               VkDeviceSize& ho, const char*& why) {
    init();
    if (!ready) return why = "the task step's functions or prep step not made", false;
    if (!p.ts || !p.task_threads) return why = "no task shader object", false;
    const uint32_t threads = p.task_threads, pw = p.task_payload_words;
    VkDeviceAddress rec_addr = 0;
    TaskPool* pool = nullptr;
    if (indirect) {
        if (!layer0_address(rec, 16, rec_addr)) return why = "its record not in guest memory", false;
        if (!l->slot->task) l->slot->task = new TaskPool;
        pool = l->slot->task;
        if (!pool->map && !pool_make(*pool, kPoolFirst)) return why = "no task pool", false;
    }
    // (the header, and a direct draw's map and payload area after it)
    uint32_t tasks = 0, groups = 0;
    if (!indirect) {
        if (!count_) return count(T_EMPTY), why = "no task workgroup", false;
        tasks = std::min(count_, max_tasks);
        if (tasks < count_) {
            count(T_CLIPPED);
            strict_event(kNames[T_CLIPPED], "program %016llx: %u task workgroups, %u dispatched", (unsigned long long)p.key, count_, tasks);   // 
        }
        groups = (uint32_t)std::min<uint64_t>((uint64_t)tasks * threads, max_groups);
    }
    const uint64_t bytes = kHeader + 4ull * groups + 4ull * tasks * pw;
    if (bytes > (4u << 20)) return why = "its map and payloads past an arena chunk", false;
    uint8_t* map;
    VkDeviceAddress ha;
    if (!draw_arena_buffer(l->slot, (uint32_t)bytes, map, ha, hb, ho)) return why = "no arena room", false;
    task_last_header = map;   // (M6c: HOS_DBG=marks dumps it with its draw)
    uint32_t h[16]{};
    h[1] = h[2] = 1;
    if (!indirect) {
        const VkDeviceAddress m = ha + kHeader, pa = m + 4ull * groups;
        h[4] = (uint32_t)m, h[5] = (uint32_t)(m >> 32), h[6] = (uint32_t)pa, h[7] = (uint32_t)(pa >> 32);
        h[8] = groups, h[9] = tasks, h[10] = tasks;
    }
    h[11] = h[12] = 1;
    h[13] = pw, h[14] = threads;
    std::memcpy(map, h, sizeof h);
    // (the task stage's push data: the draw's, the header's address in the fallback's members (Eden's task reads its first
    // from MESH_FIRST_PUSH_OFFSET and, its indirect input being 0, a word of the records' address it does not use))
    {
        const uint64_t zero = 0;
        std::memcpy(push + 56, &first, 4);
        std::memcpy(push + 64, &ha, 8), std::memcpy(push + 72, &ha, 8), std::memcpy(push + 80, &zero, 8);
    }
    // (where it is recorded: a job's task step, or the list's own command buffer outside its rendering)
    VkCommandBuffer pre, task;
    const bool job = l->secondary;
    if (job) {
        if (!l->task_open) {
            l->task_pre = cmd_secondary_plain(l->slot), l->task_cb = cmd_secondary_plain(l->slot), l->task_open = true;
            heap_bind(l->task_pre), heap_bind(l->task_cb);
        }
        pre = l->task_pre, task = l->task_cb;
    } else {
        pre = task = pass_outside(l);
        heap_bind(pre);
    }
    static const uint32_t reset[4] = {0, 1, 1, 0};
    pUpdateBuffer(pre, hb, ho, sizeof reset, reset);   // (a list run again: its groups counted again from 0)
    const VkShaderStageFlagBits cs = VK_SHADER_STAGE_COMPUTE_BIT;
    if (indirect) {
        struct Push {
            VkDeviceAddress header, record, pool;
            uint32_t pool_words, max_tasks, max_groups, threads, payload_words;
        } pp{ha, rec_addr, pool->address, (uint32_t)std::min<uint64_t>((pool->bytes - kHeader) / 4, 0xFFFFFFFFull), max_tasks, max_groups,
             threads, pw};
        vkCmdBindShadersEXT(pre, 1, &cs, &prep);
        heap_push(pre, &pp, sizeof pp);
        const uint32_t mk = marks_on() ? mark_before(pre, "task prep step", p.key, l->info.kind, l->info.lr, 0, 0) : 0;   // 
        pDispatch(pre, 1, 1, 1);
        if (marks_on()) mark_after(pre, mk);
    }
    if (!job) task_barrier(pre, false);
    vkCmdBindShadersEXT(task, 1, &cs, &p.ts);
    heap_push(task, push, p.push_bytes);
    const uint32_t mk = marks_on() ? mark_before(task, indirect ? "task step indirect" : "task step", p.key, l->info.kind, l->info.lr, tasks, threads) : 0;
    uint32_t ps = ~0u;
    if (pstats_on()) {   // (M6h: HOS_DBG=pstats)
        PstatWhat w;
        w.what = indirect ? "task step indirect" : "task step", w.key = p.key, w.codes[0] = p.codes[0], w.codes[1] = p.codes[1];
        w.codes[2] = p.task_code, w.kind = l->info.kind, w.lr = l->info.lr, w.count = tasks, w.instances = threads;
        ps = pstat_before(task, w);
    }
    if (indirect) pDispatchIndirect(task, hb, ho + 40);
    else pDispatch(task, tasks, 1, 1);
    if (pstats_on()) pstat_after(task, ps);
    if (marks_on()) mark_after(task, mk);
    if (!job) {
        task_barrier(task, true);
        pass_resume(l);
    }
    if (meshcap_now()) meshcap_other((uint64_t)task, 4, p.key, tasks);   // 
    count(indirect ? T_INDIRECT : T_DIRECT);
    count(job ? T_JOB : T_INLINE);
    st_task_groups.fetch_add(tasks, std::memory_order_relaxed);
    return true;
}

bool task_pool_span(CmdSlot* slot, const uint8_t*& host, uint64_t& dev, uint64_t& bytes) {
    if (!slot || !slot->task || !slot->task->map) return false;
    host = slot->task->map, dev = slot->task->address, bytes = slot->task->bytes;
    return true;
}

void task_list_end(List* l) {
    if (!l->task_open) return;
    vkEndCommandBuffer(l->task_pre), vkEndCommandBuffer(l->task_cb);
    l->task_open = false;
}

void task_stats(char* out, size_t cap, double frames) {
    int k = std::snprintf(out, cap, "task steps a frame :");
    for (int i = 0; i < T_COUNT; i++)
        k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "%s %s %.2f", i ? "," : "", kNames[i],
                           (i == T_POOLS || i == T_GROWN ? st[i].load() : st[i].exchange(0)) / (i == T_POOLS || i == T_GROWN ? 1.0 : frames));
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; direct draws' task workgroups %.1f a frame; a pool's most use %.2f MB",
                       st_task_groups.exchange(0) / frames, st_pool_most.load() / 1048576.0);
}

}  // namespace hsd
