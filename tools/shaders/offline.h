// The direct back end's translation of one NVN2 stage, offline (no device, no game state): its code read from a control
// section and code area (as the romfs seed holds them), the translator's questions answered as vk_shader.cpp /
// vk_compute.cpp answer them with nothing bound, and the profile they translate with on this card. Shared by meshtool
// (tools/shaders/meshtool.cpp) and the shader census (tools/shadercensus). Nothing of the game is in this file.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/control_flow.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/translate_program.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/ir_opt/oot_visual_workaround.h"

namespace Shader::Optimization {   // (the compute bank's layout the global-memory pass reads: vk_compute.cpp sets them)
extern thread_local u32 g_nvn2_compute_ssbo_base, g_nvn2_compute_ssbo_count, g_nvn2_compute_layout_end;
}

namespace offline {

constexpr uint32_t kControl = 0xd8, kAmpereHeader = 128;

/// (the differential phase of work/shader-plan, M3-M9: censustool --front new) the direct Ampere front instead of the
/// lowering to Maxwell
inline bool new_front = false;

inline uint64_t fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {   // (vk_shader.cpp hash_bytes)
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 0x100000001b3ull;
    return h;
}

struct Code {
    uint32_t stage = 0;   // (NVN stage: 0 vertex, 1 fragment, 5 compute, 6 task, 7 mesh)
    uint64_t key = 0;     // (the back end's code key: the control section and code area hashed)
    uint32_t threads = 0, payload_bytes = 0, max_vertices = 0, max_primitives = 0, topology = 0, native = 0, local_mem = 0;
    uint32_t texture_bound = ~0u, local[3]{1, 1, 1}, shared = 0;
    uint32_t cs_ubo = 0, cs_ssbo = 0, cs_tex = 0, cs_img = 0, cs_push = 0;   // (compute: vk_compute.cpp Layout)
    bool header_only = false;
    std::vector<uint64_t> code;
    std::vector<u128> native_code;   // (the native instructions up to the program's end, for the new front)
    std::vector<uint8_t> c1;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
    uint32_t cs_ssbo_at() const { return 0x10 + cs_push + 16 * cs_ubo; }
};

/// A stage from its control section (kControl bytes) and code area (as vk_shader.cpp parse_code / vk_compute.cpp lower)
inline Code load(const uint8_t* ctl, const uint8_t* area_p, size_t area_n) {
    Code c;
    std::vector<uint8_t> area(area_p, area_p + area_n);
    uint32_t len, c1o, w58, w60;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&c1o, ctl + 0x40, 4), std::memcpy(&st, ctl + 0x56, 2);
    std::memcpy(&w58, ctl + 0x58, 4), std::memcpy(&w60, ctl + 0x60, 4);
    c.stage = st;
    c.key = fnv(area.data(), area.size(), fnv(ctl, kControl));
    const bool graphics = st != 5;
    if (st == 6 || st == 7)
        c.threads = w58 & 0xffff, c.payload_bytes = w58 >> 16, c.max_primitives = w60 & 0xffff, c.max_vertices = w60 >> 16, c.topology = ctl[0x5f];
    if (!graphics) {
        std::memcpy(c.local, ctl + 0x58, 12), std::memcpy(&c.shared, ctl + 0x64, 4);
        c.cs_ubo = ctl[0xb1], c.cs_ssbo = ctl[0xb2], c.cs_tex = ctl[0xb3], c.cs_img = ctl[0xb6];
        c.cs_push = ((uint32_t)ctl[0xa9] * 4 + 15) & ~15u;
    }
    const uint32_t hdr = graphics ? kAmpereHeader : 0;
    if (graphics) {
        if (area.size() < hdr) throw Shader::RuntimeError("code area too short");
        std::memcpy(c.sph, area.data(), sizeof c.sph);
        if (len <= hdr + 16) {
            c.header_only = true;
            return c;
        }
    }
    using Shader::Ampere::Instruction;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState cap;
    for (uint32_t i = 0; hdr + (i + 1) * 16 <= len && hdr + (i + 1) * 16 <= area.size(); i++) {
        Instruction& insn = native.emplace_back();
        std::memcpy(&insn, area.data() + hdr + i * 16, 16);
        const auto r = cap.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) throw Shader::RuntimeError("bad branch target");
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    c.native = (uint32_t)native.size();
    if (new_front) {
        for (const Instruction& insn : native)
            c.native_code.push_back({uint64_t(insn.words[0]) | uint64_t(insn.words[1]) << 32, uint64_t(insn.words[2]) | uint64_t(insn.words[3]) << 32});
        c.code.assign(sizeof(Shader::ProgramHeader) / 8, 0);
        if (graphics) std::memcpy(c.code.data(), area.data(), sizeof(Shader::ProgramHeader));
        if (c1o <= area.size()) c.c1.assign(area.begin() + c1o, area.end());
        if (graphics) {
            Shader::ProgramHeader h;
            std::memcpy(&h, c.sph, sizeof h);
            c.local_mem = (uint32_t)h.LocalMemorySize() + h.common3.shader_local_memory_crs_size;
        }
        return c;
    }
    Shader::Ampere::native_matrix_multiply = !graphics;   // (compute: the matrix multiplies kept whole, as vk_compute.cpp)
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    Shader::Ampere::native_matrix_multiply = false;
    if (!lowered) throw Shader::RuntimeError("lowering failed at {:#x}: {}", lowered.error_offset, lowered.error);
    c.texture_bound = lowered.required_texture_buffer;
    c.code.assign(sizeof(Shader::ProgramHeader) / 8, 0);
    if (graphics) std::memcpy(c.code.data(), area.data(), sizeof(Shader::ProgramHeader));
    for (const uint64_t x : lowered.instructions) {
        if (c.code.size() * 8 % 32 == 0) c.code.push_back(0);
        c.code.push_back(x);
    }
    c.code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(c.code, lowered.source_instruction_offsets);
    if (c1o <= area.size()) c.c1.assign(area.begin() + c1o, area.end());
    if (graphics) {
        Shader::ProgramHeader h;
        std::memcpy(&h, c.sph, sizeof h);
        c.local_mem = (uint32_t)h.LocalMemorySize() + h.common3.shader_local_memory_crs_size;
    }
    return c;
}
/// A stage .bin (the control section then the code area, as M0's seed.py writes them)
inline Code load_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
    if (b.size() < kControl + kAmpereHeader) throw Shader::RuntimeError("{}: too short", path);
    return load(b.data(), b.data() + kControl, b.size() - kControl);
}

/// The translator's questions with nothing bound (vk_shader.cpp Env / vk_compute.cpp Env): driver bank words 0, game
/// buffers not read, c1 the program's own, the storage tables as direct's
class Env final : public Shader::Environment {
public:
    Env(const Code& c, Shader::Stage st, std::array<u32, 3> regs = {}) : nvn(c) {
        stage = st;
        start_address = 0;
        if (st != Shader::Stage::Compute) std::memcpy(&sph, c.code.data(), sizeof sph);
        is_proprietary_driver = true;
        mesh_registers = regs;
    }
    u64 ReadInstruction(u32 a) override { return a / 8 < nvn.code.size() ? nvn.code[a / 8] : 0; }
    u128 ReadInstruction128(u32 a) override {
        if (a % 16 != 0 || a / 16 >= nvn.native_code.size()) throw Shader::RuntimeError("instruction read past the program at {:#x}", a);
        return nvn.native_code[a / 16];
    }
    u32 TranslateProgramAddress(u32 a) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(nvn.code, a)) return *t;
        throw Shader::RuntimeError("indirect branch to {:#x}", a);
    }
    u32 ReadCbufValue(u32 i, u32 o) override {
        if (i == 1) {
            uint32_t v = 0;
            if ((size_t)o + 4 <= nvn.c1.size()) std::memcpy(&v, nvn.c1.data() + o, 4);
            return v;
        }
        if (i >= 3) game_words++;
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return nvn.texture_bound != ~0u ? nvn.texture_bound : stage == Shader::Stage::Compute ? 0 : 2; }
    u32 LocalMemorySize() const override { return stage == Shader::Stage::Compute ? 0 : nvn.local_mem; }
    u32 SharedMemorySize() const override { return stage == Shader::Stage::Compute ? nvn.shared : 0; }
    std::array<u32, 3> WorkgroupSize() const override {
        if (stage != Shader::Stage::Compute) return {1, 1, 1};
        return {std::max(1u, nvn.local[0]), std::max(1u, nvn.local[1]), std::max(1u, nvn.local[2])};
    }
    bool HasHLEMacroState() const override {
        return stage == Shader::Stage::VertexA || stage == Shader::Stage::VertexB || stage == Shader::Stage::Compute;
    }
    bool HasNativeInstructions() const override { return true; }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        if (stage == Shader::Stage::Compute) {
            const uint32_t base = nvn.cs_ssbo_at();
            return index == 0 && offset >= base && offset < base + 16 * nvn.cs_ssbo && (offset - base) % 16 == 0;
        }
        static constexpr uint32_t kSsboTable[8] = {0x110, 0x510, 0x410, 0x210, 0x310, 0, 0x670, 0x770};
        const uint32_t base = nvn.stage < 8 ? kSsboTable[nvn.stage] : 0;
        return index == 0 && base && offset >= base && offset < base + 256 && (offset - base) % 16 == 0;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        if (bank != 0 || !HasHLEMacroState()) return std::nullopt;
        if (stage == Shader::Stage::Compute) {
            switch (offset) {
            case 0x0: return Shader::ReplaceConstant::NumWorkgroupsX;
            case 0x4: return Shader::ReplaceConstant::NumWorkgroupsY;
            case 0x8: return Shader::ReplaceConstant::NumWorkgroupsZ;
            default: return std::nullopt;
            }
        }
        switch (offset) {
        case 0x640: return Shader::ReplaceConstant::BaseVertex;
        case 0x644: return Shader::ReplaceConstant::BaseInstance;
        case 0x648: return Shader::ReplaceConstant::DrawID;
        default: return std::nullopt;
        }
    }
    void Dump(u64, u64) override {}
    const Code& nvn;
    int game_words = 0;
};

/// vk_shader.cpp make_profile on this card (RX 6950 XT, RADV), pinned to 32 lanes as profile_graphics; and its
/// per-GPU settings (frag_nan_guard, position_invariant on AMD)
inline bool explicit_shared = false;   // (the translator-fix prototype: shared memory in explicit layout, typed 8/16-bit access)
inline Shader::Profile profile() {
    Shader::Backend::SPIRV::frag_nan_guard = true;
    Shader::Backend::SPIRV::position_invariant = true;
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_vote = true;
    p.supported_subgroup_stages = 0x7fffffff;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.max_subgroup_size = 32;
    p.min_ssbo_alignment = 4;
    p.ssbo_bda = true, p.ssbo_bda_wide = true, p.heap_handles = true;
    if (explicit_shared) p.support_explicit_workgroup_layout = p.support_workgroup_layout_8bit_access = p.support_workgroup_layout_16bit_access = true;
    return p;
}
inline Shader::HostTranslateInfo host_info() {
    Shader::HostTranslateInfo h{};
    h.support_float64 = h.support_float16 = h.support_int64 = true;
    h.support_snorm_render_buffer = h.support_viewport_index_layer = h.support_conditional_barrier = true;
    h.min_ssbo_alignment = 4, h.heap_handles = true;
    return h;
}

inline std::array<u32, 3> regs_of(const Code& m, const Code* t) {   // (vk_shader.cpp mesh_registers_of)
    const uint32_t topology = m.topology == 3 ? 4 : m.topology == 1 ? 0 : m.topology;
    const uint32_t most = std::max(m.max_vertices, m.max_primitives);
    const uint32_t lines = t ? (t->payload_bytes + 127) / 128 : 0;
    return {(topology & 0xF) | (std::min(most, 0x100u) << 4) | (std::min(most, 0x100u) << 13), std::min(m.threads, 0x3Fu) << 20,
            (t ? std::min(t->threads, 0x3Fu) : 0u) | (std::min(lines, 0x1FFu) << 12) | (std::min(lines, 0x1FFu) << 22)};
}

/// One translation and the pools it lives in
struct Translation {
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow;
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::IR::Program program;
};
/// The stage translated to IR (one exit for task and mesh stages, as the back end translates them)
inline std::unique_ptr<Translation> translate(Env& env) {
    auto t = std::make_unique<Translation>();
    const bool cs = env.ShaderStage() == Shader::Stage::Compute;
    const bool one_exit = env.ShaderStage() == Shader::Stage::Mesh || env.ShaderStage() == Shader::Stage::Task;
    if (cs) {   // (vk_compute.cpp translate_variant: the bank's storage table for the global-memory pass)
        Shader::Optimization::g_nvn2_compute_ssbo_base = env.nvn.cs_ssbo ? env.nvn.cs_ssbo_at() : 0;
        Shader::Optimization::g_nvn2_compute_ssbo_count = env.nvn.cs_ssbo ? env.nvn.cs_ssbo : 16;
        Shader::Optimization::g_nvn2_compute_layout_end = 0;
    }
    if (new_front) {
        Shader::Ampere::Flow::CFG cfg(env, t->flow, one_exit);
        t->program = Shader::Ampere::TranslateProgram(t->insts, t->blocks, env, cfg, host_info());
    } else {
        Shader::Maxwell::Flow::CFG cfg(env, t->flow, sizeof(Shader::ProgramHeader), one_exit);
        t->program = Shader::Maxwell::TranslateProgram(t->insts, t->blocks, env, cfg, host_info());
    }
    if (cs) Shader::Optimization::g_nvn2_compute_ssbo_base = 0, Shader::Optimization::g_nvn2_compute_ssbo_count = 16;
    return t;
}

}  // namespace offline
