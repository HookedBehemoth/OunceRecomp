// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's moves: MOV (a register, immediate, constant or uniform register) and UMOV.

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
void MOV(Translator& t) {
    // 72-75: the quad lanes, all four
    t.Expect(72, 4, 0xf, "MOV lane mask");
    t.Expect(76, 15, 0, "MOV mode");
    const AluForm form{t.insn.Form()};
    if (form == AluForm::RegReg) {
        t.Expect(40, 24, 0, "MOV mode");
    } else if (form == AluForm::CbufReg || form == AluForm::URegReg) {
        t.Expect(62, 2, 0, "MOV mode");
    } else if (form != AluForm::ImmReg) {
        t.Fail("MOV source form");
    }
    t.SetR(t.insn.Dst(), t.Src1().value);
}

void UMOV(Translator& t) {
    // URd = an immediate (0x882) or URb (0xc82, at 32)
    t.Expect(72, 19, 0, "UMOV mode");
    const IR::U32 value{t.insn.Opcode() == 0x882 ? t.ir.Imm32(t.insn.Imm32()) : t.UR(t.insn.UReg32())};
    t.SetUR(t.insn.Dst(), value);
}
} // Anonymous namespace

void TranslateMove(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0x202:
    case 0x802:
    case 0xa02:
    case 0xc02:
        return MOV(t);
    case 0x882:
    case 0xc82:
        return UMOV(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
