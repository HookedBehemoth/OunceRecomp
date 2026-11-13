// Late scheduling of a translated stage's SPIR-V: the translator's register-pressure pass (docs/direct/spv-late-report.md;
// the cause in docs/direct/hf-fix-report.md, "The translator").
//
// The translator emits a stage in the native code's order, and the game's compiler loads early: a mesh stage that reads 112
// rows of a probe table has all of them in registers before the first is used. RADV has 256 vector registers a thread; past
// them ACO spills to scratch memory, and a mesh stage that spills lost the device on RX 6000 (the Hyrule Field hang). ACO does
// not reorder instructions, so the order the translator emits decides the demand.
//
// The pass, block by block (nothing moves from one block into another: not into a branch, not into or out of a loop):
//   - instructions with effects keep their order: stores, atomics, barriers, image writes, mesh outputs, loads of memory the
//     stage can write (function, private, workgroup, output, storage buffer), calls of functions that do any of that, the
//     block's label, phis and variables (first) and its merge and terminator (last);
//   - every other instruction (arithmetic, composites, access chains, loads of uniform, input, push-constant and
//     uniform-constant memory, loads by address, subgroup operations, calls of functions that only compute) is emitted just
//     before the first instruction that needs it, operands first, the larger operand tree first; and once an instruction is
//     emitted, every instruction that is ready and frees at least as many values as it makes follows at once (a row is
//     consumed as it is loaded);
//   - a load by address (and a call that reads by address) stays between the instructions around it after which memory read
//     by address may hold other values: a store or copy by address, any atomic, a barrier, a call of a function with effects;
//   - a subgroup operation does not move in a module that can demote or end an invocation (its lanes would differ).
// A block's new order is kept only when it holds fewer 32-bit values at once than the order it had, by the pass's own count
// (values live into and out of the block from the function's control flow; a block's order changes neither), and only in a
// block that held `min_peak` values or more: a block far from the register limit is left word for word as the translator
// emitted it. So the count never rises in any block. The count is the pass's, not ACO's: tools/meshlab/late_gate.py holds
// RADV's compile of every stage against it.
//
// Values are not changed: the same instructions, in another order within their blocks. The same input gives the same
// output (no order taken from a hash table). Stands alone: no header of the back end.
#pragma once

#include <cstdint>
#include <vector>

namespace hsd {

struct LateOptions {
    /// a block that holds fewer 32-bit values at once than this is not touched
    uint32_t min_peak = 96;
    /// a line a block looked at goes here (the tool's --blocks)
    void (*log)(const char* line) = nullptr;
};

struct LateResult {
    uint32_t moved = 0;            ///< instructions whose place changed
    uint32_t blocks = 0;           ///< blocks looked at (at min_peak or more)
    uint32_t blocks_changed = 0;   ///< blocks whose new order was kept
    uint32_t peak_before = 0;      ///< the most 32-bit values a block of the module held at once, as it came
    uint32_t peak_after = 0;       ///< and as it leaves
};

/// Reorders `words` (a SPIR-V module) in place; returns how many instructions moved (0: left as it was, also when the
/// module is not readable)
uint32_t spv_schedule_late(std::vector<uint32_t>& words, LateResult* result = nullptr, const LateOptions& options = {});

}  // namespace hsd
