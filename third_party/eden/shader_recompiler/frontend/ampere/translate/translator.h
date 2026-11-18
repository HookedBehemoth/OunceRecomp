// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's instruction translation: one Translator a block, reading the native instruction's
// operands (GPRs, uniform registers, predicates, immediates, constant buffers, by the ALU forms) and emitting IR. The
// instruction families are in translate/*.cpp; every instruction or mode they do not cover fails the translation with
// the opcode's name.

#pragma once

#include <string_view>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/decode.h"
#include "shader_recompiler/frontend/common/location.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace Shader::Ampere {

/// An ALU source with its float modifiers (the integer ones are per instruction)
struct Operand {
    IR::U32 value;
    bool abs{};
    bool neg{};
    bool immediate{};
};

class Translator {
public:
    explicit Translator(Environment& env_, IR::Block& block) : env{env_}, ir{block} {}

    [[noreturn]] void Fail(std::string_view reason) const;
    /// Fails unless bits [start, start + count) are value
    void Expect(u32 start, u32 count, u64 value, std::string_view what) const;

    IR::U32 R(u32 reg);
    void SetR(u32 reg, const IR::U32& value);
    IR::F32 F(u32 reg);
    void SetF(u32 reg, const IR::F32& value);
    IR::U32 UR(u32 reg);
    void SetUR(u32 reg, const IR::U32& value);
    IR::U1 P(u32 pred, bool negated = false);
    void SetP(u32 pred, const IR::U1& value);
    /// UP0-UP6 (UPT: true)
    IR::U1 UP(u32 pred, bool negated = false);
    void SetUP(u32 pred, const IR::U1& value);
    /// set_pred_src(start..start + 3, start + 3), and set_upred_src
    IR::U1 PredSrc(u32 start);
    IR::U1 UPredSrc(u32 start);

    /// The ALU constant-buffer operand (32-58); bindless (bit 91) and unaligned ones fail
    IR::U32 Cbuf();
    /// encode_alu_src0, the second and third ALU sources by the form (encode_alu_base)
    Operand Src0();
    Operand Src1();
    Operand Src2();
    IR::F32 Float(const Operand& operand);

    /// The datapath's registers and predicates: URs and UPs for a uniform datapath instruction, else GPRs and Ps. A
    /// register field is 8 bits; a uniform one must name UR0-UR63 (URZ: 0)
    IR::U32 X(u32 reg);
    void SetX(u32 reg, const IR::U32& value);
    [[nodiscard]] bool XIsZero(u32 reg) const;
    IR::U1 XPredSrc(u32 start);
    void SetXP(u32 pred, const IR::U1& value);

    Environment& env;
    IR::IREmitter ir;
    Location pc;
    Insn insn;
    bool uniform{}; ///< insn is on the uniform datapath
};

void TranslateFloat(Translator& t);
void TranslateInteger(Translator& t);
void TranslateConversion(Translator& t);
void TranslateMove(Translator& t);
void TranslateMemory(Translator& t);
void TranslateAttribute(Translator& t);
void TranslateTexture(Translator& t);
void TranslateSurface(Translator& t);
void TranslateHalf(Translator& t);
void TranslateExit(Translator& t);
void TranslateAtomic(Translator& t);
void TranslateSpecial(Translator& t);
void TranslateBarrier(Translator& t);
void TranslateIsbe(Translator& t);
void TranslatePredicate(Translator& t);
void TranslateWarp(Translator& t);
void TranslateMatrix(Translator& t);
void TranslateGeometry(Translator& t);

/// Emits the instructions of [begin, end) into block
void Translate(Environment& env, IR::Block* block, Location begin, Location end);

} // namespace Shader::Ampere
