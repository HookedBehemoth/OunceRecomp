// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's barriers and cache controls: BAR.SYNC 0 (the workgroup's execution and memory
// barrier), MEMBAR (a memory barrier of the workgroup, or of the device for any wider scope), ERRBAR (waits for
// earlier accesses' errors only: nothing here), CCTL (cache control: nothing, Vulkan's memory is coherent to the
// barriers above). The subgroup barrier around LDSM is LDSM's (the warp family).

#include "shader_recompiler/frontend/ampere/translate/memory.h"

namespace Shader::Ampere {
namespace {
void BAR(Translator& t) {
    // BAR.SYNC 0 with every thread (bit 80: .DEFER_BLOCKING); a guarded one would be a barrier in divergent control
    // flow, which Vulkan does not allow
    t.Expect(16, 48, 0, "BAR mode");
    ExpectOnly(t, HighBits(80, 81), "BAR mode");
    if (!t.insn.AlwaysExecutes()) {
        t.Fail("guarded barrier");
    }
    t.ir.Barrier();
}

void MEMBAR(Translator& t) {
    // the scope 76-78 (0 the workgroup), .SC 80
    t.Expect(16, 48, 0, "MEMBAR mode");
    ExpectOnly(t, HighBits(76, 79) | HighBits(80, 81), "MEMBAR mode");
    if (t.insn.Field(76, 3) == 0) {
        t.ir.WorkgroupMemoryBarrier();
    } else {
        t.ir.DeviceMemoryBarrier();
    }
}

void ERRBAR(Translator& t) {
    t.Expect(16, 48, 0, "ERRBAR mode");
    ExpectOnly(t, 0, "ERRBAR mode");
}

void CCTL(Translator& t) {
    // [Ra + offset 32-63], the operation 87-90 (prefetch, write back, invalidate)
    t.Expect(16, 8, 0, "CCTL mode");
    ExpectOnly(t, HighBits(87, 91), "CCTL mode");
    if (t.insn.Field(87, 4) > 8) {
        t.Fail("CCTL operation");
    }
}
} // Anonymous namespace

void TranslateBarrier(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0xb1d:
        return BAR(t);
    case 0x992:
        return MEMBAR(t);
    case 0x9ab:
        return ERRBAR(t);
    case 0x98f:
        return CCTL(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
