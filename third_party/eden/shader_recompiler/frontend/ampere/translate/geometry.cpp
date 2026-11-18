// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd. (OpOut)

// (fork) The direct Ampere front's geometry-stage output: OUT Rd, Rhandle, Rstream (78-79: 1 EMIT, 2 CUT, 3 EMIT then
// CUT) as EmitVertex/EndPrimitive. The output handle names the vertex AST writes; a Vulkan geometry stage has one
// current output vertex, so every handle is 0 (AST's vertex operand does not address outputs). Also the at-exit
// prologue's LEPC (the control flow graph accepts it there only, geometry.h), whose address the BMOV it feeds drops.

#include "shader_recompiler/frontend/ampere/control_flow.h"
#include "shader_recompiler/frontend/ampere/translate/memory.h"

namespace Shader::Ampere {
namespace {
void OUT(Translator& t) {
    if (t.env.ShaderStage() != Stage::Geometry) {
        t.Fail("OUT outside a geometry stage");
    }
    ExpectOnly(t, HighBits(64, 72) | HighBits(78, 80), "OUT mode");
    if (t.insn.Reg32() != RZ) {
        t.Fail("OUT to a stream other than 0");
    }
    const u32 type{t.insn.Field(78, 2)};
    if (type == 0) {
        t.Fail("OUT.FINAL outside the at-exit handler");
    }
    const IR::U32 stream{t.ir.Imm32(0)};
    if ((type & 1) != 0) {
        t.ir.EmitVertex(stream);
    }
    if ((type & 2) != 0) {
        t.ir.EndPrimitive(stream);
    }
    t.SetR(t.insn.Dst(), t.ir.Imm32(0));
}

void LEPC(Translator& t) {
    const u32 dst{t.insn.Dst()};
    t.SetR(dst, t.ir.Imm32(Flow::ByteOffsetOf(t.pc)));
    t.SetR(dst + 1, t.ir.Imm32(0));
}
} // Anonymous namespace

void TranslateGeometry(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0x324:
        return OUT(t);
    case 0x34e:
        return LEPC(t);
    case 0x357: // BMOV.64 ATEXIT_PC of the at-exit prologue
        return;
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
