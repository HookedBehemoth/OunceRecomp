// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's packed half-float ALU: HADD2, HFMA2, HMUL2, HMNMX2, HSETP2. A source's swizzle is Maxwell's (H1_H0, F32, H0_H0,
// H1_H1), so the Maxwell front's half helpers read it.

#include "shader_recompiler/frontend/ampere/translate/translator.h"
#include <vector>

#include "shader_recompiler/frontend/maxwell/translate/impl/common_funcs.h"
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

/// encode_alu_base's second (slot 1) or third (slot 2) source of an F16v2 op by the form: a register at 64 (swizzle 81-82,
/// abs 83, neg 84), or at 32 (60-61, 62, 63), a constant or uniform register (60-61, 62, 63), an immediate pair
HalfSource HalfSlot(Translator& t, u32 slot) {
    const AluForm form{t.insn.Form()};
    const bool at_64{slot == 1 ? form == AluForm::RegImm || form == AluForm::RegCbuf || form == AluForm::RegUReg
                               : form == AluForm::RegReg || form == AluForm::ImmReg || form == AluForm::CbufReg ||
                                     form == AluForm::URegReg};
    if (at_64) {
        return {t.R(t.insn.Reg64()), static_cast<Swizzle>(t.insn.Field(81, 2)), t.insn.Bit(83), t.insn.Bit(84)};
    }
    const auto modified{[&](const IR::U32& value) {
        return HalfSource{value, static_cast<Swizzle>(t.insn.Field(60, 2)), t.insn.Bit(62), t.insn.Bit(63)};
    }};
    switch (form) {
    case AluForm::RegReg:
        return modified(t.R(t.insn.Reg32()));
    case AluForm::ImmReg:
    case AluForm::RegImm:
        return {t.ir.Imm32(t.insn.Imm32()), Swizzle::H1_H0, false, false};
    case AluForm::CbufReg:
    case AluForm::RegCbuf:
        return modified(t.Cbuf());
    default:
        return modified(t.UR(t.insn.UReg32()));
    }
}

/// A two-source op's second source: src1, or src2 when the form puts a register in src1 (HADD2, HSETP2: src1 not a
/// register)
HalfSource HalfB(Translator& t) {
    const AluForm form{t.insn.Form()};
    const bool src2{form == AluForm::RegImm || form == AluForm::RegCbuf || form == AluForm::RegUReg};
    return HalfSlot(t, src2 ? 2 : 1);
}

/// HMUL2 and HMNMX2 take their second source in src1 only
void Src1Form(Translator& t) {
    const AluForm form{t.insn.Form()};
    if (form == AluForm::RegImm || form == AluForm::RegCbuf || form == AluForm::RegUReg) {
        t.Fail("F16v2 source form");
    }
}

using HalfPair = std::pair<IR::F16F32F64, IR::F16F32F64>;

/// The pairs of the sources, all F32 when one has the F32 swizzle, with abs and neg applied
std::vector<HalfPair> Pairs(Translator& t, std::initializer_list<HalfSource> sources, bool* promoted) {
    std::vector<HalfPair> pairs;
    bool promotion{};
    for (const HalfSource& source : sources) {
        pairs.push_back(Maxwell::Extract(t.ir, source.value, source.swizzle));
        promotion |= pairs.back().first.Type() != IR::Type::F16;
    }
    size_t i{};
    for (const HalfSource& source : sources) {
        auto& [lhs, rhs]{pairs[i++]};
        if (promotion && lhs.Type() == IR::Type::F16) {
            lhs = t.ir.FPConvert(32, lhs);
            rhs = t.ir.FPConvert(32, rhs);
        }
        lhs = t.ir.FPAbsNeg(lhs, source.abs, source.neg);
        rhs = t.ir.FPAbsNeg(rhs, source.abs, source.neg);
    }
    if (promoted) {
        *promoted = promotion;
    }
    return pairs;
}

/// Packs the two results, F16 (from F32 when promoted), saturated by 77
void SetHalfResult(Translator& t, IR::F16F32F64 lhs, IR::F16F32F64 rhs, bool promoted, bool saturate) {
    if (saturate) {
        lhs = t.ir.FPSaturate(lhs);
        rhs = t.ir.FPSaturate(rhs);
    }
    if (promoted) {
        lhs = t.ir.FPConvert(16, lhs);
        rhs = t.ir.FPConvert(16, rhs);
    }
    t.SetR(t.insn.Dst(), t.ir.PackFloat2x16(t.ir.CompositeConstruct(lhs, rhs)));
}

IR::FmzMode HalfFmz(const Insn& insn) {
    return insn.Bit(80) ? IR::FmzMode::FTZ : IR::FmzMode::None;
}

/// .DNZ (76): a zero factor gives the addend (x * 0 = 0 for any x, as Maxwell's FMZ)
IR::F16F32F64 Dnz(Translator& t, const IR::F16F32F64& a, const IR::F16F32F64& b, const IR::F16F32F64& addend,
                  const IR::F16F32F64& result) {
    const auto zero{[&](const IR::F16F32F64& x) {
        return x.Type() == IR::Type::F16 ? t.ir.FPEqual(x, t.ir.FPConvert(16, t.ir.Imm32(0.0f)))
                                         : t.ir.FPEqual(x, t.ir.Imm32(0.0f));
    }};
    return IR::F16F32F64{t.ir.Select(t.ir.LogicalOr(zero(a), zero(b)), addend, result)};
}

void HFMA2(Translator& t) {
    // Rd = a * b + c per half; 76 .DNZ, 77 saturate, 79 .RELU, 80 FTZ, 86 negates b's high half
    t.Expect(78, 1, 0, "HFMA2.F32");
    t.Expect(85, 1, 0, "HFMA2.BF16_V2");
    const bool relu{t.insn.Bit(79)};
    t.Expect(87, 4, relu ? 7 : 0, "HFMA2 mode");
    if (relu && t.insn.Bit(77)) {
        t.Fail("HFMA2 saturate and RELU");
    }
    HalfSource b{HalfSlot(t, 1)};
    if (t.insn.Bit(86)) {
        if (b.swizzle != Swizzle::H1_H0 || b.abs || b.neg) {
            t.Fail("HFMA2 high-half negation with a modified source");
        }
        b.value = t.ir.BitwiseXor(b.value, t.ir.Imm32(0x80000000U));
    }
    bool promoted{};
    const std::vector<HalfPair> p{Pairs(t, {HalfSrc0(t), b, HalfSlot(t, 2)}, &promoted)};
    const IR::FpControl control{.no_contraction = true, .rounding = IR::FpRounding::DontCare, .fmz_mode = HalfFmz(t.insn)};
    IR::F16F32F64 lhs{t.ir.FPFma(p[0].first, p[1].first, p[2].first, control)};
    IR::F16F32F64 rhs{t.ir.FPFma(p[0].second, p[1].second, p[2].second, control)};
    if (t.insn.Bit(76)) {
        lhs = Dnz(t, p[0].first, p[1].first, p[2].first, lhs);
        rhs = Dnz(t, p[0].second, p[1].second, p[2].second, rhs);
    }
    SetHalfResult(t, lhs, rhs, promoted, t.insn.Bit(77));
    if (relu) {
        // per half: a NaN of either sign is the canonical 0x7fff, a negative value (and -0) is 0
        auto& ir{t.ir};
        const IR::U32 packed{t.R(t.insn.Dst())};
        IR::U32 result{ir.Imm32(0)};
        for (u32 half = 0; half < 2; ++half) {
            const IR::U32 bits{ir.BitFieldExtract(packed, ir.Imm32(half * 16), ir.Imm32(16))};
            const IR::U1 nan{ir.IGreaterThan(ir.BitwiseAnd(bits, ir.Imm32(0x7fff)), ir.Imm32(0x7c00), false)};
            const IR::U1 negative{ir.INotEqual(ir.BitwiseAnd(bits, ir.Imm32(0x8000)), ir.Imm32(0))};
            const IR::U32 value{ir.Select(nan, ir.Imm32(0x7fff), ir.Select(negative, ir.Imm32(0), bits))};
            result = ir.BitFieldInsert(result, value, ir.Imm32(half * 16), ir.Imm32(16));
        }
        t.SetR(t.insn.Dst(), result);
    }
}

void HMUL2(Translator& t) {
    // 76 .DNZ, 77 saturate, 80 FTZ
    t.Expect(78, 2, 0, "HMUL2 mode");
    t.Expect(81, 10, 0, "HMUL2 mode");
    Src1Form(t);
    bool promoted{};
    const std::vector<HalfPair> p{Pairs(t, {HalfSrc0(t), HalfB(t)}, &promoted)};
    const IR::FpControl control{.no_contraction = true, .rounding = IR::FpRounding::DontCare, .fmz_mode = HalfFmz(t.insn)};
    IR::F16F32F64 lhs{t.ir.FPMul(p[0].first, p[1].first, control)};
    IR::F16F32F64 rhs{t.ir.FPMul(p[0].second, p[1].second, control)};
    if (t.insn.Bit(76)) {
        const auto zero{[&](const IR::F16F32F64& like) {
            return like.Type() == IR::Type::F16 ? IR::F16F32F64{t.ir.FPConvert(16, t.ir.Imm32(0.0f))}
                                                : IR::F16F32F64{t.ir.Imm32(0.0f)};
        }};
        lhs = Dnz(t, p[0].first, p[1].first, zero(lhs), lhs);
        rhs = Dnz(t, p[0].second, p[1].second, zero(rhs), rhs);
    }
    SetHalfResult(t, lhs, rhs, promoted, t.insn.Bit(77));
}

void HMNMX2(Translator& t) {
    // per half: min when 87-90 holds, else max; 80 FTZ; .F32 (78), .NAN (81), .XORSIGN (82), .BF16_V2 (85) are not
    // translated
    t.Expect(76, 3, 0, "HMNMX2 mode");
    t.Expect(79, 1, 0, "HMNMX2 mode");
    t.Expect(81, 6, 0, "HMNMX2 mode");
    Src1Form(t);
    const HalfSource b{HalfB(t)};
    const std::vector<HalfPair> p{Pairs(t, {HalfSrc0(t), b}, nullptr)};
    const IR::U1 min{t.XPredSrc(87)};
    const IR::FpControl control{.no_contraction = false, .fmz_mode = HalfFmz(t.insn)};
    const auto select{[&](const IR::F16F32F64& x, const IR::F16F32F64& y) {
        const bool half{x.Type() == IR::Type::F16};
        const IR::F32 a{half ? IR::F32{t.ir.FPConvert(32, x)} : IR::F32{x}};
        const IR::F32 c{half ? IR::F32{t.ir.FPConvert(32, y)} : IR::F32{y}};
        const IR::F32 value{t.ir.Select(min, t.ir.FPMin(a, c, control), t.ir.FPMax(a, c, control))};
        return half ? IR::F16F32F64{t.ir.FPConvert(16, value)} : IR::F16F32F64{value};
    }};
    const bool promoted{p[0].first.Type() != IR::Type::F16};
    SetHalfResult(t, select(p[0].first, p[1].first), select(p[0].second, p[1].second), promoted, false);
}

void HSETP2(Translator& t) {
    // P (81-83) = (compare 76-79 of the low halves) op (69-70) Pc (87-90), Q (84-86) the high halves'; .H_AND (71): P
    // both halves' compare, Q its negation, each combined with Pc
    t.Expect(64, 5, 0, "HSETP2 mode");
    const u32 bop{t.insn.Field(69, 2)};
    if (bop > 2) {
        t.Fail("predicate combination");
    }
    const std::vector<HalfPair> p{Pairs(t, {HalfSrc0(t), HalfB(t)}, nullptr)};
    const IR::FpControl control{.no_contraction = false, .fmz_mode = HalfFmz(t.insn)};
    const auto compare{static_cast<Maxwell::FPCompareOp>(t.insn.Field(76, 4))};
    const IR::U1 low{Maxwell::FloatingPointCompare(t.ir, p[0].first, p[1].first, compare, control)};
    const IR::U1 high{Maxwell::FloatingPointCompare(t.ir, p[0].second, p[1].second, compare, control)};
    const IR::U1 accum{t.PredSrc(87)};
    const auto op{static_cast<Maxwell::BooleanOp>(bop)};
    if (t.insn.Bit(71)) {
        const IR::U1 both{t.ir.LogicalAnd(low, high)};
        t.SetP(t.insn.PredDst(81), Maxwell::PredicateCombine(t.ir, both, accum, op));
        t.SetP(t.insn.PredDst(84), Maxwell::PredicateCombine(t.ir, t.ir.LogicalNot(both), accum, op));
    } else {
        t.SetP(t.insn.PredDst(81), Maxwell::PredicateCombine(t.ir, low, accum, op));
        t.SetP(t.insn.PredDst(84), Maxwell::PredicateCombine(t.ir, high, accum, op));
    }
}

void HADD2(Translator& t) {
    // 77 saturate, 78 .F32 (the first lanes' sum as one F32), 80 FTZ
    t.Expect(76, 1, 0, "HADD2 mode");
    t.Expect(79, 1, 0, "HADD2 mode");
    t.Expect(81, 11, 0, "HADD2 mode");
    const HalfSource a{HalfSrc0(t)};
    const HalfSource b{HalfB(t)};
    if (t.insn.Form() != AluForm::RegReg && t.insn.Form() != AluForm::RegImm && t.insn.Form() != AluForm::RegCbuf) {
        t.Fail("HADD2 source form");
    }
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
    case 0x031:
        return HFMA2(t);
    case 0x032:
        return HMUL2(t);
    case 0x034:
        return HSETP2(t);
    case 0x040:
        return HMNMX2(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
