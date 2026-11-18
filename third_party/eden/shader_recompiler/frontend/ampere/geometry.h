// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// (fork) The at-exit hook every NVN geometry stage sets up. The program starts with
//   LEPC Rr; MOV Rk, handler; IADD3 Rr = Rr + Rk; IMAD.X/IADD3.X Rr+1 (carry); BMOV.64 ATEXIT_PC, Rr
// so that each EXIT first runs the handler placed after the program's last EXIT:
//   OUT.FINAL RZ, Rhandle, RZ; BMOV.64 ATEXIT_PC, 0; EXIT
// OUT.FINAL only closes the stage's output, which a Vulkan geometry stage does on its own at its end, so the front
// drops the hook: the control flow graph accepts LEPC and BMOV.64 ATEXIT_PC only inside this prologue, and never walks
// into the handler.

#pragma once

#include <optional>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/common/location.h"

namespace Shader::Ampere::Flow {

/// In a geometry stage starting with the prologue above (its handler checked): the location after its BMOV. nullopt
/// when the program does not start with an LEPC; a prologue or handler of another shape fails the translation.
[[nodiscard]] std::optional<Location> AtExitPrologueEnd(Environment& env, Location start);

} // namespace Shader::Ampere::Flow
