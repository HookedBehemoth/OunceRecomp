// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// (fork) BuildASL: a front's control flow graph structured into the IR's abstract syntax list (gotos removed into
// conditionals, loops and break variables), each code block's instructions emitted by the front's callback.

#pragma once

#include <functional>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/common/flow.h"
#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/object_pool.h"

namespace Shader {
struct HostTranslateInfo;

/// Emits the instructions of flow_block, from begin up to end, into block.
using TranslateBlock = std::function<void(IR::Block& block, const Flow::Block& flow_block)>;

[[nodiscard]] IR::AbstractSyntaxList BuildASL(ObjectPool<IR::Inst>& inst_pool,
                                              ObjectPool<IR::Block>& block_pool, Environment& env,
                                              Flow::CFG& cfg, const HostTranslateInfo& host_info,
                                              const TranslateBlock& translate);

} // namespace Shader
