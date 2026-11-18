// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's moves: MOV (a register, immediate, constant or uniform register), UMOV, CS2R of SRZ
// and S2R (the special registers Maxwell's S2R reads).

#include <fmt/format.h>

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

void CS2R(Translator& t) {
    // CS2R.64 Rd, SRZ: a zeroed pair; the clocks and counters are not translated
    t.Expect(72, 8, 255, "CS2R special register");
    t.Expect(80, 1, 1, "CS2R size");
    const u32 dst{t.insn.Dst()};
    t.SetR(dst, t.ir.Imm32(0));
    if (dst != RZ) {
        t.SetR(dst + 1, t.ir.Imm32(0));
    }
}

/// S2R's special registers, numbered as Maxwell's
IR::U32 SpecialRegister(Translator& t, u32 index) {
    auto& ir{t.ir};
    switch (index) {
    case 0:
        return ir.LaneId();
    case 17:
        return ir.InvocationId();
    case 18:
        return ir.BitCast<IR::U32>(ir.YDirection());
    case 19:
        return IR::U32{ir.Select(ir.IsHelperInvocation(), ir.Imm32(-1), ir.Imm32(0))};
    case 29:
        return ir.InvocationInfo();
    case 30:
        return ir.SR_WScaleFactorXY();
    case 31:
        return ir.SR_WScaleFactorZ();
    case 32: {
        const IR::Value tid{ir.LocalInvocationId()};
        return ir.BitFieldInsert(ir.BitFieldInsert(IR::U32{ir.CompositeExtract(tid, 0)},
                                                   IR::U32{ir.CompositeExtract(tid, 1)}, ir.Imm32(16), ir.Imm32(8)),
                                 IR::U32{ir.CompositeExtract(tid, 2)}, ir.Imm32(26), ir.Imm32(6));
    }
    case 33:
        return ir.LocalInvocationIdX();
    case 34:
        return ir.LocalInvocationIdY();
    case 35:
        return ir.LocalInvocationIdZ();
    case 37:
        return ir.WorkgroupIdX();
    case 38:
        return ir.WorkgroupIdY();
    case 39:
        return ir.WorkgroupIdZ();
    case 56:
        return ir.SubgroupEqMask();
    case 57:
        return ir.SubgroupLtMask();
    case 58:
        return ir.SubgroupLeMask();
    case 59:
        return ir.SubgroupGtMask();
    case 60:
        return ir.SubgroupGeMask();
    case 132:
        // SR_VARIABLE_RATE: without variable-rate shading every fragment is 1x1 (its width and height in bytes 1 and 2)
        return ir.Imm32(0x00010100);
    default:
        t.Fail(fmt::format("special register {}", index));
    }
}

void S2R(Translator& t) {
    t.Expect(32, 32, 0, "S2R mode");
    t.Expect(80, 12, 0, "S2R mode");
    t.Expect(64, 8, 0, "S2R mode");
    t.SetR(t.insn.Dst(), SpecialRegister(t, t.insn.Field(72, 8)));
}
} // Anonymous namespace

void TranslateMove(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0x202:
    case 0x802:
    case 0xa02:
    case 0xc02:
        return MOV(t);
    case 0x805:
        return CS2R(t);
    case 0x919:
        return S2R(t);
    case 0x882:
    case 0xc82:
        return UMOV(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
