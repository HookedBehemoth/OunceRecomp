// Programs to shader objects (vk_shader.h). Lifted from gpu/legacy/vk/vk_shader.cpp (the code's reading and lowering, Env, the
// profile, the graphics translation, the key, the cache's record format) with its switches as the constants
// the frozen back end runs with (RX_VK_DESC_HEAP_BDA=2: storage buffers by address, wide), and from vk_shader_object.cpp
// so_make (the shader objects). What differs (docs/direct/shader-contract.md, "What a draw provides"): uniform buffers are
// read through device addresses (an address table in the frame arena, each stage's part's address pushed), not ring
// descriptors; textures, images and samplers are the pools' set's entries (vk_pool.h), written at registration and indexed
// by the handle the shader reads; a stage is translated from its code alone (no draw state reaches Env), so it can be
// translated before its first draw.
// The caches beside the executable (docs/direct/shader-contract.md, "The caches"):
// - ounce_renderer_programs.bin: translated variants by code pair. Stamped by the translation's code only
//   (OUNCE_RENDERER_SHADER_STAMP, renderer/meson/shader_stamp.py: the translator, the compiler and flags that build it,
//   and the regions marked "stamp graphics" and "stamp shared" here, vk_spv_late.*, vk_state.h), the record format
//   (kDiskFormat), the device's answers and the switches that change the translation; a file of another stamp is made
//   again. Edits outside the marked regions keep it.
// - ounce_renderer_shader_objects.bin: driver binaries by create info, under the driver's binary UUID and version (sobin).
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vk_check.h"
#include "os.h"
#include "vk_spv_late.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/control_flow.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/translate_program.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/oot_visual_workaround.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"
#include "common/settings.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

#include "vk_pool.h"
#include "vk_image.h"
#include "vk_compute.h"
#include "vk_shader.h"
#include "vk_state.h"
#include "vk_shader_parts.h"
#include "vk_work.h"

#ifndef OUNCE_RENDERER_SHADER_STAMP
#define OUNCE_RENDERER_SHADER_STAMP 0ull
#endif

namespace hsd {
static_assert(kMeshFirstPush == Shader::Backend::SPIRV::MESH_FIRST_PUSH_OFFSET, "the mesh draw's first: Eden's push offset");
double seconds_since_device();   // (vk_draw.cpp)

namespace {

using clk = std::chrono::steady_clock;
constexpr uint32_t kControlMagic = 0x98761234;
constexpr uint32_t kControlSize = 0xd8;
constexpr uint32_t kAmpereHeader = 128;

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

std::string hex16(uint64_t v) {
    char b[20];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

// <stamp graphics>
// ---- a stage's code (vk_shader.cpp Code, read_code, parse_code; graphics stages) ----
struct Code {
    int stage = -1;   // (NVN stage)
    uint64_t key = 0;
    std::vector<u128> code;   // (the native instructions, up to the program's end)
    std::vector<uint8_t> c1;
    bool header_only = false;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
    bool ok = false;
    // (NVN2's task (6) and mesh (7) stages: control section +0x58 threads 15:0, payload bytes 31:16; +0x60 max primitives
    // 15:0, max vertices 31:16; +0x5f the output topology, 1 points, 3 triangles (vk_shader.cpp parse_code))
    uint32_t mesh_threads = 0, payload_bytes = 0, max_vertices = 0, max_primitives = 0, topology = 0;
};
void mesh_fields(const uint8_t* ctl, Code& c) {
    if (c.stage != 6 && c.stage != 7) return;
    uint32_t w58, w60;
    std::memcpy(&w58, ctl + 0x58, 4), std::memcpy(&w60, ctl + 0x60, 4);
    c.mesh_threads = w58 & 0xFFFF, c.payload_bytes = w58 >> 16;
    c.max_primitives = w60 & 0xFFFF, c.max_vertices = w60 >> 16;
    c.topology = ctl[0x5f];
}
bool control_end(const uint8_t* ctl, uint32_t* end_out) {
    uint32_t magic, len, c1o, end;
    uint16_t st;
    std::memcpy(&magic, ctl, 4), std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&c1o, ctl + 0x40, 4), std::memcpy(&end, ctl + 0x44, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    if (magic != kControlMagic || !len || end > (16u << 20) || c1o > end || len > end || st > 7) return false;
    return *end_out = end, true;
}
/// The stage's package read: its control section and code area (nullopt-like: ok false)
bool read_package(const nvn_stage_data& sd, uint8_t ctl[kControlSize], std::vector<uint8_t>& area) {
    uint32_t end = 0;
    if (!sd.control || !sd.data || !host->gpu_read(sd.control, ctl, kControlSize) || !control_end(ctl, &end)) return false;
    area.resize(end);
    return host->gpu_read(sd.data, area.data(), end) != 0;
}
/// The stage's key, NVN stage and header (what a draw's key needs), without lowering it
bool scan_code(const nvn_stage_data& sd, Code& c) {
    uint8_t ctl[kControlSize];
    std::vector<uint8_t> area;
    if (!read_package(sd, ctl, area)) return false;
    uint32_t len;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&st, ctl + 0x56, 2);
    c.key = hash_bytes(area.data(), area.size(), hash_bytes(ctl, kControlSize));
    c.stage = st;
    if (st != 5) std::memcpy(c.sph, area.data(), std::min<size_t>(sizeof c.sph, area.size()));
    c.header_only = st != 5 && len <= kAmpereHeader + 16;
    mesh_fields(ctl, c);
    return true;
}
/// The stage's native instructions up to the program's end, as the translator reads them, from its control section
/// and code area
bool parse_bytes(const uint8_t* ctl, const std::vector<uint8_t>& area, Code& c) {
    uint32_t len, c1o;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&c1o, ctl + 0x40, 4), std::memcpy(&st, ctl + 0x56, 2);
    c.key = hash_bytes(area.data(), area.size(), hash_bytes(ctl, kControlSize));
    c.stage = st;
    mesh_fields(ctl, c);
    const bool graphics = st != 5;
    const uint32_t hdr = graphics ? kAmpereHeader : 0;
    if (graphics) std::memcpy(c.sph, area.data(), std::min<size_t>(sizeof c.sph, area.size()));
    if (graphics && len <= hdr + 16) return c.header_only = true, c.ok = true;
    using Shader::Ampere::Instruction;
    Shader::Ampere::ProgramCaptureState capture;
    for (uint32_t i = 0; hdr + (i + 1) * sizeof(Instruction) <= len; i++) {
        Instruction insn;
        std::memcpy(&insn, area.data() + hdr + i * sizeof(Instruction), sizeof insn);
        c.code.push_back({uint64_t(insn.words[0]) | uint64_t(insn.words[1]) << 32, uint64_t(insn.words[2]) | uint64_t(insn.words[3]) << 32});
        const auto r = capture.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            static std::atomic<int> told{0};
            if (told++ < 40) logf("direct: program stage %016llx (NVN stage %u): a branch target outside the program at +%#x", (unsigned long long)c.key, st, i * 16);
            return false;
        }
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    c.c1.assign(area.begin() + c1o, area.end());
    return c.ok = true;
}
bool parse_code(const nvn_stage_data& sd, Code& c) {
    uint8_t ctl[kControlSize];
    std::vector<uint8_t> area;
    return read_package(sd, ctl, area) && parse_bytes(ctl, area, c);
}
const Shader::ProgramHeader& header_of(const uint8_t* sph) { return *reinterpret_cast<const Shader::ProgramHeader*>(sph); }

// </stamp graphics>
// <stamp shared>
// ---- the bound textures' answers (vk_shader.cpp texture_answer, canonical_answer) ----
uint8_t live_answer(uint32_t handle) {
    const uint8_t a = pool_answer(handle & 0xFFFFF);
    return a == 0xFF ? (uint8_t)Shader::TextureType::Color2D : a;   // (not registered: what the frozen back end answers)
}
uint8_t canonical(uint8_t type, bool dim_fixed, uint8_t desc_type, bool depth_float) {
    const uint8_t dim = type & 0x7f;
    if (dim_fixed && dim != (uint8_t)Shader::TextureType::Color2DRect && dim != (uint8_t)Shader::TextureType::Buffer)
        type = (uint8_t)((type & 0x80) | desc_type);
    if (depth_float) return type & 0x7f;
    return type;
}

// </stamp shared>
// <stamp graphics>
// ---- the translator's questions (vk_shader.cpp Env), answered from the stage's code alone ----
// No draw state reaches a translation: a driver bank's word reads 0, so a texture's type is its instruction's (Color2D where
// the instruction has none, a size query's) and no texture is an integer one. The draw checks the bound textures against
// that (shader_pick, check_textures): one that contradicts it is an error
class Env final : public Shader::Environment {
public:
    Env(const Code& c, Shader::Stage st) : nvn{c} {
        stage = st;
        start_address = 0;
        std::memcpy(&sph, nvn.sph, sizeof sph);
        local_memory = static_cast<uint32_t>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        is_proprietary_driver = true;
    }
    /// (a passthrough geometry stage) the attributes it passes on: those the vertex stage before it stores
    void pass_through(const Shader::VaryingState& vertex_stores) {
        gp_passthrough_mask.fill(0);
        for (size_t i = 0; i < gp_passthrough_mask.size() * 32; i++)
            if (!vertex_stores.mask[i]) gp_passthrough_mask[i / 32] |= 1u << (i % 32);
    }
    u64 ReadInstruction(u32 address) override { throw Shader::LogicError("a 64-bit instruction read at {:#x}", address); }
    u128 ReadInstruction128(u32 address) override {
        if (address % 16 || address / 16 >= nvn.code.size()) throw Shader::RuntimeError("instruction read past the program at {:#x}", address);
        return nvn.code[address / 16];
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 1) {
            uint32_t v = 0;
            if ((size_t)offset + 4 <= nvn.c1.size()) std::memcpy(&v, nvn.c1.data() + offset, 4);
            return v;
        }
        if (index >= 3) game_words++;   // (a driver bank's or a game buffer's word: not the stage's own, read 0)
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat{}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return local_memory; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return stage == Shader::Stage::VertexA || stage == Shader::Stage::VertexB; }
    bool HasNativeInstructions() const override { return true; }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        // (step B: from the program alone. A word of the driver bank's storage buffer table is a storage buffer's
        // address whatever is bound there; any other is not taken for one: its accesses stay global memory, which is read by
        // address all the same (every stage by address))
        static constexpr uint32_t kSsboTable[8] = {0x110, 0x510, 0x410, 0x210, 0x310, 0, 0x670, 0x770};
        const uint32_t base = nvn.stage >= 0 && nvn.stage < 8 ? kSsboTable[nvn.stage] : 0;
        return index == 0 && base && offset >= base && offset < base + 16 * 16 && (offset - base) % 16 == 0;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        if (bank != 0 || !HasHLEMacroState()) return std::nullopt;
        switch (offset) {
        case 0x640: return Shader::ReplaceConstant::BaseVertex;
        case 0x644: return Shader::ReplaceConstant::BaseInstance;
        case 0x648: return Shader::ReplaceConstant::DrawID;
        default: return std::nullopt;
        }
    }
    void Dump(u64, u64) override {}
    /// a mesh program's SET_MESH_SHADER_A / B and SET_MESH_INIT_SHADER, and the header-only stage whose output map lists
    /// its per-primitive outputs (vk_shader.cpp Env::set_mesh)
    void set_mesh(const std::array<u32, 3>& registers, const uint8_t* primitive_sph) {
        mesh_registers = registers;
        if (primitive_sph) std::memcpy(&mesh_primitive_header, primitive_sph, sizeof mesh_primitive_header);
    }
    int game_words = 0;   // (words of game buffers the translator asked for: answered 0)

private:
    const Code& nvn;
    uint32_t local_memory = 0;
};

// </stamp graphics>
// <stamp shared>
// ---- the profile (vk_shader.cpp make_profile with RX_VK_DESC_HEAP_BDA=2 on layer 0) ----
Shader::Profile make_profile() {
    // (debug.loopcap=<n>, a diagnosis: the trips the translator's loop safety counter allows, 8192 without it; in the
    // caches' stamps, device_stamp)
    if (const char* v = dbg_value(SET_DBG_LOOPCAP); v && std::atoi(v) > 0) {
        Settings::values.shader_loop_safety_count = (u32)std::atoi(v);
        logf("direct: loopcap: loops end after %u trips (a diagnosis)", Settings::values.shader_loop_safety_count);
    }
    {   // (the translation's per-GPU settings, as gpu/legacy/vk/vk_coopmat.cpp coopmat_init sets them on this card: in the stamp)
        using namespace Shader::Backend::SPIRV;
        frag_nan_guard = true;
        position_invariant = dev.props.vendorID != 0x10DE;
    }
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_vertex_instance_id = false;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_vote = true;
    p.supported_subgroup_stages = VK_SHADER_STAGE_ALL;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_native_ndc = false;
    p.support_scaled_attributes = false;
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = caps.subgroup_size > 32 && (caps.sg32_stages & VK_SHADER_STAGE_ALL_GRAPHICS) != VK_SHADER_STAGE_ALL_GRAPHICS;
    p.lower_left_origin_mode = false;
    p.need_declared_frag_colors = false;
    p.max_subgroup_size = p.warp_size_potentially_larger_than_guest ? caps.subgroup_max : 32;
    p.support_maximal_reconvergence = caps.maximal_reconvergence;
    p.has_broken_spirv_fragment_shuffle = caps.driver_id == VK_DRIVER_ID_AMD_PROPRIETARY;
    p.min_ssbo_alignment = dev.props.limits.minStorageBufferOffsetAlignment;
    p.ssbo_bda = true;
    p.ssbo_bda_check = false;
    p.ssbo_bda_wide = true;
    p.pool_handles = true;   // (step B: the shader reads every handle and indexes the pools with it)
    return p;
}
const Shader::Profile& profile() {
    static const Shader::Profile p = make_profile();
    return p;
}
const Shader::Profile& profile_graphics(VkShaderStageFlags stage) {
    static const Shader::Profile pinned = [] {
        Shader::Profile q = profile();
        q.warp_size_potentially_larger_than_guest = false, q.max_subgroup_size = 32;
        return q;
    }();
    return (caps.sg32_stages & stage) ? pinned : profile();
}
Shader::HostTranslateInfo host_info() {
    Shader::HostTranslateInfo h{};
    h.support_float64 = true;
    h.support_float16 = true;
    h.support_int64 = true;
    h.needs_demote_reorder = false;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.min_ssbo_alignment = dev.props.limits.minStorageBufferOffsetAlignment;
    h.support_geometry_shader_passthrough = false;
    h.support_conditional_barrier = true;
    h.pool_handles = true;
    return h;
}

// </stamp shared>
// <stamp graphics>
struct Translation {
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Flow::Block> flow_blocks;
    Shader::IR::Program program;
};
std::unique_ptr<Translation> translate(Env& env, bool one_exit = false) {
    auto t = std::make_unique<Translation>();
    // (M4b: a mesh program gets one exit: its counts are set there, as the frozen back end translates it)
    Shader::Ampere::Flow::CFG cfg{env, t->flow_blocks, one_exit};
    t->program = Shader::Ampere::TranslateProgram(t->insts, t->blocks, env, cfg, host_info());
    return t;
}

/// A vertex and fragment program's key: its codes and the colour types the fragment stage writes (the vertex input formats
/// are the draw's push data, read by the vertex stage)
uint64_t graphics_key(uint64_t vs_key, uint64_t fs_key, const uint8_t* fs_sph, const uint8_t color_types[8]) {
    uint64_t key = vs_key ^ (fs_sph ? fs_key * 31 : 0);
    uint8_t written[8];
    for (int i = 0; i < 8; i++) written[i] = fs_sph && header_of(fs_sph).ps.HasOutputComponents(i) ? color_types[i] : 0;
    return hash_bytes(written, 8, key);
}
/// A mesh-only program's key (vk_shader.cpp mesh_key with no task stage): its code, the header-only stage's, the colour
/// types the fragment stage writes
uint64_t mesh_key(uint64_t mesh_code, uint64_t fs_key, const uint8_t* fs_sph, const uint8_t* prim_sph, const uint8_t color_types[8]) {
    uint64_t key = hash_bytes("mesh", 4, mesh_code);
    if (fs_sph) key = key * 1099511628211ull ^ fs_key;
    if (prim_sph) key = hash_bytes(prim_sph, sizeof(Shader::ProgramHeader), key);
    uint8_t written[8];
    for (int i = 0; i < 8; i++) written[i] = fs_sph && header_of(fs_sph).ps.HasOutputComponents(i) ? color_types[i] : 0;
    return hash_bytes(written, 8, key);
}
/// SET_MESH_SHADER_A / B and SET_MESH_INIT_SHADER of a mesh program (vk_shader.cpp mesh_registers_of; M4c: with its task
/// stage's threads and payload lines when it has one)
std::array<u32, 3> mesh_registers_of(const Code& mesh, const Code* task = nullptr) {
    const uint32_t topology = mesh.topology == 3 ? 4 : mesh.topology == 1 ? 0 : mesh.topology;
    const uint32_t most = std::max(mesh.max_vertices, mesh.max_primitives);
    const uint32_t lines = task ? (task->payload_bytes + 127) / 128 : 0;
    return {(topology & 0xF) | (std::min(most, 0x100u) << 4) | (std::min(most, 0x100u) << 13), std::min(mesh.mesh_threads, 0x3Fu) << 20,
            (task ? std::min(task->mesh_threads, 0x3Fu) : 0u) | (std::min(lines, 0x1FFu) << 12) | (std::min(lines, 0x1FFu) << 22)};
}
/// A task+mesh program's key: the mesh-only key salted with its task stage's code
uint64_t task_mesh_key(uint64_t task_code, uint64_t mesh_code, uint64_t fs_key, const uint8_t* fs_sph, const uint8_t* prim_sph,
                       const uint8_t color_types[8]) {
    return hash_bytes("task", 4, mesh_key(mesh_code, fs_key, fs_sph, prim_sph, color_types) ^ task_code * 0x9E3779B97F4A7C15ull);
}

// ---- variants ----
struct VStage {
    int nvn_stage = 0;
    VkShaderStageFlagBits vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    Shader::Info info;
    std::vector<uint8_t> c1;
    std::vector<uint32_t> spirv;
    uint32_t threads = 0, payload_words = 0;   // (M4c: a task stage's threads and payload words, which the draw's task header needs)
};
struct Variant {
    DrawProg d;
    std::vector<VStage> stages;
    Variant* next = nullptr;
};
// </stamp graphics>
void count_code(const Variant& v) {
    int64_t b = sizeof(Variant);
    for (const VStage& st : v.stages) b += (int64_t)(sizeof(VStage) + st.spirv.capacity() * 4 + st.c1.capacity());
    mem_account_main(MO_M_SHADERS, b);
}
struct StageScan {
    uint64_t key = 0;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
};
/// The variants of one pair of stage codes, shared by every program slot with that code (the game sets the same code in
/// thousands of program objects: 12,528 set in Kokiri for 149 variants drawn). Made once, never freed
struct Family {
    std::atomic<Variant*> variants{nullptr};
    std::atomic<bool> loaded{false};   // (its variants kept in the cache file are made)
    std::mutex mu;   // (pending, waiting, and loaded's change)
    std::unordered_set<uint64_t> pending;   // (translations queued, by key)
    std::unordered_set<uint64_t> failed;    // (translations that failed)
    std::vector<std::atomic<int>*> waiting;  // (other slots of the code, made drawable when its kept variants are)
};
std::mutex families_mu;
std::unordered_map<uint64_t, Family*> families;   // (by pair_key of the stages' code keys)
std::atomic<int64_t> st_families{0};

/// A program with a task stage, its draws counted by its stages' code (M4c: drawn and skipped)
struct TaskMesh {
    std::atomic<uint64_t> id{0};
    uint64_t task = 0, mesh = 0, fs = 0;
    std::atomic<int64_t> direct{0}, indirect{0}, programs{0};   // (M4c: direct, indirect: skipped)
    std::atomic<int64_t> drawn_direct{0}, drawn_indirect{0};
    const char* why = nullptr;   // (a program of these codes that cannot be drawn: why)
    int64_t seen[4]{};
};
TaskMesh task_mesh[512];
std::atomic<int64_t> st_task_mesh_full{0};
TaskMesh* task_mesh_of(uint64_t task, uint64_t mesh, uint64_t fs) {
    const uint64_t k = task ^ mesh * 31 ^ fs * 961;
    for (TaskMesh& t : task_mesh) {
        uint64_t have = t.id.load(std::memory_order_acquire);
        if (!have) {
            static std::mutex mu;
            std::lock_guard lk(mu);
            have = t.id.load(std::memory_order_acquire);
            if (!have) {
                t.task = task, t.mesh = mesh, t.fs = fs;
                t.id.store(k, std::memory_order_release);
                have = k;
            }
        }
        if (have == k) return &t;
    }
    st_task_mesh_full.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

struct Slot {
    uint64_t serial = 0;
    nvn_prog prog{};
    // (0 being read, 1 a vertex (+ fragment) or mesh-only (+ fragment) program, 2 not one, 3 its code not readable, 4 a task
    // stage (M4b: not drawn))
    std::atomic<int> state{0};
    StageScan vs, fs, prim, gs;   // (M4b: vs is the mesh stage's of a mesh program; prim its header-only stage)
    bool has_fs = false, mesh = false, has_prim = false;
    bool has_gs = false;        // (a vertex program with a geometry stage between its vertex and fragment stages)
    bool task = false;          // (M4c: a task+mesh program, its task stage run as a compute step; vs is its mesh stage)
    uint64_t task_key = 0;      // (its task stage's code key)
    const char* why = "";
    Family* fam = nullptr;   // (set before state 1)
    TaskMesh* tm = nullptr;  // (state 4)
    work::TicketRef scan;    // (its reading, queued at P1 when the game set it: bumped when a draw needs it first)
    uint64_t keys[6]{};      // (logs: each stage's code key and NVN stage, set before the state)
    int16_t stages[6]{-1, -1, -1, -1, -1, -1};
};

constexpr uint32_t kSlots = 1u << 17;
std::unique_ptr<std::atomic<Slot*>[]> table;
std::mutex table_mu;
std::atomic<double> last_miss_s{0};
std::atomic<int64_t> st_object_ns{0}, st_disk_ns{0}, st_ubos_max{0}, st_ubos_sum{0}, st_ubos_variants{0}, st_game_words{0}, st_multi_inputs{0}, st_contradicted{0}, st_slots{0}, st_full{0}, st_translated{0}, st_failed{0}, st_from_disk{0}, st_objects_made{0}, st_late_stages{0}, st_late_changed{0}, st_late_moved{0}, st_late_ns{0},
    st_object_failed{0}, st_translate_ns{0}, st_queued{0}, st_misses{0}, st_scan_failed{0}, st_bumped{0}, st_read_here{0};

uint32_t slot_hash(uint64_t serial) { return (uint32_t)((serial * 0x9E3779B97F4A7C15ull) >> 47) & (kSlots - 1); }
Slot* slot_find(uint64_t serial) {
    if (!table) return nullptr;
    for (uint32_t k = slot_hash(serial), n = 0; n < kSlots; k = (k + 1) & (kSlots - 1), n++) {
        Slot* s = table[k].load(std::memory_order_acquire);
        if (!s) return nullptr;
        if (s->serial == serial) return s;
    }
    return nullptr;
}

// ---- the program cache file (its record: vk_shader.cpp's put_info / get_info, the rest this back end's) ----
constexpr uint32_t kDiskMagic = 0x50445348;   // ('HSDP')
constexpr uint64_t kDiskFormat = 4;            // (the record's layout below: a change to it changes this)
std::mutex disk_mu;
FILE* disk_out = nullptr;
FILE* disk_in = nullptr;
bool disk_on = false;
struct DiskRecord {
    int64_t at;
    size_t size;
};
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_by_pair;   // (vertex code key ^ fragment code key * 31)

uint64_t pair_key(uint64_t vs, uint64_t fs) { return vs ^ fs * 31; }
/// a slot's family and cache key: a mesh program's code key salted so it never meets a vertex program's
uint64_t pair_key_of(bool mesh, uint64_t first, uint64_t fs) { return pair_key(mesh ? hash_bytes("mesh", 4, first) : first, fs); }

void put_bytes(std::vector<uint8_t>& o, const void* d, size_t n) { o.insert(o.end(), (const uint8_t*)d, (const uint8_t*)d + n); }
template <class T> void put(std::vector<uint8_t>& o, const T& v) { put_bytes(o, &v, sizeof v); }
template <class V> void put_list(std::vector<uint8_t>& o, const V& v) {
    put(o, (uint64_t)v.size());
    for (const auto& e : v) put(o, e);
}
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool bad = false;
    bool has(uint64_t n) {
        if (!bad && n > (uint64_t)(end - p)) bad = true;
        return !bad;
    }
    template <class T> T get() {
        alignas(T) uint8_t b[sizeof(T)]{};
        if (has(sizeof(T))) std::memcpy(b, p, sizeof(T)), p += sizeof(T);
        return *(T*)b;
    }
    uint64_t count(size_t elem) {
        const uint64_t n = get<uint64_t>();
        if (!bad && n > (uint64_t)(end - p) / (elem ? elem : 1)) bad = true;
        return bad ? 0 : n;
    }
    template <class V> void list(V& v) {
        const uint64_t n = count(sizeof(typename V::value_type));
        v.clear();
        v.reserve((size_t)n);
        for (uint64_t i = 0; i < n && !bad; i++) v.push_back(get<typename V::value_type>());
    }
};
void put_info(std::vector<uint8_t>& o, const Shader::Info& i) {
    put_bytes(o, &i, sizeof i);
    put_list(o, i.constant_buffer_descriptors);
    put_list(o, i.storage_buffers_descriptors);
    put_list(o, i.texture_buffer_descriptors);
    put_list(o, i.image_buffer_descriptors);
    put_list(o, i.texture_descriptors);
    put_list(o, i.image_descriptors);
    put(o, (uint64_t)i.legacy_stores_mapping.size());
    for (const auto& kv : i.legacy_stores_mapping) put(o, kv.first), put(o, kv.second);
    put(o, (uint64_t)i.handle_programs.size());
    for (const auto& h : i.handle_programs) put_list(o, h);
}
void get_info(Reader& r, Shader::Info& i) {
    if (!r.has(sizeof i)) return;
    alignas(Shader::Info) static thread_local uint8_t fresh[sizeof(Shader::Info)];
    std::memcpy(fresh, (const void*)&i, sizeof i);
    std::memcpy((void*)&i, r.p, sizeof i);
    r.p += sizeof i;
#define KEEP(m) std::memcpy((void*)&i.m, fresh + ((const uint8_t*)&i.m - (const uint8_t*)&i), sizeof i.m)
    KEEP(constant_buffer_descriptors);
    KEEP(storage_buffers_descriptors);
    KEEP(texture_buffer_descriptors);
    KEEP(image_buffer_descriptors);
    KEEP(texture_descriptors);
    KEEP(image_descriptors);
    KEEP(legacy_stores_mapping);
    KEEP(handle_programs);
#undef KEEP
    r.list(i.constant_buffer_descriptors);
    r.list(i.storage_buffers_descriptors);
    r.list(i.texture_buffer_descriptors);
    r.list(i.image_buffer_descriptors);
    r.list(i.texture_descriptors);
    r.list(i.image_descriptors);
    for (uint64_t n = r.count(2 * sizeof(Shader::IR::Attribute)); n && !r.bad; n--) {
        const auto k = r.get<Shader::IR::Attribute>();
        i.legacy_stores_mapping[k] = r.get<Shader::IR::Attribute>();
    }
    i.handle_programs.resize((size_t)r.count(sizeof(uint64_t)));
    for (auto& h : i.handle_programs) r.list(h);
    auto fit = [](const auto& list) {
        for (const auto& d : list)
            if (d.count > 4096) return false;
        return true;
    };
    if (!fit(i.constant_buffer_descriptors) || !fit(i.storage_buffers_descriptors) || !fit(i.texture_descriptors) ||
        !fit(i.image_descriptors) || !fit(i.texture_buffer_descriptors) || !fit(i.image_buffer_descriptors))
        r.bad = true;
}

// <stamp graphics>
/// The register-pressure pass on mesh stages' SPIR-V (vk_spv_late.h; docs/direct/spv-late.md): on, unless
/// debug.nolate (a diagnosis: the translator's order as it is; the program cache is stamped by it, so the two do not mix)
bool late_mesh() {
    static const bool on = !dbg(SET_DBG_NOLATE);
    return on;
}
/// The same on vertex stages: off, unless debug.latevertex (a trial: 21 vertex stages of the game spill as RADV compiles
/// them and 769 are at the register limit, none with the pass; but no harness holds a vertex stage's outputs against it yet)
bool late_vertex() {
    static const bool on = late_mesh() && dbg(SET_DBG_LATEVERTEX);
    return on;
}
/// A stage's SPIR-V as the translator emitted it, through the pass
void schedule_late(std::vector<uint32_t>& spirv, bool on) {
    if (!on) return;
    const auto t0 = std::chrono::steady_clock::now();
    LateResult r;
    spv_schedule_late(spirv, &r);
    st_late_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
    st_late_stages.fetch_add(1, std::memory_order_relaxed);
    st_late_changed.fetch_add(r.moved ? 1 : 0, std::memory_order_relaxed);
    st_late_moved.fetch_add(r.moved, std::memory_order_relaxed);
}

// </stamp graphics>

/// What translated code depends on besides the translator's source: the device's subgroup answers, the loop cap
/// (debug.loopcap), maximal reconvergence, the compiler that built the translator, the record's in-memory layout
uint64_t device_stamp() {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto add = [&h](const void* d, size_t n) { h = hash_bytes(d, n, h); };
    const uint32_t lanes[] = {profile().warp_size_potentially_larger_than_guest ? 1u : 0u, profile().max_subgroup_size,
                              profile().has_broken_spirv_fragment_shuffle ? 1u : 0u, (uint32_t)caps.sg32_stages,
                              (uint32_t)Settings::values.shader_loop_safety_count, caps.maximal_reconvergence ? 1u : 0u};
    add(lanes, sizeof lanes);
    add(__VERSION__, sizeof __VERSION__);
    const uint32_t abi[] = {(uint32_t)sizeof(Shader::Info), (uint32_t)sizeof(std::vector<uint32_t>),
#if defined(_LIBCPP_VERSION)
                            1u, (uint32_t)_LIBCPP_VERSION
#else
                            3u, 0u
#endif
    };
    add(abi, sizeof abi);
    return h;
}

/// The program cache's stamp: the translation's code (OUNCE_RENDERER_SHADER_STAMP), the record format (kDiskFormat), the
/// device, and the switches that change the graphics translation
uint64_t disk_stamp() {
    uint64_t h = device_stamp();
    const auto add = [&h](const void* d, size_t n) { h = hash_bytes(d, n, h); };
    const uint64_t translator[2] = {OUNCE_RENDERER_SHADER_STAMP, kDiskFormat};
    add(translator, sizeof translator);
    const uint32_t depth = host->depth_minus_w() != 0;
    add(&depth, sizeof depth);
    using namespace Shader::Backend::SPIRV;
    const uint32_t flags = (position_invariant ? 1u : 0u) | (frag_nan_guard ? 2u : 0u) | (late_mesh() ? 4u : 0u) | (late_vertex() ? 8u : 0u);
    add(&flags, sizeof flags);
    return h;
}

std::string exe_dir() {
    char b[OS_PATH_MAX];
    std::string p(b, (size_t)os_exe_path(b, sizeof b));
    return p.substr(0, p.find_last_of('/') + 1);
}

// ---- a cache file's index (<file>.idx): its records' {key, offset, size} up to an offset, so a start reads the index
// and the heads of the records after it, not the head of every record of a file of gigabytes (a precompiled game's
// program cache: 33,478 records, 4.2 GB; 9.6 s from a cold disk without it). For the header it was made under (stamp);
// its last record's head is checked against the file's (the key: the program cache's is the pair, the record's first
// field after its head) ----
struct IndexEntry {
    uint64_t key;
    int64_t at;   // (the record's bytes, after its 16-byte head)
    uint64_t size;
};
constexpr uint64_t kIndexMagic = 0x3158444950445348ull;   // ("HSDPIDX1")
/// The index's entries when it is the file's (f, its size whole, made under stamp); the offset it covers, else 0
int64_t index_load(const std::string& path, uint64_t stamp, os_file* f, int64_t whole, std::vector<IndexEntry>& out, bool key_after_head) {
    os_file* x = os_file_open(path.c_str(), OS_READ, nullptr);
    if (!x) return 0;
    uint64_t head[4]{};
    std::vector<IndexEntry> e;
    const int64_t n = os_file_size(x);
    bool ok = n >= 32 && os_file_read_at(x, head, 32, 0) == 32 && head[0] == kIndexMagic && head[1] == stamp && (int64_t)head[2] <= whole &&
              (uint64_t)(n - 32) == head[3] * sizeof(IndexEntry);
    if (ok) {
        e.resize(head[3]);
        ok = os_file_read_at(x, e.data(), e.size() * sizeof(IndexEntry), 32) == (int64_t)(e.size() * sizeof(IndexEntry));
    }
    os_file_close(x);
    if (ok && !e.empty()) {   // (the last record where the index says, of the size and key it says)
        uint64_t rec[3];
        ok = os_file_read_at(f, rec, 24, (uint64_t)(e.back().at - 16)) == 24 && rec[key_after_head ? 2 : 0] == e.back().key && rec[1] == e.back().size &&
             e.back().at + (int64_t)e.back().size == (int64_t)head[2];
    }
    if (!ok) return 0;
    out = std::move(e);
    return (int64_t)head[2];
}
void index_save(const std::string& path, uint64_t stamp, int64_t end, const std::vector<IndexEntry>& e) {
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const uint64_t head[4] = {kIndexMagic, stamp, (uint64_t)end, e.size()};
    const bool ok = fwrite(head, 8, 4, f) == 4 && fwrite(e.data(), sizeof(IndexEntry), e.size(), f) == e.size();
    if (fclose(f) == 0 && ok) os_rename(tmp.c_str(), path.c_str(), true);
}
std::vector<IndexEntry> disk_index;   // (the program cache's records in file order: key is the record's pair; under disk_mu)
int64_t disk_end = 0;                 // (the file's end its records reach; under disk_mu)
uint64_t disk_stamp_now = 0;

void disk_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        const std::string path = exe_dir() + "ounce_renderer_programs.bin";
        os_file* lock = os_file_open((path + ".lock").c_str(), OS_READ | OS_WRITE | OS_CREATE, nullptr);   // (held for the process's life)
        if (!lock || !os_file_trylock(lock)) {
            os_file_close(lock);
            logf("direct: program cache %s: another game is using it: this one runs without it", path.c_str());
            return;
        }
        const uint64_t stamp = disk_stamp();
        int64_t good = 0, whole = 0;
        size_t records = 0;
        bool scanned = false;
        if (os_file* f = os_file_open(path.c_str(), OS_READ, nullptr)) {   // (the index, then the heads after it: no read-ahead)
            os_file_random(f);
            whole = os_file_size(f);
            uint64_t head[2]{};
            if (whole >= 16 && os_file_read_at(f, head, 16, 0) == 16 && (uint32_t)head[0] == kDiskMagic && head[1] == stamp) {
                good = std::max<int64_t>(16, index_load(path + ".idx", stamp, f, whole, disk_index, true));
                while (good + 32 <= whole) {
                    uint64_t rec[3];   // (key, size, then the record's first field: the pair)
                    if (os_file_read_at(f, rec, 24, (uint64_t)good) != 24) break;
                    if (rec[1] < 8 || rec[1] > (uint64_t)(whole - good - 16)) break;
                    disk_index.push_back({rec[2], good + 16, rec[1]});
                    good += 16 + (int64_t)rec[1];
                    scanned = true;
                }
                for (const IndexEntry& e : disk_index) disk_by_pair[e.key].push_back({e.at, (size_t)e.size});
                records = disk_index.size();
            }
            os_file_close(f);
        }
        disk_stamp_now = stamp, disk_end = good;
        if (scanned) index_save(path + ".idx", stamp, good, disk_index);
        if (good) {
            std::error_code ec;
            if (good != whole && (std::filesystem::resize_file(path, (uintmax_t)good, ec), ec)) good = 0;
        }
        if (good) {
            disk_out = fopen(path.c_str(), "ab");
        } else {   // (none, or another stamp: made again)
            disk_by_pair.clear(), disk_index.clear();
            records = 0, disk_end = 16;
            disk_out = fopen(path.c_str(), "wb");
            if (disk_out) {
                const uint32_t h[2] = {kDiskMagic, 0};
                fwrite(h, 4, 2, disk_out), fwrite(&stamp, 8, 1, disk_out), fflush(disk_out);
            }
        }
        disk_in = disk_out ? fopen(path.c_str(), "rb") : nullptr;
        disk_on = disk_out && disk_in;
        logf("direct: program cache %s: %zu variants kept (%lld KB)%s", path.c_str(), records, (long long)(good >> 10),
             whole && !good ? "; another stamp's file was truncated" : "");
    });
}

void disk_save(const Variant& v, uint64_t pair) {
    disk_init();
    if (!disk_on) return;
    std::vector<uint8_t> o;
    o.reserve(128 << 10);
    put(o, pair);
    put(o, v.d.key);
    put(o, (uint64_t)v.stages.size());
    for (const VStage& st : v.stages) {
        put(o, st.nvn_stage);
        put(o, st.vk_stage);
        put(o, st.threads), put(o, st.payload_words);
        put_list(o, st.spirv);
        put_list(o, st.c1);
        put_info(o, st.info);
    }
    const uint64_t head[2] = {v.d.key, o.size()};
    std::lock_guard lk(disk_mu);
    fseeko(disk_out, 0, SEEK_END);
    const int64_t at = ftello(disk_out);
    fwrite(head, 8, 2, disk_out);
    fwrite(o.data(), 1, o.size(), disk_out);
    fflush(disk_out);
    disk_by_pair[pair].push_back({at + 16, o.size()});   // (a program set after this finds it: shader_warm's)
    disk_index.push_back({pair, at + 16, o.size()});
    disk_end = at + 16 + (int64_t)o.size();
}

bool disk_read(const DiskRecord& d, Variant& v) {
    std::vector<uint8_t> data(d.size);
    {
        std::lock_guard lk(disk_mu);
        fseeko(disk_in, d.at, SEEK_SET);
        if (fread(data.data(), 1, data.size(), disk_in) != data.size()) return false;
    }
    Reader r{data.data(), data.data() + data.size()};
    r.get<uint64_t>();   // (the pair)
    v.d.key = r.get<uint64_t>();
    const uint64_t n = r.count(1);
    if (r.bad || n == 0 || n > 3) return false;
    for (uint64_t k = 0; k < n && !r.bad; k++) {
        VStage& st = v.stages.emplace_back();
        st.nvn_stage = r.get<int>();
        st.vk_stage = r.get<VkShaderStageFlagBits>();
        st.threads = r.get<uint32_t>(), st.payload_words = r.get<uint32_t>();
        r.list(st.spirv);
        r.list(st.c1);
        get_info(r, st.info);
        if (st.spirv.empty()) r.bad = true;
    }
    return !r.bad;
}

// ---- c1: the programs' own constants, in device memory made at load ----
std::mutex c1_mu;
struct C1Chunk {
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    uint64_t used = 0, size = 0;
};
C1Chunk c1_chunk;
VkDeviceAddress c1_put(const std::vector<uint8_t>& bytes, uint32_t used) {
    const uint64_t n = (std::max<uint64_t>({bytes.size(), used, 16}) + 255) & ~255ull;
    std::lock_guard lk(c1_mu);
    if (c1_chunk.used + n > c1_chunk.size) {
        const uint64_t size = std::max<uint64_t>(n, 4ull << 20);
        VkBuffer b;
        VkDeviceMemory m;
        C1Chunk c;
        if (!mapped_buffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, b, m, c.map, c.address, MO_CONST)) return 0;
        c.size = size;
        c1_chunk = c;
    }
    std::memset(c1_chunk.map + c1_chunk.used, 0, n);
    std::memcpy(c1_chunk.map + c1_chunk.used, bytes.data(), bytes.size());
    const VkDeviceAddress a = c1_chunk.address + c1_chunk.used;
    c1_chunk.used += n;
    return a;
}

// ---- the shader objects' binaries, graphics and compute: ounce_renderer_shader_objects.bin ----
// A vkCreateShadersEXT from SPIR-V is paid once per driver: each object made from SPIR-V has its binary
// (vkGetShaderBinaryDataEXT) appended, keyed by a hash of everything its create info carries (stage, next stage, flags, the
// subgroup size, the SPIR-V, the push range); a later make with the same key creates it from the binary. No build stamp
// is in it: a translation that changed comes out as other SPIR-V, another key. VK_EXT_shader_object ties a binary to shaderBinaryUUID and
// shaderBinaryVersion: the file's header holds them, and a file of another driver is made again. A binary the driver still
// refuses (VK_INCOMPATIBLE_SHADER_BINARY_EXT) is a cache miss: the SPIR-V is compiled, counted. The file is locked as the
// program cache is (another game: none). Records: {key, size} then the bytes.
namespace sobin {
constexpr uint32_t kMagic = 0x4f445348;   // "HSDO"
constexpr uint32_t kVersion = 1;
std::once_flag once;
std::mutex mu;
os_file* file = nullptr;   // (the binaries, read back)
FILE* out = nullptr;
std::unordered_map<uint64_t, std::pair<int64_t, uint32_t>> at;   // (key -> offset of the bytes, size)
PFN_vkGetShaderBinaryDataEXT get_binary = nullptr;
std::atomic<int64_t> st_hits{0}, st_saved{0}, st_refused{0};
std::vector<IndexEntry> index;   // (the records in file order; under mu after init)
int64_t end = 0;                 // (the file's end its records reach)

struct Header {
    uint32_t magic, version;
    uint8_t uuid[VK_UUID_SIZE];
    uint32_t binary_version, pad;
};
Header header{};   // (the file's, from init)
Header want() {
    VkPhysicalDeviceShaderObjectPropertiesEXT so{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &so;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    Header h{};
    h.magic = kMagic, h.version = kVersion, h.binary_version = so.shaderBinaryVersion;
    std::memcpy(h.uuid, so.shaderBinaryUUID, VK_UUID_SIZE);
    return h;
}
void init() {
    std::call_once(once, [] {
        get_binary = (PFN_vkGetShaderBinaryDataEXT)vkGetDeviceProcAddr(dev.device, "vkGetShaderBinaryDataEXT");
        const std::string path = exe_dir() + "ounce_renderer_shader_objects.bin";
        os_file* lock = get_binary ? os_file_open((path + ".lock").c_str(), OS_READ | OS_WRITE | OS_CREATE, nullptr) : nullptr;
        if (!lock || !os_file_trylock(lock)) {   // (the lock is held for the process's life)
            os_file_close(lock);
            logf("direct: shader object binaries %s: %s: none kept or saved", path.c_str(),
                 get_binary ? "another game is using it" : "no vkGetShaderBinaryDataEXT");
            return;
        }
        const Header h = want();
        header = h;
        int64_t good = 0, whole = 0;
        bool scanned = false;
        const uint64_t stamp = hash_bytes(&h, sizeof h);
        if (os_file* f = os_file_open(path.c_str(), OS_READ, nullptr)) {   // (the index, then the heads after it: no read-ahead)
            os_file_random(f);
            whole = os_file_size(f);
            Header got{};
            if (whole >= (int64_t)sizeof got && os_file_read_at(f, &got, sizeof got, 0) == (int64_t)sizeof got && !std::memcmp(&got, &h, sizeof h)) {
                good = std::max<int64_t>(sizeof h, index_load(path + ".idx", stamp, f, whole, index, false));
                while (good + 16 <= whole) {
                    uint64_t rec[2];
                    if (os_file_read_at(f, rec, 16, (uint64_t)good) != 16 || !rec[1] || rec[1] > (uint64_t)(whole - good - 16) || rec[1] > 0xFFFFFFFFull) break;
                    index.push_back({rec[0], good + 16, rec[1]});
                    good += 16 + (int64_t)rec[1];
                    scanned = true;
                }
                for (const IndexEntry& e : index) at[e.key] = {e.at, (uint32_t)e.size};
            }
            os_file_close(f);
        }
        if (scanned) index_save(path + ".idx", stamp, good, index);
        end = good;
        std::error_code ec;
        if (good && good != whole && (std::filesystem::resize_file(path, (uintmax_t)good, ec), ec)) good = 0;
        if (good) {
            out = fopen(path.c_str(), "ab");
        } else {   // (none, or another driver's: made again)
            at.clear(), index.clear();
            end = sizeof h;
            if ((out = fopen(path.c_str(), "wb"))) fwrite(&h, sizeof h, 1, out), fflush(out);
        }
        file = out ? os_file_open(path.c_str(), OS_READ, nullptr) : nullptr;
        if (!file) at.clear();
        logf("direct: shader object binaries %s: %zu kept (driver binary version %u)", path.c_str(), at.size(), h.binary_version);
    });
}
uint64_t key_of(const VkShaderCreateInfoEXT& ci, bool size32) {
    // (the layout is the pools' set's, the same for every shader object: its push range's size stands for it)
    const uint32_t head[5] = {(uint32_t)ci.stage, (uint32_t)ci.nextStage, (uint32_t)ci.flags, size32 ? 32u : 0u,
                              ci.pPushConstantRanges ? ci.pPushConstantRanges->size : 0u};
    uint64_t k = hash_bytes(head, sizeof head);
    return hash_bytes(ci.pCode, ci.codeSize, k);
}
/// The binary kept for key, or empty
std::vector<uint8_t> find(uint64_t key) {
    std::pair<int64_t, uint32_t> where{0, 0};
    {
        std::lock_guard lk(mu);
        const auto it = at.find(key);
        if (it == at.end()) return {};
        where = it->second;
    }
    std::vector<uint8_t> b(where.second);
    if (os_file_read_at(file, b.data(), b.size(), (uint64_t)where.first) != (int64_t)b.size()) return {};
    return b;
}
void save(uint64_t key, VkShaderEXT obj) {
    if (!out || !get_binary) return;
    size_t n = 0;
    if (get_binary(dev.device, obj, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<uint8_t> data(n);
    if (get_binary(dev.device, obj, &n, data.data()) != VK_SUCCESS) return;
    const uint64_t h[2] = {key, n};
    std::lock_guard lk(mu);
    if (at.count(key)) return;
    fseeko(out, 0, SEEK_END);
    const int64_t pos = ftello(out);
    if (fwrite(h, 8, 2, out) != 2 || fwrite(data.data(), 1, n, out) != n || fflush(out) != 0) return;
    at[key] = {pos + 16, (uint32_t)n};
    index.push_back({key, pos + 16, n});
    end = pos + 16 + (int64_t)n;
    st_saved.fetch_add(1, std::memory_order_relaxed);
}
/// The index written for what the file holds now (the precompile's end)
void index_now() {
    std::lock_guard lk(mu);
    if (out) index_save(exe_dir() + "ounce_renderer_shader_objects.bin.idx", hash_bytes(&header, sizeof header), end, index);
}
}  // namespace sobin

/// The variant's push layout and draw-side tables (v.d), then its shader objects (none for a variant no draw can draw)
bool finish(Variant& v) {
    DrawProg& d = v.d;
    uint32_t slots = 0;
    for (size_t si = 0; si < v.stages.size(); si++) {
        const VStage& st = v.stages[si];
        const Shader::Info& info = st.info;
        const uint8_t stage = (uint8_t)st.nvn_stage;
        if (info.uses_render_area) d.uses_render_area = true;
        // (uniform buffers: by device address, each stage's in its part of the draw's table, in its descriptors' order;
        // the push data holds the address of each stage's part, UNIFORM_TABLE_PUSH_OFFSET)
        // (a graphics program's compute stage is its task stage)
        const uint32_t slot = Shader::Backend::SPIRV::UniformTableSlot(st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT ? Shader::Stage::Fragment
                                                                       : st.vk_stage == VK_SHADER_STAGE_GEOMETRY_BIT ? Shader::Stage::Geometry
                                                                       : st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT  ? Shader::Stage::Task
                                                                                                                     : Shader::Stage::VertexB);
        if (!info.constant_buffer_descriptors.empty()) {
            if (slots >> slot & 1) d.skip = d.skip ? d.skip : "two stages in one uniform table slot";
            slots |= 1u << slot;
            d.table_first[slot] = (uint8_t)d.ubos.size();
        }
        for (const auto& c : info.constant_buffer_descriptors) {
            if (c.count != 1) d.skip = d.skip ? d.skip : "a uniform buffer array";
            const uint32_t used = c.index < info.constant_buffer_used_sizes.size() ? info.constant_buffer_used_sizes[c.index] : 0;
            d.ubos.push_back({stage, (uint8_t)c.index, c.index == 0 || c.index == 2 || c.index == kRootBank ? std::clamp<uint32_t>((used + 15) & ~15u, 16, 0xA00) : used});
        }
        // (storage buffers: by address in the shader (ssbo_bda); textures and images: the pools' set, by the handle)
        if (!info.storage_buffers_descriptors.empty() && !info.ssbo_bda) d.skip = d.skip ? d.skip : "storage buffers not by address";
        if (!info.texture_buffer_descriptors.empty()) d.skip = d.skip ? d.skip : "a texel buffer";
        if (!info.image_buffer_descriptors.empty()) d.skip = d.skip ? d.skip : "an image buffer";
        for (const auto& t : info.texture_descriptors) {   // (the handle words its textures read, for the logs; M4h: every
                                                           // descriptor, two-word and computed ones too)
            DrawProg::TexWord w{stage, (uint8_t)t.cbuf_index, (uint16_t)t.cbuf_offset};
            w.secondary = t.has_secondary, w.sbank = (uint8_t)t.secondary_cbuf_index, w.soffset = (uint16_t)t.secondary_cbuf_offset;
            w.shift = (uint8_t)t.shift_left, w.sshift = (uint8_t)t.secondary_shift_left, w.type = (uint8_t)t.type;
            w.count = (uint8_t)std::min<uint32_t>(t.count, 255), w.computed = t.handle_program != ~0u;
            d.tex_words.push_back(w);
        }
        if (st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT)
            for (uint32_t i = 0; i < 16; i++)
                if (info.loads.Generic(i)) d.vs_loads |= 1u << i;
        if (st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT || st.vk_stage == VK_SHADER_STAGE_GEOMETRY_BIT) {   // (a geometry stage last)
            d.frag_inputs = 0;
            for (uint32_t i = 0; i < 32; i++)
                if (info.stores.Generic(i)) d.frag_inputs |= 1u << i;
        }
        if (st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT)
            for (int i = 0; i < 8; i++) {
                if (info.stores_frag_color[i]) d.frag_stores |= (uint8_t)(1u << i);
                d.frag_components[i] = info.frag_color_components[i];
            }
        {   // (the shape, for the draw log)
            int bank = 0, game = 0, computed = 0;
            for (const auto& t : info.texture_descriptors) {
                if (t.handle_program != ~0u) computed++;
                else if (t.cbuf_index == 0 || t.cbuf_index == 2) bank++;
                else game++;
            }
            const size_t k = std::strlen(d.shape);
            std::snprintf(d.shape + k, sizeof d.shape - k, "%s%s: tex %d bank %d game %d computed, img %zu, ssbo %zu%s%s",
                          k ? "; " : "", st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT ? "vs" : st.vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT ? "ms"
                                         : st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT ? "ts" : st.vk_stage == VK_SHADER_STAGE_GEOMETRY_BIT ? "gs" : "fs", bank, game, computed,
                          info.image_descriptors.size(), info.storage_buffers_descriptors.size(),
                          info.uses_global_memory ? ", global memory" : "", info.texture_buffer_descriptors.size() ? ", texel buffers" : "");
        }
        if (st.nvn_stage >= 0 && st.nvn_stage < 8)
            d.c1[st.nvn_stage] = c1_put(st.c1, info.constant_buffer_used_sizes.size() > 1 ? info.constant_buffer_used_sizes[1] : 0);
        if (st.vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT) d.mesh = true;
        if (st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT) d.task = true, d.task_threads = st.threads, d.task_payload_words = st.payload_words;
        {   // (M4b: the storage buffer slots it reads, for the count of slots read that nothing bound)
            static constexpr uint32_t kSsboTable[8] = {0x110, 0x510, 0x410, 0x210, 0x310, 0, 0x670, 0x770};
            const uint32_t base = st.nvn_stage >= 0 && st.nvn_stage < 8 ? kSsboTable[st.nvn_stage] : 0;
            for (const auto& sb : info.storage_buffers_descriptors)
                if (base && sb.cbuf_index == 0 && sb.cbuf_offset >= base && sb.cbuf_offset < base + 256)
                    for (uint32_t j = 0; j < std::max<uint32_t>(sb.count, 1) && (sb.cbuf_offset - base) / 16 + j < 16; j++)
                        d.ssbo_slots[st.nvn_stage] |= (uint16_t)(1u << ((sb.cbuf_offset - base) / 16 + j));
        }
    }
    d.table_slots = (uint8_t)slots;
    d.push_bytes = kTablePush + 8 * std::max<uint32_t>(1, 32 - __builtin_clz(slots | 1));
    {   // (M3b: what the address table holds: uniform buffers a variant reads)
        const int64_t n = (int64_t)d.ubos.size();
        int64_t m = st_ubos_max.load(std::memory_order_relaxed);
        while (n > m && !st_ubos_max.compare_exchange_weak(m, n)) {}
        st_ubos_sum.fetch_add(n, std::memory_order_relaxed), st_ubos_variants.fetch_add(1, std::memory_order_relaxed);
    }
    if (d.push_bytes > caps.max_push) d.skip = d.skip ? d.skip : "push data over the device's limit";
    // (what a draw checks of its textures whose handle is a driver bank word: that the bound one is what the translation
    // from the code alone took it for, Env's Color2D and not integer as its descriptor canonicalises it; one in a game
    // buffer or made of two words the draw does not see)
    for (const VStage& st : v.stages)
        for (const auto& t : st.info.texture_descriptors) {
            if (t.count != 1 || t.has_secondary || t.handle_program != ~0u || !(t.cbuf_index == 0 || t.cbuf_index == 2)) continue;
            DrawProg::Answer x{};
            x.stage = (uint8_t)st.nvn_stage, x.bank = (uint8_t)t.cbuf_index, x.offset = (uint16_t)t.cbuf_offset;
            x.dim_fixed = t.type_fixed, x.desc_type = (uint8_t)t.type, x.depth_float = t.is_depth && !t.is_integer;
            x.want = canonical((uint8_t)Shader::TextureType::Color2D, x.dim_fixed, x.desc_type, x.depth_float);
            d.answers.push_back(x);
        }
    // (the shader objects, unlinked: vertex then fragment)
    static const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo size32 = [] {
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo r{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        r.requiredSubgroupSize = 32;
        return r;
    }();
    for (size_t si = 0; si < v.stages.size() && !d.skip; si++) {
        const VStage& st = v.stages[si];
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sz = size32;
        VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
        pool_shader_layout(ci);
        // (M4b: a mesh stage never has a task stage before it: the device has none enabled)
        if (st.vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT) ci.flags |= VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
        ci.stage = st.vk_stage;
        const bool geometry_next = st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT && v.stages.back().vk_stage == VK_SHADER_STAGE_GEOMETRY_BIT;
        ci.nextStage = geometry_next ? VK_SHADER_STAGE_GEOMETRY_BIT
                     : st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT || st.vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT || st.vk_stage == VK_SHADER_STAGE_GEOMETRY_BIT
                     ? VK_SHADER_STAGE_FRAGMENT_BIT : 0;
        // (M4c: a task stage is a compute shader; its program has no task shader object)
        ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        ci.codeSize = st.spirv.size() * 4;
        ci.pCode = st.spirv.data();
        ci.pName = "main";
        if (caps.sg32_stages & st.vk_stage) ci.pNext = &sz;
        VkShaderEXT obj = VK_NULL_HANDLE;
        const auto t0 = clk::now();
        sobin::init();
        const uint64_t bkey = sobin::key_of(ci, (caps.sg32_stages & st.vk_stage) != 0);
        VkResult r = VK_ERROR_UNKNOWN;
        bool from_binary = false;
        if (std::vector<uint8_t> bin = sobin::find(bkey); !bin.empty()) {   // (M4e: made before by this driver: no compile)
            VkShaderCreateInfoEXT bi = ci;
            bi.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT, bi.codeSize = bin.size(), bi.pCode = bin.data();
            r = vkCreateShadersEXT(dev.device, 1, &bi, nullptr, &obj);
            from_binary = r == VK_SUCCESS;
            if (from_binary) sobin::st_hits.fetch_add(1, std::memory_order_relaxed);
            else obj = VK_NULL_HANDLE, sobin::st_refused.fetch_add(1, std::memory_order_relaxed);
        }
        if (!from_binary) r = vkCreateShadersEXT(dev.device, 1, &ci, nullptr, &obj);
        if (r == VK_SUCCESS && !from_binary) sobin::save(bkey, obj);
        st_object_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed);
        if (r != VK_SUCCESS) {
            st_object_failed.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> told{0};
            if (told++ < 20) logf("direct: program %016llx: its %s shader object not made (%d)", (unsigned long long)d.key,
                                  st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT ? "vertex" : st.vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT ? "mesh"
                                  : st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT ? "task (compute)"
                                  : st.vk_stage == VK_SHADER_STAGE_GEOMETRY_BIT ? "geometry" : "fragment", (int)r);
            return false;
        }
        object_made(1, "a shader object");
        st_objects_made.fetch_add(1, std::memory_order_relaxed);
        (st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT ? d.vs : st.vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT ? d.ms
         : st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT ? d.ts : st.vk_stage == VK_SHADER_STAGE_GEOMETRY_BIT ? d.gs : d.fs) = obj;
    }
    if (d.skip) {
        static std::mutex mu;
        static std::unordered_set<std::string> told;
        std::lock_guard lk(mu);
        if (told.insert(d.skip).second || told.size() < 40)
            logf("direct: program %016llx is drawn as step A cannot: %s (skipped, counted)", (unsigned long long)d.key, d.skip);
    }
    return true;
}

void publish(Family& s, Variant* v, uint64_t serial) {
    Variant* head = s.variants.load(std::memory_order_acquire);
    if (head) {   // (a program's second variant or later: for other colour target types)
        st_multi_inputs.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> told{0};
        if (told++ < 30) {
            int n = 1;
            for (const Variant* o = head; o; o = o->next) n++;
            logf("direct: program serial %llu has variant %d: other colour target types (key %016llx)", (unsigned long long)serial, n,
                 (unsigned long long)v->d.key);
        }
    }
    do v->next = head;
    while (!s.variants.compare_exchange_weak(head, v, std::memory_order_release, std::memory_order_acquire));
}

// <stamp graphics>
// ---- translation (vk_shader.cpp translate_into's graphics half), from the stages' code alone ----
/// A stage's SPIR-V: its bindings numbered from 0, its own (each stage's shader object has its own mappings, finish()), so a
/// stage's SPIR-V does not depend on the stages beside it
std::vector<uint32_t> emit_stage(VkShaderStageFlagBits stage, const Shader::RuntimeInfo& rt, Shader::IR::Program& program) {
    Shader::Backend::Bindings bindings{};
    return Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(stage), rt, program, bindings);
}

/// A vertex program: its vertex stage, its geometry stage when it has one (stored last: stages vertex, fragment, geometry;
/// translated for triangle input, the only input the draw path gives it), its fragment stage
bool translate_variant(Variant& v, const Code& vsc, const Code* gsc, const Code* fsc, const uint8_t color_types[8]) {
    try {
        Env vs_env{vsc, Shader::Stage::VertexB};
        auto vs = translate(vs_env);
        Shader::RuntimeInfo vrt{};
        if (!gsc && !vs->program.info.stores[Shader::IR::Attribute::PointSize]) vrt.fixed_state_point_size = 1.0f;
        vrt.vertex_formats_pushed = true;   // (the attributes' formats: the draw's push data, vk_draw.cpp vertex_fetch)
        vrt.previous_stage_stores.mask.set();
        vrt.convert_depth_mode = !gsc && host->depth_minus_w() != 0;   // (in the last stage before the rasterizer)
        VStage& vst = v.stages.emplace_back();
        vst.nvn_stage = 0, vst.vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
        vst.spirv = emit_stage(VK_SHADER_STAGE_VERTEX_BIT, vrt, vs->program);
        schedule_late(vst.spirv, late_vertex());
        vst.info = vs->program.info;
        vst.c1 = vsc.c1;
        std::unique_ptr<Translation> gs;
        if (gsc) {
            Env gs_env{*gsc, Shader::Stage::Geometry};
            gs_env.pass_through(vs->program.info.stores);
            gs = translate(gs_env);
            st_game_words.fetch_add(gs_env.game_words, std::memory_order_relaxed);
        }
        if (fsc) {
            Env fs_env{*fsc, Shader::Stage::Fragment};
            auto fs = translate(fs_env);
            st_game_words.fetch_add(fs_env.game_words, std::memory_order_relaxed);
            Shader::RuntimeInfo frt{};
            frt.fragment_inputs_pushed = true;   // (its inputs are what it loads; what the stage before writes is the draw's data)
            frt.previous_stage_stores.mask.set();
            for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(color_types[i]);
            uint32_t float_targets = 0;   // (the frozen back end's NaN guard of float targets, on by default)
            for (int i = 0; i < 8; i++)
                if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
            Shader::Optimization::GuardFragmentOutputs(fs->program, float_targets);
            VStage& fst = v.stages.emplace_back();
            fst.nvn_stage = 1, fst.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fst.spirv = emit_stage(VK_SHADER_STAGE_FRAGMENT_BIT, frt, fs->program);
            fst.info = fs->program.info;
            fst.c1 = fsc->c1;
        }
        if (gs) {
            Shader::RuntimeInfo grt{};
            grt.previous_stage_stores = vs->program.info.stores;
            grt.input_topology = Shader::InputTopology::Triangles;
            grt.convert_depth_mode = host->depth_minus_w() != 0;
            VStage& gst = v.stages.emplace_back();
            gst.nvn_stage = 2, gst.vk_stage = VK_SHADER_STAGE_GEOMETRY_BIT;
            gst.spirv = emit_stage(VK_SHADER_STAGE_GEOMETRY_BIT, grt, gs->program);
            gst.info = gs->program.info;
            gst.c1 = gsc->c1;
        }
        st_game_words.fetch_add(vs_env.game_words, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& e) {
        static std::atomic<int> told{0};
        if (told++ < 30) logf("direct: program %016llx not translated: %s", (unsigned long long)v.d.key, e.what());
        return false;
    }
}

// </stamp graphics>

uint64_t slot_pair(const Slot& s) {   // (M4c: a task+mesh program's salted with its task code: never a mesh-only program's family)
    return s.task ? pair_key(hash_bytes("task", 4, s.task_key) ^ hash_bytes("mesh", 4, s.vs.key), s.has_fs ? s.fs.key : 0)
                  : pair_key_of(s.mesh, s.vs.key ^ (s.has_gs ? hash_bytes("geom", 4, s.gs.key) : 0), s.has_fs ? s.fs.key : 0);
}

// <stamp graphics>
/// A mesh-only program: its mesh stage (no task stage before it: the device has no task stage enabled) and fragment
/// stage, as vk_shader.cpp translate_mesh_into translates them without the fallback
bool translate_mesh_variant(Variant& v, const Code& mc, const Code* prim, const Code* fsc, const uint8_t color_types[8]) {
    try {
        Env menv{mc, Shader::Stage::Mesh};
        menv.set_mesh(mesh_registers_of(mc), prim ? prim->sph : nullptr);
        auto ms = translate(menv, true);
        Shader::RuntimeInfo mrt{};
        mrt.previous_stage_stores.mask.set();
        mrt.convert_depth_mode = host->depth_minus_w() != 0;
        mrt.mesh_without_task = true;
        VStage& mst = v.stages.emplace_back();
        mst.nvn_stage = 7, mst.vk_stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        mst.spirv = emit_stage(VK_SHADER_STAGE_MESH_BIT_EXT, mrt, ms->program);
        schedule_late(mst.spirv, late_mesh());
        mst.info = ms->program.info;
        mst.c1 = mc.c1;
        st_game_words.fetch_add(menv.game_words, std::memory_order_relaxed);
        if (fsc) {
            Env fs_env{*fsc, Shader::Stage::Fragment};
            auto fs = translate(fs_env);
            st_game_words.fetch_add(fs_env.game_words, std::memory_order_relaxed);
            Shader::RuntimeInfo frt{};
            frt.previous_stage_stores = ms->program.info.stores;
            for (const u32 line : ms->program.mesh.primitive_lines) {   // (its per-primitive generics: Eden's MakeRuntimeInfo)
                const auto attribute = static_cast<Shader::IR::Attribute>(line);
                if (Shader::IR::IsGeneric(attribute)) frt.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
            }
            // (points: a generic the fragment stage reads and the mesh stage never writes is the point sprite's coordinate)
            if (ms->program.mesh.topology == 0)
                for (size_t i = 0; i < Shader::IR::NUM_GENERICS; i++)
                    if (fs->program.info.loads.Generic(i) && !ms->program.info.stores.Generic(i)) frt.point_coord_generics.set(i);
            for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(color_types[i]);
            uint32_t float_targets = 0;
            for (int i = 0; i < 8; i++)
                if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
            Shader::Optimization::GuardFragmentOutputs(fs->program, float_targets);
            VStage& fst = v.stages.emplace_back();
            fst.nvn_stage = 1, fst.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fst.spirv = emit_stage(VK_SHADER_STAGE_FRAGMENT_BIT, frt, fs->program);
            fst.info = fs->program.info;
            fst.c1 = fsc->c1;
        }
        return true;
    } catch (const std::exception& e) {
        static std::atomic<int> told{0};
        if (told++ < 30) logf("direct: mesh program %016llx not translated: %s", (unsigned long long)v.d.key, e.what());
        return false;
    }
}

/// A task+mesh program: its task stage as a compute shader (Eden's fallback task half, RuntimeInfo::task_buffer: it writes
/// its mesh workgroups and payload to the draw's task header), its mesh stage native with no task stage before it (it reads
/// them from there), its fragment stage. Stages in the order mesh, fragment, task (stage 0 is the mesh stage, as a mesh-only
/// program's)
bool translate_task_mesh_variant(Variant& v, const Code& tc, const Code& mc, const Code* prim, const Code* fsc,
                                 const uint8_t color_types[8]) {
    const size_t at = v.stages.size();
    try {
        const std::array<u32, 3> regs = mesh_registers_of(mc, &tc);
        Env tenv{tc, Shader::Stage::Task};
        tenv.set_mesh(regs, nullptr);
        auto ts = translate(tenv, true);
        const uint32_t threads = ts->program.workgroup_size[0];
        if (threads == 0 || threads > 256)   // (a map entry holds the index in its task in 8 bits)
            throw Shader::NotImplementedException("task stage of {} threads", threads);
        st_game_words.fetch_add(tenv.game_words, std::memory_order_relaxed);
        Env menv{mc, Shader::Stage::Mesh};
        menv.set_mesh(regs, prim ? prim->sph : nullptr);
        auto ms = translate(menv, true);
        Shader::RuntimeInfo mrt{};
        mrt.previous_stage_stores = ts->program.info.stores;
        mrt.convert_depth_mode = host->depth_minus_w() != 0;
        mrt.mesh_without_task = true;
        mrt.task_buffer = true;
        VStage& mst = v.stages.emplace_back();
        mst.nvn_stage = 7, mst.vk_stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        mst.spirv = emit_stage(VK_SHADER_STAGE_MESH_BIT_EXT, mrt, ms->program);
        schedule_late(mst.spirv, late_mesh());
        mst.info = ms->program.info;
        mst.c1 = mc.c1;
        st_game_words.fetch_add(menv.game_words, std::memory_order_relaxed);
        if (fsc) {
            Env fs_env{*fsc, Shader::Stage::Fragment};
            auto fs = translate(fs_env);
            st_game_words.fetch_add(fs_env.game_words, std::memory_order_relaxed);
            Shader::RuntimeInfo frt{};
            frt.previous_stage_stores = ms->program.info.stores;
            for (const u32 line : ms->program.mesh.primitive_lines) {
                const auto attribute = static_cast<Shader::IR::Attribute>(line);
                if (Shader::IR::IsGeneric(attribute)) frt.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
            }
            if (ms->program.mesh.topology == 0)
                for (size_t i = 0; i < Shader::IR::NUM_GENERICS; i++)
                    if (fs->program.info.loads.Generic(i) && !ms->program.info.stores.Generic(i)) frt.point_coord_generics.set(i);
            for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(color_types[i]);
            uint32_t float_targets = 0;
            for (int i = 0; i < 8; i++)
                if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
            Shader::Optimization::GuardFragmentOutputs(fs->program, float_targets);
            VStage& fst = v.stages.emplace_back();
            fst.nvn_stage = 1, fst.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fst.spirv = emit_stage(VK_SHADER_STAGE_FRAGMENT_BIT, frt, fs->program);
            fst.info = fs->program.info;
            fst.c1 = fsc->c1;
        }
        Shader::RuntimeInfo trt{};
        trt.previous_stage_stores.mask.set();
        trt.mesh_fallback = true;
        trt.task_buffer = true;
        std::vector<uint32_t> tspirv = emit_stage(VK_SHADER_STAGE_COMPUTE_BIT, trt, ts->program);
        VStage& tst = v.stages.emplace_back();
        tst.nvn_stage = 6, tst.vk_stage = VK_SHADER_STAGE_COMPUTE_BIT;
        tst.spirv = std::move(tspirv);
        tst.info = ts->program.info;
        tst.c1 = tc.c1;
        tst.threads = threads, tst.payload_words = ts->program.mesh.payload_words;
        static std::atomic<int> told{0};
        if (told++ < 64)
            logf("direct: task+mesh program %016llx translated: task %016llx (%u threads, payload %u words) as a compute step, mesh %016llx "
                 "(%u threads, payload %u words, max vertices %u primitives %u) native", (unsigned long long)v.d.key, (unsigned long long)tc.key,
                 threads, ts->program.mesh.payload_words, (unsigned long long)mc.key, ms->program.workgroup_size[0], ms->program.mesh.payload_words,
                 ms->program.mesh.max_vertices, ms->program.mesh.max_primitives);
        return true;
    } catch (const std::exception& e) {
        v.stages.resize(at);
        static std::atomic<int> told{0};
        if (told++ < 30)
            logf("direct: task+mesh program %016llx (task %016llx mesh %016llx) not translated: %s (its draws skipped, counted)",
                 (unsigned long long)v.d.key, (unsigned long long)tc.key, (unsigned long long)mc.key, e.what());
        return false;
    }
}

// </stamp graphics>

/// (a worker) the program's stages read, and its kept variants made
void scan_slot(Slot* s) {
    Code codes[6];
    int vs = -1, fs = -1, mesh = -1, task = -1, prim = -1, gs = -1;
    bool other = false;
    for (uint32_t i = 0; i < s->prog.nstage && i < 6; i++) {
        if (!scan_code(s->prog.sd[i], codes[i])) {
            s->why = "its code is not readable";
            st_scan_failed.fetch_add(1, std::memory_order_relaxed);
            return s->state.store(3, std::memory_order_release);
        }
        const Code& c = codes[i];
        s->keys[i] = c.key, s->stages[i] = (int16_t)(c.header_only ? 100 + c.stage : c.stage);
        if (c.stage == 6) task = (int)i;   // (M4b: a task stage in any form makes the program task+mesh: never drawn here)
        else if (c.header_only) prim = (int)i;
        else if (c.stage == 0) vs = (int)i;
        else if (c.stage == 1) fs = (int)i;
        else if (c.stage == 7) mesh = (int)i;
        else if (c.stage == 2 && gs < 0) gs = (int)i;
        else other = true;
    }
    if (gs >= 0 && (task >= 0 || mesh >= 0 || !dev.features.geometryShader)) other = true;   // (a geometry stage only after a vertex stage)
    if (task >= 0) {   // (M4c: a task+mesh program: its task stage a compute step, its mesh stage native; drawn by DRAW_MESH only)
        s->tm = task_mesh_of(codes[task].key, mesh >= 0 ? codes[mesh].key : 0, fs >= 0 ? codes[fs].key : 0);
        // (what cannot be drawn so: skipped and counted by program, named once with the reason (rule 2))
        s->why = mesh < 0 ? "a task stage with no mesh stage" : vs >= 0 ? "a task stage beside a vertex stage"
               : other ? "a task stage beside a geometry or tessellation stage" : codes[task].header_only ? "a task stage with no code"
               : codes[mesh].header_only ? "a mesh stage with no code" : nullptr;
        if (s->why) {
            if (s->tm && s->tm->programs.fetch_add(1, std::memory_order_relaxed) == 0)
                logf("direct: program serial %llu has a task stage: task %016llx mesh %016llx fragment %016llx: not drawn: %s",
                     (unsigned long long)s->serial, (unsigned long long)codes[task].key, (unsigned long long)(mesh >= 0 ? codes[mesh].key : 0),
                     (unsigned long long)(fs >= 0 ? codes[fs].key : 0), s->why);
            return s->state.store(4, std::memory_order_release);
        }
        if (s->tm) s->tm->programs.fetch_add(1, std::memory_order_relaxed);
        s->why = "";
        s->task = true, s->task_key = codes[task].key;
    }
    if (mesh >= 0 && vs < 0 && !other) {   // (M4b: a mesh-only program; M4c: or the mesh stage of a task+mesh program)
        s->mesh = true;
        if (prim >= 0) s->has_prim = true, s->prim.key = codes[prim].key, std::memcpy(s->prim.sph, codes[prim].sph, sizeof s->prim.sph);
        vs = mesh;
    } else if (vs < 0 || other || mesh >= 0) {
        s->why = vs < 0 ? "no vertex or mesh stage (compute)" : mesh >= 0 ? "a vertex and a mesh stage" : "a tessellation stage, or a second geometry stage";
        return s->state.store(2, std::memory_order_release);
    }
    s->vs.key = codes[vs].key, std::memcpy(s->vs.sph, codes[vs].sph, sizeof s->vs.sph);
    if (fs >= 0) s->has_fs = true, s->fs.key = codes[fs].key, std::memcpy(s->fs.sph, codes[fs].sph, sizeof s->fs.sph);
    if (gs >= 0) s->has_gs = true, s->gs.key = codes[gs].key, std::memcpy(s->gs.sph, codes[gs].sph, sizeof s->gs.sph);
    const uint64_t pair = slot_pair(*s);
    bool fresh = false;
    {
        std::lock_guard lk(families_mu);
        auto [it, made] = families.try_emplace(pair, nullptr);
        if (made) it->second = new Family, st_families.fetch_add(1, std::memory_order_relaxed);
        s->fam = it->second, fresh = made;
    }
    static const bool log_mesh = dbg_log(SET_DBG_LOGMESH);
    if (fresh && s->mesh && log_mesh)
        logf("direct: program serial %llu is %s: %s%s%smesh %016llx fragment %016llx (header-only stage %016llx)", (unsigned long long)s->serial,
             s->task ? "task+mesh" : "mesh-only", s->task ? "task " : "", s->task ? hex16(s->task_key).c_str() : "", s->task ? " (a compute step) " : "",
             (unsigned long long)s->vs.key, (unsigned long long)(s->has_fs ? s->fs.key : 0), (unsigned long long)(s->has_prim ? s->prim.key : 0));
    if (!fresh) {   // (the code's variants are made by the slot that saw it first: this one is drawable when they are)
        std::lock_guard lk(s->fam->mu);
        if (!s->fam->loaded.load(std::memory_order_acquire)) return s->fam->waiting.push_back(&s->state);
        return s->state.store(1, std::memory_order_release);
    }
    disk_init();
    std::vector<DiskRecord> recs;
    {
        std::lock_guard lk(disk_mu);
        const auto it = disk_by_pair.find(pair);
        if (it != disk_by_pair.end()) recs = it->second;
    }
    for (const DiskRecord& d : recs) {
        auto v = std::make_unique<Variant>();
        const auto t0 = clk::now();
        const bool read = disk_read(d, *v);
        st_disk_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed);
        if (!read || v->stages.size() != 1u + s->has_fs + s->task + s->has_gs || (v->stages[0].vk_stage == VK_SHADER_STAGE_MESH_BIT_EXT) != s->mesh ||
            (v->stages.back().vk_stage == VK_SHADER_STAGE_COMPUTE_BIT) != s->task ||
            (v->stages.back().vk_stage == VK_SHADER_STAGE_GEOMETRY_BIT) != s->has_gs)
            continue;
        v->d.codes[0] = s->vs.key, v->d.codes[1] = s->has_fs ? s->fs.key : 0, v->d.codes[2] = s->has_prim ? s->prim.key : s->has_gs ? s->gs.key : 0;
        v->d.task_code = s->task ? s->task_key : 0;
        if (!finish(*v)) continue;
        st_from_disk.fetch_add(1, std::memory_order_relaxed);
        count_code(*v);
        publish(*s->fam, v.release(), s->serial);
    }
    {
        std::lock_guard lk(s->fam->mu);
        s->fam->loaded.store(true, std::memory_order_release);
        for (std::atomic<int>* w : s->fam->waiting) w->store(1, std::memory_order_release);
        s->fam->waiting.clear();
    }
    s->state.store(1, std::memory_order_release);
}

Slot* slot_make(const nvn_prog& p) {
    std::lock_guard lk(table_mu);
    if (!table) {
        table.reset(new std::atomic<Slot*>[kSlots]);
        for (uint32_t i = 0; i < kSlots; i++) table[i].store(nullptr, std::memory_order_relaxed);
    }
    if (Slot* s = slot_find(p.serial)) return s;
    if (st_slots.load(std::memory_order_relaxed) >= (int64_t)(kSlots * 3 / 4)) return st_full.fetch_add(1, std::memory_order_relaxed), nullptr;
    Slot* s = new Slot;
    s->serial = p.serial, s->prog = p;
    s->scan = work::make([s] { scan_slot(s); });
    for (uint32_t k = slot_hash(p.serial);; k = (k + 1) & (kSlots - 1))
        if (!table[k].load(std::memory_order_relaxed)) {
            table[k].store(s, std::memory_order_release);
            break;
        }
    st_slots.fetch_add(1, std::memory_order_relaxed);
    work::push(s->scan, work::P1);
    return s;
}

/// The bound textures the variant reads by a driver bank word, against what its translation from the code alone took
/// them for (finish: DrawProg::answers). One that contradicts it (an integer format, a rectangle, a buffer, or another type
/// at a size query) is sampled wrong: an error, logged once a program and texture word with the texture, and counted
void check_textures(const DState& s, const DrawProg& p) {
    for (const DrawProg::Answer& a : p.answers) {
        const uint32_t h = bank_word(s, a.bank, a.stage, a.offset);
        if (!h) continue;
        const uint8_t bound = canonical(live_answer(h), a.dim_fixed, a.desc_type, a.depth_float);
        if (bound == a.want) continue;
        st_contradicted.fetch_add(1, std::memory_order_relaxed);
        static std::mutex mu;
        static std::unordered_set<uint64_t> told;
        {
            std::lock_guard lk(mu);
            if (!told.insert(p.key ^ ((uint64_t)a.stage << 56 | (uint64_t)a.bank << 48 | (uint64_t)a.offset << 32 | bound)).second) continue;
        }
        char tex[256];
        pool_describe(h & 0xFFFFF, tex, sizeof tex);
        char where[160];
        std::snprintf(where, sizeof where, "program %016llx stage %d bank %d +%#x handle %#x: bound type %#x%s, translated for %#x",
                      (unsigned long long)p.key, a.stage, a.bank, a.offset, h, bound & 0x7f, bound & 0x80 ? " integer" : "", a.want);
        logf("direct: error: a bound texture contradicts the translation from the code alone: %s; %s", where, tex);
        if (strict_on()) strict_event("a bound texture contradicts the translation from the code alone", "%s", where);
    }
}

}  // namespace

void shader_program_set(const nvn_prog* p) {
    if (!p || !pool_on() || !p->nstage) return;
    nvn_prog copy;
    std::memcpy(&copy, p, sizeof copy);
    slot_make(copy);
}

void shader_describe(const DState& s, char* out, size_t cap) {
    const Slot* slot = s.has_prog ? slot_find(s.prog.serial) : nullptr;
    const int state = slot ? slot->state.load(std::memory_order_acquire) : -1;
    static const char* const kinds[] = {"being read", "drawable", "not drawable", "unreadable", "task+mesh"};
    int k = std::snprintf(out, cap, "program serial %llu %s%s:", (unsigned long long)(s.has_prog ? s.prog.serial : 0),
                          state < 0 ? "unknown" : kinds[std::min(state, 4)], slot && slot->mesh ? " mesh-only" : "");
    if (state > 0)
        for (int i = 0; i < 6 && k < (int)cap - 40; i++)
            if (slot->stages[i] >= 0) k += std::snprintf(out + k, cap - k, " %s%d %016llx", slot->stages[i] >= 100 ? "h" : "", slot->stages[i] % 100, (unsigned long long)slot->keys[i]);
}

void shader_task_mesh_draw(const DState& s, bool indirect, bool drawn) {
    const Slot* slot = s.has_prog ? slot_find(s.prog.serial) : nullptr;
    if (!slot || !slot->tm) return;
    TaskMesh& t = *slot->tm;
    (drawn ? indirect ? t.drawn_indirect : t.drawn_direct : indirect ? t.indirect : t.direct).fetch_add(1, std::memory_order_relaxed);
    if (!drawn && slot->state.load(std::memory_order_relaxed) == 4) t.why = slot->why;
}
bool shader_has_task(const DState& s) {
    const Slot* slot = s.has_prog ? slot_find(s.prog.serial) : nullptr;
    return slot && slot->tm;
}

void shader_task_mesh_log(double frames) {
    double all = 0, drawn = 0;
    for (TaskMesh& t : task_mesh) {   // (a line each: task, mesh and fragment code, draws a frame drawn and skipped)
        if (!t.id.load(std::memory_order_acquire)) break;
        const int64_t now[4] = {t.drawn_direct.load(std::memory_order_relaxed), t.drawn_indirect.load(std::memory_order_relaxed),
                                t.direct.load(std::memory_order_relaxed), t.indirect.load(std::memory_order_relaxed)};
        double a[4];
        for (int k = 0; k < 4; k++) a[k] = (now[k] - t.seen[k]) / frames, t.seen[k] = now[k];
        all += a[0] + a[1] + a[2] + a[3], drawn += a[0] + a[1];
        if (a[0] || a[1] || a[2] || a[3])
            logf("direct: task+mesh program: task %016llx mesh %016llx fragment %016llx: drawn %.2f direct and %.2f indirect, skipped %.2f direct "
                 "and %.2f indirect draws a frame (%lld program objects)%s%s", (unsigned long long)t.task, (unsigned long long)t.mesh,
                 (unsigned long long)t.fs, a[0], a[1], a[2], a[3], (long long)t.programs.load(std::memory_order_relaxed), t.why ? "; not drawable: " : "",
                 t.why ? t.why : "");
    }
    logf("direct: task+mesh draws a frame: %.2f recorded, %.2f drawn (task stage a compute step); programs past the table %lld", all, drawn,
         (long long)st_task_mesh_full.load());
}

const DrawProg* shader_pick(const DState& s, const uint8_t color_types[8], ShaderMiss& why) {
    if (!s.has_prog) return why = SM_NO_PROGRAM, nullptr;
    Slot* slot = slot_find(s.prog.serial);
    if (!slot) slot = pool_on() ? slot_make(s.prog) : nullptr;
    if (!slot) return why = SM_NOT_PARSED, nullptr;
    int state = slot->state.load(std::memory_order_acquire);
    if (state == 0) {   // (still queued with the load's programs: this draw needs it now. Read here, or waited for: a draw
                        // the game makes once, such as the pause picture's, has no later frame to be drawn in)
        if (work::run_here(slot->scan)) st_read_here.fetch_add(1, std::memory_order_relaxed);
        state = slot->state.load(std::memory_order_acquire);
        if (state == 0) return why = SM_NOT_PARSED, nullptr;   // (another slot of its code still reads the family's variants)
    }
    if (state == 4) return why = SM_TASK_MESH, nullptr;
    if (state != 1) return why = SM_NOT_GRAPHICS, nullptr;
    const uint64_t key = slot->task ? task_mesh_key(slot->task_key, slot->vs.key, slot->fs.key, slot->has_fs ? slot->fs.sph : nullptr,
                                                    slot->has_prim ? slot->prim.sph : nullptr, color_types)
                         : slot->mesh ? mesh_key(slot->vs.key, slot->fs.key, slot->has_fs ? slot->fs.sph : nullptr, slot->has_prim ? slot->prim.sph : nullptr, color_types)
                                      : graphics_key(slot->vs.key, slot->fs.key, slot->has_fs ? slot->fs.sph : nullptr, color_types) ^
                                            (slot->has_gs ? hash_bytes("geom", 4, slot->gs.key) : 0);
    for (const Variant* v = slot->fam->variants.load(std::memory_order_acquire); v; v = v->next)
        if (v->d.key == key) return check_textures(s, v->d), why = SM_NONE, &v->d;
    // (none: translated on a worker, once for these colour types)
    st_misses.fetch_add(1, std::memory_order_relaxed);
    last_miss_s.store(seconds_since_device(), std::memory_order_relaxed);
    {
        std::lock_guard lk(slot->fam->mu);
        if (slot->fam->failed.count(key)) return why = SM_FAILED, nullptr;
        if (!slot->fam->pending.insert(key).second) return why = SM_TRANSLATING, nullptr;
    }
    struct Job {
        Slot* slot;
        uint64_t key;
        uint8_t ct[8];
    };
    auto job = std::make_shared<Job>();
    job->slot = slot, job->key = key;
    std::memcpy(job->ct, color_types, 8);
    st_queued.fetch_add(1, std::memory_order_relaxed);
    work::run(work::P0, [job] {
        const auto t0 = clk::now();
        Slot& sl = *job->slot;
        Code vsc, fsc;
        bool ok = false;
        auto v = std::make_unique<Variant>();
        v->d.key = job->key;
        int vs = -1, fs = -1, ms = -1, prim = -1, ts = -1, gs = -1;
        Code codes[6];
        for (uint32_t i = 0; i < sl.prog.nstage && i < 6; i++)
            if (parse_code(sl.prog.sd[i], codes[i])) {
                if (codes[i].header_only) prim = codes[i].stage != 6 ? (int)i : prim;
                else if (codes[i].stage == 0) vs = (int)i;
                else if (codes[i].stage == 1) fs = (int)i;
                else if (codes[i].stage == 7) ms = (int)i;
                else if (codes[i].stage == 6) ts = (int)i;
                else if (codes[i].stage == 2 && gs < 0) gs = (int)i;
            }
        // (M4c: a task stage only where the slot found one, and then always)
        const bool made = (ts >= 0) == sl.task && (fs >= 0) == sl.has_fs && (gs >= 0) == sl.has_gs &&
                          (sl.task ? ms >= 0 && translate_task_mesh_variant(*v, codes[ts], codes[ms], prim >= 0 ? &codes[prim] : nullptr,
                                                                            fs >= 0 ? &codes[fs] : nullptr, job->ct)
                           : sl.mesh ? ms >= 0 && translate_mesh_variant(*v, codes[ms], prim >= 0 ? &codes[prim] : nullptr, fs >= 0 ? &codes[fs] : nullptr, job->ct)
                                     : vs >= 0 && translate_variant(*v, codes[vs], gs >= 0 ? &codes[gs] : nullptr, fs >= 0 ? &codes[fs] : nullptr, job->ct));
        if (made) {
            disk_save(*v, slot_pair(sl));
            v->d.codes[0] = sl.vs.key, v->d.codes[1] = sl.has_fs ? sl.fs.key : 0, v->d.codes[2] = sl.has_prim ? sl.prim.key : sl.has_gs ? sl.gs.key : 0;
            v->d.task_code = sl.task ? sl.task_key : 0;
            ok = finish(*v);
        }
        st_translate_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed);
        if (ok) {
            st_translated.fetch_add(1, std::memory_order_relaxed);
            count_code(*v);
            publish(*sl.fam, v.release(), sl.serial);
        } else {
            st_failed.fetch_add(1, std::memory_order_relaxed);
        }
        std::lock_guard lk(sl.fam->mu);
        sl.fam->pending.erase(job->key);
        if (!ok) sl.fam->failed.insert(job->key);
    });
    return why = SM_TRANSLATING, nullptr;
}

// ---- ahead of the draws: a program from its stages' bytes (shader_warm, the game's --precompile) ----
PreResult shader_precompile(const PreStage* st, int n) {
    Code codes[6];
    int vs = -1, fs = -1, mesh = -1, task = -1, prim = -1, gs = -1;
    bool other = n > 6;
    for (int i = 0; i < n && i < 6; i++) {
        uint32_t end = 0;
        std::vector<uint8_t> area(st[i].area, st[i].area + st[i].area_n);
        if (!control_end(st[i].ctl, &end) || end != st[i].area_n || !parse_bytes(st[i].ctl, area, codes[i])) return PRE_FAILED;
        const Code& c = codes[i];
        if (c.stage == 5) return PRE_OTHER;   // (compute: its translation reads the dispatch's bank)
        if (c.stage == 6) task = i;
        else if (c.header_only) prim = i;
        else if (c.stage == 0) vs = i;
        else if (c.stage == 1) fs = i;
        else if (c.stage == 7) mesh = i;
        else if (c.stage == 2 && gs < 0) gs = i;
        else other = true;
    }
    // (what scan_slot draws: a vertex program with a geometry stage only on a device with them, a mesh-only or task+mesh one)
    if (gs >= 0 && (task >= 0 || mesh >= 0 || !dev.features.geometryShader)) other = true;
    if (task >= 0 && (mesh < 0 || vs >= 0 || other || codes[task].header_only || codes[mesh].header_only)) return PRE_OTHER;
    const bool is_mesh = mesh >= 0 && vs < 0 && !other;
    if (!is_mesh && (vs < 0 || other || mesh >= 0)) return PRE_OTHER;
    const uint8_t ct[8]{};   // (float targets: every pass the logs saw; another target type is its own variant, made at its draw)
    const Code* fsc = fs >= 0 ? &codes[fs] : nullptr;
    const Code* pc = prim >= 0 ? &codes[prim] : nullptr;
    const Code* gsc = gs >= 0 ? &codes[gs] : nullptr;
    const Code& first = is_mesh ? codes[mesh] : codes[vs];
    const uint64_t key = task >= 0 ? task_mesh_key(codes[task].key, first.key, fsc ? fsc->key : 0, fsc ? fsc->sph : nullptr, pc ? pc->sph : nullptr, ct)
                         : is_mesh ? mesh_key(first.key, fsc ? fsc->key : 0, fsc ? fsc->sph : nullptr, pc ? pc->sph : nullptr, ct)
                                   : graphics_key(first.key, fsc ? fsc->key : 0, fsc ? fsc->sph : nullptr, ct) ^ (gsc ? hash_bytes("geom", 4, gsc->key) : 0);
    const uint64_t pair = task >= 0 ? pair_key(hash_bytes("task", 4, codes[task].key) ^ hash_bytes("mesh", 4, first.key), fsc ? fsc->key : 0)
                                    : pair_key_of(is_mesh, first.key ^ (gsc ? hash_bytes("geom", 4, gsc->key) : 0), fsc ? fsc->key : 0);
    const size_t want = 1u + (fsc != nullptr) + (task >= 0) + (gsc != nullptr);
    auto v = std::make_unique<Variant>();
    bool kept = false;
    disk_init();
    std::vector<DiskRecord> recs;
    {
        std::lock_guard lk(disk_mu);
        if (const auto it = disk_by_pair.find(pair); it != disk_by_pair.end()) recs = it->second;
    }
    for (const DiskRecord& d : recs) {
        auto r = std::make_unique<Variant>();
        if (disk_read(d, *r) && r->d.key == key && r->stages.size() == want) {
            v = std::move(r), kept = true;
            break;
        }
    }
    if (!kept) {
        v->d.key = key;
        const bool made = task >= 0 ? translate_task_mesh_variant(*v, codes[task], codes[mesh], pc, fsc, ct)
                          : is_mesh ? translate_mesh_variant(*v, codes[mesh], pc, fsc, ct)
                                    : translate_variant(*v, codes[vs], gsc, fsc, ct);
        if (!made) return PRE_FAILED;
        disk_save(*v, pair);
    }
    v->d.codes[0] = first.key, v->d.codes[1] = fsc ? fsc->key : 0, v->d.codes[2] = pc ? pc->key : gsc ? gsc->key : 0;
    v->d.task_code = task >= 0 ? codes[task].key : 0;
    const bool ok = finish(*v);   // (its objects: made from SPIR-V once per driver, their binaries kept)
    for (VkShaderEXT o : {v->d.vs, v->d.fs, v->d.gs, v->d.ms, v->d.ts})
        if (o) vkDestroyShaderEXT(dev.device, o, nullptr);
    return !ok ? PRE_FAILED : kept ? PRE_KEPT : PRE_MADE;
}

void shader_cache_index() {
    {
        std::lock_guard lk(disk_mu);
        if (disk_on) index_save(exe_dir() + "ounce_renderer_programs.bin.idx", disk_stamp_now, disk_end, disk_index);
    }
    sobin::index_now();
}

uint64_t shader_cache_stamp() {
    disk_init();
    sobin::init();
    const sobin::Header h = sobin::want();
    return hash_bytes(&h, sizeof h, disk_stamp());
}

void shader_held_stats(char* out, size_t cap) {
    std::snprintf(out, cap, "; shaders: %lld objects for %lld draw variants, %lld compute objects (no byte figure either)",
                  (long long)st_objects_made.load(), (long long)(st_translated.load() + st_from_disk.load()),
                  (long long)compute_objects_made());
}

void shader_stats(char* out, size_t cap, double frames) {
    const size_t queued = work::queued();
    char pool[160];
    work::stats(pool, sizeof pool);
    std::snprintf(out, cap, "programs (since the start): %lld set (%lld distinct codes), %lld variants translated (%.1f s on the workers), %lld failed, %lld "
                  "from the cache (%.1f s read), %lld shader objects made (%.1f s, %lld failed; %lld from binaries, %lld binaries saved, %lld refused), %lld stages unreadable, table full %lld; misses %.1f a frame, "
                  "%zu jobs waiting; variants past a program's first (other colour target types): %lld; bound textures that contradict the translation: %lld; game buffer words "
                  "asked at translation %lld; uniform buffers a variant reads: %.1f on average, %lld at most; the last miss %.2f s after the device; stages through the register-pressure pass at translation %lld, changed %lld (%lld instructions moved, %.1f ms in all); shader workers: %s; programs a draw moved ahead of the load's %lld, read at their first draw %lld", (long long)st_slots.load(), (long long)st_families.load(), (long long)st_translated.load(), st_translate_ns.load() / 1e9,
                  (long long)st_failed.load(), (long long)st_from_disk.load(), st_disk_ns.load() / 1e9, (long long)st_objects_made.load(),
                  st_object_ns.load() / 1e9,
                  (long long)st_object_failed.load(), (long long)sobin::st_hits.load(), (long long)sobin::st_saved.load(),
                  (long long)sobin::st_refused.load(), (long long)st_scan_failed.load(), (long long)st_full.load(),
                  st_misses.exchange(0) / frames, queued, (long long)st_multi_inputs.load(), (long long)st_contradicted.load(),
                  (long long)st_game_words.load(), st_ubos_variants.load() ? (double)st_ubos_sum.load() / st_ubos_variants.load() : 0.0,
                  (long long)st_ubos_max.load(), last_miss_s.load(), (long long)st_late_stages.load(), (long long)st_late_changed.load(),
                  (long long)st_late_moved.load(), st_late_ns.load() / 1e6, pool, (long long)st_bumped.load(), (long long)st_read_here.load());
}

}  // namespace hsd

// ---- the pieces compute programs share with these (vk_shader_parts.h; vk_compute.cpp) ----
namespace hsd::shader_parts {
bool read_stage(const nvn_stage_data& sd, uint8_t ctl[0xd8], std::vector<uint8_t>& area) { return read_package(sd, ctl, area); }
const Shader::Profile& stage_profile(VkShaderStageFlags stage) { return profile_graphics(stage); }
Shader::HostTranslateInfo translate_info() { return host_info(); }
VkDeviceAddress put_c1(const std::vector<uint8_t>& bytes, uint32_t used) { return c1_put(bytes, used); }
void info_put(std::vector<uint8_t>& o, const Shader::Info& i) { put_info(o, i); }
bool info_get(const uint8_t*& p, const uint8_t* end, Shader::Info& i) {
    Reader r{p, end};
    get_info(r, i);
    p = r.p;
    return !r.bad;
}
uint64_t device_stamp() { return hsd::device_stamp(); }
uint64_t object_key(const VkShaderCreateInfoEXT& ci, bool size32) {
    sobin::init();
    return sobin::key_of(ci, size32);
}
std::vector<uint8_t> object_binary(uint64_t key) { return sobin::find(key); }
void object_keep(uint64_t key, VkShaderEXT obj) { sobin::save(key, obj); }
std::string program_dir() { return exe_dir(); }
uint8_t answer_of(uint32_t handle) { return live_answer(handle); }
uint8_t answer_canonical(uint8_t type, bool dim_fixed, uint8_t desc_type, bool depth_float) {
    return canonical(type, dim_fixed, desc_type, depth_float);
}
}  // namespace hsd::shader_parts
