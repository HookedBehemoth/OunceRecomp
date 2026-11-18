// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.; the semantics of IADD3's and
// SHF's results as NAK's constant folding (ir.rs) computes them

// (fork) The direct Ampere front's 32-bit integer ALU: IADD3 (and UIADD3), IMAD (low word, .WIDE, .HI), LEA, LOP3,
// PLOP3, ISETP, IMNMX, IABS, SHF, SEL.

#include "shader_recompiler/frontend/ampere/translate/translator.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/common_funcs.h"

namespace Shader::Ampere {
namespace {
using Maxwell::BooleanOp;
using Maxwell::CompareOp;

/// The second source of an integer op: its 62-63 (a float op's modifiers) must be clear where the form has them
IR::U32 IntSrc1(Translator& t) {
    const AluForm form{t.insn.Form()};
    if (form == AluForm::RegReg || form == AluForm::CbufReg || form == AluForm::URegReg) {
        t.Expect(62, 2, 0, "integer source modifiers");
    }
    return t.Src1().value;
}

IR::U32 IntSrc2(Translator& t) {
    const AluForm form{t.insn.Form()};
    if (form == AluForm::RegCbuf || form == AluForm::RegUReg) {
        t.Expect(62, 2, 0, "integer source modifiers");
    }
    return t.Src2().value;
}

IR::U32 One(Translator& t, const IR::U1& pred) {
    return IR::U32{t.ir.Select(pred, t.ir.Imm32(1), t.ir.Imm32(0))};
}

void IADD3(Translator& t, bool uniform) {
    // Rd = a + b + c, each negated by 72, 63, 75 (.X, 74: inverted, the carries 87-90 and 77-80 added); Pu and Pv
    // (81-83, 84-86) the sum's carries past 32 and 33 bits
    const bool extended{t.insn.Bit(74)};
    const AluForm form{t.insn.Form()};
    if (form != AluForm::RegReg && form != AluForm::ImmReg && form != AluForm::CbufReg && form != AluForm::URegReg) {
        t.Fail("IADD3 source form");
    }
    const bool negate[3]{t.insn.Bit(72), form != AluForm::ImmReg && t.insn.Bit(63), t.insn.Bit(75)};
    t.Expect(73, 1, 0, "IADD3 mode");
    t.Expect(76, 1, 0, "IADD3 mode");
    if (form != AluForm::ImmReg && !uniform) {
        t.Expect(62, 1, 0, "IADD3 mode");
    }
    // the uniform form (encode_ualu): URa at 24, URb at 32 or an immediate, URc at 64; the carries are UPs
    const IR::U32 src[3]{uniform ? t.UR(t.insn.Field(24, 6)) : t.Src0().value,
                         uniform ? (form == AluForm::ImmReg ? t.ir.Imm32(t.insn.Imm32()) : t.UR(t.insn.UReg32()))
                                 : t.Src1().value,
                         uniform ? t.UR(t.insn.Field(64, 6)) : t.Src2().value};
    if (uniform && form != AluForm::RegReg && form != AluForm::ImmReg) {
        t.Fail("UIADD3 source form");
    }
    const auto set_pred{[&](u32 pred, const IR::U1& value) { uniform ? t.SetUP(pred, value) : t.SetP(pred, value); }};
    const auto pred_src{[&](u32 start) { return uniform ? t.UPredSrc(start) : t.PredSrc(start); }};
    const auto set_result{[&](const IR::U32& value) { uniform ? t.SetUR(t.insn.Dst(), value) : t.SetR(t.insn.Dst(), value); }};
    const u32 carry_out[2]{t.insn.PredDst(81), t.insn.PredDst(84)};
    const bool carries{carry_out[0] != PT || carry_out[1] != PT};
    if (!extended) {
        t.Expect(77, 4, 0xf, "IADD3 carry input");
        t.Expect(87, 4, 0xf, "IADD3 carry input");
    }
    if (!carries) {
        IR::U32 sum{};
        for (int i = 0; i < 3; ++i) {
            IR::U32 value{src[i]};
            if (negate[i]) {
                value = extended ? t.ir.BitwiseNot(value) : IR::U32{t.ir.INeg(value)};
            }
            sum = i == 0 ? value : IR::U32{t.ir.IAdd(sum, value)};
        }
        if (extended) {
            for (const u32 bit : {87U, 77U}) {
                if (t.insn.Field(bit, 4) != 0xf) {
                    sum = IR::U32{t.ir.IAdd(sum, One(t, pred_src(bit)))};
                }
            }
        }
        set_result(sum);
        return;
    }
    // with carries: the sum in 64 bits (a negation is ~x + 1, or ~x with .X)
    IR::U64 sum{};
    for (int i = 0; i < 3; ++i) {
        IR::U64 value{t.ir.UConvert(64, negate[i] ? t.ir.BitwiseNot(src[i]) : src[i])};
        if (negate[i] && !extended) {
            value = IR::U64{t.ir.IAdd(value, t.ir.Imm64(u64{1}))};
        }
        sum = i == 0 ? value : IR::U64{t.ir.IAdd(sum, value)};
    }
    if (extended) {
        for (const u32 bit : {87U, 77U}) {
            if (t.insn.Field(bit, 4) != 0xf) {
                sum = IR::U64{t.ir.IAdd(sum, t.ir.UConvert(64, One(t, pred_src(bit))))};
            }
        }
    }
    const IR::Value words{t.ir.UnpackUint2x32(sum)};
    const IR::U32 high{t.ir.CompositeExtract(words, 1)};
    set_result(IR::U32{t.ir.CompositeExtract(words, 0)});
    set_pred(carry_out[0], t.ir.INotEqual(high, t.ir.Imm32(0)));
    set_pred(carry_out[1], t.ir.IGreaterThanEqual(high, t.ir.Imm32(2), false));
}

/// The high word of the 64-bit product a * b (16-bit pieces; each partial sum fits 32 bits)
IR::U32 MulHigh(Translator& t, const IR::U32& a, const IR::U32& b, bool is_signed) {
    auto& ir{t.ir};
    const IR::U32 mask{ir.Imm32(0xffff)}, sixteen{ir.Imm32(16)};
    const IR::U32 al{ir.BitwiseAnd(a, mask)}, ah{ir.ShiftRightLogical(a, sixteen)};
    const IR::U32 bl{ir.BitwiseAnd(b, mask)}, bh{ir.ShiftRightLogical(b, sixteen)};
    const IR::U32 ll{ir.IMul(al, bl)}, hl{ir.IMul(ah, bl)}, lh{ir.IMul(al, bh)}, hh{ir.IMul(ah, bh)};
    const IR::U32 mid{ir.IAdd(ir.IAdd(ir.ShiftRightLogical(ll, sixteen), ir.BitwiseAnd(hl, mask)),
                              ir.BitwiseAnd(lh, mask))};
    IR::U32 high{ir.IAdd(ir.IAdd(hh, ir.ShiftRightLogical(hl, sixteen)),
                         ir.IAdd(ir.ShiftRightLogical(lh, sixteen), ir.ShiftRightLogical(mid, sixteen)))};
    if (is_signed) {
        const IR::U32 zero{ir.Imm32(0)};
        const IR::U32 fix_a{ir.Select(ir.ILessThan(a, zero, true), b, zero)};
        const IR::U32 fix_b{ir.Select(ir.ILessThan(b, zero, true), a, zero)};
        high = ir.ISub(high, ir.IAdd(fix_a, fix_b));
    }
    return high;
}

void IMAD(Translator& t) {
    // Rd = a * b + c (75 negates c; .X, 74: ~c and the carry 87-90 added); no carry out
    if (t.insn.PredDst(81) != PT) {
        t.Fail("IMAD carry out");
    }
    const bool extended{t.insn.Bit(74)};
    if (!extended) {
        t.Expect(87, 4, 0xf, "IMAD carry input");
    }
    t.Expect(76, 5, 0, "IMAD mode");
    t.Expect(84, 3, 0, "IMAD mode");
    t.Expect(72, 1, 0, "IMAD negated product");
    const AluForm form{t.insn.Form()};
    const IR::U32 a{t.Src0().value};
    const IR::U32 b{IntSrc1(t)};
    IR::U32 c;
    bool negate_c{};
    switch (form) {
    case AluForm::RegImm:
    case AluForm::RegCbuf:
        t.Expect(75, 1, 0, "IMAD negated addend");
        c = IntSrc2(t);
        break;
    case AluForm::RegUReg:
        t.Expect(62, 1, 0, "IMAD mode");
        negate_c = t.insn.Bit(63);
        c = t.UR(t.insn.UReg32());
        break;
    default:
        negate_c = t.insn.Bit(75);
        c = t.R(t.insn.Reg64());
        break;
    }
    if (negate_c) {
        c = extended ? t.ir.BitwiseNot(c) : IR::U32{t.ir.INeg(c)};
    }
    const bool zero_product{t.insn.Src0() == RZ || (form == AluForm::ImmReg && t.insn.Imm32() == 0) ||
                            (form == AluForm::RegReg && t.insn.Reg32() == RZ) ||
                            ((form == AluForm::RegImm || form == AluForm::RegCbuf || form == AluForm::RegUReg) &&
                             t.insn.Reg64() == RZ)};
    IR::U32 value{zero_product ? c : IR::U32{t.ir.IAdd(t.ir.IMul(a, b), c)}};
    if (extended) {
        value = IR::U32{t.ir.IAdd(value, One(t, t.PredSrc(87)))};
    }
    t.SetR(t.insn.Dst(), value);
}

void IMAD_WIDE(Translator& t, bool wide) {
    // .WIDE: Rd:Rd+1 = a * b + Rc:Rc+1; .HI: Rd = the high word of that; 73 signed
    if (t.insn.PredDst(81) != PT) {
        t.Fail("IMAD carry out");
    }
    t.Expect(74, 7, 0, "IMAD.WIDE mode");
    t.Expect(87, 4, 0xf, "IMAD.WIDE mode");
    t.Expect(84, 3, 0, "IMAD.WIDE mode");
    t.Expect(72, 1, 0, "IMAD.WIDE mode");
    const bool is_signed{t.insn.Bit(73)};
    const u32 dst{t.insn.Dst()};
    if (wide && dst != RZ && dst % 2 != 0) {
        t.Fail("unaligned destination pair");
    }
    const IR::U32 a{t.Src0().value};
    const IR::U32 b{IntSrc1(t)};
    IR::U32 c_low, c_high;
    switch (t.insn.Form()) {
    case AluForm::RegReg:
    case AluForm::ImmReg:
    case AluForm::CbufReg:
    case AluForm::URegReg: {
        const u32 c{t.insn.Reg64()};
        if (c != RZ && c % 2 != 0) {
            t.Fail("unaligned addend pair");
        }
        c_low = t.R(c);
        c_high = c == RZ ? t.ir.Imm32(0) : t.R(c + 1);
        break;
    }
    case AluForm::RegCbuf: {
        if (t.insn.CbufBindless() || t.insn.CbufOffset() % 8 != 0) {
            t.Fail("addend constant buffer pair");
        }
        const IR::U32 bank{t.ir.Imm32(t.insn.CbufBank())};
        c_low = t.ir.GetCbuf(bank, t.ir.Imm32(t.insn.CbufOffset()));
        c_high = t.ir.GetCbuf(bank, t.ir.Imm32(t.insn.CbufOffset() + 4));
        break;
    }
    default:
        t.Fail("IMAD.WIDE source form");
    }
    const IR::U64 product{t.ir.PackUint2x32(t.ir.CompositeConstruct(t.ir.IMul(a, b), MulHigh(t, a, b, is_signed)))};
    const IR::U64 sum{t.ir.IAdd(product, t.ir.PackUint2x32(t.ir.CompositeConstruct(c_low, c_high)))};
    const IR::Value words{t.ir.UnpackUint2x32(sum)};
    if (wide) {
        t.SetR(dst, IR::U32{t.ir.CompositeExtract(words, 0)});
        if (dst != RZ) {
            t.SetR(dst + 1, IR::U32{t.ir.CompositeExtract(words, 1)});
        }
    } else {
        t.SetR(dst, IR::U32{t.ir.CompositeExtract(words, 1)});
    }
}

void LOP3(Translator& t) {
    // Rd = LUT(a, b, c) (72-79); 81-83: P = (result != 0)
    t.Expect(80, 1, 0, "LOP3.PAND");
    t.Expect(87, 4, 0xf, "LOP3 predicate input");
    const IR::U32 a{t.Src0().value};
    const IR::U32 b{IntSrc1(t)};
    const IR::U32 c{IntSrc2(t)};
    const IR::U32 result{Maxwell::ApplyLUT(t.ir, a, b, c, t.insn.Field(72, 8))};
    t.SetR(t.insn.Dst(), result);
    t.SetP(t.insn.PredDst(81), t.ir.INotEqual(result, t.ir.Imm32(0)));
}

void ISETP(Translator& t) {
    // 76-78 the compare, 73 signed, 74-75 combined with 87-90; .EX (72): the high words of a 64-bit compare with the
    // low words' result in 68-71
    const u32 bop{t.insn.Field(74, 2)};
    if (bop > 2) {
        t.Fail("predicate combination");
    }
    const bool is_signed{t.insn.Bit(73)};
    const auto compare{static_cast<CompareOp>(t.insn.Field(76, 3))};
    const IR::U32 a{t.Src0().value};
    const IR::U32 b{IntSrc1(t)};
    IR::U1 cmp;
    if (t.insn.Bit(72)) {
        const IR::U1 low{t.PredSrc(68)};
        const IR::U1 equal{t.ir.IEqual(a, b)};
        switch (compare) {
        case CompareOp::Equal:
            cmp = t.ir.LogicalAnd(equal, low);
            break;
        case CompareOp::NotEqual:
            cmp = t.ir.LogicalOr(t.ir.INotEqual(a, b), low);
            break;
        case CompareOp::LessThan:
        case CompareOp::LessThanEqual:
            cmp = t.ir.LogicalOr(t.ir.ILessThan(a, b, is_signed), t.ir.LogicalAnd(equal, low));
            break;
        case CompareOp::GreaterThan:
        case CompareOp::GreaterThanEqual:
            cmp = t.ir.LogicalOr(t.ir.IGreaterThan(a, b, is_signed), t.ir.LogicalAnd(equal, low));
            break;
        default:
            t.Fail("extended compare");
        }
    } else {
        cmp = Maxwell::IntegerCompare(t.ir, a, b, compare, is_signed);
    }
    const IR::U1 accum{t.PredSrc(87)};
    const auto op{static_cast<BooleanOp>(bop)};
    t.SetP(t.insn.PredDst(81), Maxwell::PredicateCombine(t.ir, cmp, accum, op));
    t.SetP(t.insn.PredDst(84), Maxwell::PredicateCombine(t.ir, t.ir.LogicalNot(cmp), accum, op));
}

void IMNMX(Translator& t) {
    // 87-90: min when it holds, else max; 73 signed
    t.Expect(74, 13, 0, "IMNMX mode");
    t.Expect(72, 1, 0, "IMNMX mode");
    const bool is_signed{t.insn.Bit(73)};
    const IR::U32 a{t.Src0().value};
    const IR::U32 b{IntSrc1(t)};
    const IR::U32 min{t.ir.IMin(a, b, is_signed)};
    const IR::U32 max{t.ir.IMax(a, b, is_signed)};
    t.SetR(t.insn.Dst(), IR::U32{t.ir.Select(t.PredSrc(87), min, max)});
}

void SHF(Translator& t) {
    // (hi:lo) shifted left or right (76) by s, the type 73-74 (S64, U64, S32, U32) giving the clamp (or with .W, 75,
    // the wrap) of s and the signedness; 80: the result's high word, else its low word
    t.Expect(72, 1, 0, "SHF mode");
    t.Expect(77, 3, 0, "SHF mode");
    t.Expect(81, 10, 0, "SHF mode");
    const u32 type{t.insn.Field(73, 2)};
    const bool wrap{t.insn.Bit(75)}, right{t.insn.Bit(76)}, high_word{t.insn.Bit(80)};
    const bool is_signed{type == 0 || type == 2};
    const u32 bits{type < 2 ? 64U : 32U};
    const u32 lo_reg{t.insn.Src0()};
    const Operand shift_operand{t.Src1()};
    const IR::U32 lo{t.Src0().value};
    const IR::U32 hi{IntSrc2(t)};
    const AluForm form{t.insn.Form()};
    const bool hi_zero{form == AluForm::RegImm ? t.insn.Imm32() == 0
                       : form == AluForm::RegCbuf || form == AluForm::RegUReg ? false
                                                                                : t.insn.Reg64() == RZ};
    auto& ir{t.ir};
    if (shift_operand.immediate) {
        const u32 raw{t.insn.Imm32()};
        const u32 s{wrap ? raw & (bits - 1) : std::min(raw, bits)};
        if (s < 32) {
            const IR::U32 amount{ir.Imm32(s)}, rest{ir.Imm32(32 - s)};
            IR::U32 value;
            if (right && high_word) {
                value = is_signed ? ir.ShiftRightArithmetic(hi, amount) : ir.ShiftRightLogical(hi, amount);
            } else if (!right && !high_word) {
                value = ir.ShiftLeftLogical(lo, amount);
            } else if (right) {
                value = s == 0 ? lo
                        : hi_zero ? IR::U32{ir.ShiftRightLogical(lo, amount)}
                                  : ir.BitwiseOr(ir.ShiftRightLogical(lo, amount), ir.ShiftLeftLogical(hi, rest));
            } else {
                value = s == 0 ? hi
                        : lo_reg == RZ ? IR::U32{ir.ShiftLeftLogical(hi, amount)}
                                       : ir.BitwiseOr(ir.ShiftLeftLogical(hi, amount), ir.ShiftRightLogical(lo, rest));
            }
            t.SetR(t.insn.Dst(), value);
            return;
        }
    }
    // the general case in 64 bits; a shift by 64 or more gives 0 or the sign
    const IR::U64 x{ir.PackUint2x32(ir.CompositeConstruct(lo, hi))};
    IR::U32 s{shift_operand.value};
    s = wrap ? ir.BitwiseAnd(s, ir.Imm32(bits - 1)) : ir.UMin(s, ir.Imm32(bits));
    const IR::U32 s63{ir.UMin(s, ir.Imm32(63))};
    IR::U64 shifted;
    if (right) {
        shifted = is_signed ? ir.ShiftRightArithmetic(x, s63) : ir.ShiftRightLogical(x, s63);
        if (!is_signed) {
            shifted = IR::U64{ir.Select(ir.IGreaterThanEqual(s, ir.Imm32(64), false), ir.Imm64(u64{0}), shifted)};
        }
    } else {
        shifted = IR::U64{ir.Select(ir.IGreaterThanEqual(s, ir.Imm32(64), false), ir.Imm64(u64{0}),
                                    ir.ShiftLeftLogical(x, s63))};
    }
    const IR::Value words{ir.UnpackUint2x32(shifted)};
    t.SetR(t.insn.Dst(), IR::U32{ir.CompositeExtract(words, high_word ? 1 : 0)});
}

void SEL(Translator& t) {
    t.Expect(72, 15, 0, "SEL mode");
    const IR::U32 a{t.Src0().value};
    const IR::U32 b{IntSrc1(t)};
    t.SetR(t.insn.Dst(), IR::U32{t.ir.Select(t.PredSrc(87), a, b)});
}

void LEA(Translator& t) {
    // Rd = (a << s) + b, s at 75-79; .HI (80): the high word of (c:a) << s; 72 negates (.X: inverts) the shifted value,
    // 63 b (register, constant and uniform forms); .X (74) adds the carry 87-90; 81-83 the carry out
    const bool extended{t.insn.Bit(74)}, high_word{t.insn.Bit(80)};
    const AluForm form{t.insn.Form()};
    if (form != AluForm::RegReg && form != AluForm::ImmReg && form != AluForm::CbufReg && form != AluForm::URegReg) {
        t.Fail("LEA source form");
    }
    t.Expect(73, 1, 0, "LEA mode");
    if (form != AluForm::ImmReg) {
        t.Expect(62, 1, 0, "LEA mode");
    }
    if (!extended) {
        t.Expect(87, 4, 0xf, "LEA carry input");
    }
    const u32 shift{t.insn.Field(75, 5)};
    const bool negate_shifted{t.insn.Bit(72)};
    const bool negate_b{form != AluForm::ImmReg && t.insn.Bit(63)};
    auto& ir{t.ir};
    const IR::U32 a{t.Src0().value};
    IR::U32 b{t.Src1().value};
    IR::U32 shifted;
    if (high_word) {
        const IR::U32 c{t.R(t.insn.Reg64())};
        shifted = shift == 0 ? c
                             : ir.BitwiseOr(ir.ShiftLeftLogical(c, ir.Imm32(shift)),
                                            ir.ShiftRightLogical(a, ir.Imm32(32 - shift)));
    } else {
        shifted = shift == 0 ? a : IR::U32{ir.ShiftLeftLogical(a, ir.Imm32(shift))};
    }
    IR::U1 overflow{ir.Imm1(false)};
    const IR::U32 zero{ir.Imm32(0)};
    if (negate_shifted) {
        if (!extended) {
            overflow = ir.IEqual(shifted, zero);
        }
        shifted = extended ? ir.BitwiseNot(shifted) : IR::U32{ir.INeg(shifted)};
    }
    if (negate_b) {
        if (!extended) {
            overflow = ir.LogicalOr(overflow, ir.IEqual(b, zero));
        }
        b = extended ? ir.BitwiseNot(b) : IR::U32{ir.INeg(b)};
    }
    IR::U32 sum{ir.IAdd(shifted, b)};
    overflow = ir.LogicalOr(overflow, ir.ILessThan(sum, shifted, false));
    if (extended) {
        const IR::U1 carry{t.PredSrc(87)};
        sum = IR::U32{ir.IAdd(sum, One(t, carry))};
        overflow = ir.LogicalOr(overflow, ir.LogicalAnd(carry, ir.IEqual(sum, zero)));
    }
    t.SetR(t.insn.Dst(), sum);
    t.SetP(t.insn.PredDst(81), overflow);
}

void PLOP3(Translator& t) {
    // dst0 (81-83) = LUT0(p0, p1, p2), its table at 64-66 and 72-76; dst1 (84-86) with the table at 16-23; the sources
    // at 87-90, 77-80 and 68-71 (a uniform predicate by 67)
    const IR::U1 inputs[3]{t.PredSrc(87), t.PredSrc(77), t.insn.Bit(67) ? t.UPredSrc(68) : t.PredSrc(68)};
    const u32 lut0{t.insn.Field(64, 3) | (t.insn.Field(72, 5) << 3)};
    const u32 lut1{t.insn.Field(16, 8)};
    const auto mask{[&](const IR::U1& p) { return IR::U32{t.ir.Select(p, t.ir.Imm32(~0U), t.ir.Imm32(0))}; }};
    const IR::U32 a{mask(inputs[0])}, b{mask(inputs[1])}, c{mask(inputs[2])};
    for (const auto& [dst, lut] : {std::pair{t.insn.PredDst(81), lut0}, std::pair{t.insn.PredDst(84), lut1}}) {
        if (dst != PT) {
            t.SetP(dst, t.ir.INotEqual(Maxwell::ApplyLUT(t.ir, a, b, c, lut), t.ir.Imm32(0)));
        }
    }
}

void IABS(Translator& t) {
    t.Expect(64, 27, 0, "IABS mode");
    t.SetR(t.insn.Dst(), t.ir.IAbs(IntSrc1(t)));
}
} // Anonymous namespace

void TranslateInteger(Translator& t) {
    switch (t.insn.AluOp()) {
    case 0x010:
        return IADD3(t, false);
    case 0x090:
        return IADD3(t, true);
    case 0x011:
        return LEA(t);
    case 0x01c:
        return PLOP3(t);
    case 0x013:
        return IABS(t);
    case 0x024:
        return IMAD(t);
    case 0x025:
        return IMAD_WIDE(t, true);
    case 0x027:
        return IMAD_WIDE(t, false);
    case 0x012:
        return LOP3(t);
    case 0x00c:
        return ISETP(t);
    case 0x017:
        return IMNMX(t);
    case 0x019:
        return SHF(t);
    case 0x007:
        return SEL(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
