// Small IR programs through the translator fork's passes and SPIR-V back end: the uniform registers and predicates
// through the SSA rewrite, SubgroupReadFirstLane and the eight SubgroupReduce ops, and the maximal-reconvergence mode.
// Writes one module per case into the folder it is given; the test validates and disassembles them.
#include <cstdio>
#include <string>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"

using namespace Shader;

struct Env final : Environment {
    explicit Env(Stage s) { stage = s; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 64; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}
};

struct Case {
    bool wider;            // (the profile's subgroup may be wider than the guest's 32 lanes)
    bool reconvergence;    // (the profile supports maximal reconvergence)
};

std::vector<u32> module(const Case& c) {
    ObjectPool<IR::Inst> insts;
    ObjectPool<IR::Block> blocks;
    IR::Program program;
    IR::Block* const block{blocks.Create(insts)};
    IR::IREmitter ir{*block};
    // (a lane's value through uniform registers and predicates, as an Ampere front writes them)
    ir.SetReg(IR::UniformReg(5), ir.LaneId());
    ir.SetReg(IR::UniformReg(63), ir.IAdd(ir.GetReg(IR::UniformReg(5)), ir.Imm32(3u)));
    ir.SetPred(IR::Pred::UP2, ir.IEqual(ir.GetReg(IR::UniformReg(5)), ir.Imm32(0u)));
    ir.SetPred(IR::Pred::UP6, ir.LogicalNot(ir.GetPred(IR::Pred::UP2)));
    const IR::U32 value{ir.Select(ir.GetPred(IR::Pred::UP6), ir.GetReg(IR::UniformReg(63)), ir.Imm32(7u))};
    std::vector<IR::U32> results{ir.SubgroupReadFirstLane(value)};
    using Op = IR::IREmitter::SubgroupReduceOp;
    for (Op op : {Op::IAdd, Op::UMin, Op::UMax, Op::SMin, Op::SMax, Op::And, Op::Or, Op::Xor}) {
        results.push_back(ir.SubgroupReduce(op, value));
    }
    for (u32 i = 0; i < results.size(); ++i) {
        ir.WriteShared(32, ir.Imm32(4 * i), results[i]);
    }
    ir.Epilogue();

    program.blocks = {block};
    program.post_order_blocks = {block};
    program.syntax_list.push_back({.data{.block = block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    program.stage = Stage::Compute;
    program.workgroup_size = {32, 1, 1};
    program.shared_memory_size = 64;
    program.info.maximal_reconvergence = true;

    Env env{Stage::Compute};
    Optimization::SsaRewritePass(program);
    Optimization::IdentityRemovalPass(program);
    Optimization::DeadCodeEliminationPass(program);
    Optimization::CollectShaderInfoPass(env, program);

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_vote = true;
    profile.supported_subgroup_stages = ~0u;
    profile.support_int64 = true;
    profile.warp_size_potentially_larger_than_guest = c.wider;
    profile.max_subgroup_size = c.wider ? 64 : 32;
    profile.support_maximal_reconvergence = c.reconvergence;
    return Backend::SPIRV::EmitSPIRV(profile, program);
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const Case cases[]{{false, true}, {true, true}, {false, false}};
    for (size_t i = 0; i < std::size(cases); ++i) {
        std::vector<u32> code;
        try {
            code = module(cases[i]);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "case %zu: %s\n", i, e.what());
            return 2;
        }
        const std::string name{std::string(argv[1]) + "/" + std::to_string(i) + ".spv"};
        FILE* f{std::fopen(name.c_str(), "wb")};
        if (!f) return 3;
        const size_t n{std::fwrite(code.data(), 4, code.size(), f)};
        std::fclose(f);
        if (n != code.size()) return 4;
    }
}
