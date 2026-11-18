// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encodings: Mesa NAK sm70_encode.rs (MIT; OpOut, OpOutFinal), nvdisasm SM86 names (LEPC, BMOV.64 ATEXIT_PC)

#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/control_flow.h"
#include "shader_recompiler/frontend/ampere/geometry.h"

namespace Shader::Ampere::Flow {
namespace {
constexpr u32 OP_LEPC = 0x34e, OP_MOV_IMM = 0x802, OP_BMOV_ATEXIT = 0x357, OP_BMOV_ATEXIT_IMM = 0x957, OP_OUT = 0x324,
              OP_EXIT = 0x94d;
/// The prologue's instructions between LEPC and BMOV at most
constexpr u32 MAX_PROLOGUE = 8;

/// Bits 64-104: the instruction's upper fields without its scheduling (105-127)
u64 UpperFields(const Insn& insn) {
    return insn.hi & ((u64{1} << 41) - 1);
}

[[noreturn]] void Fail(Location pc, const Insn& insn, std::string_view reason) {
    throw NotImplementedException("Ampere {:03x} {} at {:#x}: {}", insn.Opcode(), OpcodeName(insn.Opcode()),
                                  ByteOffsetOf(pc), reason);
}
} // Anonymous namespace

std::optional<Location> AtExitPrologueEnd(Environment& env, Location start) {
    const u32 base{ByteOffsetOf(start)};
    const Insn lepc{ReadInsn(env, start)};
    if (lepc.Opcode() != OP_LEPC) {
        return std::nullopt;
    }
    // LEPC Rr: the byte address of the LEPC itself into Rr:Rr+1
    const u32 reg{lepc.Dst()};
    if (env.ShaderStage() != Stage::Geometry || !lepc.AlwaysExecutes() || reg == RZ || reg % 2 != 0 ||
        (lepc.lo >> 24) != 0 || UpperFields(lepc) != 0) {
        Fail(start, lepc, "LEPC outside a geometry stage's at-exit prologue");
    }
    std::optional<u32> handler;
    std::optional<Location> end;
    for (u32 i = 1; i <= MAX_PROLOGUE && !end; ++i) {
        const Location pc{LocationOf(base + i * 16)};
        const Insn insn{ReadInsn(env, pc)};
        if (!insn.AlwaysExecutes()) {
            Fail(pc, insn, "guarded instruction in the at-exit prologue");
        }
        switch (insn.Opcode()) {
        case OP_MOV_IMM:
            if (!handler && insn.Field(72, 4) == 0xf) {
                handler = insn.Imm32();
            }
            break;
        case OP_BMOV_ATEXIT:
            // BMOV.64 ATEXIT_PC, Rr (Rr at 32-39)
            if (insn.lo != (u64{OP_BMOV_ATEXIT} | u64{PT} << 12 | u64{reg} << 32) || UpperFields(insn) != 0) {
                Fail(pc, insn, "at-exit prologue mode");
            }
            end = LocationOf(base + (i + 1) * 16);
            break;
        default:
            if (insn.Opcode() >= 0x940 && insn.Opcode() <= 0x95f) {
                Fail(pc, insn, "control flow in the at-exit prologue");
            }
            break;
        }
    }
    if (!end || !handler || *handler % 16 != 0) {
        Fail(start, lepc, "at-exit prologue without its BMOV.64 ATEXIT_PC or handler address");
    }
    // The handler: OUT.FINAL RZ, Rhandle, RZ (78-79 = 0); BMOV.64 ATEXIT_PC, 0; EXIT
    const u32 at{base + *handler};
    const Insn out{ReadInsn(env, LocationOf(at))};
    if ((out.lo & ~(u64{0xff} << 24)) != (u64{OP_OUT} | u64{PT} << 12 | u64{RZ} << 16 | u64{RZ} << 32) ||
        UpperFields(out) != 0) {
        Fail(LocationOf(at), out, "at-exit handler without OUT.FINAL");
    }
    const Insn clear{ReadInsn(env, LocationOf(at + 16))};
    if (clear.lo != (u64{OP_BMOV_ATEXIT_IMM} | u64{PT} << 12) || UpperFields(clear) != 0) {
        Fail(LocationOf(at + 16), clear, "at-exit handler without BMOV.64 ATEXIT_PC, 0");
    }
    const Insn exit{ReadInsn(env, LocationOf(at + 32))};
    if (exit.lo != (u64{OP_EXIT} | u64{PT} << 12) || UpperFields(exit) != (u64{PT} << 23)) {
        Fail(LocationOf(at + 32), exit, "at-exit handler without EXIT");
    }
    return end;
}

} // namespace Shader::Ampere::Flow
