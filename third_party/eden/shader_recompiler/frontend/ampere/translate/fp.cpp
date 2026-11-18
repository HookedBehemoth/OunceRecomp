// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's 32-bit float ALU: FADD, FMUL, FFMA, FMNMX, FSETP, FSET, FSEL, MUFU, FRND.

#include <numbers>

#include "shader_recompiler/frontend/ampere/translate/translator.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/common_funcs.h"

namespace Shader::Ampere {
namespace {
using Maxwell::BooleanOp;
using Maxwell::FPCompareOp;

/// set_rnd_mode(78..80)
IR::FpRounding Rounding(const Insn& insn) {
    switch (insn.Field(78, 2)) {
    case 0:
        return IR::FpRounding::RN;
    case 1:
        return IR::FpRounding::RM;
    case 2:
        return IR::FpRounding::RP;
    default:
        return IR::FpRounding::RZ;
    }
}

/// Bit 80: .FTZ (denormals flushed); off, they are kept
IR::FmzMode Fmz(const Insn& insn) {
    return insn.Bit(80) ? IR::FmzMode::FTZ : IR::FmzMode::None;
}

IR::FpControl Control(const Insn& insn) {
    return {.no_contraction = true, .rounding = Rounding(insn), .fmz_mode = Fmz(insn)};
}

/// FADD's second source is src1 unless it is not a register (encode_alu: then src2, with src1 RZ)
Operand FaddB(Translator& t) {
    switch (t.insn.Form()) {
    case AluForm::RegImm:
    case AluForm::RegCbuf:
    case AluForm::RegUReg:
        return t.Src2();
    default:
        return t.Src1();
    }
}

IR::F32 Saturate(Translator& t, const IR::F32& value) {
    return t.insn.Bit(77) ? IR::F32{t.ir.FPSaturate(value)} : value;
}

void FADD(Translator& t) {
    t.Expect(81, 10, 0, "FADD mode");
    const IR::F32 a{t.Float(t.Src0())};
    const IR::F32 b{t.Float(FaddB(t))};
    t.SetF(t.insn.Dst(), Saturate(t, t.ir.FPAdd(a, b, Control(t.insn))));
}

void FMUL(Translator& t) {
    // dnz (76, x * 0 = 0), 81-83 and the predicate 87-90 are not used
    t.Expect(76, 1, 0, "FMUL.DNZ");
    t.Expect(81, 3, 0, "FMUL mode");
    t.Expect(87, 4, 0, "FMUL mode");
    IR::F32 a{t.Float(t.Src0())};
    const IR::F32 b{t.Float(t.Src1())};
    const u32 scale{t.insn.Field(84, 3)};
    if (scale != 4) {
        // .D8 .D4 .D2 (1-3) and .M2 .M4 .M8 (5-7): a power of two applied to the product (FTZ and RN only)
        if (scale == 0 || !t.insn.Bit(80) || t.insn.Field(78, 2) != 0) {
            t.Fail("FMUL scale mode");
        }
        static constexpr float FACTOR[8]{0.0f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        a = t.ir.FPMul(a, t.ir.Imm32(FACTOR[scale]));
    }
    t.SetF(t.insn.Dst(), Saturate(t, t.ir.FPMul(a, b, Control(t.insn))));
}

void FFMA(Translator& t) {
    t.Expect(76, 1, 0, "FFMA.DNZ");
    t.Expect(81, 10, 0, "FFMA mode");
    const IR::F32 a{t.Float(t.Src0())};
    const IR::F32 b{t.Float(t.Src1())};
    const IR::F32 c{t.Float(t.Src2())};
    t.SetF(t.insn.Dst(), Saturate(t, t.ir.FPFma(a, b, c, Control(t.insn))));
}

void FMNMX(Translator& t) {
    // 87-90: min when the predicate holds, else max; 81: .NAN, an unordered pair gives the canonical NaN
    t.Expect(74, 6, 0, "FMNMX mode");
    t.Expect(82, 5, 0, "FMNMX mode");
    const IR::F32 a{t.Float(t.Src0())};
    const IR::F32 b{t.Float(t.Src1())};
    const IR::FpControl control{.no_contraction = false, .fmz_mode = Fmz(t.insn)};
    const IR::F32 max{t.ir.FPMax(a, b, control)};
    const IR::F32 min{t.ir.FPMin(a, b, control)};
    IR::F32 value{t.ir.Select(t.PredSrc(87), min, max)};
    if (t.insn.Bit(81)) {
        value = IR::F32{t.ir.Select(t.ir.FPUnordered(a, b), t.ir.Imm32(std::bit_cast<f32>(0x7fffffffU)), value)};
    }
    t.SetF(t.insn.Dst(), value);
}

void FSETP(Translator& t) {
    const u32 bop{t.insn.Field(74, 2)};
    if (bop > 2) {
        t.Fail("predicate combination");
    }
    const IR::F32 a{t.Float(t.Src0())};
    const IR::F32 b{t.Float(t.Src1())};
    const IR::FpControl control{.no_contraction = false, .fmz_mode = Fmz(t.insn)};
    const IR::U1 cmp{
        Maxwell::FloatingPointCompare(t.ir, a, b, static_cast<FPCompareOp>(t.insn.Field(76, 4)), control)};
    const IR::U1 accum{t.PredSrc(87)};
    const auto op{static_cast<BooleanOp>(bop)};
    t.SetP(t.insn.PredDst(81), Maxwell::PredicateCombine(t.ir, cmp, accum, op));
    t.SetP(t.insn.PredDst(84), Maxwell::PredicateCombine(t.ir, t.ir.LogicalNot(cmp), accum, op));
}

void FSET(Translator& t) {
    // .BF: 1.0 when (compare op Pc) holds, else 0
    const u32 bop{t.insn.Field(74, 2)};
    if (bop > 2) {
        t.Fail("predicate combination");
    }
    t.Expect(81, 6, 0, "FSET mode");
    if (t.insn.Form() != AluForm::URegReg) {
        t.Expect(91, 1, 0, "FSET mode");
    }
    if (t.insn.Form() != AluForm::RegReg && t.insn.Form() != AluForm::ImmReg && t.insn.Form() != AluForm::CbufReg &&
        t.insn.Form() != AluForm::URegReg) {
        t.Fail("FSET source form");
    }
    t.Expect(64, 8, 0, "FSET third source");
    const IR::F32 a{t.Float(t.Src0())};
    const IR::F32 b{t.Float(t.Src1())};
    const IR::FpControl control{.no_contraction = false, .fmz_mode = Fmz(t.insn)};
    const IR::U1 cmp{
        Maxwell::FloatingPointCompare(t.ir, a, b, static_cast<FPCompareOp>(t.insn.Field(76, 4)), control)};
    const IR::U1 result{Maxwell::PredicateCombine(t.ir, cmp, t.PredSrc(87), static_cast<BooleanOp>(bop))};
    t.SetF(t.insn.Dst(), IR::F32{t.ir.Select(result, t.ir.Imm32(1.0f), t.ir.Imm32(0.0f))});
}

void FSEL(Translator& t) {
    const IR::F32 a{t.Float(t.Src0())};
    const IR::F32 b{t.Float(t.Src1())};
    t.SetF(t.insn.Dst(), IR::F32{t.ir.Select(t.PredSrc(87), a, b)});
}

void MUFU(Translator& t) {
    // 74-79 the function; COS and SIN take the angle in turns
    const u32 function{t.insn.Field(74, 6)};
    t.Expect(80, 11, 0, "MUFU mode");
    // .F16 (73): the source the half 60 selects, the result an F16 in the low half (the high half 0)
    const bool half{t.insn.Bit(73)};
    IR::F32 x;
    if (half) {
        if (t.insn.Form() != AluForm::RegReg) {
            t.Fail("MUFU F16 source form");
        }
        t.Expect(61, 1, 0, "MUFU F16 source swizzle");
        const IR::Value halves{t.ir.UnpackFloat2x16(t.R(t.insn.Reg32()))};
        const IR::F16 h{t.ir.CompositeExtract(halves, t.insn.Field(60, 1))};
        x = IR::F32{t.ir.FPAbsNeg(IR::F32{t.ir.FPConvert(32, h)}, t.insn.Bit(62), t.insn.Bit(63))};
    } else {
        x = t.Float(t.Src1());
    }
    const auto radians{[&] { return t.ir.FPMul(x, t.ir.Imm32(2.0f * std::numbers::pi_v<f32>)); }};
    IR::F32 value;
    switch (function) {
    case 0:
        value = t.ir.FPCos(radians());
        break;
    case 1:
        value = t.ir.FPSin(radians());
        break;
    case 2:
        value = t.ir.FPExp2(x);
        break;
    case 3:
        value = t.ir.FPLog2(x);
        break;
    case 4:
        value = t.ir.FPRecip(x);
        break;
    case 5:
        value = t.ir.FPRecipSqrt(x);
        break;
    case 8:
        value = t.ir.FPSqrt(x);
        break;
    default:
        t.Fail("MUFU function");
    }
    if (half) {
        const IR::F16F32F64 zero{t.ir.FPConvert(16, t.ir.Imm32(0.0f))};
        t.SetR(t.insn.Dst(), t.ir.PackFloat2x16(t.ir.CompositeConstruct(t.ir.FPConvert(16, value), zero)));
        return;
    }
    t.SetF(t.insn.Dst(), value);
}

void FRND(Translator& t) {
    // F32 to F32 (75-76, 84-85: 2); 78-79 the rounding
    t.Expect(75, 2, 2, "FRND destination type");
    t.Expect(84, 2, 2, "FRND source type");
    t.Expect(81, 3, 0, "FRND mode");
    t.Expect(86, 6, 0, "FRND mode");
    const IR::F32 x{t.Float(t.Src1())};
    const IR::FpControl control{.no_contraction = false, .fmz_mode = Fmz(t.insn)};
    IR::F32 value;
    switch (t.insn.Field(78, 2)) {
    case 0:
        value = t.ir.FPRoundEven(x, control);
        break;
    case 1:
        value = t.ir.FPFloor(x, control);
        break;
    case 2:
        value = t.ir.FPCeil(x, control);
        break;
    default:
        value = t.ir.FPTrunc(x, control);
        break;
    }
    t.SetF(t.insn.Dst(), value);
}
} // Anonymous namespace

void TranslateFloat(Translator& t) {
    switch (t.insn.AluOp()) {
    case 0x021:
        return FADD(t);
    case 0x020:
        return FMUL(t);
    case 0x023:
        return FFMA(t);
    case 0x009:
        return FMNMX(t);
    case 0x00b:
        return FSETP(t);
    case 0x00a:
        return FSET(t);
    case 0x008:
        return FSEL(t);
    case 0x108:
        return MUFU(t);
    case 0x107:
        return FRND(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
