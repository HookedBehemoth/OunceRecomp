// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's memory operands, shared by the loads and stores (memory.cpp) and the atomics
// (atomic.cpp): the access size, a global address of a register and a uniform register, a shared or local address,
// and register vectors. A global address is built as GlobalMemoryToStorageBuffer tracks one: the two words of the
// pointer packed (PackUint2x32 of a composite), plus the immediate offset (IAdd64).

#pragma once

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {

/// set_mem_type(73..76): U8, S8, U16, S16, B32, B64, B128
struct MemType {
    int bits;
    bool is_signed;
};
MemType MemTypeOf(Translator& t);

/// A global address: Ra (24-31; 64 bits when reg_wide_bit is set) plus, when bit 91 is set and ureg_start is not
/// negative, the uniform register at ureg_start (64 bits when bit 72 is set), plus the signed offset 40-63
IR::U64 GlobalAddress(Translator& t, u32 reg_wide_bit, int ureg_start);

/// (Ra << stride 78-79) + URb (when bit 91; at ureg_start) + the offset 40-63 (signed with a register): shared memory
IR::U32 SharedAddress(Translator& t, int ureg_start);

/// count consecutive registers from reg (RZ: zeros); fails unless reg is aligned to count
IR::Value RegVector(Translator& t, u32 reg, int count);
/// Writes count words of a vector (or the one U32 value when count is 1) to consecutive registers from reg
void SetRegVector(Translator& t, u32 reg, const IR::Value& value, int count);

/// The bits [start, end) of the instruction's bits 64-91, as a mask for ExpectOnly
constexpr u32 HighBits(u32 start, u32 end) {
    return static_cast<u32>(((u64{1} << (end - start)) - 1) << (start - 64));
}
/// Fails when any of bits 64-91 outside allowed (HighBits masks) is set
void ExpectOnly(Translator& t, u32 allowed, std::string_view what);

/// The memory access fields every global access may carry: the type (73-75), the order and scope (77-80) and the
/// eviction priority (84-86), none of which changes what the access does here
constexpr u32 kAccessBits = HighBits(73, 76) | HighBits(77, 81) | HighBits(84, 87);
/// 81-83: a predicate output; only PT (none) is translated
constexpr u32 kPredicateOutput = HighBits(81, 84);
void ExpectNoPredicateOutput(Translator& t);

} // namespace Shader::Ampere
