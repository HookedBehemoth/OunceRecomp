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
#include <unordered_set>
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

bool program_answers_hold(const NvnState& s, const Program& p);   // (vk_shader.cpp)
namespace {

int64_t now_ns() { return host ? host->now_ns() : 0; }

// ---- switches (read once) ----
constexpr uint32_t kSplitCut = 1u << 13;
// (HOS_ENG_JOBS_SPLIT=1: every per-draw refusal but "descriptors": a run before a draw with a classic storage buffer drew wrong in
// one Hyrule Field pass (e6-impl.md), not yet understood)
constexpr uint32_t kSplitDefault = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 12) | kSplitCut;
struct Sw {
    bool on = false, check = false, rec = true, self_pass = true, split = false, sampled = false, prework = false;
    uint32_t split_mask = 0;   // (HOS_ENG_JOBS_SPLIT=<mask>: 1 << why for the refusals a job is split at, 1 << 13 the barrier / dispatch cuts; 1: the default set)
    Sw() {
        const char* e = getenv("HOS_ENG_JOBS");
        on = e && atoi(e) == 1;
        const char* c = getenv("HOS_ENG_JOBS_CHECK");
        check = on && c && atoi(c) != 0;
        const char* r = getenv("HOS_ENG_JOBS_REC");
        rec = !(r && atoi(r) == 0);
        const char* p = getenv("HOS_ENG_JOBS_SELFPASS");
        self_pass = !ENG_JOB_HAVE_PASS || (p && atoi(p) != 0);
        const char* sp = getenv("HOS_ENG_JOBS_SPLIT");   // (a job split around the draws it cannot record; not with the check)
        split = on && !check && sp && strtoul(sp, nullptr, 0) != 0;
        split_mask = split ? (uint32_t)strtoul(sp, nullptr, 0) : 0;
        if (split_mask == 1) split_mask = kSplitDefault;
        // (EngJob.tex: what the recorded draws' programs read, not every bound word; on by default, HOS_ENG_JOBS_SAMPLED=0: every bound
        // word, as before: e6-impl.md 3, not-resident fallbacks 2.6 / 3.0 -> 0 a frame)
        const char* sa = getenv("HOS_ENG_JOBS_SAMPLED");
        sampled = on && !(sa && atoi(sa) == 0);
        const char* pw = getenv("HOS_ENG_JOBS_PREWORK");   // (fallbacks.md 4: indirect mesh draws, waits and the barrier after them)
        prework = on && pw && atoi(pw) != 0;
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
    int8_t ncolor;   // (the colour targets it was made for: the pass formats' / the replay's)
    int16_t op;
    VkFormat color0, depth;
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
    t.op = (int16_t)op;
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
           WHY_NOT_RECORDED, WHY_GATED, WHY_TEX_VARIANT, WHY_N };
const char* const kWhy[WHY_N] = {"recorded", "no pass formats", "program not seen", "program not ready", "draw kind", "vertex memory",
                               "index memory", "ops not taken", "command pool full", "descriptors", "not recorded", "draws left out by a condition",
                               "texture variant"};
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
    std::vector<EngDescPatch> patches;   // (bank words from deferred handles: rewritten at the submit)
    std::vector<EngDrawDesc> descs;      // (check: per job draw, its descriptors as made here; why EDW_N: none made)
    std::vector<EngSeg> segs;            // (HOS_ENG_JOBS_SPLIT: the runs of draws recorded)
    std::vector<uint32_t> dsig;          // (and each draw op's payload hash)
    std::vector<uint64_t> ubo;           // (the uniform ranges its recorded draws read, pairs)
    std::vector<EngPreCopy> pre;         // (HOS_ENG_JOBS_PREWORK)
    std::vector<EngWait> waits;
    bool pre_barrier = false;
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
             C_LOOKUP_HITS, C_PROG_ANSWERS, C_CHECK_UNFILTERED, C_CHECK_FILTERED, C_DESC_CHECKED, C_DESC_MISMATCH, C_DESC_NONE, C_PATCHED, C_PATCH_NO_JOB, C_KIND_INDIRECT, C_KIND_MESH_OTHER, C_KIND_PRIM, C_MESH_REC, C_VTX_UNSET, C_IND_REC, C_N };
std::atomic<int64_t> cnt[C_N];
std::atomic<int64_t> why_n[WHY_N];
std::atomic<int64_t> why_draw_n[WHY_N];   // (HOS_ENG_JOBS_SPLIT: draws left to the replay inside split jobs, by why)
std::atomic<int64_t> split_jobs, split_segs;
std::atomic<int64_t> group_mismatch[G_N];
std::atomic<int64_t> op_refused[NOP_COUNT_];
std::atomic<int64_t> desc_why[EDW_N];
const char* const kDescWhy[EDW_N] = {"made", "no heap program", "uniform buffer not in layer 0", "uniform buffer unaligned",
                                     "storage buffer not bound in layer 0", "written storage buffer", "storage buffer not read by address", "texel buffer", "storage image",
                                     "handle program", "handle in a uniform buffer", "deferred handle pushed", "ring", "stream"};
inline void add(Count c, int64_t v = 1) { cnt[c].fetch_add(v, std::memory_order_relaxed); }

// ---- the programs the replay picked for job draws: by what program_for picks by, the job threads' lookup ----
struct Pub { const Program* p; const SoProg* so; };
std::shared_mutex pub_mu;
std::unordered_map<uint64_t, Pub> published;
std::unordered_set<uint64_t> fallback_keys;   // (R1, HOS_ENG_WHYLOG: pick keys whose program is the task/mesh fallback)
uint64_t pick_key(const NvnState& s, const uint8_t ct[8], bool mesh = false) {
    uint64_t h = fnv(mesh ? kFnv0 ^ 0x4d455348 : kFnv0, s.vattr, sizeof s.vattr);
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
    std::vector<uint64_t> ubo;   // (E6: the recorded draws' uniform ranges, pairs)
    std::vector<EngPreCopy> pre;   // (HOS_ENG_JOBS_PREWORK)
    std::vector<EngWait> waits;
    bool pre_barrier = false;
    uint32_t npkt = 0;
    uint16_t ops[NOP_COUNT_];
    EngBindRefs refs;      // (the deferred bind each slot's word came from: the submit's rewrite of the banks)
    uint32_t nbinds = 0;   // (the job's deferred handle binds so far, in record order: as resolve_deferred meets them)
    std::vector<EngDescPatch> patches;
    std::vector<EngDrawDesc> descs;
    EngDrawDesc desc;
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
    uint32_t ndraws = 0, gated = 0;
    void fail(uint32_t w) {
        if (why == WHY_OK) why = w;
    }
    // (HOS_ENG_JOBS_SPLIT) the draw refused (dwhy: this draw's why, the job goes on), the run being recorded, the runs made
    bool split = false;
    uint32_t dwhy = WHY_OK, first_dwhy = WHY_OK, draw_i = 0, seg_first = 0, seg_ndraws = 0, ninline = 0, serial = 0;
    bool cut = false;   // (a run ended at an op the replay does between runs: a barrier, a dispatch)
    const Program* dprog = nullptr;   // (R1: the program record_draw found for the draw, before a refusal)
    bool any_draw = false;            // (a draw op met: a barrier after it is not the pre-work's)
    uint64_t view = 0;
    const VkCommandBufferBeginInfo* bi = nullptr;
    std::vector<EngSeg> segs;
    std::vector<uint32_t> dsig;
    void refuse(uint32_t w) {
        if (!split || !(sw().split_mask >> w & 1)) return fail(w);
        if (dwhy == WHY_OK) dwhy = w;
    }
};

/// Handle words bound (what: 0 texture, 1 sampler, 2 image; 3 uniform, 4 storage ranges), immediate (src 0) or deferred
/// (resolve_deferred's rule: a handle bind that reads the game's array at the submit)
inline bool deferred_handles(uint32_t op, const uint8_t* a, uint32_t avail) {
    if (op != NOP_BIND_TEX && op != NOP_BIND_IMG && op != NOP_BIND_SAMP) return false;
    nvn_p_bind_handles b;
    std::memset(&b, 0, sizeof b);
    std::memcpy(&b, a, std::min<size_t>(sizeof b, avail));
    return b.src && b.n > 0 && b.n <= 32;
}
void note_bind(Walk& w, int what, int stage, int first, int n, const uint64_t* h, uint64_t src, uint32_t bind = 0) {
    if (stage < 0 || stage >= kNvnStages || first < 0) return;
    const int lim = what == 2 ? 32 : what >= 3 ? 16 : 128;
    const uint64_t step = what >= 3 ? sizeof(NVNbufferRange) : 8;
    for (int k = 0; k < n && first + k < lim; k++) {
        w.d.src[what][stage][first + k] = src ? src + step * (uint64_t)k : 0;
        if (what < 3) w.refs[what][stage][first + k] = src && bind ? bind << 8 | (uint32_t)k : 0;
        if (src) {
            if (what < 3) w.deferred.push_back(src + step * (uint64_t)k);
        } else if (h && (what == 0 || what == 2) && h[k] && !sw().sampled) {
            w.tex.push_back((uint32_t)h[k]);
        }
    }
}

void record_draw(Walk& w, Rec& r, int op, const void* pl) {
    const NvnState& s = w.s;
    if (!s.render_enabled || s.raster_discard) {   // (as the replay leaves them out; a condition read now may read otherwise there)
        add(C_DRAWS_SKIPPED);
        if (!s.render_enabled) r.gated++;
        return;
    }
    const bool mesh = op == NOP_DRAW_MESH;
    nvn_p_draw dr{};
    nvn_p_draw_idx di{};
    nvn_p_draw_mesh dm{};
    nvn_p_draw_indirect dn{};
    // (HOS_ENG_JOBS_INDIRECT=1, fallbacks.md 2a/2b: DrawArrays/ElementsIndirect(+Count) from their records in layer 0 (Vulkan's layout),
    // and a direct mesh draw's first task from a slot of the job stream)
    static const bool ind_on = getenv("HOS_ENG_JOBS_INDIRECT") && atoi(getenv("HOS_ENG_JOBS_INDIRECT")) != 0;
    const bool ind = op == NOP_DRAW_INDIRECT;
    if (op == NOP_DRAW) std::memcpy(&dr, pl, sizeof dr);
    else if (op == NOP_DRAW_IDX) std::memcpy(&di, pl, sizeof di);
    else if (mesh) std::memcpy(&dm, pl, sizeof dm);
    else if (ind && ind_on) std::memcpy(&dn, pl, sizeof dn);
    else return r.refuse(WHY_DRAW_KIND), void(add(C_KIND_INDIRECT));
    if (ind && (dn.max < 0 || dn.stride < 0)) return r.refuse(WHY_DRAW_KIND), void(add(C_KIND_INDIRECT));
    // (a mesh draw: DrawMeshTasks(first, n); an indirect one takes the replay's argument conversion (fallbacks.md 2c))
    const bool mesh_ind = mesh && dm.indirect && sw().prework && dm.max >= 0 && dm.max <= 64 && dm.stride >= 0;   // (fallbacks.md 2c)
    if (mesh && !mesh_ind && (dm.kind != 0 || dm.indirect || (dm.x != 0 && !ind_on))) return r.refuse(WHY_DRAW_KIND), void(add(C_KIND_MESH_OTHER));
    const int prim = op == NOP_DRAW ? dr.prim : ind ? dn.prim : di.prim;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (!mesh && (prim == NVN_DRAW_PRIMITIVE_QUADS || !eng_draw_topology(prim, &topo))) return r.refuse(WHY_DRAW_KIND), void(add(C_KIND_PRIM));
    if (!s.has_prog) return r.refuse(WHY_NO_PROG);
    Pub pub{};
    {
        std::shared_lock lk(pub_mu);
        const auto it = published.find(pick_key(s, r.types, mesh));
        if (it == published.end()) return r.refuse(WHY_NO_PROG);
        pub = it->second;
    }
    const Program& p = *pub.p;
    const SoProg& so = *pub.so;
    r.dprog = &p;
    if (!p.ready.load(std::memory_order_acquire) || p.failed || !so.ready.load(std::memory_order_acquire) || so.failed ||
        p.mesh != mesh || p.fallback || !(mesh ? so.ms : so.vs))
        return r.refuse(WHY_PROG_NOT_READY);
    // (the program's texture answers: the published one is the variant the replay picked for other textures of these inputs; a
    // texture of another type or integer-ness here needs another variant, which only the replay picks)
    if (!program_answers_hold(s, p)) return r.refuse(WHY_TEX_VARIANT);
    // (HOS_ENG_JOBS_VTX_CHECK=1: a vertex stream it loads with no buffer in the job; =2 also an attribute the job never set: both
    // its caller's. Off by default: =2 refuses ~98 HF draws a frame, which inherit the vertex state of their layer list, e6-impl.md)
    static const int vtx_check = getenv("HOS_ENG_JOBS_VTX_CHECK") ? atoi(getenv("HOS_ENG_JOBS_VTX_CHECK")) : 0;
    if (vtx_check && !eng_draw_vertex_bound(s, p, vtx_check >= 2)) return add(C_VTX_UNSET), r.refuse(WHY_VERTEX_MEM);
    const HeapProgram* hp = so.heap ? desc_heap_program(p) : nullptr;
    if (!hp) return r.refuse(WHY_DESCRIPTORS), void(desc_why[EDW_NO_HEAP].fetch_add(1, std::memory_order_relaxed));
    EngDrawDesc& dd = sw().check && !w.descs.empty() ? w.descs.back() : w.desc;
    if (!eng_draw_descriptors(s, p, *hp, r.at, w.refs, dd)) {
        desc_why[std::min<uint32_t>(dd.why, EDW_N - 1)].fetch_add(1, std::memory_order_relaxed);
        return r.refuse(WHY_DESCRIPTORS);
    }
    if (sw().sampled) {   // (the textures this draw reads; a draw refused below adds its own too: conservative)
        eng_draw_sampled(s, p, w.refs, w.tex);
    }
    if (mesh && dm.x != 0 && !eng_draw_mesh_first(dd, (uint32_t)dm.x)) return r.refuse(WHY_DESCRIPTORS);
    w.patches.insert(w.patches.end(), dd.patches.begin(), dd.patches.end());
    for (size_t k = 0; k + 1 < dd.ubos.size(); k += 2) {   // (consecutive draws bind the same ranges: kept once in a row)
        const size_t m = w.ubo.size();
        bool seen = false;
        for (size_t q = m >= 16 ? m - 16 : 0; q + 1 < m && !seen; q += 2) seen = w.ubo[q] == dd.ubos[k] && w.ubo[q + 1] == dd.ubos[k + 1];
        if (!seen) w.ubo.push_back(dd.ubos[k]), w.ubo.push_back(dd.ubos[k + 1]);
    }
    eng_draw_state(r.cb, s, p, so, r.at, topo, r.first, mesh);
    if (r.first) desc_heap_bind_job(r.cb);
    r.first = false;
    desc_heap_push(r.cb, dd.push, dd.push_bytes);
    if (mesh_ind) {   // (its arguments, made by the GPU culling, converted into slots of the job stream by the pre-work at the call site)
        const uint32_t n = (uint32_t)std::max(dm.max, 1), stride = (uint32_t)std::max(dm.stride, 12);
        VkBuffer sb;
        uint64_t soff;
        uint8_t* smap;
        VkDeviceAddress sa;
        const BufferRef src = dm.indirect < (1ull << 40) ? layer0_buffer(dm.indirect, (uint64_t)(n - 1) * stride + 4) : BufferRef{};
        const BufferRef cnt = dm.count_addr && dm.count_addr < (1ull << 40) ? layer0_buffer(dm.count_addr, 4) : BufferRef{};
        if (!src.buffer || (dm.count_addr && !cnt.buffer) || !eng_job_stream_alloc(n * 12, 16, &sb, &soff, &smap, &sa))
            return r.refuse(WHY_INDEX_MEM);
        for (uint32_t i = 0; i < n; i++) {   // ({x, 1, 1}: x copied from the record's count word, as mesh_indirect_convert)
            const uint32_t one[3] = {0, 1, 1};
            std::memcpy(smap + i * 12, one, 12);
            w.pre.push_back({src.buffer, src.offset + (uint64_t)i * stride, sb, soff + (uint64_t)i * 12, 4});
        }
        if (cnt.buffer) vkCmdDrawMeshTasksIndirectCountEXT(r.cb, sb, soff, cnt.buffer, cnt.offset, n, 12);
        else vkCmdDrawMeshTasksIndirectEXT(r.cb, sb, soff, n, 12);
        r.ndraws++;
        add(C_DRAWS_REC), add(C_MESH_REC);
        return;
    }
    if (mesh) {
        vkCmdDrawMeshTasksEXT(r.cb, (uint32_t)std::max(dm.y, 0), 1, 1);
        r.ndraws++;
        add(C_DRAWS_REC), add(C_MESH_REC);
        return;
    }
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
            r.refuse(WHY_VERTEX_MEM);
            continue;
        }
        vb[b] = br.buffer, vo[b] = br.offset, vz[b] = s.vbuf[b].size ? s.vbuf[b].size : VK_WHOLE_SIZE, vs[b] = s.vstream[b].stride;
    }
    if (r.dwhy) return;   // (split: this draw is the replay's)
    vkCmdBindVertexBuffers2(r.cb, 0, 16, vb, vo, vz, vs);
    if (ind) {   // (fallbacks.md 2a: the game's records are VkDrawIndirectCommand / VkDrawIndexedIndirectCommand)
        const uint32_t stride = (uint32_t)std::max(dn.stride, dn.indexed ? 20 : 16), n = (uint32_t)std::max(dn.max, 1);
        const BufferRef args = dn.indirect < (1ull << 40) ? layer0_buffer(dn.indirect, (uint64_t)n * stride) : BufferRef{};
        const BufferRef cnt = dn.count_addr && dn.count_addr < (1ull << 40) ? layer0_buffer(dn.count_addr, 4) : BufferRef{};
        if (!args.buffer || (dn.count_addr && !cnt.buffer)) return r.refuse(WHY_INDEX_MEM);
        if (dn.indexed) {
            if (dn.itype == 0 && !dev.index_uint8) return r.refuse(WHY_DRAW_KIND);
            const BufferRef ib = dn.ibuf < (1ull << 40) ? layer0_buffer(dn.ibuf, 4) : BufferRef{};
            if (!ib.buffer) return r.refuse(WHY_INDEX_MEM);
            vkCmdBindIndexBuffer(r.cb, ib.buffer, ib.offset,
                                 dn.itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : dn.itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            if (cnt.buffer) vkCmdDrawIndexedIndirectCount(r.cb, args.buffer, args.offset, cnt.buffer, cnt.offset, n, stride);
            else vkCmdDrawIndexedIndirect(r.cb, args.buffer, args.offset, n, stride);
        } else {
            if (cnt.buffer) vkCmdDrawIndirectCount(r.cb, args.buffer, args.offset, cnt.buffer, cnt.offset, n, stride);
            else vkCmdDrawIndirect(r.cb, args.buffer, args.offset, n, stride);
        }
        add(C_IND_REC);
    } else if (op == NOP_DRAW_IDX) {
        if (di.itype == 0 && !dev.index_uint8) return r.refuse(WHY_DRAW_KIND);
        const uint32_t unit = di.itype == 0 ? 1 : di.itype == 1 ? 2 : 4;
        const BufferRef ib = di.ibuf < (1ull << 40) ? layer0_buffer(di.ibuf, (uint64_t)std::max(di.count, 1) * unit) : BufferRef{};
        if (!ib.buffer) return r.refuse(WHY_INDEX_MEM);
        vkCmdBindIndexBuffer(r.cb, ib.buffer, ib.offset,
                             di.itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : di.itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(r.cb, (uint32_t)di.count, (uint32_t)std::max(1, di.instances), 0, di.base_vertex, (uint32_t)di.base_instance);
    } else {
        vkCmdDraw(r.cb, (uint32_t)dr.count, (uint32_t)std::max(1, dr.instances), (uint32_t)dr.first, (uint32_t)dr.base_instance);
    }
    r.ndraws++;
    add(C_DRAWS_REC);
}

/// (HOS_ENG_JOBS_SPLIT) the run being recorded ends before an op the replay does between runs (an empty open run stays open)
void split_cut(Rec* r) {
    r->cut = true;
    if (!r->cb) return;
    if (r->draw_i > r->seg_first) {
        vkEndCommandBuffer(r->cb);
        r->segs.push_back({r->seg_first, r->draw_i - r->seg_first, r->cb, r->ndraws - r->seg_ndraws});
        r->cb = VK_NULL_HANDLE;
    } else {
        r->seg_first = r->draw_i;
    }
}
void on_draw(Walk& w, Rec* r, int op, const void* pl, size_t n) {
    add(C_DRAWS);
    r->any_draw = true;
    if (sw().check) {
        w.descs.emplace_back();
        w.descs.back().why = EDW_N;   // (none made, until record_draw makes it)
        w.trace.emplace_back();
        DrawTrace& t = w.trace.back();
        trace_of(w.s, w.d, op, pl, n, t);
        const uint32_t wh[2] = {(uint32_t)std::max(0, (int)r->at.width), (uint32_t)std::max(0, (int)r->at.height)};
        t.h[G_SIZE] = fnv(kFnv0, wh, sizeof wh);
        if (w.s.has_prog && r->have_fmt) {   // (what the program the replay picked for these inputs reads)
            Pub pub{};
            {
                std::shared_lock lk(pub_mu);
                const auto it = published.find(pick_key(w.s, r->types, op == NOP_DRAW_MESH));
                if (it != published.end()) pub = it->second;
            }
            if (pub.p && pub.p->ready.load(std::memory_order_acquire) && !pub.p->failed) {
                reads_of(w.s, w.d, *pub.p, r->at.n, t);
                t.ncolor = (int8_t)r->at.n, t.color0 = r->at.color[0], t.depth = r->at.depth;
                t.h[G_VKPROG] = pub.p->key ^ pub.p->variant * 31;
            }
        }
    }
    if (!r->split || op == NOP_DISPATCH || op == NOP_DISPATCH_INDIRECT) {
        if (r->cb) record_draw(w, *r, op, pl);
        return;
    }
    // (HOS_ENG_JOBS_SPLIT) a run of the draws this thread records, cut at each draw it cannot: that one is the replay's
    const uint32_t i = r->draw_i++;
    r->dsig.push_back(eng_draw_sig(op, pl, n));
    if (r->why != WHY_OK || !r->bi) return;
    if (!r->cb) {   // (a run after a refused draw: its own secondary, all its state set again)
        if (!(r->cb = take_cb(r->serial)) || vkBeginCommandBuffer(r->cb, r->bi) != VK_SUCCESS) return void(r->cb = VK_NULL_HANDLE), r->fail(WHY_POOL);
        r->first = true, r->seg_first = i, r->seg_ndraws = r->ndraws;
    }
    r->dwhy = WHY_OK;
    r->dprog = nullptr;
    {   // (bisection: HOS_ENG_SPLIT_VIEW=<view> HOS_ENG_SPLIT_LO=a HOS_ENG_SPLIT_HI=b: that view's draw ops outside [a, b) left to the replay)
        static const uint64_t bv = getenv("HOS_ENG_SPLIT_VIEW") ? strtoull(getenv("HOS_ENG_SPLIT_VIEW"), nullptr, 0) : 0;
        static const uint32_t lo = getenv("HOS_ENG_SPLIT_LO") ? (uint32_t)atoi(getenv("HOS_ENG_SPLIT_LO")) : 0;
        static const uint32_t hi = getenv("HOS_ENG_SPLIT_HI") ? (uint32_t)atoi(getenv("HOS_ENG_SPLIT_HI")) : ~0u;
        if (bv && r->view == bv && (i < lo || i >= hi)) r->dwhy = WHY_DESCRIPTORS;
    }
    if (r->dwhy == WHY_OK) {
        static const uint64_t lv = getenv("HOS_ENG_SPLIT_LOGVIEW") ? strtoull(getenv("HOS_ENG_SPLIT_LOGVIEW"), nullptr, 0) : 0;
        static std::atomic<int> told{0};
        if (lv && r->view == lv && i == 0 && cnt[C_JOBS].load(std::memory_order_relaxed) > 100000 && told++ < 12) {
            const NvnState& q = w.s;
            uint32_t cntv = 0, inst = 0;
            if (op == NOP_DRAW_IDX) { nvn_p_draw_idx di; std::memcpy(&di, pl, sizeof di); cntv = di.count, inst = di.instances; }
            else if (op == NOP_DRAW) { nvn_p_draw dr; std::memcpy(&dr, pl, sizeof dr); cntv = dr.count, inst = dr.instances; }
            logf("vk: e6 job %u draw 0: op %d count %u inst %u vb0 %#llx+%llu vb1 %#llx vp %.0f %.0f %.0f %.0f sc %d %d %d %d dr %.2f %.2f "
                 "re %d rd %d prog %016llx vb0 buf %p+%llu", r->serial, op, cntv, inst, (unsigned long long)q.vbuf[0].address, (unsigned long long)q.vbuf[0].size,
                 (unsigned long long)q.vbuf[1].address, q.viewport[0][0], q.viewport[0][1], q.viewport[0][2], q.viewport[0][3], q.scissor[0],
                 q.scissor[1], q.scissor[2], q.scissor[3], q.depth_near, q.depth_far, (int)q.render_enabled, (int)q.raster_discard,
                 (unsigned long long)prog_stage_sig(q.prog), (void*)layer0_buffer(q.vbuf[0].address, 1).buffer,
                 (unsigned long long)layer0_buffer(q.vbuf[0].address, 1).offset);
        }
        record_draw(w, *r, op, pl);
    }
    if (r->dwhy == WHY_OK) return;
    why_draw_n[r->dwhy].fetch_add(1, std::memory_order_relaxed);
    {   // (R1, HOS_ENG_WHYLOG=1: each (program, verdict, descriptor kind) once: what the refusal needs)
        static const bool wl = getenv("HOS_ENG_WHYLOG") && atoi(getenv("HOS_ENG_WHYLOG")) != 0;
        static std::mutex wm;
        static std::unordered_set<uint64_t> seen;
        if (wl) {
            const NvnState& q = w.s;
            const uint64_t key = q.has_prog ? prog_stage_sig(q.prog) : 0;
            const uint32_t dk = r->dwhy == WHY_DESCRIPTORS ? w.desc.why : 0;
            const uint64_t k = fnv(fnv(kFnv0, &key, 8), &r->dwhy, 4) ^ (uint64_t)dk * 0x9e3779b97f4a7c15ull;
            bool fresh;
            {
                std::lock_guard lk(wm);
                fresh = seen.insert(k).second;
            }
            if (fresh) {
                nvn_p_draw_mesh dm{};
                if (op == NOP_DRAW_MESH) std::memcpy(&dm, pl, sizeof dm);
                const Program* pr = r->dprog;
                bool fb_key = false;
                {
                    std::shared_lock lk(pub_mu);
                    fb_key = fallback_keys.count(pick_key(q, r->types, op == NOP_DRAW_MESH)) != 0;
                }
                const std::string bda = pr && r->dwhy == WHY_DESCRIPTORS ? eng_bda_refusal(*pr) : std::string();
                logf("eng whylog: view %#llx program %016llx: %s%s%s; op %d mesh kind %d x %d indirect %#llx max %d stride %d; fallback %d mesh %d (replay's pick a fallback: %d)%s%s",
                     (unsigned long long)r->view, (unsigned long long)key, kWhy[r->dwhy], dk ? ": " : "", dk ? kDescWhy[std::min<uint32_t>(dk, EDW_N - 1)] : "",
                     op, dm.kind, dm.x, (unsigned long long)dm.indirect, dm.max, dm.stride, pr ? (int)pr->fallback : -1, pr ? (int)pr->mesh : -1, (int)fb_key,
                     bda.empty() ? "" : "; storage on descriptors: ", bda.c_str());
            }
        }
    }
    {
        static std::atomic<int> told{0};
        if (told < 40 && cnt[C_JOBS].load(std::memory_order_relaxed) > 120000 && told++ < 40)
            logf("vk: eng jobs split: job %u draw op %u refused: %s (op %d, %s)", r->serial, i, kWhy[r->dwhy], op,
                 r->dwhy == WHY_DESCRIPTORS ? kDescWhy[std::min<uint32_t>(w.desc.why, EDW_N - 1)] : "");
    }
    if (r->first_dwhy == WHY_OK) r->first_dwhy = r->dwhy;
    r->ninline++;
    if (i > r->seg_first) {   // (the run so far closed; an empty one stays open for the next draw)
        vkEndCommandBuffer(r->cb);
        r->segs.push_back({r->seg_first, i - r->seg_first, r->cb, r->ndraws - r->seg_ndraws});
        r->cb = VK_NULL_HANDLE;
    } else {
        r->seg_first = i + 1;
    }
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
        case NOP_BEGIN: case NOP_JUMP: case NOP_JOB: case NOP_ENG: break;   // (NOP_ENG: E4's marks, no GPU work)
        case NOP_PKT: {
            const auto rp = payload<nvn_p_pkt>(a, avail);
            add(C_PKT);
            w.npkt++;
            if (const nvn_pkt* k = nvn_pkt_live(&rp)) {
                walk_packet(w, r, *k);
                // (the records it stands for are walked at the submit all the same: their deferred binds keep the numbering)
                for (const uint8_t *q = p + h.size, *qe = std::min(p + h.size + rp.skip, end); q + 8 <= qe;) {
                    nvn_op_hdr qh;
                    std::memcpy(&qh, q, sizeof qh);
                    if (qh.size < 8 || (qh.size & 7)) break;
                    if (deferred_handles(qh.op, q + 8, qh.size - 8)) w.nbinds++;
                    q += qh.size;
                }
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
            const uint32_t bind = deferred_handles(h.op, a, avail) ? ++w.nbinds : 0;
            note_bind(w, h.op == NOP_BIND_TEX ? 0 : h.op == NOP_BIND_SAMP ? 1 : 2, b.stage, b.first, n, b.h, b.src, bind);
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
        case NOP_WAIT_EVENT:
            if (sw().prework) {   // (fallbacks.md 4: met at the job-call site, in replay order)
                const auto e = payload<nvn_p_wait_event>(a, avail);
                w.waits.push_back({e.gpu, e.mode, e.value});
                break;
            }
            if (h.op < NOP_COUNT_) op_refused[h.op].fetch_add(1, std::memory_order_relaxed);
            r->fail(WHY_OPS);
            break;
        case NOP_BARRIER:
            if (sw().prework && !r->any_draw) {   // (before the job's first draw: the pre-work's)
                w.pre_barrier = true;
                break;
            }
            if (r->split && (sw().split_mask & kSplitCut)) {   // (split: the run so far ends here; the replay records the barrier between the runs)
                split_cut(r);
                break;
            }
            if (h.op < NOP_COUNT_) op_refused[h.op].fetch_add(1, std::memory_order_relaxed);
            r->fail(WHY_OPS);
            break;
        case NOP_DISPATCH: case NOP_DISPATCH_INDIRECT:
            if (r->split && (sw().split_mask & kSplitCut)) {   // (split: the replay dispatches it between the runs)
                add(C_DRAWS);
                split_cut(r);
                break;
            }
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
    int64_t desc_i = -1;    // (the job draw whose descriptors heap_draw gives next)
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
    static std::atomic<int> render_told{0}, target_told{0};
    std::atomic<int>& budget = groups == 1u << G_RENDER ? render_told : groups & (1u << G_BLEND | 1u << G_CHMASK) ? target_told : mismatch_told;
    if (budget.fetch_add(1, std::memory_order_relaxed) >= (&budget == &render_told ? 5 : &budget == &target_told ? 20 : 40)) return;
    char line[600];
    int n = snprintf(line, sizeof line, "vk: eng jobs: MISMATCH job %u (view %#llx) draw %zu of %zu (op %d):", r.serial,
                     (unsigned long long)(r.j ? r.j->view : 0), i, r.j ? r.j->trace.size() : 0, (int)a.op);
    for (int g = 0; g < G_N && n < 560; g++)
        if (groups >> g & 1)
            n += snprintf(line + n, sizeof line - n, " %s (%016llx job, %016llx replay);", kGroup[g], (unsigned long long)a.h[g],
                          (unsigned long long)b.h[g]);
    if (groups & (1u << G_BLEND | 1u << G_CHMASK))
        snprintf(line + n, sizeof line - n, " targets: job %d colour (%d, depth %d), replay %d (%d, depth %d)", a.ncolor, (int)a.color0,
                 (int)a.depth, b.ncolor, (int)b.color0, (int)b.depth);
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

uint32_t eng_draw_sig(int op, const void* payload, size_t n) {
    size_t m = op == NOP_DRAW ? sizeof(nvn_p_draw) : op == NOP_DRAW_IDX ? sizeof(nvn_p_draw_idx) : op == NOP_DRAW_MESH ? sizeof(nvn_p_draw_mesh)
             : op == NOP_DRAW_INDIRECT ? sizeof(nvn_p_draw_indirect) : 0;
    m = std::min(m, n);
    uint64_t h = fnv(kFnv0 ^ (uint64_t)op, payload, m);
    return (uint32_t)(h ^ (h >> 32));
}
int eng_job_why(uint64_t slot_handle) {
    if (!sw().on) return -1;
    uint32_t serial;
    {
        std::lock_guard lk(handle_mu);
        const auto it = by_handle.find(slot_handle);
        if (it == by_handle.end()) return -1;
        serial = it->second;
    }
    const Job& j = ring()[serial % kRing];
    if (j.serial.load(std::memory_order_acquire) != serial) return -1;
    return (int)j.why;
}
const char* eng_job_why_name(int why) { return why >= 0 && why < WHY_N ? kWhy[why] : "no job"; }

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
    r.desc_i = (int64_t)i;
    if (p.fallback) {   // (R1: a job draw's program the replay picked is the task/mesh compute fallback: never published)
        std::unique_lock lk(pub_mu);
        fallback_keys.insert(pick_key(s, ct, p.mesh));
    }
    if (so && !p.fallback) {   // (for the job threads: the program these draws pick, by what program_for picks by)
        const uint64_t k = pick_key(s, ct, p.mesh);
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
        t.ncolor = (int8_t)at.n, t.color0 = at.color[0], t.depth = at.depth;
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

bool eng_job_stream_alloc(uint32_t n, uint64_t align, VkBuffer* buf, uint64_t* off, uint8_t** map, VkDeviceAddress* addr) {
    // (one host-visible buffer for every job thread, taken in turn and wrapping: a block is reused ~64 MB of banks later, dozens
    // of frames after the GPU read it)
    struct Stream { VkBuffer b = VK_NULL_HANDLE; VmaAllocation a = nullptr; uint8_t* map = nullptr; VkDeviceAddress addr = 0; uint64_t size = 0; };
    static const Stream st = [] {
        Stream x;
        void* m = nullptr;
        x.size = 64ull << 20;
        if (!create_buffer(x.size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           true, x.b, x.a, &m, true) || !m) {
            logf("vk: eng jobs: the job stream was not made");
            return Stream{};
        }
        x.map = (uint8_t*)m;
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = x.b;
        x.addr = vkGetBufferDeviceAddress(dev.device, &da);
        return x;
    }();
    static std::atomic<uint64_t> at{0};
    if (!st.map || !n || n > (1u << 20)) return false;
    align = std::max<uint64_t>(align, 16);
    uint64_t was = at.load(std::memory_order_relaxed), from;
    do {
        from = (was + align - 1) & ~(align - 1);
        if (from + n > st.size) from = 0;
    } while (!at.compare_exchange_weak(was, from + n, std::memory_order_relaxed));
    *buf = st.b, *off = from, *map = st.map + from, *addr = st.addr + from;
    return true;
}

/// The words of a stage's driver bank (c0 / c2) or c1 the program reads (its handles, its storage tables; c1 and others: all of it)
bool read_same(const Program& p, uint32_t stage, uint32_t index, const uint8_t* a, const uint8_t* b, uint32_t n) {
    if (stage >= p.stages.size() || (index != 0 && index != 2)) return !std::memcmp(a, b, n);
    const Shader::Info& info = p.stages[stage].info;
    const auto word = [&](uint32_t o, uint32_t len) { return o + len > n || !std::memcmp(a + o, b + o, len); };
    for (const auto& x : info.texture_descriptors)
        for (uint32_t k = 0; k < x.count; k++) {
            const uint32_t step = 1u << x.size_shift;
            if (x.handle_program == ~0U && x.cbuf_index == index && !word(x.cbuf_offset + k * step, 4)) return false;
            if (x.has_secondary && x.secondary_cbuf_index == index && !word(x.secondary_cbuf_offset + k * step, 4)) return false;
        }
    for (const auto& x : info.image_descriptors)
        for (uint32_t k = 0; k < x.count; k++)
            if (x.handle_program == ~0U && x.cbuf_index == index && !word(x.cbuf_offset + k * (1u << x.size_shift), 4)) return false;
    for (const auto& x : info.storage_buffers_descriptors)
        for (uint32_t k = 0; k < x.count; k++)
            if (x.cbuf_index == index && !word(x.cbuf_offset + k * 16, 16)) return false;
    return true;
}
/// An Indirect bank (HeapProgram::banks[k]): the handle words its textures read
bool bank_read_same(const Program& p, size_t k, const uint8_t* a, const uint8_t* b, uint32_t n) {
    const HeapProgram* hp = desc_heap_program(p);
    if (!hp || k >= hp->banks.size()) return !std::memcmp(a, b, n);
    for (const HeapTex& t : hp->tex)
        if (t.kind == HeapTex::Indirect && t.bank == k && t.cbuf_offset + 4 <= n && std::memcmp(a + t.cbuf_offset, b + t.cbuf_offset, 4))
            return false;
    return true;
}
void eng_job_replay_desc(const EngDrawDesc& d, const NvnState& s) {
    RState& r = rs();
    if (!r.active || !r.j || r.desc_i < 0 || !sw().check) return;
    const size_t i = (size_t)r.desc_i;
    r.desc_i = -1;
    if (i >= r.j->descs.size()) return;
    const EngDrawDesc& w = r.j->descs[i];
    if (w.why != EDW_OK) return add(C_DESC_NONE);
    add(C_DESC_CHECKED);
    const char* what = nullptr;
    size_t at = 0;
    if (w.els.size() != d.els.size()) what = "buffer element count";
    for (size_t k = 0; !what && k < w.els.size(); k++) {
        const EngDescEl &a = w.els[k], &b = d.els[k];
        at = k;
        if (a.kind != b.kind) what = "buffer element kind";
        else if (a.kind == 1 && (a.buffer != b.buffer || a.offset != b.offset || a.range != b.range)) what = "buffer range";
        else if (a.kind == 2 && (a.n != b.n || !read_same(*w.prog, a.stage, a.index, a.bytes, b.bytes, a.n))) what = "uploaded bank bytes";
    }
    if (!what && w.handles != d.handles) what = "pushed handles";
    if (!what && w.banks.size() != d.banks.size()) what = "bank count";
    for (size_t k = 0; !what && k < w.banks.size(); k++)
        if (w.banks[k].second != d.banks[k].second || !d.banks[k].first ||
            !bank_read_same(*w.prog, k, w.banks[k].first, d.banks[k].first, w.banks[k].second))
            what = "indirect bank bytes", at = k;
    if (!what && (w.push_bytes != d.push_bytes || std::memcmp(w.push, d.push, desc_heap_push_base()))) what = "classic push words";
    if (!what) return;
    add(C_DESC_MISMATCH);
    static std::atomic<int> told{0};
    if (told++ < 30) {
        char extra[200] = "";
        if (!std::strcmp(what, "uploaded bank bytes") || !std::strcmp(what, "indirect bank bytes")) {
            const EngDescEl* a = !std::strcmp(what, "uploaded bank bytes") ? &w.els[at] : nullptr;
            const uint8_t* pa = a ? a->bytes : w.banks[at].first;
            const uint8_t* pb = a ? d.els[at].bytes : d.banks[at].first;
            const uint32_t n = a ? a->n : w.banks[at].second;
            for (uint32_t o = 0; o + 4 <= n; o += 4)
                if (std::memcmp(pa + o, pb + o, 4)) {
                    uint32_t x, y;
                    std::memcpy(&x, pa + o, 4), std::memcpy(&y, pb + o, 4);
                    const uint32_t st = a && a->stage < w.prog->stages.size() ? (uint32_t)w.prog->stages[a->stage].nvn_stage : 0;
                    const uint32_t now = a ? driver_bank_word(s, a->index, (int)st, o) : 0;
                    snprintf(extra, sizeof extra, ": first at +%#x, %#x job / %#x replay (bank c%u stage %u; the replay's state now %#x)", o, x,
                             y, a ? a->index : 0, st, now);
                    break;
                }
        }
        logf("vk: eng jobs: DESC MISMATCH job %u (view %#llx) draw %zu: %s (element %zu)%s", r.serial, (unsigned long long)r.j->view, i,
             what, at, extra);
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
    if (sw().split) {   // (HOS_ENG_JOBS_SPLIT: jobs split around the draws the replay keeps, those draws by why)
        static int64_t l_sj, l_ss, l_dw[WHY_N];
        char dl[400];
        int m = 0;
        dl[0] = 0;
        for (int i = 0; i < WHY_N && m < 380; i++) {
            const int64_t v = why_draw_n[i].load(std::memory_order_relaxed);
            if (v != l_dw[i]) m += snprintf(dl + m, sizeof dl - m, " %s %.1f,", kWhy[i], (v - l_dw[i]) / frames);
            l_dw[i] = v;
        }
        const int64_t sj = split_jobs.load(std::memory_order_relaxed), ss = split_segs.load(std::memory_order_relaxed);
        logf("vk: a frame: eng jobs split: %.1f jobs in %.1f runs; draws left to the replay:%s", (sj - l_sj) / frames, (ss - l_ss) / frames,
             dl[0] ? dl : " none");
        l_sj = sj, l_ss = ss;
    }
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
    {
        static int64_t last_d[EDW_N];
        char dl[600];
        n = 0;
        dl[0] = 0;
        for (int i = 0; i < EDW_N && n < 580; i++) {
            const int64_t v = desc_why[i].load(std::memory_order_relaxed);
            if (v != last_d[i]) n += snprintf(dl + n, sizeof dl - n, " %s %.1f,", kDescWhy[i], (v - last_d[i]) / frames);
            last_d[i] = v;
        }
        logf("vk: eng jobs: draws whose descriptors were not made, a frame:%s; patched at the submit %.1f words a frame (%.2f jobs not "
             "found); draws not taken: %.1f indirect, %.1f mesh with a first or indirect, %.1f quads or other primitives; %.1f mesh draws recorded; "
             "%.1f with a vertex stream the job does not bind; %.1f indirect draws recorded",
             dl[0] ? dl : " none", (c[C_PATCHED] - last[C_PATCHED]) / frames, (c[C_PATCH_NO_JOB] - last[C_PATCH_NO_JOB]) / frames,
             (c[C_KIND_INDIRECT] - last[C_KIND_INDIRECT]) / frames, (c[C_KIND_MESH_OTHER] - last[C_KIND_MESH_OTHER]) / frames,
             (c[C_KIND_PRIM] - last[C_KIND_PRIM]) / frames, (c[C_MESH_REC] - last[C_MESH_REC]) / frames,
             (c[C_VTX_UNSET] - last[C_VTX_UNSET]) / frames, (c[C_IND_REC] - last[C_IND_REC]) / frames);
        if (sw().check)
            logf("vk: eng jobs check: descriptors: %.1f draws a frame compared with the replay's, %.2f differ (%lld so far), %.1f not made "
                 "on the job thread", (c[C_DESC_CHECKED] - last[C_DESC_CHECKED]) / frames, (c[C_DESC_MISMATCH] - last[C_DESC_MISMATCH]) / frames,
                 (long long)c[C_DESC_MISMATCH], (c[C_DESC_NONE] - last[C_DESC_NONE]) / frames);
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
    w.tex.clear(), w.deferred.clear(), w.trace.clear(), w.ubo.clear(), w.pre.clear(), w.waits.clear(), w.pre_barrier = false, w.npkt = 0;
    std::memset(w.refs, 0, sizeof w.refs);
    w.nbinds = 0, w.patches.clear(), w.descs.clear();
    std::memset(w.ops, 0, sizeof w.ops);
    Rec r;
    r.split = sw().split, r.serial = ji->serial, r.view = ji->view;
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
    }
    VkCommandBufferInheritanceRenderingInfo ri{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (r.cb) {   // (the begin infos live through the walk: a split job's later runs begin with them)
        ri.colorAttachmentCount = fmt.ncolor;
        ri.pColorAttachmentFormats = fmt.color;
        ri.depthAttachmentFormat = fmt.depth, ri.stencilAttachmentFormat = fmt.stencil;
        ri.rasterizationSamples = r.at.samples;
        ii.pNext = &ri;
        bi.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        bi.pInheritanceInfo = &ii;
        r.bi = &bi;
        if (vkBeginCommandBuffer(r.cb, &bi) != VK_SUCCESS) r.cb = VK_NULL_HANDLE, r.why = WHY_POOL;
    }
    walk_records(w, &r, rec, rec + bytes);
    if (r.why == WHY_OK && r.gated) r.why = WHY_GATED;   // (E5's conditional rendering, not a guess made at record time)
    if (r.cb) vkEndCommandBuffer(r.cb);
    if (r.split && (r.ninline || r.cut)) {   // (split: the last run; none recorded at all: the first refused draw's why, as without the split)
        if (r.cb && r.draw_i > r.seg_first) r.segs.push_back({r.seg_first, r.draw_i - r.seg_first, r.cb, r.ndraws - r.seg_ndraws});
        r.cb = r.segs.empty() ? VK_NULL_HANDLE : r.segs[0].cb;
        if (r.why == WHY_OK && r.segs.empty()) r.why = r.first_dwhy;
        if (r.why == WHY_OK) split_jobs.fetch_add(1, std::memory_order_relaxed), split_segs.fetch_add((int64_t)r.segs.size(), std::memory_order_relaxed);
    }
    uniq(w.tex), uniq(w.deferred);
    // (published in the ring before the handle: the replay meets the job's records after this returns)
    Job& j = ring()[ji->serial % kRing];
    j.serial.store(0, std::memory_order_release);
    j.handle = handle, j.view = ji->view;
    std::memcpy(j.rect, ji->rect, sizeof j.rect);
    j.why = r.why, j.bytes = (uint32_t)bytes, j.npkt = w.npkt;
    std::memcpy(j.ops, w.ops, sizeof j.ops);
    j.tex.swap(w.tex), j.deferred.swap(w.deferred), j.trace.swap(w.trace);
    j.patches.swap(w.patches), j.descs.swap(w.descs);
    if (!r.split || !(r.ninline || r.cut)) r.segs.clear();
    j.segs.swap(r.segs);
    j.dsig.swap(r.dsig);
    j.ubo.swap(w.ubo);
    j.pre.swap(w.pre), j.waits.swap(w.waits), j.pre_barrier = w.pre_barrier;
    j.used_value.store(0, std::memory_order_relaxed);
    j.replayed.store(false, std::memory_order_relaxed);
    j.pub.view_key = ji->view;
    j.pub.fmt = fmt;
    j.pub.cb = r.why == WHY_OK ? r.cb : VK_NULL_HANDLE;
    j.pub.ndraws = r.ndraws;
    j.pub.tex = j.tex.data(), j.pub.ntex = (uint32_t)j.tex.size();
    j.pub.deferred = j.deferred.data(), j.pub.ndeferred = (uint32_t)j.deferred.size();
    j.pub.serial = ji->serial;
    j.pub.segs = j.segs.data(), j.pub.nsegs = (uint32_t)j.segs.size(), j.pub.ndrawops = r.draw_i;
    j.pub.dsig = j.dsig.data();
    j.pub.ubo = j.ubo.data(), j.pub.nubo = (uint32_t)(j.ubo.size() / 2);
    j.pub.pre = j.pre.data(), j.pub.npre = (uint32_t)j.pre.size();
    j.pub.waits = j.waits.data(), j.pub.nwaits = (uint32_t)j.waits.size(), j.pub.pre_barrier = j.pre_barrier;
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

/// (the host, at the submit, host/nvn_native.c resolve_deferred, on the submitting thread) a job's deferred handle bind number bind
/// (1 first) as the game's array holds it now: the bank words made from it rewritten
extern "C" void mkwvk_eng_job_deferred(uint32_t serial, uint32_t bind, const uint64_t* h, int n) {
    using namespace mkwvk;
    if (!sw().on || !serial || !bind) return;
    Job& j = ring()[serial % kRing];
    if (j.serial.load(std::memory_order_acquire) != serial) return add(C_PATCH_NO_JOB);
    int64_t done = 0;
    for (const EngDescPatch& pt : j.patches)
        if (pt.bind == bind - 1 && (int)pt.index < n) *pt.word = (uint32_t)h[pt.index], done++;
    add(C_PATCHED, done);
}
