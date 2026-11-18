// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's moves between predicates and registers: R2P, P2R and UP2UR (P2R of the uniform
// predicates into a UR).

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
/// The immediate mask (32-63) of P0-P6, and the byte (76-77) of the register they map to
u32 PredicateMask(Translator& t) {
    if (t.insn.Form() != AluForm::ImmReg) {
        t.Fail("predicate mask source form");
    }
    const u32 mask{t.insn.Imm32()};
    if (mask > 0x7f) {
        t.Fail("predicate mask past P6");
    }
    return mask;
}

void R2P(Translator& t) {
    // Pi = bit (byte * 8 + i) of Ra, for each i in the mask
    t.Expect(16, 8, 0, "R2P destination");
    t.Expect(64, 12, 0, "R2P mode");
    t.Expect(78, 14, 0, "R2P mode");
    const u32 mask{PredicateMask(t)};
    const u32 base{t.insn.Field(76, 2) * 8};
    const IR::U32 source{t.Src0().value};
    for (u32 i = 0; i < 7; ++i) {
        if ((mask >> i) & 1) {
            const IR::U32 bit{t.ir.BitwiseAnd(source, t.ir.Imm32(1U << (base + i)))};
            t.SetP(i, t.ir.INotEqual(bit, t.ir.Imm32(0)));
        }
    }
}

void P2R(Translator& t) {
    // Rd = Ra with bit (byte * 8 + i) replaced by Pi, for each i in the mask; UP2UR: URd, URa and the UPs
    t.Expect(64, 12, 0, "P2R mode");
    t.Expect(78, 13, 0, "P2R mode");
    const u32 mask{PredicateMask(t)};
    const u32 base{t.insn.Field(76, 2) * 8};
    auto& ir{t.ir};
    IR::U32 value{ir.BitwiseAnd(t.Src0().value, ir.Imm32(~(mask << base)))};
    for (u32 i = 0; i < 7; ++i) {
        if ((mask >> i) & 1) {
            const IR::U1 pred{t.uniform ? t.UP(i) : t.P(i)};
            value = ir.BitwiseOr(value, IR::U32{ir.Select(pred, ir.Imm32(1U << (base + i)), ir.Imm32(0))});
        }
    }
    t.SetX(t.insn.Dst(), value);
}
} // Anonymous namespace

void TranslatePredicate(Translator& t) {
    switch (t.insn.AluOp()) {
    case 0x004:
        return R2P(t);
    case 0x003:
    case 0x083:
        return P2R(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
