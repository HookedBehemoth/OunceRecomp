// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's attribute access: ALD and AST (the stage's inputs and outputs at a fixed address),
// IPA (a fragment stage's interpolated inputs, also indexed by a uniform register) and PIXLD (the sample index).

#include "shader_recompiler/frontend/ampere/translate/memory.h"
#include "shader_recompiler/program_header.h"

namespace Shader::Ampere {
namespace {
/// ALD and AST: the address 40-49, components 74-75 (+ 1); patch (76), physical (77) and indexed (Ra) accesses and
/// ALD of outputs (79) are not translated
u32 CheckAttributeAccess(Translator& t) {
    t.Expect(76, 2, 0, "patch or physical attribute access");
    t.Expect(24, 8, RZ, "indexed attribute access");
    const u32 address{t.insn.Field(40, 10)};
    const u32 components{t.insn.Field(74, 2) + 1};
    if (address % 4 != 0 || address + components * 4 > 0x400) {
        t.Fail("attribute address");
    }
    return components;
}

void ALD(Translator& t) {
    t.Expect(79, 1, 0, "output attribute read");
    const u32 components{CheckAttributeAccess(t)};
    const u32 address{t.insn.Field(40, 10)};
    const u32 dst{t.insn.Dst()};
    if (dst == RZ) {
        return;
    }
    const IR::U32 vertex{t.R(t.insn.Reg32())};
    for (u32 i = 0; i < components; ++i) {
        t.SetF(dst + i, t.ir.GetAttribute(IR::Attribute{address / 4 + i}, vertex));
    }
}

void AST(Translator& t) {
    const u32 components{CheckAttributeAccess(t)};
    const u32 address{t.insn.Field(40, 10)};
    if (address == 0x5c && components == 1) {
        // a per-primitive hint (the shading rate) without a meaning here: nothing reads it back
        return;
    }
    const u32 data{t.insn.Reg32()};
    const IR::U32 vertex{t.R(t.insn.Reg64())};
    for (u32 i = 0; i < components; ++i) {
        const IR::F32 value{data == RZ ? t.ir.Imm32(0.0f) : t.F(data + i)};
        t.ir.SetAttribute(IR::Attribute{address / 4 + i}, value, vertex);
    }
}

void IPA(Translator& t) {
    // the attribute 64-71 (address / 4), the location 76-77 (default or centroid), the frequency 78-79 (pass or
    // constant); no offset register (32-39) and no predicate output (81-83). 0x526: the attribute's byte address plus
    // a uniform register (24-29, bit 91), an indexed input; its interpolation is the base attribute's
    const bool indexed{t.insn.Opcode() == 0x526};
    t.Expect(32, 8, RZ, "IPA offset");
    t.Expect(79, 1, 0, "IPA frequency");
    t.Expect(81, 3, PT, "IPA predicate output");
    t.Expect(84, 7, 0, "IPA mode");
    t.Expect(91, 1, indexed ? 1 : 0, "IPA mode");
    if (t.insn.Field(76, 2) > 1) {
        t.Fail("IPA location");
    }
    const bool constant{t.insn.Bit(78)};
    const IR::Attribute attribute{t.insn.Field(64, 8)};
    const u32 index{indexed ? t.insn.Field(24, 6) : URZ};
    if (indexed) {
        t.Expect(30, 2, 0, "IPA index register");
    }
    IR::F32 value{index == URZ ? t.ir.GetAttribute(attribute)
                               : t.ir.GetAttributeIndexed(t.ir.IAdd(t.UR(index), t.ir.Imm32(t.insn.Field(64, 8) * 4)))};
    const bool is_legacy{IR::IsLegacyAttribute(attribute)};
    if (IR::IsGeneric(attribute) || is_legacy) {
        // IPA.PASS of a perspective input gives it times the interpolated w (NAK nak_nir_lower_fs_inputs.c multiplies
        // by 1 / IPA.PASS of the position's w on SM70+); .CONSTANT gives the provoking vertex's value as it is
        bool is_perspective{is_legacy};
        if (!is_legacy) {
            const ProgramHeader& sph{t.env.SPH()};
            const std::array input_map{sph.ps.GenericInputMap(IR::GenericAttributeIndex(attribute))};
            PixelImap effective{PixelImap::Unused};
            for (const PixelImap component : input_map) {
                if (component != PixelImap::Unused) {
                    effective = component;
                    break;
                }
            }
            is_perspective = effective == PixelImap::Perspective || effective == PixelImap::Unused;
        }
        if (is_perspective && !constant) {
            value = t.ir.FPMul(value, t.ir.GetAttribute(IR::Attribute::PositionW));
        }
    }
    t.SetF(t.insn.Dst(), value);
}

void PIXLD(Translator& t) {
    // the value 78-80: only MY_INDEX (3), the sample's index, is translated
    t.Expect(24, 40, 0, "PIXLD mode");
    ExpectOnly(t, HighBits(78, 84), "PIXLD mode");
    t.Expect(81, 3, PT, "PIXLD predicate output");
    if (t.insn.Field(78, 3) != 3) {
        t.Fail("PIXLD value");
    }
    t.SetR(t.insn.Dst(), t.ir.SampleId());
}
} // Anonymous namespace

void TranslateAttribute(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0x321:
        return ALD(t);
    case 0x322:
        return AST(t);
    case 0x326:
    case 0x526:
        return IPA(t);
    case 0x925:
        return PIXLD(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
