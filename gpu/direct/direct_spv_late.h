// Late scheduling of a translated stage's SPIR-V (docs/direct/hf-fix-report.md, "The translator").
//
// The translator emits a stage in the native code's order, and the game's compiler loads early: a mesh stage that reads 112
// rows of a probe table has all of them in registers before the first is used. RADV has 256 vector registers a thread; past
// them ACO spills to scratch memory, and a mesh stage that spills lost the device on RX 6000 (the Hyrule Field hang). This
// pass keeps every instruction with an effect in its order (stores, barriers, subgroup operations, loads of memory the stage
// may write, calls of functions that write) and emits every other instruction just before the first instruction of its block
// that needs it, operands first, the larger operand tree first; and once an instruction is emitted, every instruction that is
// ready and frees at least as many values as it makes follows it at once (a row is consumed as it is loaded). Values are not
// changed: the same instructions, in another order within each block.
#pragma once

#include <cstdint>
#include <vector>

namespace hsd {

/// Reorders `words` (a SPIR-V module) in place; returns how many instructions moved (0: left as it was, also when the module
/// is not readable)
uint32_t spv_schedule_late(std::vector<uint32_t>& words);

}  // namespace hsd
