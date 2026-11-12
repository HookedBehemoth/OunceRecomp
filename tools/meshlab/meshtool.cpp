// meshtool: one NVN2 mesh stage (and the task stage before it) through the direct back end's translator fork (gpu/direct/eden),
// as direct_shader.cpp translates a task+mesh or mesh-only program, offline: the IR (as the translator reads the game's code),
// a structured print of it, the mesh stage's SPIR-V, and a summary of what the stage reads and writes. For the inventory and
// the decompilation of the game's mesh stages (docs/direct/m6e-report.md); nothing of the game is in this file.
//
//   meshtool <mesh stage .bin> <out prefix> [<task stage .bin>]
// A stage .bin: the stage's control section (0xd8 bytes) then its code area, as the romfs seed holds it (M0's seed.py).
// Writes <prefix>.ir, <prefix>.ir.c, <prefix>.spv and prints the summary (one "key value" line each).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"
#include "ir_pretty.h"

namespace {

struct Code {
    uint32_t stage = 0, threads = 0, payload_bytes = 0, max_vertices = 0, max_primitives = 0, topology = 0, native = 0, local_mem = 0;
    std::vector<uint64_t> code;
    std::vector<uint8_t> c1;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
};

Code load(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
    if (b.size() < 0xd8 + 128) throw Shader::RuntimeError("{}: too short", path);
    const uint8_t* ctl = b.data();
    std::vector<uint8_t> area(b.begin() + 0xd8, b.end());
    Code c;
    uint32_t len, c1o, w58, w60;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&c1o, ctl + 0x40, 4), std::memcpy(&st, ctl + 0x56, 2);
    std::memcpy(&w58, ctl + 0x58, 4), std::memcpy(&w60, ctl + 0x60, 4);
    c.stage = st, c.threads = w58 & 0xffff, c.payload_bytes = w58 >> 16, c.max_primitives = w60 & 0xffff, c.max_vertices = w60 >> 16;
    c.topology = ctl[0x5f];
    std::memcpy(c.sph, area.data(), sizeof c.sph);
    using Shader::Ampere::Instruction;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState cap;
    for (uint32_t i = 0; 128 + (i + 1) * 16 <= len; i++) {
        Instruction& insn = native.emplace_back();
        std::memcpy(&insn, area.data() + 128 + i * 16, 16);
        const auto r = cap.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) throw Shader::RuntimeError("bad branch target");
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    c.native = (uint32_t)native.size();
    Shader::Ampere::native_matrix_multiply = false;
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    if (!lowered) throw Shader::RuntimeError("lowering failed at {:#x}: {}", lowered.error_offset, lowered.error);
    c.code.assign(sizeof(Shader::ProgramHeader) / 8, 0);
    std::memcpy(c.code.data(), area.data(), sizeof(Shader::ProgramHeader));
    for (const uint64_t x : lowered.instructions) {
        if (c.code.size() * 8 % 32 == 0) c.code.push_back(0);
        c.code.push_back(x);
    }
    c.code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(c.code, lowered.source_instruction_offsets);
    c.c1.assign(area.begin() + c1o, area.end());
    Shader::ProgramHeader h;
    std::memcpy(&h, c.sph, sizeof h);
    c.local_mem = (uint32_t)h.LocalMemorySize() + h.common3.shader_local_memory_crs_size;
    return c;
}

// (direct_shader.cpp Env with no state: driver bank words read as 0, the storage tables as direct's)
class Env final : public Shader::Environment {
public:
    Env(const Code& c, Shader::Stage st, std::array<u32, 3> regs) : nvn(c) {
        stage = st;
        start_address = 0;
        std::memcpy(&sph, c.code.data(), sizeof sph);
        is_proprietary_driver = true;
        mesh_registers = regs;
    }
    u64 ReadInstruction(u32 a) override { return a / 8 < nvn.code.size() ? nvn.code[a / 8] : 0; }
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
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return nvn.local_mem; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool HasNativeInstructions() const override { return true; }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        static constexpr uint32_t kSsboTable[8] = {0x110, 0x510, 0x410, 0x210, 0x310, 0, 0x670, 0x770};
        const uint32_t base = nvn.stage < 8 ? kSsboTable[nvn.stage] : 0;
        return index == 0 && base && offset >= base && offset < base + 256 && (offset - base) % 16 == 0;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}
    const Code& nvn;
};

Shader::Profile profile() {   // (direct_shader.cpp make_profile on this card, pinned to 32 lanes as profile_graphics)
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
    return p;
}

std::array<u32, 3> regs_of(const Code& m, const Code* t) {   // (direct_shader.cpp mesh_registers_of)
    const uint32_t topology = m.topology == 3 ? 4 : m.topology == 1 ? 0 : m.topology;
    const uint32_t most = std::max(m.max_vertices, m.max_primitives);
    const uint32_t lines = t ? (t->payload_bytes + 127) / 128 : 0;
    return {(topology & 0xF) | (std::min(most, 0x100u) << 4) | (std::min(most, 0x100u) << 13), std::min(m.threads, 0x3Fu) << 20,
            (t ? std::min(t->threads, 0x3Fu) : 0u) | (std::min(lines, 0x1FFu) << 12) | (std::min(lines, 0x1FFu) << 22)};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return std::fprintf(stderr, "meshtool <mesh.bin> <out prefix> [task.bin]\n"), 2;
    try {
        const Code m = load(argv[1]);
        Code t;
        const bool task = argc > 3;
        if (task) t = load(argv[3]);
        Env env(m, Shader::Stage::Mesh, regs_of(m, task ? &t : nullptr));
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> fb;
        Shader::ObjectPool<Shader::IR::Inst> insts;
        Shader::ObjectPool<Shader::IR::Block> blocks;
        Shader::HostTranslateInfo h{};
        h.support_float64 = h.support_float16 = h.support_int64 = true;
        h.support_snorm_render_buffer = h.support_viewport_index_layer = h.support_conditional_barrier = true;
        h.min_ssbo_alignment = 4, h.heap_handles = true;
        Shader::Maxwell::Flow::CFG cfg(env, fb, sizeof(Shader::ProgramHeader), true);
        Shader::IR::Program p = Shader::Maxwell::TranslateProgram(insts, blocks, env, cfg, h);
        const std::string out = argv[2];
        { std::ofstream o(out + ".ir"); o << Shader::IR::DumpProgram(p); }
        Shader::RuntimeInfo rt{};
        rt.previous_stage_stores.mask.set();
        rt.convert_depth_mode = true;
        rt.mesh_without_task = true;
        rt.task_buffer = task;
        Shader::Backend::Bindings b{};
        const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile(), rt, p, b);
        { std::ofstream o(out + ".spv", std::ios::binary); o.write((const char*)spv.data(), spv.size() * 4); }
        { std::ofstream o(out + ".ir.c"); o << pretty::print(p); }
        { std::ofstream o(out + ".flat.c"); o << pretty::print(p, true); }
        const auto& in = p.info;
        std::printf("native_instructions %u\nlocal_memory %u\nthreads %u\nmax_vertices %u\nmax_primitives %u\ntopology %u\npayload_words %u\n", m.native,
                    m.local_mem, p.workgroup_size[0], p.mesh.max_vertices, p.mesh.max_primitives, p.mesh.topology, p.mesh.payload_words);
        std::printf("vertex_lines");
        for (u32 l : p.mesh.vertex_lines) std::printf(" %s", Shader::IR::NameOf(static_cast<Shader::IR::Attribute>(l)).c_str());
        std::printf("\nprimitive_lines");
        for (u32 l : p.mesh.primitive_lines) std::printf(" %s", Shader::IR::NameOf(static_cast<Shader::IR::Attribute>(l)).c_str());
        std::printf("\ncbufs");
        for (u32 i = 0; i < Shader::Info::MAX_CBUFS; i++)
            if (in.constant_buffer_mask & (1u << i)) std::printf(" c%u:%u", i, in.constant_buffer_used_sizes[i]);
        std::printf("\nssbos");
        for (const auto& d : in.storage_buffers_descriptors) std::printf(" c%u+%#x%s", d.cbuf_index, d.cbuf_offset, d.is_written ? ":written" : "");
        std::printf("\ntextures %zu\nimages %zu\nglobal_memory %d\n", in.texture_descriptors.size(), in.image_descriptors.size(), (int)in.uses_global_memory);
        // (the IR's opcodes: what the stage does, counted)
        std::map<std::string, int> ops;
        for (Shader::IR::Block* bl : p.blocks)
            for (const Shader::IR::Inst& i : bl->Instructions()) ops[std::string(Shader::IR::NameOf(i.GetOpcode()))]++;
        std::printf("ops");
        for (const auto& [k, n] : ops) std::printf(" %s:%d", k.c_str(), n);
        std::printf("\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
    return 0;
}
