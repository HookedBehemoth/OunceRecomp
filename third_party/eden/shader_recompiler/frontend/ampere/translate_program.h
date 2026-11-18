// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// (fork) The direct Ampere front's entry: a program's control flow graph structured with the Ampere translators
// (BuildASL), then finished by the common pass pipeline (FinishProgram).

#pragma once

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/control_flow.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"

namespace Shader {
struct HostTranslateInfo;
}

namespace Shader::Ampere {

[[nodiscard]] IR::Program TranslateProgram(ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool,
                                           Environment& env, Flow::CFG& cfg, const HostTranslateInfo& host_info);

} // namespace Shader::Ampere
