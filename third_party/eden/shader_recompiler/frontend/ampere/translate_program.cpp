// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/ampere/translate/translator.h"
#include "shader_recompiler/frontend/ampere/translate_program.h"
#include "shader_recompiler/frontend/common/finish_program.h"
#include "shader_recompiler/frontend/common/structured_control_flow.h"
#include "shader_recompiler/host_translate_info.h"

namespace Shader::Ampere {

IR::Program TranslateProgram(ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool, Environment& env,
                             Flow::CFG& cfg, const HostTranslateInfo& host_info) {
    HostTranslateInfo normalized_host_info{host_info};
    normalized_host_info.ApplyDescriptorLimitPolicy();
    const TranslateBlock translate{[&env](IR::Block& block, const Shader::Flow::Block& flow_block) {
        Translate(env, &block, flow_block.begin, flow_block.end);
    }};
    IR::Program program{FinishProgram(BuildASL(inst_pool, block_pool, env, cfg, normalized_host_info, translate),
                                      env, normalized_host_info)};
    // warp operations after a reconvergence point see the lanes reconverged (BSSY/BSYNC are not translated)
    const Info& info{program.info};
    program.info.maximal_reconvergence = info.uses_subgroup_vote || info.uses_subgroup_shuffles ||
                                         info.uses_subgroup_mask || info.uses_subgroup_reduce ||
                                         info.uses_subgroup_invocation_id;
    return program;
}

} // namespace Shader::Ampere
