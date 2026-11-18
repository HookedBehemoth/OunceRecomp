// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// (fork) The direct Ampere front's control flow graph, built from the native 16-byte instructions: relative branches
// (with BRA's second predicate), jump tables (BRX/BRXU, the table tracked in the native code), CALL.REL.NOINC with a
// return link and RET.REL.NODEC as Eden's inlined calls, EXIT, KILL, and every other guarded instruction as a
// conditional block. Reconvergence (BSSY, BSYNC, BREAK, WARPSYNC) has no data semantics under SPIR-V's structured
// merges and is ignored; barrier-register moves and anything else that transfers control fail the translation.

#pragma once

#include <utility>
#include <vector>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/decode.h"
#include "shader_recompiler/frontend/common/flow.h"
#include "shader_recompiler/object_pool.h"

namespace Shader::Ampere::Flow {

using Shader::Flow::Block;
using Shader::Flow::EndClass;
using Shader::Flow::Function;
using Shader::Flow::FunctionId;
using Shader::Flow::Label;
using Shader::Flow::Stack;

/// A block's locations are byte offsets from the program's first instruction plus this bias, so that the virtual block
/// of a guarded first instruction (4 bytes before it) has a location
constexpr u32 LOCATION_BIAS = 16;

[[nodiscard]] constexpr Location LocationOf(u32 byte_offset) {
    return Location{byte_offset + LOCATION_BIAS, LocationStride::Ampere};
}
[[nodiscard]] constexpr u32 ByteOffsetOf(Location location) {
    return location.Offset() - LOCATION_BIAS;
}

/// The instruction at a location: env.ReadInstruction128 at start_address plus its byte offset
[[nodiscard]] Insn ReadInsn(Environment& env, Location location);

class CFG : public Shader::Flow::CFG {
    enum class AnalysisState {
        Branch,
        Continue,
    };

public:
    explicit CFG(Environment& env, ObjectPool<Block>& block_pool, bool exits_to_dispatcher = false);

private:
    void AnalyzeLabel(FunctionId function_id, Label& label);
    bool InspectVisitedBlocks(FunctionId function_id, const Label& label);
    AnalysisState AnalyzeInst(Block* block, FunctionId function_id, Location pc);

    /// The instruction at pc runs when cond holds: block becomes a virtual block branching on it to a block of the
    /// instruction alone (or ends before pc, which is analyzed again as its own block). Returns that block, nullptr in
    /// the second case.
    Block* AnalyzeCondInst(Block* block, FunctionId function_id, Location pc, EndClass insn_end_class,
                           IR::Condition cond);

    AnalysisState AnalyzeBRA(Block* block, FunctionId function_id, Location pc, const Insn& insn);
    AnalysisState AnalyzeBRX(Block* block, FunctionId function_id, Location pc, const Insn& insn);
    AnalysisState AnalyzeEXIT(Block* block, FunctionId function_id, Location pc, const Insn& insn);
    AnalysisState AnalyzeCALL(Block* block, FunctionId function_id, Location pc, const Insn& insn);
    AnalysisState AnalyzeRET(Block* block, FunctionId function_id, Location pc, const Insn& insn);

    /// A relative branch's target, checked to be an instruction of the program
    Location BranchTarget(Location pc, const Insn& insn) const;

    Block* AddLabel(Block* block, Stack stack, Location pc, FunctionId function_id);

    Environment& env;
    ObjectPool<Block>& block_pool;
    Location program_start;
    Block* dispatch_block{};
    /// Exits without an instruction (EXIT's second predicate), placed after their function's blocks once it is built
    std::vector<std::pair<FunctionId, Block*>> late_exits;
    /// Per function: the register holding its return address (main: none)
    std::vector<u32> function_links;
};

} // namespace Shader::Ampere::Flow
