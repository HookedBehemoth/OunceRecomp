// (native-vk) Graphics programs as VK_EXT_shader_object stages (vk_shader_object.h), RX_VK_SHADER_OBJECTS=1.
//
// A pipeline is a program variant times the state a draw bakes into it (vertex layout, target formats, samples): OoT's first
// gameplay in Kokiri made ~740 of them from ~420 programs, each a driver compile of ~41 ms on RADV, and the draws waited for them
// (docs/native-vk.md, work/assets/native/shader-stream.md). A shader object is a stage alone, all its state dynamic: one compile a
// stage of a program variant, and the driver's binary of it (vkGetShaderBinaryDataEXT) makes it again in ~0.02 ms.
//
// - Identity: a stage's binary is keyed by its SPIR-V's hash, its stage and next stage, the program's set layout (bindings, push
//   descriptors), the push range and the subgroup pin: what the driver compiles from. The file's head names the driver
//   (shaderBinaryUUID, shaderBinaryVersion, pipelineCacheUUID, device): another driver's binaries are let go whole.
// - hookshot_shader_objects.bin beside the other caches: records {u32 tag, u32 size, u64 key, bytes}; tag 1 a stage's binary,
//   tag 2 a program variant made (its stage code keys, key, variant): what the prefetch makes when the game sets those stages.
// - The prefetch: nvnProgramSetShaders[Ext] hands the program over (so_program_set); a thread of its own reads its stages' keys
//   and makes the shader objects of every variant of them made in an earlier run, from the program cache and the binaries. The
//   game sets its programs while it loads, seconds before their first draws (shader-stream.md 1.2, 1.6).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vk_paths.h"
#include "vk_shader_object.h"
#include "vk_jobs.h"
#include "vk_draw.h"   // (ub_thread_off)

namespace mkwvk {
namespace {

inline uint64_t so_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

// ---- the modules' SPIR-V (make_module), by module ----
struct ModSrc {
    uint64_t hash = 0;
    std::vector<uint32_t> words;   // (let go once the stage's shader object is made)
};
std::mutex mod_mu;
std::unordered_map<VkShaderModule, ModSrc> mod_src;

// ---- the binary cache ----
constexpr char kMagic[8] = {'H', 'K', 'S', 'O', 'B', 'J', '0', '1'};
constexpr uint32_t kTagBinary = 1, kTagProgram = 2, kTagCompute = 3;   // (3: a compute program variant made, a ProgNote)
struct ProgNote { uint64_t codes[4], key, variant; };
static_assert(sizeof(ProgNote) == 48, "a file format");
std::mutex cache_mu;
bool cache_ready = false;
FILE* cache_out = nullptr;
std::unordered_map<uint64_t, std::vector<uint8_t>> binaries;
std::unordered_map<uint64_t, std::vector<ProgNote>> notes_by_codes;   // (by the hash of the codes)
std::unordered_set<uint64_t> noted;                                   // (key ^ variant mixes noted)
std::vector<ProgNote> compute_notes;   // (the compute program variants made in earlier runs: the prefetch makes them all at its start)

std::atomic<int64_t> st_from_binary{0}, st_compiled{0}, st_compile_us{0}, st_prefetched{0}, st_prefetch_us{0};

uint64_t codes_hash(const uint64_t codes[4]) {
    uint64_t h = 0x50524F47ull;
    for (int i = 0; i < 4; i++) h = so_mix(h, codes[i]);
    return h;
}

struct Head {
    char magic[8];
    uint32_t version, binary_version, vendor, device, driver, pad;
    uint8_t binary_uuid[VK_UUID_SIZE], cache_uuid[VK_UUID_SIZE];
};
Head head_now() {
    Head h{};
    std::memcpy(h.magic, kMagic, 8);
    h.version = 1, h.binary_version = dev.shader_binary_version;
    h.vendor = dev.props.vendorID, h.device = dev.props.deviceID, h.driver = dev.props.driverVersion;
    std::memcpy(h.binary_uuid, dev.shader_binary_uuid, VK_UUID_SIZE);
    std::memcpy(h.cache_uuid, dev.props.pipelineCacheUUID, VK_UUID_SIZE);
    return h;
}

/// (cache_mu held) the file read, once
void cache_init_locked() {
    if (cache_ready) return;
    cache_ready = true;
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string path(exe, n);
    path = path.substr(0, path.find_last_of("\\/") + 1) + "hookshot_shader_objects.bin";
    if (getenv("RX_VK_SO_NO_CACHE")) {
        logf("vk: shader objects: no binary cache (RX_VK_SO_NO_CACHE)");
        return;
    }
    const Head want = head_now();
    std::vector<uint8_t> data;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        _fseeki64(f, 0, SEEK_END);
        const int64_t sz = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        if (sz >= (int64_t)sizeof(Head)) {
            data.resize((size_t)sz);
            if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        }
        fclose(f);
    }
    size_t good = 0, nbin = 0, nprog = 0, ncomp = 0;
    if (data.size() >= sizeof(Head) && !std::memcmp(data.data(), &want, sizeof want)) {
        good = sizeof(Head);
        while (good + 16 <= data.size()) {
            uint32_t tag, size;
            uint64_t key;
            std::memcpy(&tag, data.data() + good, 4), std::memcpy(&size, data.data() + good + 4, 4), std::memcpy(&key, data.data() + good + 8, 8);
            if (size > data.size() - good - 16) break;   // (a record cut short: the run ended while writing it)
            const uint8_t* b = data.data() + good + 16;
            if (tag == kTagBinary) {
                binaries[key].assign(b, b + size), nbin++;
            } else if ((tag == kTagProgram || tag == kTagCompute) && size == sizeof(ProgNote)) {
                ProgNote pn;
                std::memcpy(&pn, b, sizeof pn);
                if (noted.insert(so_mix(pn.key, pn.variant)).second) {
                    if (tag == kTagCompute) compute_notes.push_back(pn), ncomp++;
                    else notes_by_codes[codes_hash(pn.codes)].push_back(pn), nprog++;
                }
            }
            good += 16 + size;
        }
    }
    if (good) {
        if (good != data.size())
            if (FILE* f = fopen(path.c_str(), "r+b")) _chsize_s(_fileno(f), (int64_t)good), fclose(f);
        cache_out = fopen(path.c_str(), "ab");
    } else {
        cache_out = fopen(path.c_str(), "wb");
        if (cache_out) fwrite(&want, sizeof want, 1, cache_out), fflush(cache_out);
    }
    logf("vk: shader objects: binary cache %s: %zu binaries, %zu programs, %zu compute programs (%zu KB)%s", path.c_str(), nbin, nprog,
         ncomp, good >> 10,
         data.size() && !good ? " (another driver's: started again)" : "");
}
void cache_put_locked(uint32_t tag, uint64_t key, const void* d, uint32_t size) {
    if (!cache_out) return;
    fwrite(&tag, 4, 1, cache_out), fwrite(&size, 4, 1, cache_out), fwrite(&key, 8, 1, cache_out);
    fwrite(d, 1, size, cache_out);
    fflush(cache_out);
}

/// What the driver compiles a program's stage from, but its SPIR-V: the program's set layout as ensure_layout makes it
uint64_t layout_sig(const Program& p, uint32_t push_size) {
    uint64_t h = so_mix(0x4C41594Full, (uint64_t)p.push << 32 | push_size);
    h = so_mix(h, p.push_stages);
    for (const Stage& st : p.stages) {
        auto add = [&](uint64_t type, const auto& descriptors) {
            for (const auto& d : descriptors) h = so_mix(h, type << 40 | (uint64_t)st.vk_stage << 32 | d.count);
        };
        add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, st.info.constant_buffer_descriptors);
        add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, st.info.storage_buffers_descriptors);
        add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, st.info.texture_buffer_descriptors);
        add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, st.info.image_buffer_descriptors);
        add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, st.info.texture_descriptors);
        add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, st.info.image_descriptors);
    }
    return h;
}

/// A stage the draw binds: vertex and fragment; task and mesh of a native mesh program (a fallback program's are compute dispatches)
bool graphics_stage(const Program& p, const Stage& st) {
    if (st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT || st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT) return true;
    return p.mesh && !p.fallback && (st.vk_stage == VK_SHADER_STAGE_TASK_BIT_EXT || st.vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT);
}
VkShaderStageFlags next_stage(VkShaderStageFlagBits s) {
    return s == VK_SHADER_STAGE_VERTEX_BIT || s == VK_SHADER_STAGE_MESH_BIT_EXT ? VK_SHADER_STAGE_FRAGMENT_BIT
         : s == VK_SHADER_STAGE_TASK_BIT_EXT ? VK_SHADER_STAGE_MESH_BIT_EXT : 0;
}
/// A stage made a shader object: the graphics ones, and the compute ones of compute programs and of the mesh fallback (when on)
/// (diagnosis, the fallback's hang) RX_VK_SO_FALLBACK_KEYS=<hex key prefixes,...>: only those programs' fallback dispatches as shader
/// objects (the others: pipelines); RX_VK_SO_FALLBACK_STAGE=task|mesh: only that stage of them. For bisecting which one hangs
bool fallback_stage_wanted(const Program& p, uint32_t si) {
    static const char* keys = getenv("RX_VK_SO_FALLBACK_KEYS");
    static const char* only = getenv("RX_VK_SO_FALLBACK_STAGE");
    if (keys && *keys) {
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)p.key);
        bool listed = false;
        for (const char* q = keys; *q && !listed;) {
            const char* e = strchr(q, ',');
            const size_t len = e ? (size_t)(e - q) : strlen(q);
            listed = len && !strncmp(hex, q, len);
            q = e ? e + 1 : q + len;
        }
        if (!listed) return false;
    }
    if (only && *only) {
        uint32_t before = 0;   // (compute stages before this one: a task program's first is its task stage)
        for (uint32_t i = 0; i < si; i++) before += p.stages[i].vk_stage == VK_SHADER_STAGE_COMPUTE_BIT;
        const bool task = p.fallback_task && before == 0;
        if (!strcmp(only, "task") != task) return false;
    }
    return true;
}
bool so_stage(const Program& p, const Stage& st, uint32_t si) {
    if (graphics_stage(p, st)) return true;
    if (st.vk_stage != VK_SHADER_STAGE_COMPUTE_BIT) return false;
    return (p.compute && so_compute_on()) || (p.fallback && so_fallback_on() && fallback_stage_wanted(p, si));
}
/// A compute program's stage asks for full subgroups, as pin_subgroup (vk_draw.cpp) asks its pipeline (the fallback's: never)
bool full_subgroups(const Program& p, const Stage& st) {
    return p.compute && st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT && (dev.sg32_stages & st.vk_stage) && p.workgroup[0] &&
           !(p.workgroup[0] % 32) && dev.vk13.computeFullSubgroups;
}
bool has_task(const Program& p) {
    for (const Stage& st : p.stages)
        if (st.vk_stage == VK_SHADER_STAGE_TASK_BIT_EXT) return true;
    return false;
}

/// The shader objects of p into so. compile: SPIR-V compiled where no binary is kept; else false (nothing made) when one is missing
bool so_make(SoProg& so, const Program& p, uint32_t push_size, bool compile) {
    struct One {
        const Stage* st;
        uint32_t index;   // (in p.stages: where a compute stage's object goes)
        uint64_t key;
        const std::vector<uint8_t>* bin;
        VkShaderEXT out;
    };
    One ones[8];
    int n = 0;
    const uint64_t lsig = layout_sig(p, push_size);
    {
        std::lock_guard lk(cache_mu);
        cache_init_locked();
        for (uint32_t si = 0; si < p.stages.size(); si++) {
            const Stage& st = p.stages[si];
            if (!so_stage(p, st, si) || n == 8) continue;
            uint64_t sh = 0;
            {
                std::lock_guard ml(mod_mu);
                const auto it = mod_src.find(st.module);
                if (it == mod_src.end()) return so.failed = true, true;   // (a module made before shader objects: not ours)
                sh = it->second.hash;
            }
            const VkShaderStageFlags next = next_stage(st.vk_stage);
            const uint64_t flags = (st.vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT && !has_task(p) ? 1 : 0) | (full_subgroups(p, st) ? 2 : 0);
            const uint64_t key = so_mix(so_mix(so_mix(sh, (uint64_t)st.vk_stage << 32 | next), lsig),
                                        (dev.sg32_stages & st.vk_stage ? 32 : 0) | flags << 8);
            const auto b = binaries.find(key);
            ones[n++] = {&st, si, key, b != binaries.end() ? &b->second : nullptr, VK_NULL_HANDLE};
            if (!compile && b == binaries.end()) return false;
        }
    }
    if (!n) return so.failed = true, true;
    static const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo size32 = [] {
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo r{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        r.requiredSubgroupSize = 32;
        return r;
    }();
    const VkPushConstantRange range{p.push_stages, 0, push_size};
    for (int i = 0; i < n; i++) {
        One& o = ones[i];
        VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
        ci.stage = o.st->vk_stage;
        ci.nextStage = next_stage(o.st->vk_stage);
        if (o.st->vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT && !has_task(p)) ci.flags |= VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
        if (full_subgroups(p, *o.st)) ci.flags |= VK_SHADER_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
        ci.pName = "main";
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &p.set_layout;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &range;
        if (dev.sg32_stages & o.st->vk_stage) ci.pNext = &size32;
        VkResult r = VK_ERROR_UNKNOWN;
        if (o.bin) {
            ci.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
            ci.codeSize = o.bin->size();
            ci.pCode = o.bin->data();
            r = vkCreateShadersEXT(dev.device, 1, &ci, nullptr, &o.out);
            if (r == VK_SUCCESS) {
                st_from_binary++;
                continue;
            }
            o.out = VK_NULL_HANDLE;
            if (!compile) {   // (the binary is refused: compiled on a worker)
                for (int k = 0; k < i; k++) vkDestroyShaderEXT(dev.device, ones[k].out, nullptr);
                return false;
            }
        }
        std::vector<uint32_t> words;
        {
            std::lock_guard ml(mod_mu);
            const auto it = mod_src.find(o.st->module);
            if (it != mod_src.end()) words = it->second.words;
        }
        // (a mesh fallback's compute stage not made: its dispatches go on with the pipelines, the draw keeps its shader objects)
        const bool fallback_cs = p.fallback && o.st->vk_stage == VK_SHADER_STAGE_COMPUTE_BIT;
        if (words.empty()) {
            static std::atomic<int> told{0};
            if (told++ < 10) logf("vk: shader objects: %016llx's SPIR-V is gone and no binary is kept: drawn without", (unsigned long long)p.key);
            if (fallback_cs) {
                so.cs_failed = true;
                continue;
            }
            so.failed = true;
            break;
        }
        ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        ci.codeSize = words.size() * 4;
        ci.pCode = words.data();
        const auto t0 = std::chrono::steady_clock::now();
        r = vkCreateShadersEXT(dev.device, 1, &ci, nullptr, &o.out);
        st_compile_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (r != VK_SUCCESS) {
            static std::atomic<int> told{0};
            if (told++ < 20)
                logf("vk: shader objects: stage %#x of %016llx not made (%d)%s", (unsigned)o.st->vk_stage, (unsigned long long)p.key, (int)r,
                     o.st->vk_stage == VK_SHADER_STAGE_COMPUTE_BIT ? ": its compute pipeline instead" : "");
            o.out = VK_NULL_HANDLE;
            if (fallback_cs) {
                so.cs_failed = true;
                continue;
            }
            so.failed = true;
            break;
        }
        st_compiled++;
        {   // (which stages were compiled, with what the binary key is made of: a key that changes from run to run misses every time)
            static std::atomic<int> told{0};
            if (told++ < 300) {
                uint64_t sh = 0;
                {
                    std::lock_guard ml(mod_mu);
                    if (const auto it = mod_src.find(o.st->module); it != mod_src.end()) sh = it->second.hash;
                }
                logf("vk: shader objects: compiled stage %#x of %016llx variant %016llx%s: SPIR-V %016llx (%zu words), layout %016llx, key %016llx",
                     (unsigned)o.st->vk_stage, (unsigned long long)p.key, (unsigned long long)p.variant, p.fallback ? " (fallback)" : "",
                     (unsigned long long)sh, words.size(), (unsigned long long)layout_sig(p, push_size), (unsigned long long)o.key);
            }
        }
        size_t sz = 0;
        if (vkGetShaderBinaryDataEXT(dev.device, o.out, &sz, nullptr) == VK_SUCCESS && sz && sz < (64u << 20)) {
            std::vector<uint8_t> bin(sz);
            if (vkGetShaderBinaryDataEXT(dev.device, o.out, &sz, bin.data()) == VK_SUCCESS) {
                bin.resize(sz);
                std::lock_guard lk(cache_mu);
                if (binaries.find(o.key) == binaries.end()) {
                    cache_put_locked(kTagBinary, o.key, bin.data(), (uint32_t)bin.size());
                    binaries.emplace(o.key, std::move(bin));
                }
            }
        }
    }
    if (so.failed) {
        for (int i = 0; i < n; i++)
            if (ones[i].out) vkDestroyShaderEXT(dev.device, ones[i].out, nullptr);
        return true;
    }
    if (so.cs_failed)   // (the fallback's dispatches all take their pipelines, not some of each)
        for (int i = 0; i < n; i++)
            if (ones[i].st->vk_stage == VK_SHADER_STAGE_COMPUTE_BIT && ones[i].out)
                vkDestroyShaderEXT(dev.device, ones[i].out, nullptr), ones[i].out = VK_NULL_HANDLE;
    for (int i = 0; i < n; i++) {
        const VkShaderStageFlagBits st = ones[i].st->vk_stage;
        if (st == VK_SHADER_STAGE_COMPUTE_BIT) {
            if (ones[i].index < 8) so.cs[ones[i].index] = ones[i].out;
            continue;
        }
        (st == VK_SHADER_STAGE_VERTEX_BIT ? so.vs : st == VK_SHADER_STAGE_FRAGMENT_BIT ? so.fs : st == VK_SHADER_STAGE_TASK_BIT_EXT ? so.ts : so.ms) =
            ones[i].out;
    }
    {   // (made: the SPIR-V copies go; the hashes stay for the binary keys)
        std::lock_guard ml(mod_mu);
        for (int i = 0; i < n; i++)
            if (ones[i].out)
                if (auto it = mod_src.find(ones[i].st->module); it != mod_src.end()) std::vector<uint32_t>().swap(it->second.words);
    }
    if (p.fallback && so_fallback_on()) {   // (which fallback programs' dispatches are shader objects: the hang's bisection reads them)
        static std::atomic<int> told{0};
        int ncs = 0;
        for (int i = 0; i < n; i++) ncs += ones[i].st->vk_stage == VK_SHADER_STAGE_COMPUTE_BIT && ones[i].out;
        if (told++ < 400)
            logf("vk: shader objects: fallback %016llx (%s): %d compute stage(s) as shader objects", (unsigned long long)p.key,
                 p.fallback_task ? "task + mesh" : "mesh", ncs);
    }
    if (p.codes[0]) {   // (the variant noted for the prefetch of a later run)
        ProgNote pn{};
        std::memcpy(pn.codes, p.codes, sizeof pn.codes);
        pn.key = p.key, pn.variant = p.variant;
        std::lock_guard lk(cache_mu);
        if (noted.insert(so_mix(pn.key, pn.variant)).second) {
            if (p.compute) {
                compute_notes.push_back(pn);
                cache_put_locked(kTagCompute, pn.key, &pn, sizeof pn);
            } else {
                notes_by_codes[codes_hash(pn.codes)].push_back(pn);
                cache_put_locked(kTagProgram, pn.key, &pn, sizeof pn);
            }
        }
    }
    return true;
}

std::mutex tab_mu;
std::unordered_map<const Program*, std::unique_ptr<SoProg>> table;

bool same_program(const SoProg& so, const Program& p) {
    if (so.key != p.key || so.variant != p.variant || so.nmods != std::min<size_t>(p.stages.size(), 8)) return false;
    for (uint32_t i = 0; i < so.nmods; i++)
        if (so.mods[i] != p.stages[i].module) return false;
    return true;
}

SoProg* entry_of(const Program& p) {
    std::lock_guard lk(tab_mu);
    auto& e = table[&p];
    if (!e || !same_program(*e, p)) {   // (another Program at the address of one let go: its own; the old objects are left, in flight)
        e.reset(new SoProg);
        e->key = p.key, e->variant = p.variant;
        e->nmods = (uint32_t)std::min<size_t>(p.stages.size(), 8);
        for (uint32_t i = 0; i < e->nmods; i++) e->mods[i] = p.stages[i].module;
    }
    return e.get();
}

// ---- the prefetch ----
std::mutex pf_mu;
std::condition_variable pf_cv;
std::deque<nvn_prog> pf_queue;
bool pf_started = false;
std::unordered_set<uint64_t> pf_seen;   // (stage address pairs already looked at: 12,500 programs from 8,400 addresses)

void prefetch_one(const nvn_prog& prog) {
    uint64_t codes[4]{};
    bool mesh = false;
    if (!program_stage_keys(prog, codes, &mesh) || (mesh && !so_mesh_on())) return;
    std::vector<ProgNote> want;
    {
        std::lock_guard lk(cache_mu);
        cache_init_locked();
        const auto it = notes_by_codes.find(codes_hash(codes));
        if (it == notes_by_codes.end()) return;
        for (const ProgNote& pn : it->second)
            if (!std::memcmp(pn.codes, codes, sizeof codes)) want.push_back(pn);
    }
    for (const ProgNote& pn : want) {
        const auto t0 = std::chrono::steady_clock::now();
        const Program* p = program_load(pn.key, pn.variant, codes);
        if (!p || (p->mesh && !so_mesh_on()) || p->compute) continue;
        if (!p->layout) program_layout(const_cast<Program&>(*p));
        if (p->fallback && !so_fallback_on()) fallback_prefetch(const_cast<Program&>(*p));   // (its dispatches' pipelines)
        SoProg* so = entry_of(*p);
        int none = 0;
        if (!so->state.compare_exchange_strong(none, 1)) continue;   // (made, or being made by a draw's worker)
        so_make(*so, *p, so_push_size(), true);
        so->state.store(2, std::memory_order_release);
        so->ready.store(true, std::memory_order_release);
        jobs_notify();
        st_prefetched++;
        st_prefetch_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    }
}

/// (the prefetch thread's start) every compute program variant made in an earlier run, from the binaries: a compute program is set
/// by the game moments before its first dispatch (the culling programs: in the same frame), so it is made from the index, not at its
/// SetShaders. One whose binary is missing is left to its dispatch (a worker's compile, as the pipeline's)
void prefetch_computes() {
    if (!so_compute_on()) return;
    std::vector<ProgNote> want;
    {
        std::lock_guard lk(cache_mu);
        cache_init_locked();
        want = compute_notes;
    }
    const auto t_all = std::chrono::steady_clock::now();
    size_t made = 0, missing = 0;
    for (const ProgNote& pn : want) {
        const auto t0 = std::chrono::steady_clock::now();
        const Program* p = program_load(pn.key, pn.variant, pn.codes);
        if (!p || !p->compute) continue;
        if (!p->layout) program_layout(const_cast<Program&>(*p));
        SoProg* so = entry_of(*p);
        int none = 0;
        if (!so->state.compare_exchange_strong(none, 1)) continue;
        if (!so_make(*so, *p, so_push_size(), false)) {   // (a binary missing: its dispatch makes it)
            so->state.store(0, std::memory_order_release);
            missing++;
            continue;
        }
        so->state.store(2, std::memory_order_release);
        so->ready.store(true, std::memory_order_release);
        jobs_notify();
        made++;
        st_prefetched++;
        st_prefetch_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    }
    if (!want.empty())
        logf("vk: shader objects: %zu of %zu compute programs prefetched from binaries (%zu without one) in %.0f ms", made, want.size(), missing,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_all).count());
}

void prefetch_thread() {
    ub_thread_off();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    prefetch_computes();
    for (;;) {
        nvn_prog prog;
        {
            std::unique_lock lk(pf_mu);
            pf_cv.wait(lk, [] { return !pf_queue.empty(); });
            prog = pf_queue.front();
            pf_queue.pop_front();
        }
        prefetch_one(prog);
    }
}

} // namespace

bool so_on() {
    return dev.shader_object;
}
bool so_mesh_on() {
    static const bool on = !getenv("RX_VK_SO_MESH") || atoi(getenv("RX_VK_SO_MESH")) != 0;
    return so_on() && on;
}

bool so_compute_on() {
    static const bool on = [] {
        if (!so_on()) return false;
        if (getenv("RX_VK_SO_COMPUTE") && atoi(getenv("RX_VK_SO_COMPUTE")) == 0) return false;
        if (getenv("RX_VK_NOOPT")) {   // (a shader object has no DISABLE_OPTIMIZATION: the pipelines, which have)
            logf("vk: shader objects: compute programs keep their pipelines (RX_VK_NOOPT)");
            return false;
        }
        logf("vk: shader objects: compute programs as shader objects (RX_VK_SO_COMPUTE=0: pipelines)");
        return true;
    }();
    return on;
}
bool so_fallback_on() {
    static const bool on = [] {
        if (!so_compute_on() || !(dev.mesh_fallback || dev.task_mesh_fallback)) return false;
        // (optimized, which their pipelines are not under the task/mesh fallback (DISABLE_OPTIMIZATION): that was not what hung the
        // GPU when they were first made shader objects (10 of 10 runs, every mesh stage, one alone too), but RADV not sending the push
        // constants to a compute shader object bound after them: fallback_record pushes them again after each bind. Optimized
        // pipelines (RX_VK_FALLBACK_OPT=1) never hung. RX_VK_SO_FALLBACK=0: their pipelines)
        const char* e = getenv("RX_VK_SO_FALLBACK");
        const bool on = !e || atoi(e) != 0;
        logf("vk: shader objects: the mesh fallback's task and mesh dispatches %s",
             on ? "as shader objects (RX_VK_SO_FALLBACK=0: pipelines)" : "keep their pipelines (RX_VK_SO_FALLBACK=0)");
        return on;
    }();
    return on;
}
bool so_compute_made(const SoProg& so, const Program& p) {
    if (!so.ready.load(std::memory_order_acquire) || so.failed || so.cs_failed) return false;
    bool any = false;
    for (size_t i = 0; i < p.stages.size(); i++)
        if (p.stages[i].vk_stage == VK_SHADER_STAGE_COMPUTE_BIT) {
            if (i >= 8 || !so.cs[i]) return false;
            any = true;
        }
    return any;
}

void so_module_made(VkShaderModule m, const uint32_t* words, size_t n) {
    if (!so_on() || !m) return;
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    for (size_t i = 0; i < n; i++) h = (h ^ words[i]) * 0x100000001b3ull;
    std::lock_guard lk(mod_mu);
    ModSrc& s = mod_src[m];   // (a module made again at the address of one destroyed: this one)
    s.hash = h;
    s.words.assign(words, words + n);
}

SoProg* so_for(const Program& p, uint32_t push_size, bool* started) {
    if (started) *started = false;
    thread_local const Program* last_p = nullptr;
    thread_local SoProg* last_so = nullptr;
    SoProg* so;
    if (last_p == &p && last_so->key == p.key && last_so->variant == p.variant &&
        (p.stages.empty() || last_so->mods[0] == p.stages[0].module))
        so = last_so;
    else
        so = entry_of(p), last_p = &p, last_so = so;
    if (so->state.load(std::memory_order_acquire) != 0) return so;
    int none = 0;
    if (!so->state.compare_exchange_strong(none, 1)) return so;
    if (started) *started = true;
    if (so_make(*so, p, push_size, false)) {   // (every binary kept: made here, now)
        so->state.store(2, std::memory_order_release);
        so->ready.store(true, std::memory_order_release);
        return so;
    }
    const Program* pp = &p;
    jobs_push([so, pp, push_size] {
        so_make(*so, *pp, push_size, true);
        so->state.store(2, std::memory_order_release);
        so->ready.store(true, std::memory_order_release);
    }, p.compute || p.fallback, &so->ready);   // (urgent for compute, as their pipelines' builds: a dispatch is left out meanwhile)
    return so;
}

void so_stats(int64_t* from_binary, int64_t* compiled, double* compile_ms, int64_t* prefetched, double* prefetch_ms) {
    *from_binary = st_from_binary.load(), *compiled = st_compiled.load(), *compile_ms = st_compile_us.load() / 1000.0;
    *prefetched = st_prefetched.load(), *prefetch_ms = st_prefetch_us.load() / 1000.0;
}

void so_program_set(const nvn_prog* p) {
    static const bool off = getenv("RX_VK_SO_PREFETCH") && atoi(getenv("RX_VK_SO_PREFETCH")) == 0;
    if (!so_on() || off || !p || !p->nstage || p->nstage > 6) return;
    uint64_t sig = p->nstage;
    for (uint32_t i = 0; i < p->nstage; i++) sig = so_mix(so_mix(sig, p->sd[i].data), p->sd[i].control);
    std::lock_guard lk(pf_mu);
    // (the same stage addresses again: looked at already. A pool let go and another archive at the same address would be missed:
    // that program is then made at its first draw, as without the prefetch)
    if (!pf_seen.insert(sig).second) return;
    if (!pf_started) {
        pf_started = true;
        std::thread(prefetch_thread).detach();
    }
    pf_queue.push_back(*p);
    pf_cv.notify_one();
}

} // namespace mkwvk
