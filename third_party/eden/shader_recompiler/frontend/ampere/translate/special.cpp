// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.; the special register numbers
// are NAK's (nak_private.h NAK_SV_*)

// (fork) The direct Ampere front's special registers: S2R, S2UR (the same registers into a uniform register) and CS2R
// of SRZ. Semantic choices: SR_THREAD_KILL is the helper-invocation mask; SR_VARIABLE_RATE is a 1x1 fragment, as
// without variable-rate shading; SR_INVOCATION_INFO is the IR's InvocationInfo (in a geometry stage the vertex count
// of the input primitive in byte 2 and 0 in byte 0, so the map index of input vertex v is v: see isbe.cpp). The
// clocks and any other register fail.

#include <fmt/format.h>

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
IR::U32 SpecialRegister(Translator& t, u32 index) {
    auto& ir{t.ir};
    switch (index) {
    case 0x00: // SR_LANEID
        return ir.LaneId();
    case 0x11: // SR_INVOCATION_ID
        return ir.InvocationId();
    case 0x12: // SR_Y_DIRECTION
        return ir.BitCast<IR::U32>(ir.YDirection());
    case 0x13: // SR_THREAD_KILL
        return IR::U32{ir.Select(ir.IsHelperInvocation(), ir.Imm32(-1), ir.Imm32(0))};
    case 0x1d: // SR_INVOCATION_INFO
        return ir.InvocationInfo();
    case 0x1e: // SR_WSCALEFACTOR_XY
        return ir.SR_WScaleFactorXY();
    case 0x1f: // SR_WSCALEFACTOR_Z
        return ir.SR_WScaleFactorZ();
    case 0x20: { // SR_TID: x in 0-15, y in 16-25, z in 26-31
        const IR::Value tid{ir.LocalInvocationId()};
        return ir.BitFieldInsert(ir.BitFieldInsert(IR::U32{ir.CompositeExtract(tid, 0)},
                                                   IR::U32{ir.CompositeExtract(tid, 1)}, ir.Imm32(16), ir.Imm32(10)),
                                 IR::U32{ir.CompositeExtract(tid, 2)}, ir.Imm32(26), ir.Imm32(6));
    }
    case 0x21:
        return ir.LocalInvocationIdX();
    case 0x22:
        return ir.LocalInvocationIdY();
    case 0x23:
        return ir.LocalInvocationIdZ();
    case 0x25:
        return ir.WorkgroupIdX();
    case 0x26:
        return ir.WorkgroupIdY();
    case 0x27:
        return ir.WorkgroupIdZ();
    case 0x38:
        return ir.SubgroupEqMask();
    case 0x39:
        return ir.SubgroupLtMask();
    case 0x3a:
        return ir.SubgroupLeMask();
    case 0x3b:
        return ir.SubgroupGtMask();
    case 0x3c:
        return ir.SubgroupGeMask();
    case 0x84: // SR_VARIABLE_RATE: the fragment's width and height in bytes 1 and 2
        return ir.Imm32(0x00010100);
    default:
        t.Fail(fmt::format("special register {:#x}", index));
    }
}

void S2R(Translator& t) {
    // Rd (S2UR: URd) = the special register at 72-79
    t.Expect(32, 32, 0, "S2R mode");
    t.Expect(80, 12, 0, "S2R mode");
    t.Expect(64, 8, 0, "S2R mode");
    t.Expect(24, 8, 0, "S2R mode");
    const IR::U32 value{SpecialRegister(t, t.insn.Field(72, 8))};
    if (t.insn.Opcode() == 0x9c3) {
        t.Expect(22, 2, 0, "S2UR destination");
        t.SetUR(t.insn.Field(16, 6), value);
    } else {
        t.SetR(t.insn.Dst(), value);
    }
}

void CS2R(Translator& t) {
    // CS2R Rd, SRZ (.64 at 80: a pair): zeros; the clocks and counters are not translated
    t.Expect(72, 8, 255, "CS2R special register");
    t.Expect(81, 11, 0, "CS2R mode");
    t.Expect(24, 40, 0, "CS2R mode");
    t.Expect(64, 8, 0, "CS2R mode");
    const u32 dst{t.insn.Dst()};
    t.SetR(dst, t.ir.Imm32(0));
    if (t.insn.Bit(80) && dst != RZ) {
        t.SetR(dst + 1, t.ir.Imm32(0));
    }
}
} // Anonymous namespace

void TranslateSpecial(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0x919:
    case 0x9c3:
        return S2R(t);
    case 0x805:
        return CS2R(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
