// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's matrix operations, warp-collective on 16-bit elements in the PTX mma/ldmatrix
// fragment layouts (two f16 a register, the low half first):
// - HMMA.16816.F16: D(16x8) = A(16x16) * B(16x8) + C as MatrixMulAddF16, which the SPIR-V back end runs on
//   cooperative matrices or in software.
// - LDSM.16.M88/.MT88 (.1, .2, .4): lanes 8m..8m+7 give the row addresses of matrix m; lane t receives row t/4,
//   bytes 4(t%4).. of each (.MT88: rows 2(t%4) and 2(t%4)+1 at column t/4). The rows other lanes stored are read
//   between two subgroup barriers: the console runs a warp's shared memory accesses in order, Vulkan orders another
//   lane's store and this lane's load only through a barrier.
// - MOVM.16.MT88: the 8x8 matrix held one row pair a lane, transposed.

#include <array>

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
IR::U32 Lane(Translator& t, const IR::U32& value, const IR::U32& lane) {
    return t.ir.ShuffleIndex(value, lane, t.ir.Imm32(31), t.ir.Imm32(0));
}

IR::U32 Pack16(Translator& t, const IR::U32& low, const IR::U32& high) {
    return t.ir.BitFieldInsert(low, high, t.ir.Imm32(16), t.ir.Imm32(16));
}

void HMMA(Translator& t) {
    // Rd..Rd+1 = A (Ra..Ra+3) * B (Rb..Rb+1, 32) + C (Rc..Rc+1, 64; RZ: zero); 75/78 the shape, 76 F32 result,
    // 82-83 the source type
    t.Expect(40, 24, 0, "HMMA mode");
    t.Expect(72, 33, 0x8, "HMMA mode other than .16816.F16");
    const auto regs = [&](u32 first, u32 count) {
        std::array<IR::U32, 4> values;
        for (u32 i = 0; i < count; ++i) {
            values[i] = first == RZ ? t.ir.Imm32(0) : t.R(first + i);
        }
        return values;
    };
    const auto a{regs(t.insn.Src0(), 4)};
    const auto b{regs(t.insn.Reg32(), 2)};
    const auto c{regs(t.insn.Reg64(), 2)};
    const IR::Value d{t.ir.MatrixMulAddF16(t.ir.CompositeConstruct(a[0], a[1], a[2], a[3]),
                                           t.ir.CompositeConstruct(b[0], b[1]), t.ir.CompositeConstruct(c[0], c[1]))};
    const u32 dst{t.insn.Dst()};
    if (dst != RZ) {
        t.SetR(dst, IR::U32{t.ir.CompositeExtract(d, 0)});
        t.SetR(dst + 1, IR::U32{t.ir.CompositeExtract(d, 1)});
    }
}

void LDSM(Translator& t) {
    // Rd..Rd+n-1 (n by 72-73: 1, 2, 4) from [Ra + URb (32, bit 91) + offset (40-63)]; 78-79 .M88 or .MT88
    t.Expect(64, 8, 0, "LDSM mode");
    t.Expect(74, 4, 0, "LDSM mode");
    t.Expect(79, 12, 0, "LDSM mode");
    t.Expect(92, 13, 0, "LDSM mode");
    if (!t.insn.Bit(91)) {
        t.Expect(32, 8, 0, "LDSM mode");
    }
    if (t.insn.Field(72, 2) == 3) {
        t.Fail("LDSM matrix count");
    }
    const u32 count{1u << t.insn.Field(72, 2)};
    const bool transposed{t.insn.Bit(78)};
    const u32 dst{t.insn.Dst()};
    auto& ir{t.ir};
    const s32 offset{static_cast<s32>(t.insn.Field(40, 24) << 8) >> 8};
    IR::U32 base{t.R(t.insn.Src0())};
    if (t.insn.Bit(91)) {
        base = ir.IAdd(base, t.UR(t.insn.UReg32()));
    }
    base = ir.IAdd(base, ir.Imm32(offset));
    const IR::U32 lane{ir.LaneId()};
    const IR::U32 quad_lane{ir.BitwiseAnd(lane, ir.Imm32(3))};
    const IR::U32 quad{ir.ShiftRightLogical(lane, ir.Imm32(2))};
    ir.SubgroupBarrier();
    std::array<IR::U32, 4> values;
    for (u32 matrix = 0; matrix < count; ++matrix) {
        if (!transposed) {
            const IR::U32 row{Lane(t, base, ir.IAdd(quad, ir.Imm32(matrix * 8)))};
            const IR::U32 address{ir.IAdd(row, ir.ShiftLeftLogical(quad_lane, ir.Imm32(2)))};
            values[matrix] = IR::U32{ir.LoadShared(32, false, address)};
            continue;
        }
        std::array<IR::U32, 2> halves;
        for (u32 half = 0; half < 2; ++half) {
            const IR::U32 row_lane{
                ir.IAdd(ir.ShiftLeftLogical(quad_lane, ir.Imm32(1)), ir.Imm32(matrix * 8 + half))};
            const IR::U32 row{Lane(t, base, row_lane)};
            const IR::U32 address{ir.IAdd(row, ir.ShiftLeftLogical(quad, ir.Imm32(1)))};
            halves[half] = IR::U32{ir.LoadShared(16, false, address)};
        }
        values[matrix] = Pack16(t, halves[0], halves[1]);
    }
    ir.SubgroupBarrier();
    if (dst != RZ) {
        for (u32 matrix = 0; matrix < count; ++matrix) {
            t.SetR(dst + matrix, values[matrix]);
        }
    }
}

void MOVM(Translator& t) {
    // Rd = Ra transposed: lane t's half h is the half (t/4)%2 of lane 8(t%4) + 4h + t/8. 78-79 .MT88 only
    t.Expect(32, 32, 0, "MOVM mode");
    t.Expect(64, 41, 0, "MOVM mode");
    auto& ir{t.ir};
    const IR::U32 value{t.R(t.insn.Src0())};
    const IR::U32 lane{ir.LaneId()};
    const IR::U32 first{ir.IAdd(ir.ShiftLeftLogical(ir.BitwiseAnd(lane, ir.Imm32(3)), ir.Imm32(3)),
                                ir.ShiftRightLogical(lane, ir.Imm32(3)))};
    const IR::U32 shift{ir.BitwiseAnd(ir.ShiftLeftLogical(lane, ir.Imm32(2)), ir.Imm32(16))};
    std::array<IR::U32, 2> halves;
    for (u32 half = 0; half < 2; ++half) {
        const IR::U32 source{Lane(t, value, half == 0 ? first : IR::U32{ir.IAdd(first, ir.Imm32(4))})};
        halves[half] = ir.BitFieldExtract(source, shift, ir.Imm32(16));
    }
    t.SetR(t.insn.Dst(), Pack16(t, halves[0], halves[1]));
}
} // Anonymous namespace

void TranslateMatrix(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0x23c:
        return HMMA(t);
    case 0x83b:
        return LDSM(t);
    case 0x23a:
        return MOVM(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
