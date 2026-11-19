// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's stage buffer: ISBERD and ISBEWR, [Ra + offset 40-55], the size 74-75 (U8, U16,
// B32), the region 76-77 (map, patch, primitive, attribute), .SKEW 78 (lane-interleaved), the output buffer 79.
// - Task and mesh stages: the IR's ISBE ops, with the back end's model of the buffer (emit_spirv_mesh.cpp): reads of
//   the input attribute region (IsbeRead) and of the output one (IsbeReadOutput), writes to the output attribute region
//   (IsbeWriteAttribute) and to the map
//   (IsbeWriteMapU8, IsbeWriteMapU32). That model lays the buffer out by address alone, so .SKEW changes nothing.
// - Geometry stages: a map read (U8) is an input vertex's handle. The handle is opaque and only ALD consumes it, so
//   it is the map index itself, which SR_INVOCATION_INFO (special.cpp) makes the input vertex number.

#include "shader_recompiler/frontend/ampere/translate/memory.h"

namespace Shader::Ampere {
namespace {
enum class Region : u32 { Map, Patch, Primitive, Attribute };

IR::U32 IsbeAddress(Translator& t) {
    t.Expect(56, 8, 0, "stage buffer offset");
    const u32 offset{t.insn.Field(40, 16)};
    const u32 ra{t.insn.Src0()};
    if (ra == RZ) {
        return t.ir.Imm32(offset);
    }
    return offset == 0 ? t.R(ra) : IR::U32{t.ir.IAdd(t.R(ra), t.ir.Imm32(offset))};
}

void ISBERD(Translator& t) {
    ExpectOnly(t, HighBits(74, 80), "ISBERD mode");
    t.Expect(32, 8, 0, "ISBERD mode");
    const u32 size{t.insn.Field(74, 2)};
    const Region region{t.insn.Field(76, 2)};
    const bool output{t.insn.Bit(79)};
    const Stage stage{t.env.ShaderStage()};
    if (stage == Stage::Geometry) {
        if (region != Region::Map || size != 0 || output || t.insn.Bit(78)) {
            t.Fail("geometry stage buffer read");
        }
        t.SetR(t.insn.Dst(), IsbeAddress(t));
        return;
    }
    if (stage != Stage::Task && stage != Stage::Mesh) {
        t.Fail("stage buffer read outside task, mesh and geometry stages");
    }
    if (region != Region::Attribute || size != 2) {
        t.Fail("stage buffer read mode");
    }
    // (the output buffer: what this stage wrote there, read back; a task stage that keeps its meshlet count in its
    // payload and adds to it)
    t.SetR(t.insn.Dst(), output ? t.ir.IsbeReadOutput(IsbeAddress(t)) : t.ir.IsbeRead(IsbeAddress(t)));
}

void ISBEWR(Translator& t) {
    ExpectOnly(t, HighBits(74, 80), "ISBEWR mode");
    t.Expect(16, 8, 0, "ISBEWR mode");
    const Stage stage{t.env.ShaderStage()};
    if (stage != Stage::Task && stage != Stage::Mesh) {
        t.Fail("stage buffer write outside task and mesh stages");
    }
    const u32 size{t.insn.Field(74, 2)};
    const Region region{t.insn.Field(76, 2)};
    if (!t.insn.Bit(79)) {
        t.Fail("stage buffer write to the input");
    }
    const IR::U32 address{IsbeAddress(t)};
    const IR::U32 value{t.R(t.insn.Reg32())};
    if (region == Region::Attribute && size == 2) {
        t.ir.IsbeWriteAttribute(address, value);
    } else if (region == Region::Map && size == 0) {
        t.ir.IsbeWriteMapU8(address, value);
    } else if (region == Region::Map && size == 2) {
        t.ir.IsbeWriteMapU32(address, value);
    } else {
        t.Fail("stage buffer write mode");
    }
}
} // Anonymous namespace

void TranslateIsbe(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0x923:
        return ISBERD(t);
    case 0x927:
        return ISBEWR(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
