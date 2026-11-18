// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's conversions: F2I (F32 to 8-, 16- and 32-bit integers, saturating, NaN to 0), I2FP,
// I2F (a byte, half or word of a register to F32), F2FP (two F32 packed as F16x2) and F2IP (two F32 packed as bytes).

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
void F2I(Translator& t) {
    // 72 signed, 75-76 the destination size (16 or 32 bits), 77 NTZ, 78-79 the rounding, 80 FTZ, 84-85 the source size
    const u32 size{t.insn.Field(75, 2)};
    if (size > 2) {
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
    static constexpr f32 LOW[3]{-128.0f, -32768.0f, -2147483648.0f};
    static constexpr f32 HIGH[2][3]{{255.0f, 65535.0f, 4294967040.0f}, {127.0f, 32767.0f, 2147483520.0f}};
    const f32 low{is_signed ? LOW[size] : 0.0f};
    const f32 high{HIGH[is_signed][size]};
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

IR::FpRounding RoundingAt(const Insn& insn, u32 start) {
    static constexpr IR::FpRounding ROUNDING[4]{IR::FpRounding::RN, IR::FpRounding::RM, IR::FpRounding::RP,
                                                IR::FpRounding::RZ};
    return ROUNDING[insn.Field(start, 2)];
}

void I2F(Translator& t) {
    // I2F.F32 of an 8-, 16- or 32-bit integer (84-85), the byte or half by 60-61, 74 signed, 78-79 the rounding
    t.Expect(75, 2, 2, "I2F destination size");
    t.Expect(72, 2, 0, "I2F mode");
    t.Expect(77, 1, 0, "I2F mode");
    t.Expect(80, 4, 0, "I2F mode");
    t.Expect(86, 5, 0, "I2F mode");
    const AluForm form{t.insn.Form()};
    if (form != AluForm::RegReg && form != AluForm::URegReg) {
        t.Fail("I2F source form");
    }
    t.Expect(62, 2, 0, "I2F source modifiers");
    const u32 size{t.insn.Field(84, 2)};
    const u32 select{t.insn.Field(60, 2)};
    const bool is_signed{t.insn.Bit(74)};
    IR::U32 value{t.Src1().value};
    if (size == 2) {
        t.Expect(60, 2, 0, "I2F word selector");
    } else if (size < 2) {
        const u32 bits{8U << size};
        if (select * bits >= 32) {
            t.Fail("I2F selector");
        }
        value = t.ir.BitFieldExtract(value, t.ir.Imm32(select * bits), t.ir.Imm32(bits), is_signed);
    } else {
        t.Fail("I2F source size");
    }
    const IR::FpControl control{.no_contraction = false, .rounding = RoundingAt(t.insn, 78)};
    t.SetF(t.insn.Dst(), IR::F32{t.ir.ConvertIToF(32, 32, is_signed, value, control)});
}

void F2FP(Translator& t) {
    // F2FP.F16.F32.PACK_AB: Rd = {f16(a) high, f16(b) low}, 79-80 the rounding; .MERGE_C (78), .H1 (72) and .RELU (77)
    // are not translated
    if (t.insn.Form() != AluForm::RegReg) {
        t.Fail("F2FP source form");
    }
    t.Expect(62, 2, 0, "F2FP source modifiers");
    t.Expect(64, 8, RZ, "F2FP third source");
    t.Expect(72, 7, 0, "F2FP mode");
    t.Expect(81, 11, 0, "F2FP mode");
    const IR::FpControl control{.no_contraction = false, .rounding = RoundingAt(t.insn, 79)};
    const IR::F16F32F64 high{t.ir.FPConvert(16, t.F(t.insn.Src0()), control)};
    const IR::F16F32F64 low{t.ir.FPConvert(16, t.F(t.insn.Reg32()), control)};
    t.SetR(t.insn.Dst(), t.ir.PackFloat2x16(t.ir.CompositeConstruct(low, high)));
}

void F2IP(Translator& t) {
    // F2IP.U8.F32: Rd = {Rc[15:0], sat_u8(a), sat_u8(b)} (bytes high to low; NaN 0), 78-79 the rounding
    if (t.insn.Form() != AluForm::RegReg) {
        t.Fail("F2IP source form");
    }
    t.Expect(62, 2, 0, "F2IP source modifiers");
    t.Expect(72, 2, 0, "F2IP mode");
    t.Expect(74, 1, 1, "F2IP type");
    t.Expect(75, 3, 0, "F2IP mode");
    t.Expect(80, 11, 0, "F2IP mode");
    auto& ir{t.ir};
    const IR::FpControl control{.no_contraction = true, .rounding = IR::FpRounding::DontCare};
    const auto byte{[&](u32 reg) {
        const IR::F32 x{t.F(reg)};
        IR::F32 rounded;
        switch (t.insn.Field(78, 2)) {
        case 0:
            rounded = ir.FPRoundEven(x, control);
            break;
        case 1:
            rounded = ir.FPFloor(x, control);
            break;
        case 2:
            rounded = ir.FPCeil(x, control);
            break;
        default:
            rounded = ir.FPTrunc(x, control);
            break;
        }
        const IR::U32 value{ir.ConvertFToI(32, false, ir.FPClamp(rounded, ir.Imm32(0.0f), ir.Imm32(255.0f)))};
        return IR::U32{ir.Select(ir.FPIsNan(x), ir.Imm32(0), value)};
    }};
    IR::U32 value{ir.BitFieldInsert(byte(t.insn.Reg32()), byte(t.insn.Src0()), ir.Imm32(8), ir.Imm32(8))};
    const u32 c{t.insn.Reg64()};
    if (c != RZ) {
        value = ir.BitFieldInsert(value, t.R(c), ir.Imm32(16), ir.Imm32(16));
    }
    t.SetR(t.insn.Dst(), value);
}
} // Anonymous namespace

void TranslateConversion(Translator& t) {
    switch (t.insn.AluOp()) {
    case 0x105:
        return F2I(t);
    case 0x045:
        return I2FP(t);
    case 0x106:
        return I2F(t);
    case 0x03e:
        return F2FP(t);
    case 0x043:
        return F2IP(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
