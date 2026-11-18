// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's packed half-float ALU: HADD2. A source's swizzle is Maxwell's (H1_H0, F32, H0_H0,
// H1_H1), so the Maxwell front's half helpers read it.

#include "shader_recompiler/frontend/ampere/translate/translator.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_helper.h"

namespace Shader::Ampere {
namespace {
using Maxwell::Swizzle;

struct HalfSource {
    IR::U32 value;
    Swizzle swizzle;
    bool abs;
    bool neg;
};

/// encode_alu_src0 with the F16v2 swizzle at 74-75
HalfSource HalfSrc0(Translator& t) {
    return {t.R(t.insn.Src0()), static_cast<Swizzle>(t.insn.Field(74, 2)), t.insn.Bit(73), t.insn.Bit(72)};
}

/// HADD2's second source: src1 when a register, else src2 (an immediate pair or a constant)
HalfSource HalfB(Translator& t) {
    switch (t.insn.Form()) {
    case AluForm::RegReg:
        return {t.R(t.insn.Reg32()), static_cast<Swizzle>(t.insn.Field(60, 2)), t.insn.Bit(62), t.insn.Bit(63)};
    case AluForm::RegImm:
        return {t.ir.Imm32(t.insn.Imm32()), Swizzle::H1_H0, false, false};
    case AluForm::RegCbuf:
        return {t.Cbuf(), static_cast<Swizzle>(t.insn.Field(60, 2)), t.insn.Bit(62), t.insn.Bit(63)};
    default:
        t.Fail("HADD2 source form");
    }
}

void HADD2(Translator& t) {
    // 77 saturate, 78 .F32 (the first lanes' sum as one F32), 80 FTZ
    t.Expect(76, 1, 0, "HADD2 mode");
    t.Expect(79, 1, 0, "HADD2 mode");
    t.Expect(81, 11, 0, "HADD2 mode");
    const HalfSource a{HalfSrc0(t)};
    const HalfSource b{HalfB(t)};
    auto [lhs_a, rhs_a]{Maxwell::Extract(t.ir, a.value, a.swizzle)};
    auto [lhs_b, rhs_b]{Maxwell::Extract(t.ir, b.value, b.swizzle)};
    if (t.insn.Bit(78)) {
        const auto f32{[&](const IR::F16F32F64& value, const HalfSource& source) {
            const IR::F16F32F64 wide{value.Type() == IR::Type::F16 ? t.ir.FPConvert(32, value) : value};
            return IR::F32{t.ir.FPAbsNeg(wide, source.abs, source.neg)};
        }};
        const IR::FpControl control{.no_contraction = true,
                                    .fmz_mode = t.insn.Bit(80) ? IR::FmzMode::FTZ : IR::FmzMode::None};
        IR::F32 sum{t.ir.FPAdd(f32(lhs_a, a), f32(lhs_b, b), control)};
        if (t.insn.Bit(77)) {
            sum = t.ir.FPSaturate(sum);
        }
        t.SetF(t.insn.Dst(), sum);
        return;
    }
    const bool promotion{lhs_a.Type() != lhs_b.Type()};
    if (promotion) {
        if (lhs_a.Type() == IR::Type::F16) {
            lhs_a = t.ir.FPConvert(32, lhs_a);
            rhs_a = t.ir.FPConvert(32, rhs_a);
        }
        if (lhs_b.Type() == IR::Type::F16) {
            lhs_b = t.ir.FPConvert(32, lhs_b);
            rhs_b = t.ir.FPConvert(32, rhs_b);
        }
    }
    lhs_a = t.ir.FPAbsNeg(lhs_a, a.abs, a.neg);
    rhs_a = t.ir.FPAbsNeg(rhs_a, a.abs, a.neg);
    lhs_b = t.ir.FPAbsNeg(lhs_b, b.abs, b.neg);
    rhs_b = t.ir.FPAbsNeg(rhs_b, b.abs, b.neg);
    const IR::FpControl control{
        .no_contraction = true,
        .rounding = IR::FpRounding::DontCare,
        .fmz_mode = t.insn.Bit(80) ? IR::FmzMode::FTZ : IR::FmzMode::None,
    };
    IR::F16F32F64 lhs{t.ir.FPAdd(lhs_a, lhs_b, control)};
    IR::F16F32F64 rhs{t.ir.FPAdd(rhs_a, rhs_b, control)};
    if (t.insn.Bit(77)) {
        lhs = t.ir.FPSaturate(lhs);
        rhs = t.ir.FPSaturate(rhs);
    }
    if (promotion) {
        lhs = t.ir.FPConvert(16, lhs);
        rhs = t.ir.FPConvert(16, rhs);
    }
    t.SetR(t.insn.Dst(), t.ir.PackFloat2x16(t.ir.CompositeConstruct(lhs, rhs)));
}
} // Anonymous namespace

void TranslateHalf(Translator& t) {
    switch (t.insn.AluOp()) {
    case 0x030:
        return HADD2(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
