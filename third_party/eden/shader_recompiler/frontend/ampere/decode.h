// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) One native Ampere (SM86) instruction of 16 bytes and its fields, named as NAK's encoder sets them: the opcode,
// the guard, the ALU operand forms, constant-buffer references, predicate sources and destinations, branch offsets.
// The direct front (control_flow.cpp, translate/) reads instructions only through these.

#pragma once

#include <string>

#include "common/common_types.h"

namespace Shader::Ampere {

constexpr u32 RZ = 255;  ///< the zero GPR
constexpr u32 URZ = 63;  ///< the zero uniform register
constexpr u32 PT = 7;    ///< the true predicate (and UPT)

/// The ALU operand forms (bits 9-11): where the second and third sources are
enum class AluForm : u32 {
    RegReg = 1,  ///< src1 GPR at 32, src2 GPR at 64
    RegImm = 2,  ///< src1 GPR at 64, src2 immediate at 32
    RegCbuf = 3, ///< src1 GPR at 64, src2 cbuf at 32
    ImmReg = 4,  ///< src1 immediate at 32, src2 GPR at 64
    CbufReg = 5, ///< src1 cbuf at 32, src2 GPR at 64
    URegReg = 6, ///< src1 uniform register at 32, src2 GPR at 64
    RegUReg = 7, ///< src1 GPR at 64, src2 uniform register at 32
};

struct Insn {
    u64 lo{};
    u64 hi{};

    [[nodiscard]] u64 Bits(u32 start, u32 count) const noexcept {
        u64 value;
        if (start >= 64) {
            value = hi >> (start - 64);
        } else if (start + count <= 64) {
            value = lo >> start;
        } else {
            value = (lo >> start) | (hi << (64 - start));
        }
        return count == 64 ? value : value & ((u64{1} << count) - 1);
    }
    [[nodiscard]] u32 Field(u32 start, u32 count) const noexcept {
        return static_cast<u32>(Bits(start, count));
    }
    [[nodiscard]] bool Bit(u32 bit) const noexcept {
        return Bits(bit, 1) != 0;
    }

    [[nodiscard]] u32 Opcode() const noexcept {
        return Field(0, 12);
    }
    /// The ALU operation without its form (bits 0-8)
    [[nodiscard]] u32 AluOp() const noexcept {
        return Field(0, 9);
    }
    [[nodiscard]] AluForm Form() const noexcept {
        return static_cast<AluForm>(Field(9, 3));
    }

    /// The uniform datapath's ALU (ULDC, UMOV, UIADD3, ...: operations 0x080-0x0bf): its guard is a uniform predicate
    [[nodiscard]] bool IsUniformDatapath() const noexcept {
        const u32 op{Field(0, 9)};
        return op >= 0x080 && op < 0x0c0;
    }

    /// set_pred: the guard predicate (12-14; a UP on the uniform datapath) and its negation (15)
    [[nodiscard]] u32 Pred() const noexcept {
        return Field(12, 3);
    }
    [[nodiscard]] bool PredInv() const noexcept {
        return Bit(15);
    }
    [[nodiscard]] bool AlwaysExecutes() const noexcept {
        return Pred() == PT && !PredInv();
    }
    [[nodiscard]] bool NeverExecutes() const noexcept {
        return Pred() == PT && PredInv();
    }

    /// set_dst: 16-23
    [[nodiscard]] u32 Dst() const noexcept {
        return Field(16, 8);
    }
    /// encode_alu_src0: 24-31, abs 73, neg 72
    [[nodiscard]] u32 Src0() const noexcept {
        return Field(24, 8);
    }
    /// encode_alu_reg: 32-39, abs 62, neg 63 (swizzle 60-61)
    [[nodiscard]] u32 Reg32() const noexcept {
        return Field(32, 8);
    }
    /// encode_alu_src2: 64-71, abs 74, neg 75 (F16: abs 83, neg 84, swizzle 81-82)
    [[nodiscard]] u32 Reg64() const noexcept {
        return Field(64, 8);
    }
    /// set_ureg at 32: 6 bits before SM100
    [[nodiscard]] u32 UReg32() const noexcept {
        return Field(32, 6);
    }
    /// encode_alu_imm: 32-63
    [[nodiscard]] u32 Imm32() const noexcept {
        return Field(32, 32);
    }

    /// set_src_cb(32..59, 91): the byte offset 38-53, the bank 54-58; bit 91 makes 32-37 a uniform register holding the
    /// bank (bindless)
    [[nodiscard]] u32 CbufOffset() const noexcept {
        return Field(38, 16);
    }
    [[nodiscard]] u32 CbufBank() const noexcept {
        return Field(54, 5);
    }
    [[nodiscard]] bool CbufBindless() const noexcept {
        return Bit(91);
    }

    /// set_pred_dst: a three-bit predicate destination, PT for none
    [[nodiscard]] u32 PredDst(u32 start) const noexcept {
        return Field(start, 3);
    }
    /// set_pred_src(range, not_bit): a three-bit predicate and its negation
    [[nodiscard]] u32 PredSrc(u32 start) const noexcept {
        return Field(start, 3);
    }

    /// set_rel_offset(34..82): a relative branch's byte offset from the next instruction, signed, in 32-bit words
    [[nodiscard]] s64 BranchOffset() const noexcept {
        const u64 words = Bits(34, 48);
        return (static_cast<s64>(words << 16) >> 16) * 4;
    }
};

/// The NAK (or nvdisasm) name of an opcode, for error messages; "?" when unknown
[[nodiscard]] std::string OpcodeName(u32 opcode);

} // namespace Shader::Ampere
