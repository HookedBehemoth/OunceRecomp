// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's conversions: F2I (F32 to 16- and 32-bit integers, saturating, NaN to 0) and I2FP.

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
void F2I(Translator& t) {
    // 72 signed, 75-76 the destination size (16 or 32 bits), 77 NTZ, 78-79 the rounding, 80 FTZ, 84-85 the source size
    const u32 size{t.insn.Field(75, 2)};
    if (size != 1 && size != 2) {
        t.Fail("F2I destination size");
    }
    t.Expect(84, 2, 2, "F2I source size");
    t.Expect(73, 2, 0, "F2I mode");
    t.Expect(81, 3, 0, "F2I mode");
    t.Expect(86, 5, 0, "F2I mode");
    const bool is_signed{t.insn.Bit(72)};
    const IR::F32 x{t.Float(t.Src1())};
    const IR::FpControl control{.no_contraction = true,
                                .rounding = IR::FpRounding::DontCare,
                                .fmz_mode = t.insn.Bit(80) ? IR::FmzMode::FTZ : IR::FmzMode::None};
    IR::F32 rounded;
    switch (t.insn.Field(78, 2)) {
    case 0:
        rounded = t.ir.FPRoundEven(x, control);
        break;
    case 1:
        rounded = t.ir.FPFloor(x, control);
        break;
    case 2:
        rounded = t.ir.FPCeil(x, control);
        break;
    default:
        rounded = t.ir.FPTrunc(x, control);
        break;
    }
    // saturated to the type's range, NaN to 0; a 32-bit range by its largest float inside and a select above it
    const bool wide{size == 2};
    const f32 low{is_signed ? (wide ? -2147483648.0f : -32768.0f) : 0.0f};
    const f32 high{is_signed ? (wide ? 2147483520.0f : 32767.0f) : (wide ? 4294967040.0f : 65535.0f)};
    const IR::F32 clamped{t.ir.FPClamp(rounded, t.ir.Imm32(low), t.ir.Imm32(high))};
    IR::U32 value{t.ir.ConvertFToI(32, is_signed, clamped)};
    if (wide) {
        const IR::U1 overflow{
            t.ir.FPGreaterThanEqual(rounded, t.ir.Imm32(is_signed ? 2147483648.0f : 4294967296.0f))};
        value = IR::U32{t.ir.Select(overflow, t.ir.Imm32(is_signed ? 0x7fffffffU : 0xffffffffU), value)};
    }
    value = IR::U32{t.ir.Select(t.ir.FPIsNan(x), t.ir.Imm32(0), value)};
    t.SetR(t.insn.Dst(), value);
}

void I2FP(Translator& t) {
    // I2FP.F32.(S|U)32: 74 signed, 78-79 the rounding
    t.Expect(75, 2, 2, "I2FP destination size");
    t.Expect(84, 2, 2, "I2FP source size");
    t.Expect(72, 2, 0, "I2FP mode");
    t.Expect(77, 1, 0, "I2FP mode");
    t.Expect(80, 4, 0, "I2FP mode");
    t.Expect(86, 5, 0, "I2FP mode");
    const AluForm form{t.insn.Form()};
    if (form == AluForm::RegReg || form == AluForm::CbufReg || form == AluForm::URegReg) {
        t.Expect(62, 2, 0, "I2FP source modifiers");
    }
    static constexpr IR::FpRounding ROUNDING[4]{IR::FpRounding::RN, IR::FpRounding::RM, IR::FpRounding::RP,
                                                IR::FpRounding::RZ};
    const IR::FpControl control{.no_contraction = false, .rounding = ROUNDING[t.insn.Field(78, 2)]};
    const IR::F32 value{t.ir.ConvertIToF(32, 32, t.insn.Bit(74), t.Src1().value, control)};
    t.SetF(t.insn.Dst(), value);
}
} // Anonymous namespace

void TranslateConversion(Translator& t) {
    switch (t.insn.AluOp()) {
    case 0x105:
        return F2I(t);
    case 0x045:
        return I2FP(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
