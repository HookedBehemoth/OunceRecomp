// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// (fork) The ISA-neutral end of a translation: FinishProgram makes a program of a front's abstract syntax list and runs
// the pass pipeline (SSA, constant propagation, storage buffers, textures, info collection) over it; and the program
// transforms that work on IR alone.

#pragma once

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader {
struct HostTranslateInfo;

/// host_info: as normalized by ApplyDescriptorLimitPolicy, the one BuildASL was given
[[nodiscard]] IR::Program FinishProgram(IR::AbstractSyntaxList&& syntax_list, Environment& env,
                                        const HostTranslateInfo& host_info);

[[nodiscard]] IR::Program MergeDualVertexPrograms(IR::Program& vertex_a, IR::Program& vertex_b,
                                                  Environment& env_vertex_b);

void ConvertLegacyToGeneric(IR::Program& program, const RuntimeInfo& runtime_info);

// Maxwell v1 and older Nvidia cards don't support setting gl_Layer from non-geometry stages.
// This creates a workaround by setting the layer as a generic output and creating a
// passthrough geometry shader that reads the generic and sets the layer.
[[nodiscard]] IR::Program GenerateGeometryPassthrough(ObjectPool<IR::Inst>& inst_pool,
                                                      ObjectPool<IR::Block>& block_pool,
                                                      const HostTranslateInfo& host_info,
                                                      IR::Program& source_program,
                                                      Shader::OutputTopology output_topology);

} // namespace Shader
