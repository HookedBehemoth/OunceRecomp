// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/common/flow.h"
#include "shader_recompiler/frontend/maxwell/instruction.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"
#include "shader_recompiler/object_pool.h"

namespace Shader::Maxwell::Flow {

using Shader::Flow::Block;
using Shader::Flow::EndClass;
using Shader::Flow::Function;
using Shader::Flow::FunctionId;
using Shader::Flow::IndirectBranch;
using Shader::Flow::Label;
using Shader::Flow::Stack;
using Shader::Flow::StackEntry;
using Shader::Flow::Token;

/// (fork) The Maxwell front's control flow builder.
class CFG : public Shader::Flow::CFG {
    enum class AnalysisState {
        Branch,
        Continue,
    };

public:
    /// start_address: the program's first instruction, a Maxwell location
    explicit CFG(Environment& env, ObjectPool<Block>& block_pool, u32 start_address,
                 bool exits_to_dispatcher = false);

private:
    void AnalyzeLabel(FunctionId function_id, Label& label);

    /// Inspect already visited blocks.
    /// Return true when the block has already been visited
    bool InspectVisitedBlocks(FunctionId function_id, const Label& label);

    AnalysisState AnalyzeInst(Block* block, FunctionId function_id, Location pc);

    void AnalyzeCondInst(Block* block, FunctionId function_id, Location pc, EndClass insn_end_class,
                         IR::Condition cond);

    /// Return true when the branch instruction is confirmed to be a branch
    bool AnalyzeBranch(Block* block, FunctionId function_id, Location pc, Instruction inst,
                       Opcode opcode);

    void AnalyzeBRA(Block* block, FunctionId function_id, Location pc, Instruction inst,
                    bool is_absolute);
    AnalysisState AnalyzeBRX(Block* block, Location pc, Instruction inst, bool is_absolute,
                             FunctionId function_id);
    AnalysisState AnalyzeEXIT(Block* block, FunctionId function_id, Location pc, Instruction inst);

    /// Return the branch target block id
    Block* AddLabel(Block* block, Stack stack, Location pc, FunctionId function_id);

    Environment& env;
    ObjectPool<Block>& block_pool;
    Location program_start;
    Block* dispatch_block{};
};

} // namespace Shader::Maxwell::Flow
