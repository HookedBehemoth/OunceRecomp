// (native-vk engine cut E3, HOS_ENG_JOBS=1) The game's render jobs recorded as Vulkan secondaries on the job threads: see eng_job.h.
//
// At a job's EndRecording (mkwvk_eng_job_recorded, on the game's thread) its records are walked from the NVN state every job
// starts from (the default state: a job sets all it draws with, engine-jobs.md section 4), the load-time lists it calls through
// their packets (E2, gpu/render/nvn_pkt.h), and each draw recorded into a secondary of the thread's own command pool, with the
// inheritance of its pass (eng_pass_formats, E4's; learned from the replay until E4 is in the tree): the program the replay
// picked for the same draws before (published by the replay, never compiled or waited for here), the fixed-function and vertex
// input state through vk_draw.cpp's own functions (the thread's caches), vertex and index buffers in layer 0's windows.
// Descriptors are not recorded yet (the program's banks, uniform buffers and texture words for the heap: the next step), so
// no job's command buffer is published to E4 yet (EngJob::cb null, counted as "descriptors"): the recording's cost, its
// coverage and the check below are what this step measures.
//
// HOS_ENG_JOBS_CHECK=1: each job draw's state as this walk has it (program, viewport, scissor, depth range, the six state
// objects, stencil and the rest, vertex input and buffers, uniform and storage buffers, handle words with the deferred ones by
// their guest address, the draw) is kept as a compact trace, and the replay, walking the same records later (NOP_JOB marks
// them), compares the state it has for each with it, field group by group, and the Vulkan program it picks with this one's.
// A difference is state a job inherits from its caller, or a decode that differs.
#include "eng_job.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "vk_shader.h"
#include "vk_shader_object.h"
#include "vk_desc_heap.h"
#include "vk_packets.h"
#include "vk_pools.h"
#include "vk_draw.h"
#include "../render/nvn_pkt.h"
#if __has_include("eng_pass.h")
#include "eng_pass.h"   // (E4: the pass table)
#define ENG_JOB_HAVE_PASS 1
#else
#define ENG_JOB_HAVE_PASS 0
#endif

namespace mkwvk {

uint64_t ub_recording_value();   // (vk_replay.cpp)

namespace {

int64_t now_ns() { return host ? host->now_ns() : 0; }

// ---- switches (read once) ----
struct Sw {
    bool on = false, check = false, rec = true, self_pass = true;
    Sw() {
        const char* e = getenv("HOS_ENG_JOBS");
        on = e && atoi(e) == 1;
        const char* c = getenv("HOS_ENG_JOBS_CHECK");
        check = on && c && atoi(c) != 0;
        const char* r = getenv("HOS_ENG_JOBS_REC");
        rec = !(r && atoi(r) == 0);
        const char* p = getenv("HOS_ENG_JOBS_SELFPASS");
        self_pass = !ENG_JOB_HAVE_PASS || (p && atoi(p) != 0);
    }
};
const Sw& sw() {
    static const Sw s;
    return s;
}

// ---- a draw's state, compact: one hash per field group (both sides make it with trace_of) ----
enum Group { G_PROG, G_VP, G_SC, G_DR, G_BLEND, G_COLOR, G_CHMASK, G_MS, G_POLY, G_DS, G_STENCIL, G_MISC, G_VATTR, G_VSTREAM,
             G_VBUF, G_UBO, G_SSBO, G_TEX, G_SAMP, G_IMG, G_DRAW, G_RENDER, G_VKPROG, G_SIZE, G_N };
const char* const kGroup[G_N] = {"program", "viewport", "scissor", "depth range", "blend", "colour", "channel mask", "multisample",
                                 "polygon", "depth-stencil", "stencil", "offset/blend colour/restart/clamp/discard", "vertex attributes",
                                 "vertex streams", "vertex buffers", "uniform buffers", "storage buffers", "textures", "samplers",
                                 "images", "draw", "render enable", "Vulkan program", "target size"};
struct DrawTrace {
    uint64_t h[G_N];
    bool filtered;   // (the groups a program reads made by what it reads: reads_of; false: the job had no program for it)
};
/// The groups made from what a program reads (the rest of a stage's bindings may be anything a job inherits: unused)
constexpr uint32_t kFiltered = 1u << G_BLEND | 1u << G_CHMASK | 1u << G_VATTR | 1u << G_VSTREAM | 1u << G_VBUF | 1u << G_UBO |
                               1u << G_SSBO | 1u << G_TEX | 1u << G_SAMP | 1u << G_IMG;
inline uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
constexpr uint64_t kFnv0 = 14695981039346656037ull;
template <class T> uint64_t hv(const T& v) { return fnv(kFnv0, &v, sizeof v); }

/// The deferred words a job bound, by what (texture, sampler, image, uniform, storage), stage and slot: the guest address the
/// replay reads them from at the submit (0: an immediate bind)
struct Deferred {
    uint64_t src[5][kNvnStages][128];
    void clear() { std::memset(src, 0, sizeof src); }
};
uint64_t words_hash(const uint64_t* w, const uint64_t* src, int n) {
    uint64_t h = kFnv0;
    for (int i = 0; i < n; i++) {
        const uint64_t v = src[i] ? (src[i] | 1ull << 63) : w[i];
        h = fnv(h, &v, 8);
    }
    return h;
}
uint64_t bufs_hash(const nvn_p_bind_buf (*b)[16], const uint64_t (*src)[128]) {
    uint64_t h = kFnv0;
    for (int st = 0; st < kNvnStages; st++)
        for (int i = 0; i < 16; i++) {
            if (src[st][i]) {
                const uint64_t v = src[st][i] | 1ull << 63;
                h = fnv(h, &v, 8);
            } else {
                h = fnv(h, &b[st][i].addr, 8), h = fnv(h, &b[st][i].size, 8);
            }
        }
    return h;
}
/// The bytes of a draw op's payload that are its arguments (a record's are padded to 8, a packet's are a union)
size_t op_args(int op) {
    switch (op) {
    case NOP_DRAW: return sizeof(nvn_p_draw);
    case NOP_DRAW_IDX: return sizeof(nvn_p_draw_idx);
    case NOP_DRAW_INDIRECT: return sizeof(nvn_p_draw_indirect);
    case NOP_DRAW_MESH: return sizeof(nvn_p_draw_mesh);
    case NOP_DISPATCH: return sizeof(nvn_p_dispatch);
    case NOP_DISPATCH_INDIRECT: return sizeof(nvn_p_dispatch_indirect);
    default: return 0;
    }
}
void trace_of(const NvnState& s, const Deferred& d, int op, const void* payload, size_t n, DrawTrace& t) {
    std::memset(&t, 0, sizeof t);
    n = std::min(n, op_args(op));
    t.h[G_PROG] = s.has_prog ? prog_stage_sig(s.prog) : 0;
    t.h[G_VP] = fnv(kFnv0, s.viewport[0], 16);
    t.h[G_SC] = hv(s.scissor);
    t.h[G_DR] = fnv(hv(s.depth_near), &s.depth_far, 4);
    t.h[G_BLEND] = hv(s.blend);
    t.h[G_COLOR] = hv(s.color);
    t.h[G_CHMASK] = hv(s.chmask);
    t.h[G_MS] = hv(s.ms);
    t.h[G_POLY] = hv(s.poly);
    t.h[G_DS] = hv(s.ds);
    t.h[G_STENCIL] = fnv(fnv(hv(s.stencil_value_mask), s.stencil_mask, 8), s.stencil_ref, 8);
    uint64_t m = fnv(hv(s.poly_offset), s.blend_color, 16);
    const uint8_t flags[3] = {s.restart, s.depth_clamp, s.raster_discard};
    m = fnv(fnv(m, flags, 3), &s.restart_index, 4);
    t.h[G_MISC] = m;
    t.h[G_VATTR] = fnv(hv(s.vattr), &s.nvattr, 4);
    t.h[G_VSTREAM] = hv(s.vstream);
    t.h[G_VBUF] = hv(s.vbuf);
    t.h[G_UBO] = bufs_hash(s.ubo, d.src[3]);
    t.h[G_SSBO] = bufs_hash(s.ssbo, d.src[4]);
    uint64_t ht = kFnv0, hs = kFnv0, hi = kFnv0;
    for (int st = 0; st < kNvnStages; st++) {
        const uint64_t a = words_hash(s.tex[st], d.src[0][st], 128), b = words_hash(s.samp[st], d.src[1][st], 128),
                       c = words_hash(s.img[st], d.src[2][st], 32);
        ht = fnv(ht, &a, 8), hs = fnv(hs, &b, 8), hi = fnv(hi, &c, 8);
    }
    t.h[G_TEX] = ht, t.h[G_SAMP] = hs, t.h[G_IMG] = hi;
    t.h[G_DRAW] = fnv(fnv(kFnv0, &op, 4), payload, n);
    t.h[G_RENDER] = s.render_enabled ? 1 : 2;
}

/// The driver bank c2's word at offset of a stage: the handle slot it is (what 0 texture, 1 sampler, 2 image; -1: none)
int c2_slot(uint32_t off, int* what) {
    if (off & 7) return -1;
    if (off >= 0x568 && off < 0x568 + 8 * 128) return *what = 1, (int)(off - 0x568) / 8;
    if (off >= 0x168 && off < 0x168 + 8 * 128) return *what = 0, (int)(off - 0x168) / 8;
    if (off >= 0x120 && off < 0x120 + 8 * 9) return *what = 2, (int)(off - 0x120) / 8;
    if (off >= 0x20 && off < 0x20 + 8 * 32) return *what = 0, (int)(off - 0x20) / 8;
    return -1;
}
uint64_t c2_word(const NvnState& s, const Deferred& d, int stage, uint32_t off) {
    int what = 0;
    const int slot = c2_slot(off, &what);
    if (slot < 0 || stage < 0 || stage >= kNvnStages) return 0xdead;
    if (d.src[what][stage][slot]) return d.src[what][stage][slot] | 1ull << 63;
    return what == 0 ? s.tex[stage][slot] : what == 1 ? s.samp[stage][slot] : s.img[stage][slot];
}
/// What program p reads of the state, one hash a group (kFiltered); ncolor: its pass's colour targets
void reads_of(const NvnState& s, const Deferred& d, const Program& p, int ncolor, DrawTrace& t) {
    uint64_t hb = kFnv0, hm = kFnv0, ha = kFnv0, hs = kFnv0, hv = kFnv0, hu = kFnv0, hss = kFnv0, ht = kFnv0, hsm = kFnv0, hi = kFnv0;
    for (int i = 0; i < std::min(ncolor, 8); i++) {
        const uint32_t en = s.color.blend_en >> i & 1, wm = s.chmask.mask >> (4 * i) & 15;
        hb = fnv(hb, &en, 4);
        if (en) hb = fnv(hb, &s.blend[i], sizeof s.blend[i]);
        hm = fnv(hm, &wm, 4);
    }
    for (const Stage& st : p.stages) {
        const int ns = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
        const Shader::Info& info = st.info;
        if (st.nvn_stage == 0)
            for (int i = 0; i < 16; i++) {
                if (!info.loads.Generic((size_t)i)) continue;
                const uint32_t a = i < s.nvattr ? s.vattr[i] : 0;
                ha = fnv(ha, &a, 4);
                nvn_va_st va;
                std::memcpy(&va, &a, 4);
                if (i < s.nvattr && va.stream < 15) {
                    hs = fnv(hs, &s.vstream[va.stream], sizeof s.vstream[va.stream]);
                    hv = fnv(hv, &s.vbuf[va.stream], sizeof s.vbuf[va.stream]);
                }
            }
        for (const auto& c : info.constant_buffer_descriptors)
            for (uint32_t k = 0; k < c.count; k++) {
                const uint32_t idx = c.index + k;
                if (idx < 3 || idx >= 3 + 16) continue;
                if (d.src[3][ns][idx - 3]) {
                    const uint64_t v = d.src[3][ns][idx - 3] | 1ull << 63;
                    hu = fnv(hu, &v, 8);
                } else {
                    hu = fnv(fnv(hu, &s.ubo[ns][idx - 3].addr, 8), &s.ubo[ns][idx - 3].size, 8);
                }
            }
        for (const auto& b : info.storage_buffers_descriptors)   // (a table in a driver bank: its words; a user buffer's: its binding, above)
            for (uint32_t k = 0; k < b.count && (b.cbuf_index == 0 || b.cbuf_index == 2); k++)
                for (uint32_t w = 0; w < 12; w += 4) {
                    const uint32_t v = driver_bank_word(s, (int)b.cbuf_index, ns, b.cbuf_offset + k * 16 + w);
                    hss = fnv(hss, &v, 4);
                }
        for (const auto& x : info.texture_descriptors) {
            for (uint32_t k = 0; k < x.count; k++) {
                if (x.handle_program != ~0U || x.cbuf_index != 2) continue;   // (a user buffer's word or a computed one: its binding)
                const uint64_t v = c2_word(s, d, ns, x.cbuf_offset + k * 8);
                ht = fnv(ht, &v, 8);
                if (x.has_secondary && x.secondary_cbuf_index == 2) {
                    const uint64_t w = c2_word(s, d, ns, x.secondary_cbuf_offset + k * 8);
                    hsm = fnv(hsm, &w, 8);
                }
            }
        }
        for (const auto& x : info.image_descriptors)
            for (uint32_t k = 0; k < x.count; k++) {
                if (x.handle_program != ~0U || x.cbuf_index != 2) continue;
                const uint64_t v = c2_word(s, d, ns, x.cbuf_offset + k * 8);
                hi = fnv(hi, &v, 8);
            }
    }
    t.h[G_BLEND] = hb, t.h[G_CHMASK] = hm, t.h[G_VATTR] = ha, t.h[G_VSTREAM] = hs, t.h[G_VBUF] = hv, t.h[G_UBO] = hu;
    t.h[G_SSBO] = hss, t.h[G_TEX] = ht, t.h[G_SAMP] = hsm, t.h[G_IMG] = hi;
    t.filtered = true;
}

// ---- the jobs: a ring by serial (kRing jobs: ~80 frames of 102) ----
enum Why { WHY_OK, WHY_NO_PASS, WHY_NO_PROG, WHY_PROG_NOT_READY, WHY_DRAW_KIND, WHY_VERTEX_MEM, WHY_INDEX_MEM, WHY_OPS, WHY_POOL, WHY_DESCRIPTORS,
           WHY_NOT_RECORDED, WHY_N };
const char* const kWhy[WHY_N] = {"recorded", "no pass formats", "program not seen", "program not ready", "draw kind", "vertex memory",
                               "index memory", "ops not taken", "command pool full", "descriptors", "not recorded"};
constexpr uint32_t kRing = 8192;
struct Job {
    std::atomic<uint32_t> serial{0};   // (0 while it is written)
    EngJob pub{};
    uint64_t handle = 0, view = 0;
    float rect[4]{};
    uint32_t why = WHY_OK, bytes = 0, npkt = 0;
    uint16_t ops[NOP_COUNT_]{};
    std::vector<uint32_t> tex;
    std::vector<uint64_t> deferred;
    std::vector<DrawTrace> trace;
    std::atomic<uint64_t> used_value{0};   // (E4: the recording that executed it)
    std::atomic<bool> replayed{false};
};
Job* ring() {
    static Job* r = new Job[kRing];
    return r;
}
std::mutex handle_mu;
std::unordered_map<uint64_t, uint32_t> by_handle;   // (the slot handle -> the serial; the newest job of a handle)
std::atomic<uint64_t> retired_through{0};

// ---- counts (relaxed atomics; the frame log reads them every 120 presents) ----
enum Count { C_JOBS, C_DRAWS, C_DRAWS_REC, C_DRAWS_SKIPPED, C_PKT, C_PKT_STALE, C_DEFERRED, C_DEFERRED_BUFS, C_REC_NS, C_DEC_NS,
             C_CHECK_JOBS, C_CHECK_DRAWS, C_CHECK_MISMATCH_DRAWS, C_CHECK_NO_JOB, C_CHECK_COUNT_DIFF, C_PUBLISHED, C_LOOKUPS,
             C_LOOKUP_HITS, C_PROG_ANSWERS, C_CHECK_UNFILTERED, C_CHECK_FILTERED, C_N };
std::atomic<int64_t> cnt[C_N];
std::atomic<int64_t> why_n[WHY_N];
std::atomic<int64_t> group_mismatch[G_N];
std::atomic<int64_t> op_refused[NOP_COUNT_];
inline void add(Count c, int64_t v = 1) { cnt[c].fetch_add(v, std::memory_order_relaxed); }

// ---- the programs the replay picked for job draws: by what program_for picks by, the job threads' lookup ----
struct Pub { const Program* p; const SoProg* so; };
std::shared_mutex pub_mu;
std::unordered_map<uint64_t, Pub> published;
uint64_t pick_key(const NvnState& s, const uint8_t ct[8]) {
    uint64_t h = fnv(kFnv0, s.vattr, sizeof s.vattr);
    h = fnv(h, &s.nvattr, 4);
    h = fnv(h, ct, 8);
    const uint64_t sig = prog_stage_sig(s.prog);
    return fnv(h, &sig, 8);
}
void color_types(const EngPassFormats& f, uint8_t types[8]) {   // (as draw() makes them from the targets' formats)
    for (int i = 0; i < 8; i++) {
        const VkFormat v = i < (int)f.ncolor ? f.color[i] : VK_FORMAT_UNDEFINED;
        switch (v) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            types[i] = (uint8_t)Shader::AttributeType::UnsignedInt;
            break;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT:
            types[i] = (uint8_t)Shader::AttributeType::SignedInt;
            break;
        default: types[i] = (uint8_t)Shader::AttributeType::Float; break;
        }
    }
}

// ---- the passes' formats: E4's table, or (until it is in the tree, HOS_ENG_JOBS_SELFPASS) what the replay drew a view's jobs into ----
std::shared_mutex pass_mu;
std::unordered_map<uint64_t, EngPassFormats> self_passes;
bool has_stencil_fmt(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_S8_UINT;
}
bool pass_formats(uint64_t view, EngPassFormats* out) {
#if ENG_JOB_HAVE_PASS
    if (!sw().self_pass) return eng_pass_formats(view, out);
#endif
    std::shared_lock lk(pass_mu);
    const auto it = self_passes.find(view);
    if (it == self_passes.end()) return false;
    *out = it->second;
    return true;
}

// ---- the job thread's command pool: its command buffers reused once their jobs are retired ----
struct TPool {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> free;
    std::vector<std::pair<VkCommandBuffer, uint32_t>> busy;   // (and its job's serial)
};
thread_local TPool* tpool = nullptr;
bool job_retired(uint32_t serial, uint32_t newest) {
    Job& j = ring()[serial % kRing];
    if (j.serial.load(std::memory_order_acquire) != serial) return true;   // (the slot holds a newer job: kRing jobs on)
    const uint64_t used = j.used_value.load(std::memory_order_acquire);
    if (used) return used <= retired_through.load(std::memory_order_acquire);
    return j.replayed.load(std::memory_order_acquire) || newest - serial > kRing / 2;
}
VkCommandBuffer take_cb(uint32_t serial) {
    if (!tpool) {
        tpool = new TPool;
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = dev.queue_family;
        if (vkCreateCommandPool(dev.device, &ci, nullptr, &tpool->pool) != VK_SUCCESS) tpool->pool = VK_NULL_HANDLE;
    }
    TPool& t = *tpool;
    if (!t.pool) return VK_NULL_HANDLE;
    for (size_t i = 0; i < t.busy.size();) {
        if (job_retired(t.busy[i].second, serial)) {
            vkResetCommandBuffer(t.busy[i].first, 0);
            t.free.push_back(t.busy[i].first);
            t.busy[i] = t.busy.back(), t.busy.pop_back();
        } else {
            i++;
        }
    }
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!t.free.empty()) {
        cb = t.free.back(), t.free.pop_back();
    } else {
        if (t.busy.size() >= 512) return VK_NULL_HANDLE;   // (jobs that are never retired: E4 not retiring them)
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = t.pool, ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev.device, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    t.busy.push_back({cb, serial});
    return cb;
}

// ---- the walk of a job's records on its thread ----
template <class T> T payload(const uint8_t* a, uint32_t avail) {
    T v;
    std::memset(&v, 0, sizeof v);
    std::memcpy(&v, a, std::min<size_t>(sizeof v, avail));
    return v;
}
struct Walk {
    NvnState s;
    Deferred d;
    std::vector<uint32_t> tex;
    std::vector<uint64_t> deferred;
    std::vector<DrawTrace> trace;
    uint32_t npkt = 0;
    uint16_t ops[NOP_COUNT_];
};
thread_local Walk* walk_tls = nullptr;
Walk& walk() {
    if (!walk_tls) walk_tls = new Walk;
    return *walk_tls;
}

/// The secondary being recorded for a job (null cb: walked for the check and the counts only)
struct Rec {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    EngTargets at{};
    uint8_t types[8]{};
    bool first = true, have_fmt = false;
    uint32_t why = WHY_OK;
    uint32_t ndraws = 0;
    void fail(uint32_t w) {
        if (why == WHY_OK) why = w;
    }
};

/// Handle words bound (what: 0 texture, 1 sampler, 2 image; 3 uniform, 4 storage ranges), immediate (src 0) or deferred
void note_bind(Walk& w, int what, int stage, int first, int n, const uint64_t* h, uint64_t src) {
    if (stage < 0 || stage >= kNvnStages || first < 0) return;
    const int lim = what == 2 ? 32 : what >= 3 ? 16 : 128;
    const uint64_t step = what >= 3 ? sizeof(NVNbufferRange) : 8;
    for (int k = 0; k < n && first + k < lim; k++) {
        w.d.src[what][stage][first + k] = src ? src + step * (uint64_t)k : 0;
        if (src) {
            if (what < 3) w.deferred.push_back(src + step * (uint64_t)k);
        } else if (h && (what == 0 || what == 2) && h[k]) {
            w.tex.push_back((uint32_t)h[k]);
        }
    }
}

void record_draw(Walk& w, Rec& r, int op, const void* pl) {
    const NvnState& s = w.s;
    if (!s.render_enabled || s.raster_discard) return void(add(C_DRAWS_SKIPPED));   // (as the replay leaves them out)
    if (op != NOP_DRAW && op != NOP_DRAW_IDX) return r.fail(WHY_DRAW_KIND);
    nvn_p_draw dr{};
    nvn_p_draw_idx di{};
    if (op == NOP_DRAW) std::memcpy(&dr, pl, sizeof dr);
    else std::memcpy(&di, pl, sizeof di);
    const int prim = op == NOP_DRAW ? dr.prim : di.prim;
    VkPrimitiveTopology topo;
    if (prim == NVN_DRAW_PRIMITIVE_QUADS || !eng_draw_topology(prim, &topo)) return r.fail(WHY_DRAW_KIND);
    if (!s.has_prog) return r.fail(WHY_NO_PROG);
    Pub pub{};
    {
        std::shared_lock lk(pub_mu);
        const auto it = published.find(pick_key(s, r.types));
        if (it == published.end()) return r.fail(WHY_NO_PROG);
        pub = it->second;
    }
    const Program& p = *pub.p;
    const SoProg& so = *pub.so;
    if (!p.ready.load(std::memory_order_acquire) || p.failed || !so.ready.load(std::memory_order_acquire) || so.failed || !so.vs ||
        p.mesh || p.fallback)
        return r.fail(WHY_PROG_NOT_READY);
    eng_draw_state(r.cb, s, p, so, r.at, topo, r.first);
    r.first = false;
    r.fail(WHY_DESCRIPTORS);   // (the program's banks, buffers and texture words: not recorded yet, see the head of this file)
    // vertex streams, from layer 0's windows (a range not bound there yet: the replay binds it, the job cannot)
    VkBuffer vb[16];
    VkDeviceSize vo[16], vz[16], vs[16];
    const VkBuffer zero = eng_draw_zero_buffer();
    for (int b = 0; b < 16; b++) {
        vb[b] = zero, vo[b] = 0, vz[b] = 65536, vs[b] = 0;
        if (b == 15 || !s.vbuf[b].address) continue;
        const uint64_t a = s.vbuf[b].address, n = std::max<uint64_t>(s.vbuf[b].size, 1);
        const BufferRef br = a < (1ull << 40) ? layer0_buffer(a, n) : BufferRef{};
        if (!br.buffer) {
            r.fail(WHY_VERTEX_MEM);
            continue;
        }
        vb[b] = br.buffer, vo[b] = br.offset, vz[b] = s.vbuf[b].size ? s.vbuf[b].size : VK_WHOLE_SIZE, vs[b] = s.vstream[b].stride;
    }
    vkCmdBindVertexBuffers2(r.cb, 0, 16, vb, vo, vz, vs);
    if (op == NOP_DRAW_IDX) {
        if (di.itype == 0 && !dev.index_uint8) return r.fail(WHY_DRAW_KIND);
        const uint32_t unit = di.itype == 0 ? 1 : di.itype == 1 ? 2 : 4;
        const BufferRef ib = di.ibuf < (1ull << 40) ? layer0_buffer(di.ibuf, (uint64_t)std::max(di.count, 1) * unit) : BufferRef{};
        if (!ib.buffer) return r.fail(WHY_INDEX_MEM);
        vkCmdBindIndexBuffer(r.cb, ib.buffer, ib.offset,
                             di.itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : di.itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(r.cb, (uint32_t)di.count, (uint32_t)std::max(1, di.instances), 0, di.base_vertex, (uint32_t)di.base_instance);
    } else {
        vkCmdDraw(r.cb, (uint32_t)dr.count, (uint32_t)std::max(1, dr.instances), (uint32_t)dr.first, (uint32_t)dr.base_instance);
    }
    r.ndraws++;
    add(C_DRAWS_REC);
}

void on_draw(Walk& w, Rec* r, int op, const void* pl, size_t n) {
    add(C_DRAWS);
    if (sw().check) {
        w.trace.emplace_back();
        DrawTrace& t = w.trace.back();
        trace_of(w.s, w.d, op, pl, n, t);
        const uint32_t wh[2] = {(uint32_t)std::max(0, (int)r->at.width), (uint32_t)std::max(0, (int)r->at.height)};
        t.h[G_SIZE] = fnv(kFnv0, wh, sizeof wh);
        if (w.s.has_prog && r->have_fmt) {   // (what the program the replay picked for these inputs reads)
            Pub pub{};
            {
                std::shared_lock lk(pub_mu);
                const auto it = published.find(pick_key(w.s, r->types));
                if (it != published.end()) pub = it->second;
            }
            if (pub.p && pub.p->ready.load(std::memory_order_acquire) && !pub.p->failed) {
                reads_of(w.s, w.d, *pub.p, r->at.n, t);
                t.h[G_VKPROG] = pub.p->key ^ pub.p->variant * 31;
            }
        }
    }
    if (r->cb) record_draw(w, *r, op, pl);
}

/// A packet's state over the walk's, as vk_replay.cpp pkt_apply sets it, then its draw
void walk_packet(Walk& w, Rec* r, const nvn_pkt& k) {
    NvnState& s = w.s;
    for (int i = 0; i < k.nbuf; i++) {
        const nvn_p_bind_buf& b = k.buf[i].b;
        if (b.stage < 0 || b.stage >= kNvnStages || b.index < 0 || b.index >= 16) continue;
        (k.buf[i].ssbo ? s.ssbo : s.ubo)[b.stage][b.index] = b;
        w.d.src[k.buf[i].ssbo ? 4 : 3][b.stage][b.index] = 0;
    }
    for (int i = 0; i < k.nhandle; i++) {
        const nvn_pk_handle& e = k.hnd[i];
        if (e.stage >= kNvnStages || e.what > 2 || e.slot >= (e.what == 2 ? 32 : 128)) continue;
        uint64_t* to = e.what == 0 ? s.tex[e.stage] : e.what == 1 ? s.samp[e.stage] : s.img[e.stage];
        to[e.slot] = e.h;
        note_bind(w, e.what, e.stage, e.slot, 1, &e.h, 0);
    }
    if (k.vtx) {
        const nvn_pk_vertex& v = *k.vtx;
        if (k.flags & NVN_PKF_VATTR) {
            std::memcpy(s.vattr, v.vattr, sizeof s.vattr);
            s.nvattr = (int)v.nvattr;
        }
        for (int i = 0; i < 16; i++) {
            if (v.vstream_mask >> i & 1) s.vstream[i] = v.vstream[i];
            if (v.vbuf_mask >> i & 1) s.vbuf[i] = v.vbuf[i];
        }
    }
    if (k.draw.op) {
        on_draw(w, r, k.draw.op, &k.draw.u, k.draw.op == NOP_DRAW ? sizeof(nvn_p_draw) : k.draw.op == NOP_DRAW_IDX ? sizeof(nvn_p_draw_idx)
                                                                                                                  : sizeof(nvn_p_draw_mesh));
        s.samples_passed += 16;
    }
}

/// The job's records, BEGIN .. END, as the serial replay decodes them (vk_replay.cpp replay(): the same state for the ops a job
/// has; the CPU-visible ones (counter reports) stay the replay's, in stream order)
void walk_records(Walk& w, Rec* r, const uint8_t* p, const uint8_t* end) {
    NvnState& s = w.s;
    while (p + 8 <= end) {
        nvn_op_hdr h;
        std::memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || p + h.size > end) return r->fail(WHY_OPS);
        const uint8_t* a = p + 8;
        const uint32_t avail = h.size - 8;
        if (h.op < NOP_COUNT_) w.ops[h.op]++;
        switch (h.op) {
        case NOP_END: return;
        case NOP_BEGIN: case NOP_JUMP: case NOP_JOB: break;
        case NOP_PKT: {
            const auto rp = payload<nvn_p_pkt>(a, avail);
            add(C_PKT);
            w.npkt++;
            if (const nvn_pkt* k = nvn_pkt_live(&rp)) {
                walk_packet(w, r, *k);
                p += h.size + rp.skip;
                continue;
            }
            add(C_PKT_STALE);   // (its records follow: decoded)
            break;
        }
        case NOP_VIEWPORTS: {
            const auto v = payload<nvn_p_viewports>(a, avail);
            for (int i = 0; i < v.n && v.first + i < 16; i++) std::memcpy(s.viewport[v.first + i], v.v[i], 16);
            break;
        }
        case NOP_SCISSOR: {
            const auto c = payload<nvn_p_scissor>(a, avail);
            s.scissor[0] = c.x, s.scissor[1] = c.y, s.scissor[2] = c.w, s.scissor[3] = c.h;
            break;
        }
        case NOP_DEPTH_RANGE: {
            const auto d = payload<nvn_p_depth_range>(a, avail);
            s.depth_near = d.n, s.depth_far = d.f;
            break;
        }
        case NOP_BIND_PROGRAM: {
            const auto b = payload<nvn_p_bind_program>(a, avail);
            if (b.prog) s.prog = *b.prog;
            s.has_prog = b.prog != nullptr && b.prog->nstage > 0;
            break;
        }
        case NOP_BIND_STATE: {
            const auto b = payload<nvn_p_bind_state>(a, avail);
            switch (b.kind) {
            case 0: {
                nvn_blend_st st;
                std::memcpy(&st, b.bytes, sizeof st);
                std::memcpy(&s.blend[st.target & 7], b.bytes, sizeof st);
                break;
            }
            case 1: std::memcpy(&s.color, b.bytes, sizeof s.color); break;
            case 2: std::memcpy(&s.chmask, b.bytes, sizeof s.chmask); break;
            case 3: std::memcpy(&s.ms, b.bytes, sizeof s.ms); break;
            case 4: std::memcpy(&s.poly, b.bytes, sizeof s.poly); break;
            case 5: std::memcpy(&s.ds, b.bytes, sizeof s.ds); break;
            default: break;
            }
            break;
        }
        case NOP_BIND_VATTR: {
            const auto v = payload<nvn_p_bind_vattr>(a, avail);
            std::memset(s.vattr, 0, sizeof s.vattr);
            for (int i = 0; i < v.n && i < 16; i++) s.vattr[i] = v.st[i];
            s.nvattr = std::min(v.n, 16);
            break;
        }
        case NOP_BIND_VSTREAM: {
            const auto v = payload<nvn_p_bind_vstream>(a, avail);
            for (int i = 0; i < v.n && i < 16; i++) s.vstream[i] = v.st[i];
            break;
        }
        case NOP_BIND_VBUF: {
            const auto v = payload<nvn_p_bind_vbuf>(a, avail);
            for (int i = 0; i < v.n && v.first + i < 16; i++) s.vbuf[v.first + i] = v.r[i];
            break;
        }
        case NOP_BIND_BUFS: {   // (deferred ranges: what the game's array holds at the submit; their array's addresses noted)
            const auto b = payload<nvn_p_bind_bufs>(a, avail);
            if (b.stage < 0 || b.stage >= kNvnStages) break;
            for (int i = 0; i < b.n && i < 16; i++) {
                const int index = b.first + i;
                if (index < 0 || index >= 16) continue;
                nvn_p_bind_buf& u = (b.ssbo ? s.ssbo : s.ubo)[b.stage][index];
                u.stage = b.stage, u.index = index, u.addr = b.r[i].address, u.size = b.r[i].size;
            }
            note_bind(w, b.ssbo ? 4 : 3, b.stage, b.first, std::min(b.n, 16), nullptr, b.src);
            if (b.src) add(C_DEFERRED_BUFS), r->fail(WHY_OPS), op_refused[NOP_BIND_BUFS].fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case NOP_BIND_UBO: case NOP_BIND_SSBO: {
            const auto b = payload<nvn_p_bind_buf>(a, avail);
            if (b.stage >= 0 && b.stage < kNvnStages && b.index >= 0 && b.index < 16) {
                (h.op == NOP_BIND_UBO ? s.ubo : s.ssbo)[b.stage][b.index] = b;
                w.d.src[h.op == NOP_BIND_UBO ? 3 : 4][b.stage][b.index] = 0;
            }
            break;
        }
        case NOP_BIND_TEX: case NOP_BIND_SAMP: case NOP_BIND_IMG: {
            nvn_p_bind_handles b;
            std::memset(&b, 0, sizeof b);
            std::memcpy(&b, a, std::min<size_t>(sizeof b, avail));
            if (b.stage < 0 || b.stage >= kNvnStages || b.first < 0) break;
            uint64_t* to = h.op == NOP_BIND_TEX ? s.tex[b.stage] : h.op == NOP_BIND_SAMP ? s.samp[b.stage] : s.img[b.stage];
            const int lim = h.op == NOP_BIND_IMG ? 32 : 128;
            int n = 0;
            while (n < b.n && b.first + n < lim) n++;
            for (int k = 0; k < n; k++) to[b.first + k] = b.h[k];
            note_bind(w, h.op == NOP_BIND_TEX ? 0 : h.op == NOP_BIND_SAMP ? 1 : 2, b.stage, b.first, n, b.h, b.src);
            if (b.src) add(C_DEFERRED, n);
            break;
        }
        case NOP_STENCIL: {
            const auto st = payload<nvn_p_stencil>(a, avail);
            for (int f = 0; f < 2; f++) {
                if (!(st.face & (1 << f))) continue;
                if (st.which == 0) s.stencil_value_mask[f] = (uint32_t)st.value_mask;
                else if (st.which == 1) s.stencil_mask[f] = (uint32_t)st.mask;
                else s.stencil_ref[f] = (uint32_t)st.ref;
            }
            break;
        }
        case NOP_POLY_OFFSET: {
            const auto o = payload<nvn_p_poly_offset>(a, avail);
            s.poly_offset[0] = o.factor, s.poly_offset[1] = o.units, s.poly_offset[2] = o.clamp;
            break;
        }
        case NOP_BLEND_COLOR: std::memcpy(s.blend_color, payload<nvn_p_blend_color>(a, avail).rgba, 16); break;
        case NOP_PRIMITIVE_RESTART: {
            const auto rr = payload<nvn_p_restart>(a, avail);
            s.restart = rr.enable != 0, s.restart_index = (uint32_t)rr.index;
            break;
        }
        case NOP_DEPTH_CLAMP: s.depth_clamp = payload<nvn_p_flag>(a, avail).value != 0; break;
        case NOP_RASTER_DISCARD: s.raster_discard = payload<nvn_p_flag>(a, avail).value != 0; break;
        case NOP_RENDER_ENABLE: s.render_enabled = payload<nvn_p_flag>(a, avail).value != 0; break;
        case NOP_RENDER_COND: {   // (as the replay reads it, from guest memory now; the reports before it are the replay's to write)
            const auto rc = payload<nvn_p_render_cond>(a, avail);
            uint64_t x = rc.x, y = rc.y;
            if (!rc.snapped) host->gpu_read(rc.addr, &x, 8), host->gpu_read(rc.addr + 16, &y, 8);
            s.render_enabled = rc.mode == 0 ? x == y : x != y;
            static const bool always = getenv("RX_VK_COND_ALWAYS") != nullptr;
            if (always) s.render_enabled = true;
            break;
        }
        case NOP_RESET_COUNTER: {
            const auto c = payload<nvn_p_counter>(a, avail);
            if (c.type == NVN_COUNTER_TYPE_SAMPLES_PASSED) s.samples_passed = 0;
            break;
        }
        case NOP_REPORT_COUNTER: case NOP_SET_TEX_POOL: case NOP_SET_SAMP_POOL: case NOP_COPY_STRIDE: break;
        case NOP_DRAW: case NOP_DRAW_IDX: case NOP_DRAW_INDIRECT: case NOP_DRAW_MESH:
            on_draw(w, r, (int)h.op, a, avail);
            s.samples_passed += 16;
            break;
        case NOP_DISPATCH: case NOP_DISPATCH_INDIRECT:
            on_draw(w, r, (int)h.op, a, avail);
            r->fail(WHY_OPS);
            op_refused[h.op].fetch_add(1, std::memory_order_relaxed);
            break;
        default:   // (targets, clears, barriers, waits, copies, uniform updates, push constants: not a job's own)
            if (h.op < NOP_COUNT_) op_refused[h.op].fetch_add(1, std::memory_order_relaxed);
            r->fail(WHY_OPS);
            break;
        }
        p += h.size;
    }
}

template <class T> void uniq(std::vector<T>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

/// The replay thread's place in a job's records (NOP_JOB), for its check and the programs it publishes
struct RState {
    bool active = false;
    Job* j = nullptr;
    uint32_t serial = 0;
    size_t i = 0;
    int64_t pending = -1;   // (the job draw whose program draw() picks next)
    DrawTrace pend{};       // (its trace as the replay has it)
    uint32_t pend_groups = ~0u;
    Deferred d;
    int told = 0;
    uint16_t ops[NOP_COUNT_]{};
};
thread_local RState* rstate = nullptr;
RState& rs() {
    if (!rstate) rstate = new RState;
    return *rstate;
}
std::atomic<int> mismatch_told{0};
void mismatch_line(const RState& r, size_t i, uint32_t groups, const DrawTrace& a, const DrawTrace& b);
/// A job draw the replay did not reach a program for (left out): its groups that need none, counted now
void flush_pending(RState& r) {
    if (r.pend_groups == ~0u || !r.j || r.pending < 0) return void(r.pend_groups = ~0u);
    const uint32_t groups = r.pend_groups;
    for (int g = 0; g < G_N; g++)
        if (groups >> g & 1) group_mismatch[g].fetch_add(1, std::memory_order_relaxed);
    if (groups && (size_t)r.pending < r.j->trace.size()) add(C_CHECK_MISMATCH_DRAWS), mismatch_line(r, (size_t)r.pending, groups, r.j->trace[(size_t)r.pending], r.pend);
    r.pend_groups = ~0u;
}
void mismatch_line(const RState& r, size_t i, uint32_t groups, const DrawTrace& a, const DrawTrace& b) {
    if (mismatch_told.fetch_add(1, std::memory_order_relaxed) >= 40) return;
    char line[600];
    int n = snprintf(line, sizeof line, "vk: eng jobs: MISMATCH job %u (view %#llx) draw %zu of %zu:", r.serial,
                     (unsigned long long)(r.j ? r.j->view : 0), i, r.j ? r.j->trace.size() : 0);
    for (int g = 0; g < G_N && n < 560; g++)
        if (groups >> g & 1)
            n += snprintf(line + n, sizeof line - n, " %s (%016llx job, %016llx replay);", kGroup[g], (unsigned long long)a.h[g],
                          (unsigned long long)b.h[g]);
    logf("%s", line);
}

}  // namespace

bool eng_job_on() { return sw().on; }
bool eng_job_check() { return sw().check; }

const EngJob* eng_job_lookup(uint64_t slot_handle) {
    if (!sw().on) return nullptr;
    add(C_LOOKUPS);
    uint32_t serial;
    {
        std::lock_guard lk(handle_mu);
        const auto it = by_handle.find(slot_handle);
        if (it == by_handle.end()) return nullptr;
        serial = it->second;
    }
    Job& j = ring()[serial % kRing];
    if (j.serial.load(std::memory_order_acquire) != serial || !j.pub.cb) return nullptr;
    j.used_value.store(ub_recording_value(), std::memory_order_release);
    add(C_LOOKUP_HITS);
    return &j.pub;
}

void eng_job_retire_through(uint64_t replay_value) {
    uint64_t was = retired_through.load(std::memory_order_relaxed);
    while (replay_value > was && !retired_through.compare_exchange_weak(was, replay_value, std::memory_order_release)) {}
}

void eng_job_replay_mark(uint32_t serial, uint32_t end) {
    if (!sw().on) return;
    RState& r = rs();
    if (!end) {
        if (r.active) {   // (a job's start inside another's records: its end not met)
            static std::atomic<int> told{0};
            if (told++ < 10) logf("vk: eng jobs: job %u begins inside job %u (%zu draws in)", serial, r.serial, r.i);
        }
        r.active = true, r.serial = serial, r.i = 0, r.pending = -1;
        std::memset(r.ops, 0, sizeof r.ops);
        Job& j = ring()[serial % kRing];
        r.j = j.serial.load(std::memory_order_acquire) == serial ? &j : nullptr;
        if (sw().check) {
            r.d.clear();
            add(C_CHECK_JOBS);
            if (!r.j) add(C_CHECK_NO_JOB);
        }
        return;
    }
    if (r.active && r.j && r.serial == serial) {
        flush_pending(r);
        if (sw().check && r.i != r.j->trace.size()) {
            add(C_CHECK_COUNT_DIFF);
            static std::atomic<int> told{0};
            if (told++ < 10)
                logf("vk: eng jobs: job %u: the replay met %zu draws, the job's walk %zu (view %#llx, %u records' bytes, %u packets)", serial,
                     r.i, r.j->trace.size(), (unsigned long long)r.j->view, r.j->bytes, r.j->npkt);
            if (told <= 4) {
                char line[900];
                int n = snprintf(line, sizeof line, "vk: eng jobs: job %u's ops (job / replay):", serial);
                for (int o = 0; o < NOP_COUNT_ && n < 860; o++)
                    if (r.j->ops[o] || r.ops[o]) n += snprintf(line + n, sizeof line - n, " %d: %u/%u", o, r.j->ops[o], r.ops[o]);
                logf("%s", line);
            }
        }
        r.j->replayed.store(true, std::memory_order_release);
    }
    r.active = false, r.j = nullptr, r.pending = -1;
}

thread_local RState* rpaused = nullptr;   // (a job's walk the replay stopped at a wait: on when it goes on from there)
thread_local const uint8_t* rpaused_at = nullptr;
void eng_job_replay_suspend(const uint8_t* at) {
    RState& r = rs();
    if (!r.active) return;
    if (!rpaused) rpaused = new RState;
    std::swap(rstate, rpaused);   // (the paused one kept; the current one a fresh, inactive one)
    rstate->active = false, rstate->j = nullptr, rstate->pending = -1, rstate->pend_groups = ~0u;
    rpaused_at = at;
}
void eng_job_replay_resume(const uint8_t* at) {
    if (!rpaused_at || at != rpaused_at || !rpaused) return;
    std::swap(rstate, rpaused);
    rpaused_at = nullptr;
}
void eng_job_replay_op(int op) {
    if (op >= 0 && op < NOP_COUNT_) rstate->ops[op]++;
}
bool eng_job_replay_active() {
    return rstate && rstate->active;
}

void eng_job_replay_bind(int what, int stage, int first, int n, uint64_t src) {
    RState& r = rs();
    if (!r.active || !sw().check || stage < 0 || stage >= kNvnStages || first < 0 || what < 0 || what > 4) return;
    const int lim = what == 2 ? 32 : what >= 3 ? 16 : 128;
    const uint64_t step = what >= 3 ? sizeof(NVNbufferRange) : 8;
    for (int k = 0; k < n && first + k < lim; k++) r.d.src[what][stage][first + k] = src ? src + step * (uint64_t)k : 0;
}

void eng_job_replay_draw_op(const NvnState& s, int op, const void* payload, size_t n) {
    RState& r = rs();
    if (!r.active) return;
    flush_pending(r);
    r.pending = (int64_t)r.i;
    if (sw().check && r.j) {
        if (r.i < r.j->trace.size()) {
            DrawTrace t;
            trace_of(s, r.d, op, payload, n, t);
            const DrawTrace& w = r.j->trace[r.i];
            uint32_t groups = 0;
            for (int g = 0; g < G_VKPROG; g++)
                if (!(kFiltered >> g & 1) && t.h[g] != w.h[g]) groups |= 1u << g;
            add(C_CHECK_DRAWS);
            r.pend = t, r.pend_groups = groups;
            if (!w.filtered || !s.render_enabled || s.raster_discard) {   // (no program to compare what it reads by: the rest now)
                if (!w.filtered) add(C_CHECK_UNFILTERED);
                for (int g = 0; g < G_N; g++)
                    if (groups >> g & 1) group_mismatch[g].fetch_add(1, std::memory_order_relaxed);
                if (groups) add(C_CHECK_MISMATCH_DRAWS), mismatch_line(r, r.i, groups, w, t);
                r.pend_groups = ~0u;
            }
        }
    }
    r.i++;
}

void eng_job_replay_program(const NvnState& s, const Program& p, const SoProg* so, const uint8_t ct[8], const EngTargets& at) {
    RState& r = rs();
    if (!r.active || r.pending < 0) return;
    const size_t i = (size_t)r.pending;
    r.pending = -1;
    if (so && !p.mesh && !p.fallback) {   // (for the job threads: the program these draws pick, by what program_for picks by)
        const uint64_t k = pick_key(s, ct);
        bool fresh = false;
        {
            std::shared_lock lk(pub_mu);
            const auto it = published.find(k);
            fresh = it == published.end() || it->second.p != &p || it->second.so != so;
        }
        if (fresh) {
            std::unique_lock lk(pub_mu);
            published[k] = Pub{&p, so};
            add(C_PUBLISHED);
            if (!p.answers.empty()) add(C_PROG_ANSWERS);
        }
    }
    if (r.j && sw().self_pass && i == 0) {   // (the pass's formats as this job's first draw has them)
        EngPassFormats f{};
        f.ncolor = (uint32_t)std::clamp(at.n, 0, 8);
        for (uint32_t c = 0; c < f.ncolor; c++) f.color[c] = at.color[c];
        f.depth = at.depth, f.stencil = has_stencil_fmt(at.depth) ? at.depth : VK_FORMAT_UNDEFINED, f.samples = at.samples;
        bool same = false;
        {
            std::shared_lock lk(pass_mu);
            const auto it = self_passes.find(r.j->view);
            same = it != self_passes.end() && std::memcmp(&it->second, &f, sizeof f) == 0;
        }
        if (!same && r.j->view) {
            std::unique_lock lk(pass_mu);
            self_passes[r.j->view] = f;
        }
    }
    if (sw().check && r.j && i < r.j->trace.size() && r.pend_groups != ~0u) {
        const DrawTrace& w = r.j->trace[i];
        DrawTrace& t = r.pend;
        uint32_t groups = r.pend_groups;
        reads_of(s, r.d, p, at.n, t);
        for (int g = 0; g < G_N; g++)
            if ((kFiltered >> g & 1) && t.h[g] != w.h[g]) groups |= 1u << g;
        t.h[G_VKPROG] = p.key ^ p.variant * 31;
        if (w.h[G_VKPROG] != t.h[G_VKPROG]) groups |= 1u << G_VKPROG;
        const uint32_t wh[2] = {at.width, at.height};
        t.h[G_SIZE] = fnv(kFnv0, wh, sizeof wh);
        if (w.h[G_SIZE] != t.h[G_SIZE]) groups |= 1u << G_SIZE;
        for (int g = 0; g < G_N; g++)
            if (groups >> g & 1) group_mismatch[g].fetch_add(1, std::memory_order_relaxed);
        if (groups) add(C_CHECK_MISMATCH_DRAWS), mismatch_line(r, i, groups, w, t);
        add(C_CHECK_FILTERED);
        r.pend_groups = ~0u;
    }
}

void eng_job_report(double frames) {
    if (!sw().on) return;
    static int64_t last[C_N], last_w[WHY_N], last_g[G_N];
    int64_t c[C_N], w[WHY_N], g[G_N];
    for (int i = 0; i < C_N; i++) c[i] = cnt[i].load(std::memory_order_relaxed);
    for (int i = 0; i < WHY_N; i++) w[i] = why_n[i].load(std::memory_order_relaxed);
    for (int i = 0; i < G_N; i++) g[i] = group_mismatch[i].load(std::memory_order_relaxed);
    const double jobs = (double)(c[C_JOBS] - last[C_JOBS]);
    char why[400];
    int n = 0;
    why[0] = 0;
    for (int i = 0; i < WHY_N && n < 380; i++)
        if (w[i] != last_w[i]) n += snprintf(why + n, sizeof why - n, " %s %.1f,", kWhy[i], (w[i] - last_w[i]) / frames);
    logf("vk: a frame: eng jobs: %.1f jobs (%.1f draws, %.1f recorded, %.1f left out as the replay leaves them), by verdict:%s "
         "walk + recording %.3f ms on the job threads (%.1f us a job); %.1f packets (%.1f stale), %.1f deferred handles, %.1f deferred "
         "ranges; %lld programs published (%lld picked by textures); E4: %.1f lookups, %.1f served",
         jobs / frames, (c[C_DRAWS] - last[C_DRAWS]) / frames, (c[C_DRAWS_REC] - last[C_DRAWS_REC]) / frames,
         (c[C_DRAWS_SKIPPED] - last[C_DRAWS_SKIPPED]) / frames, why, (c[C_REC_NS] - last[C_REC_NS]) / frames * 1e-6,
         jobs ? (c[C_REC_NS] - last[C_REC_NS]) / jobs * 1e-3 : 0.0, (c[C_PKT] - last[C_PKT]) / frames,
         (c[C_PKT_STALE] - last[C_PKT_STALE]) / frames, (c[C_DEFERRED] - last[C_DEFERRED]) / frames,
         (c[C_DEFERRED_BUFS] - last[C_DEFERRED_BUFS]) / frames, (long long)c[C_PUBLISHED], (long long)c[C_PROG_ANSWERS],
         (c[C_LOOKUPS] - last[C_LOOKUPS]) / frames, (c[C_LOOKUP_HITS] - last[C_LOOKUP_HITS]) / frames);
    if (sw().check) {
        n = 0;
        char gl[700];
        gl[0] = 0;
        for (int i = 0; i < G_N && n < 680; i++)
            if (g[i]) n += snprintf(gl + n, sizeof gl - n, " %s %lld,", kGroup[i], (long long)g[i]);
        logf("vk: eng jobs check: %.1f jobs a frame met by the replay (%.2f not found), %.1f draws compared (%.1f with what their program reads, %.1f without a program on the job's side), %.2f differ; so far %lld "
             "draws differ, %lld jobs with another draw count; by group:%s",
             (c[C_CHECK_JOBS] - last[C_CHECK_JOBS]) / frames, (c[C_CHECK_NO_JOB] - last[C_CHECK_NO_JOB]) / frames,
             (c[C_CHECK_DRAWS] - last[C_CHECK_DRAWS]) / frames, (c[C_CHECK_FILTERED] - last[C_CHECK_FILTERED]) / frames,
             (c[C_CHECK_UNFILTERED] - last[C_CHECK_UNFILTERED]) / frames, (c[C_CHECK_MISMATCH_DRAWS] - last[C_CHECK_MISMATCH_DRAWS]) / frames,
             (long long)c[C_CHECK_MISMATCH_DRAWS], (long long)c[C_CHECK_COUNT_DIFF], gl[0] ? gl : " none");
    }
    {   // (the ops that kept a job from being recorded, so far)
        static int told = 0;
        if (told++ % 5 == 0) {
            char ol[600];
            n = 0;
            ol[0] = 0;
            for (int i = 0; i < NOP_COUNT_ && n < 580; i++) {
                const int64_t v = op_refused[i].load(std::memory_order_relaxed);
                if (v) n += snprintf(ol + n, sizeof ol - n, " op %d: %lld,", i, (long long)v);
            }
            if (ol[0]) logf("vk: eng jobs: ops not taken so far:%s", ol);
        }
    }
    std::memcpy(last, c, sizeof last), std::memcpy(last_w, w, sizeof last_w), std::memcpy(last_g, g, sizeof last_g);
}

}  // namespace mkwvk

extern "C" void mkwvk_eng_job_recorded(const rx_eng_job_info* ji, const uint8_t* rec, uint64_t bytes, uint64_t handle) {
    using namespace mkwvk;
    if (!sw().on || !ji || !rec || !ji->serial) return;
    const int64_t t0 = now_ns();
    static const bool can = [] {
        const bool v = desc_heap_on() && eng_reg_on() && so_on();
        logf("vk: eng jobs (HOS_ENG_JOBS): %s%s", v ? "jobs recorded as secondaries on their threads" :
             "jobs walked only (secondaries need RX_VK_DESC_HEAP=1, HOS_ENG_REG=1 and shader objects)",
             sw().check ? ", checked against the replay (HOS_ENG_JOBS_CHECK)" : "");
        return v;
    }();
    Walk& w = walk();
    w.s = NvnState{};
    w.d.clear();
    w.tex.clear(), w.deferred.clear(), w.trace.clear(), w.npkt = 0;
    std::memset(w.ops, 0, sizeof w.ops);
    Rec r;
    EngPassFormats fmt{};
    r.at.width = (uint32_t)std::max(0.0f, ji->rect[2] - ji->rect[0]);
    r.at.height = (uint32_t)std::max(0.0f, ji->rect[3] - ji->rect[1]);
    if (!sw().rec || !can) {
        r.why = WHY_NOT_RECORDED;
    }
    if (ji->view && pass_formats(ji->view, &fmt)) {
        r.have_fmt = true;
        r.at.n = (int)fmt.ncolor;
        for (int i = 0; i < 8; i++) r.at.color[i] = i < (int)fmt.ncolor ? fmt.color[i] : VK_FORMAT_UNDEFINED;
        r.at.depth = fmt.depth, r.at.samples = fmt.samples ? fmt.samples : VK_SAMPLE_COUNT_1_BIT;
        color_types(fmt, r.types);
    }
    if (r.why != WHY_OK) {
    } else if (!r.have_fmt) {
        r.why = WHY_NO_PASS;
    } else if (!(r.cb = take_cb(ji->serial))) {
        r.why = WHY_POOL;
    } else {
        VkCommandBufferInheritanceRenderingInfo ri{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
        ri.colorAttachmentCount = fmt.ncolor;
        ri.pColorAttachmentFormats = fmt.color;
        ri.depthAttachmentFormat = fmt.depth, ri.stencilAttachmentFormat = fmt.stencil;
        ri.rasterizationSamples = r.at.samples;
        VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        ii.pNext = &ri;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        bi.pInheritanceInfo = &ii;
        if (vkBeginCommandBuffer(r.cb, &bi) != VK_SUCCESS) r.cb = VK_NULL_HANDLE, r.why = WHY_POOL;
    }
    walk_records(w, &r, rec, rec + bytes);
    if (r.cb) vkEndCommandBuffer(r.cb);
    uniq(w.tex), uniq(w.deferred);
    // (published in the ring before the handle: the replay meets the job's records after this returns)
    Job& j = ring()[ji->serial % kRing];
    j.serial.store(0, std::memory_order_release);
    j.handle = handle, j.view = ji->view;
    std::memcpy(j.rect, ji->rect, sizeof j.rect);
    j.why = r.why, j.bytes = (uint32_t)bytes, j.npkt = w.npkt;
    std::memcpy(j.ops, w.ops, sizeof j.ops);
    j.tex.swap(w.tex), j.deferred.swap(w.deferred), j.trace.swap(w.trace);
    j.used_value.store(0, std::memory_order_relaxed);
    j.replayed.store(false, std::memory_order_relaxed);
    j.pub.view_key = ji->view;
    j.pub.fmt = fmt;
    j.pub.cb = r.why == WHY_OK ? r.cb : VK_NULL_HANDLE;
    j.pub.ndraws = r.ndraws;
    j.pub.tex = j.tex.data(), j.pub.ntex = (uint32_t)j.tex.size();
    j.pub.deferred = j.deferred.data(), j.pub.ndeferred = (uint32_t)j.deferred.size();
    j.pub.serial = ji->serial;
    j.serial.store(ji->serial, std::memory_order_release);
    {
        std::lock_guard lk(handle_mu);
        by_handle[handle] = ji->serial;
        if (by_handle.size() > 4 * kRing)   // (handles of jobs long gone)
            for (auto it = by_handle.begin(); it != by_handle.end();)
                it = ring()[it->second % kRing].serial.load(std::memory_order_relaxed) != it->second ? by_handle.erase(it) : std::next(it);
    }
    why_n[r.why].fetch_add(1, std::memory_order_relaxed);
    add(C_JOBS);
    add(C_REC_NS, now_ns() - t0);
}
