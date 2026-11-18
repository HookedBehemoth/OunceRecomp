// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Names: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.; nvdisasm's as nv_isa_solver publishes them

#include <algorithm>
#include <array>
#include <utility>

#include "shader_recompiler/frontend/ampere/decode.h"

namespace Shader::Ampere {
namespace {
// (the opcodes both games' seeds use)
constexpr std::pair<u32, const char*> NAMES[]{
    {0x202, "MOV"}, {0x207, "SEL"}, {0x208, "FSEL"}, {0x209, "FMNMX"}, {0x20a, "FSET"}, {0x20b, "FSETP"}, {0x20c,
    "ISETP"}, {0x210, "IADD3"}, {0x211, "LEA"}, {0x212, "LOP3"}, {0x213, "IABS"}, {0x217, "IMNMX"}, {0x219, "SHF"},
    {0x220, "FMUL"}, {0x221, "FADD"}, {0x223, "FFMA"}, {0x224, "IMAD"}, {0x230, "HADD2"}, {0x231, "HFMA2"}, {0x232,
    "HMUL2"}, {0x234, "HSETP2"}, {0x23a, "MOVM"}, {0x23c, "HMMA"}, {0x23e, "F2FP"}, {0x240, "HMNMX2"}, {0x243,
    "F2IP"}, {0x245, "I2FP"}, {0x287, "USEL"}, {0x28c, "UISETP"}, {0x290, "UIADD3"}, {0x291, "ULEA"}, {0x292,
    "ULOP3"}, {0x299, "USHF"}, {0x2a4, "UIMAD"}, {0x2a5, "UIMAD"}, {0x2bd, "UFLO"}, {0x2be, "UBREV"}, {0x2bf,
    "UPOPC"}, {0x300, "FLO"}, {0x301, "BREV"}, {0x305, "F2I"}, {0x306, "I2F"}, {0x307, "FRND"}, {0x308, "MUFU"},
    {0x309, "POPC"}, {0x321, "ALD"}, {0x322, "AST"}, {0x324, "OUT"}, {0x326, "IPA"}, {0x34e, "LEPC"}, {0x357,
    "BMOV.64.ATEXIT_PC"}, {0x361, "TEX"}, {0x381, "LD"}, {0x386, "ST"}, {0x387, "ST"}, {0x388, "ST"}, {0x38c,
    "ATOM"}, {0x3a1, "MATCH"}, {0x3a8, "ATOM"}, {0x3a9, "ATOM"}, {0x3c2, "R2UR"}, {0x3c4, "REDUX"}, {0x419, "SHF"},
    {0x421, "FADD"}, {0x423, "FFMA"}, {0x424, "IMAD"}, {0x431, "HFMA2"}, {0x434, "HSETP2"}, {0x499, "USHF"},
    {0x4a4, "UIMAD"}, {0x526, "IPA"}, {0x589, "SHFL"}, {0x593, "SUATOM"}, {0x59f, "SURED"}, {0x621, "FADD"},
    {0x623, "FFMA"}, {0x624, "IMAD"}, {0x802, "MOV"}, {0x803, "P2R"}, {0x804, "R2P"}, {0x805, "CS2R"}, {0x806,
    "VOTE"}, {0x807, "SEL"}, {0x808, "FSEL"}, {0x809, "FMNMX"}, {0x80a, "FSET"}, {0x80b, "FSETP"}, {0x80c,
    "ISETP"}, {0x810, "IADD3"}, {0x811, "LEA"}, {0x812, "LOP3"}, {0x816, "PRMT"}, {0x817, "IMNMX"}, {0x819, "SHF"},
    {0x81a, "SGXT"}, {0x81c, "PLOP3"}, {0x820, "FMUL"}, {0x822, "FSWZADD"}, {0x823, "FFMA"}, {0x824, "IMAD"},
    {0x825, "IMAD"}, {0x827, "IMAD.HI.imm"}, {0x831, "HFMA2"}, {0x832, "HMUL2"}, {0x83b, "LDSM"}, {0x840,
    "HMNMX2"}, {0x882, "UMOV"}, {0x883, "UP2UR"}, {0x886, "VOTE"}, {0x887, "USEL"}, {0x88c, "UISETP"}, {0x890,
    "UIADD3"}, {0x891, "ULEA"}, {0x892, "ULOP3"}, {0x896, "UPRMT"}, {0x899, "USHF"}, {0x89c, "PLOP3"}, {0x8a4,
    "UIMAD"}, {0x8a5, "UIMAD"}, {0x905, "F2I"}, {0x918, "NOP"}, {0x919, "S2R"}, {0x91a, "DEPBAR"}, {0x923,
    "ISBERD"}, {0x927, "ISBEWR"}, {0x941, "BSYNC"}, {0x942, "BREAK"}, {0x944, "CALL"}, {0x945, "BSSY"}, {0x947,
    "BRA"}, {0x948, "WARPSYNC"}, {0x949, "BRX"}, {0x94d, "EXIT"}, {0x950, "RET"}, {0x957, "BMOV.64.ATEXIT_PC.imm"},
    {0x958, "BRXU"}, {0x95b, "KILL"}, {0x981, "LD"}, {0x983, "LD"}, {0x984, "LD"}, {0x986, "ST"}, {0x987, "ST"},
    {0x989, "SHFL"}, {0x98e, "ATOM"}, {0x992, "MEMBAR"}, {0x9a8, "ATOM"}, {0x9ab, "ERRBAR"}, {0x9c3, "S2R"},
    {0xa02, "MOV"}, {0xa07, "SEL"}, {0xa08, "FSEL"}, {0xa09, "FMNMX"}, {0xa0a, "FSET"}, {0xa0b, "FSETP"}, {0xa0c,
    "ISETP"}, {0xa10, "IADD3"}, {0xa11, "LEA"}, {0xa12, "LOP3"}, {0xa13, "IABS"}, {0xa17, "IMNMX"}, {0xa20,
    "FMUL"}, {0xa23, "FFMA"}, {0xa24, "IMAD"}, {0xa45, "I2FP"}, {0xab9, "LDC"}, {0xabb, "ULDC"}, {0xb05, "F2I"},
    {0xb07, "FRND.cb"}, {0xb08, "MUFU"}, {0xb1d, "BAR"}, {0xb60, "TEX"}, {0xb63, "TLD4"}, {0xb66, "TLD"}, {0xb69,
    "TMML"}, {0xb6c, "TXD"}, {0xb6f, "TXQ"}, {0xb82, "LDC"}, {0xb97, "SULD"}, {0xb9b, "SUST.P"}, {0xc02, "MOV"},
    {0xc07, "SEL"}, {0xc08, "FSEL"}, {0xc09, "FMNMX"}, {0xc0b, "FSETP"}, {0xc0c, "ISETP"}, {0xc10, "IADD3"},
    {0xc11, "LEA"}, {0xc12, "LOP3"}, {0xc13, "IABS"}, {0xc17, "IMNMX"}, {0xc19, "SHF"}, {0xc20, "FMUL"}, {0xc23,
    "FFMA"}, {0xc24, "IMAD"}, {0xc45, "I2FP"}, {0xc82, "MOV"}, {0xd00, "FLO"}, {0xd05, "F2I.ur"}, {0xd06, "I2F"},
    {0xd08, "MUFU"}, {0xd09, "POPC"}, {0xe21, "FADD"}, {0xe23, "FFMA"}, {0xe24, "IMAD"}, {0xf60, "TEX"}, {0xf89,
    "SHFL"}, {0xf8c, "ATOMS.POPC.INC"}
};
} // Anonymous namespace

std::string OpcodeName(u32 opcode) {
    const auto it{std::ranges::find(NAMES, opcode, &std::pair<u32, const char*>::first)};
    return it != std::end(NAMES) ? it->second : "?";
}

} // namespace Shader::Ampere
