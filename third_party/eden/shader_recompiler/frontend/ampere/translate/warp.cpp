// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's warp operations: VOTE and VOTEU (the ballot into a GPR or a uniform register, the
// vote into P or UP), SHFL (every lane and clamp form), MATCH.ALL, REDUX (into a uniform register) and FSWZADD.
// Semantics as the hardware's (PTX shfl.sync, match.all.sync, redux.sync): SHFL's c packs the clamp (bits 0-4) and the
// segment mask (8-12), an out-of-bounds lane reads its own value; FSWZADD's swizzle holds lane 0's operation in bits
// 6-7 and lane 3's in 0-1 (NAK's OpFSwzAdd), 0 a + b, 1 a - b, 2 b - a, 3 b. In fragment stages the quad shuffles
// (c = 0x1c03, an immediate lane) are QuadBroadcast and QuadSwap, so helper lanes take part as derivatives need.

#include <array>
#include <optional>

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
void VOTE(Translator& t) {
    // Rd (VOTEU: URd) = the ballot of Pa (87-90); Pd (VOTEU: UPd, 81-83) = ALL, ANY or EQ (72-73) of it
    const bool uniform{t.insn.Opcode() == 0x886};
    t.Expect(24, 48, 0, "VOTE mode");
    t.Expect(74, 7, 0, "VOTE mode");
    t.Expect(84, 3, 0, "VOTE mode");
    t.Expect(91, 14, 0, "VOTE mode");
    const IR::U1 pred{t.PredSrc(87)};
    const u32 dst{t.insn.PredDst(81)};
    if (dst != PT) {
        IR::U1 vote;
        switch (t.insn.Field(72, 2)) {
        case 0:
            vote = t.ir.VoteAll(pred);
            break;
        case 1:
            vote = t.ir.VoteAny(pred);
            break;
        case 2:
            vote = t.ir.VoteEqual(pred);
            break;
        default:
            t.Fail("VOTE operation");
        }
        uniform ? t.SetUP(dst, vote) : t.SetP(dst, vote);
    } else if (t.insn.Field(72, 2) == 3) {
        t.Fail("VOTE operation");
    }
    if (uniform) {
        t.Expect(22, 2, 0, "VOTEU uniform destination");
        if (t.insn.Dst() != URZ) {
            t.SetUR(t.insn.Dst(), t.ir.SubgroupBallot(pred));
        }
    } else if (t.insn.Dst() != RZ) {
        t.SetR(t.insn.Dst(), t.ir.SubgroupBallot(pred));
    }
}

/// An operand that is an immediate or a register (RZ reads as the immediate 0)
struct LaneOperand {
    IR::U32 value;
    std::optional<u32> immediate;
};

LaneOperand RegOrImm(Translator& t, bool is_immediate, u32 immediate, u32 reg) {
    if (is_immediate) {
        return {t.ir.Imm32(immediate), immediate};
    }
    if (reg == RZ) {
        return {t.ir.Imm32(0), 0u};
    }
    return {t.R(reg), std::nullopt};
}

void SHFL(Translator& t) {
    // Rd = Ra of the lane by the mode (58-59: IDX, UP, DOWN, BFLY), the lane b and c; Pd (81-83) = in bounds.
    // 0x389: b Rb (32), c Rc (64); 0x589: b Rb, c imm 40-52; 0x989: b imm 53-57, c Rc; 0xf89: b imm, c imm
    const u32 opcode{t.insn.Opcode()};
    const bool lane_imm{opcode == 0x989 || opcode == 0xf89};
    const bool c_imm{opcode == 0x589 || opcode == 0xf89};
    if (!lane_imm) {
        t.Expect(53, 5, 0, "SHFL mode");
    } else {
        t.Expect(32, 8, 0, "SHFL mode");
    }
    if (!c_imm) {
        t.Expect(40, 13, 0, "SHFL mode");
    } else {
        t.Expect(64, 8, 0, "SHFL mode");
    }
    t.Expect(60, 4, 0, "SHFL mode");
    t.Expect(72, 9, 0, "SHFL mode");
    t.Expect(84, 21, 0, "SHFL mode");
    const LaneOperand lane{RegOrImm(t, lane_imm, t.insn.Field(53, 5), t.insn.Reg32())};
    const LaneOperand c{RegOrImm(t, c_imm, t.insn.Field(40, 13), t.insn.Reg64())};
    const u32 mode{t.insn.Field(58, 2)};
    const IR::U32 value{t.R(t.insn.Src0())};
    const u32 in_bounds{t.insn.PredDst(81)};

    constexpr u32 QUAD{(0x1c << 8) | 3};
    if (t.env.ShaderStage() == Stage::Fragment && c.immediate == QUAD && lane.immediate) {
        const u32 l{*lane.immediate};
        if (mode == 0 && l <= 3) {
            t.SetR(t.insn.Dst(), t.ir.QuadBroadcast(value, t.ir.Imm32(l)));
            t.SetP(in_bounds, t.ir.Imm1(true));
            return;
        }
        if (mode == 3 && l >= 1 && l <= 3) {
            t.SetR(t.insn.Dst(), t.ir.QuadSwap(value, t.ir.Imm32(l - 1)));
            t.SetP(in_bounds, t.ir.Imm1(true));
            return;
        }
    }
    const IR::U32 clamp{c.immediate ? t.ir.Imm32(*c.immediate & 0x1f)
                                    : t.ir.BitFieldExtract(c.value, t.ir.Imm32(0), t.ir.Imm32(5))};
    const IR::U32 segment{c.immediate ? t.ir.Imm32((*c.immediate >> 8) & 0x1f)
                                      : t.ir.BitFieldExtract(c.value, t.ir.Imm32(8), t.ir.Imm32(5))};
    IR::U32 result;
    switch (mode) {
    case 0:
        result = t.ir.ShuffleIndex(value, lane.value, clamp, segment);
        break;
    case 1:
        result = t.ir.ShuffleUp(value, lane.value, clamp, segment);
        break;
    case 2:
        result = t.ir.ShuffleDown(value, lane.value, clamp, segment);
        break;
    default:
        result = t.ir.ShuffleButterfly(value, lane.value, clamp, segment);
        break;
    }
    if (in_bounds != PT) {
        t.SetP(in_bounds, t.ir.GetInBoundsFromOp(result));
    }
    t.SetR(t.insn.Dst(), result);
}

void MATCH(Translator& t) {
    // MATCH.ALL Rd, Pd (81-83), Ra (.U64 at 73: Ra, Ra + 1): Pd = every active lane holds the first's value, Rd = the
    // active lanes then, else 0. MATCH.ANY (79) is not translated
    t.Expect(32, 32, 0, "MATCH mode");
    t.Expect(64, 9, 0, "MATCH mode");
    t.Expect(74, 7, 0, "MATCH mode");
    t.Expect(84, 21, 0, "MATCH mode");
    const bool wide{t.insn.Bit(73)};
    const u32 src{t.insn.Src0()};
    if (wide && src != RZ && src % 2 != 0) {
        t.Fail("unaligned MATCH.U64 source");
    }
    const auto equal_to_first = [&](const IR::U32& value) {
        return t.ir.IEqual(value, t.ir.SubgroupReadFirstLane(value));
    };
    IR::U1 equal{equal_to_first(t.R(src))};
    if (wide && src != RZ) {
        equal = t.ir.LogicalAnd(equal, equal_to_first(t.R(src + 1)));
    }
    const IR::U1 all{t.ir.VoteAll(equal)};
    t.SetP(t.insn.PredDst(81), all);
    if (t.insn.Dst() != RZ) {
        t.SetR(t.insn.Dst(), IR::U32{t.ir.Select(all, t.ir.SubgroupBallot(t.ir.Imm1(true)), t.ir.Imm32(0))});
    }
}

void REDUX(Translator& t) {
    // URd = the reduction (78-80: AND, OR, XOR, SUM, MIN, MAX; signed 73) of Ra over the active lanes
    t.Expect(22, 2, 0, "REDUX uniform destination");
    t.Expect(32, 41, 0, "REDUX mode");
    t.Expect(74, 4, 0, "REDUX mode");
    t.Expect(81, 24, 0, "REDUX mode");
    using Op = IR::IREmitter::SubgroupReduceOp;
    const bool is_signed{t.insn.Bit(73)};
    Op op;
    switch (t.insn.Field(78, 3)) {
    case 0:
        op = Op::And;
        break;
    case 1:
        op = Op::Or;
        break;
    case 2:
        op = Op::Xor;
        break;
    case 3:
        op = Op::IAdd;
        break;
    case 4:
        op = is_signed ? Op::SMin : Op::UMin;
        break;
    case 5:
        op = is_signed ? Op::SMax : Op::UMax;
        break;
    default:
        t.Fail("REDUX operation");
    }
    t.SetUR(t.insn.Dst(), t.ir.SubgroupReduce(op, t.R(t.insn.Src0())));
}

void FSWZADD(Translator& t) {
    // Rd = Ra op Rb (64) by this lane's operation in the swizzle (32-39); rounding 78-79, .FTZ 80; .NDV (77) fails
    t.Expect(40, 24, 0, "FSWZADD mode");
    t.Expect(72, 6, 0, "FSWZADD mode");
    t.Expect(81, 24, 0, "FSWZADD mode");
    const u32 swizzle{t.insn.Field(32, 8)};
    // the quad lanes (bit i: lane i) where a is negated or dropped and where b is negated
    u32 negate_a{}, drop_a{}, negate_b{};
    for (u32 lane = 0; lane < 4; ++lane) {
        switch ((swizzle >> (6 - lane * 2)) & 3) {
        case 1:
            negate_b |= 1u << lane;
            break;
        case 2:
            negate_a |= 1u << lane;
            break;
        case 3:
            drop_a |= 1u << lane;
            break;
        default:
            break;
        }
    }
    auto& ir{t.ir};
    const IR::U32 quad_lane{ir.BitwiseAnd(ir.LaneId(), ir.Imm32(3))};
    const auto in = [&](u32 lanes) {
        return ir.INotEqual(ir.BitFieldExtract(ir.Imm32(lanes), quad_lane, ir.Imm32(1)), ir.Imm32(0));
    };
    IR::F32 a{t.F(t.insn.Src0())};
    IR::F32 b{t.F(t.insn.Reg64())};
    if (negate_a != 0) {
        a = IR::F32{ir.Select(in(negate_a), ir.FPNeg(a), a)};
    }
    if (drop_a != 0) {
        // b + -0 is b, signed zeros and NaNs included
        a = IR::F32{ir.Select(in(drop_a), ir.Imm32(-0.0f), a)};
    }
    if (negate_b != 0) {
        b = IR::F32{ir.Select(in(negate_b), ir.FPNeg(b), b)};
    }
    static constexpr std::array rounding{IR::FpRounding::RN, IR::FpRounding::RM, IR::FpRounding::RP,
                                         IR::FpRounding::RZ};
    const IR::FpControl control{.no_contraction = true,
                                .rounding = rounding[t.insn.Field(78, 2)],
                                .fmz_mode = t.insn.Bit(80) ? IR::FmzMode::FTZ : IR::FmzMode::None};
    t.SetF(t.insn.Dst(), ir.FPAdd(a, b, control));
}
} // Anonymous namespace

void TranslateWarp(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0x806:
    case 0x886:
        return VOTE(t);
    case 0x389:
    case 0x589:
    case 0x989:
    case 0xf89:
        return SHFL(t);
    case 0x3a1:
        return MATCH(t);
    case 0x3c4:
        return REDUX(t);
    case 0x822:
        return FSWZADD(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
