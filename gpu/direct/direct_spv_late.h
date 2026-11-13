// Late scheduling of a translated stage's SPIR-V: the translator's register-pressure pass (docs/direct/spv-late.md;
// the cause in docs/direct/hang-fix.md, "The translator").
//
// The translator emits a stage in the native code's order, and the game's compiler loads early: a mesh stage that reads 112
// rows of a probe table has all of them in registers before the first is used. RADV has 256 vector registers a thread; past
// them ACO spills to scratch memory, and a mesh stage that spills lost the device on RX 6000 (the Hyrule Field hang). ACO does
// not reorder instructions, so the order the translator emits decides the demand.
//
// The pass, block by block:
//   - instructions with effects keep their order: stores, atomics, barriers, image reads and writes, mesh outputs, loads of
//     memory the stage can write (function, private, workgroup, output, storage buffer), calls of functions that do any of
//     that, the block's label, phis and variables (first) and its merge and terminator (last);
//   - every other instruction (arithmetic, composites, access chains, loads of uniform, input, push-constant and
//     uniform-constant memory, loads by address, subgroup operations, calls of functions that only compute) may move within
//     its block. A block that holds no more than `budget` 32-bit values at once is left as the translator emitted it. A
//     block past it is emitted in the translator's order with those instructions held back while the block is at the budget
//     and placed as soon as there is room again, or when something needs them (operands first, the larger operand tree
//     first): the loads stay ahead of their uses by what the budget allows, which the driver's own scheduling wants. Where
//     that does not bring the block near the budget (values held on entry), every such instruction is emitted at its first
//     use instead, and what frees at least as many values as it makes follows at once (a row is consumed as it is loaded);
//   - a load by address (and a call that reads by address) stays between the instructions around it after which memory read
//     by address may hold other values: a store or copy by address, any atomic, a barrier, a call of a function with effects;
//   - a subgroup operation does not move in a module that can demote or end an invocation (its lanes would differ).
// A block's new order is kept only when it holds fewer values at once than the order it had, by the pass's own count
// (values held into and out of the block from the function's control flow), and a function is changed only when its worst
// block then holds fewer: the count never rises in any block.
//
// Into another block (the second step, only in a function that still holds `sink_peak` values after the first): an
// instruction that only computes, or loads from uniform, input, push-constant or by-address memory, moves down into the one
// other block that holds every use of its value (a phi's use is in the block the value comes from) when that block is not in
// a loop its own block is outside of (it runs at most as often as before: never into a loop), holding its operands that far
// costs fewer values than not holding its result, and, for a load by address, no instruction that may write by address lies
// on any way between the two. So an instruction does go into a branch when all its uses are there. Subgroup operations and
// calls never leave their block. The step is kept only when the function then holds fewer values than without it.
//
// The count is the pass's, not ACO's (it knows nothing of variables the driver turns into registers, of values the driver
// keeps in scalar registers, or of what the driver's own scheduler does): tools/meshlab/late_gate.py holds RADV's compile of
// every stage against it.
//
// Values are not changed: the same instructions, in another order. The same input gives the same output (no order taken
// from a hash table). Stands alone: no header of the back end.
#pragma once

#include <cstdint>
#include <vector>

namespace hsd {

struct LateOptions {
    /// the 32-bit values a block may hold at once: a block within it is not touched, a block past it is brought to it
    uint32_t budget = 160;
    /// a function that still holds this many after that has its instructions that only compute moved down into the one
    /// other block that uses them, when the function then holds fewer (0: never)
    uint32_t sink_peak = 0;
    /// a line a block looked at goes here (the tool's --blocks)
    void (*log)(const char* line) = nullptr;
};

struct LateResult {
    uint32_t moved = 0;            ///< instructions whose place changed
    uint32_t blocks = 0;           ///< blocks looked at (past the budget)
    uint32_t blocks_changed = 0;   ///< blocks whose new order was kept
    uint32_t sunk = 0;             ///< instructions moved into another block
    uint32_t peak_before = 0;      ///< the most 32-bit values a block of the module held at once, as it came
    uint32_t peak_after = 0;       ///< and as it leaves
};

/// Reorders `words` (a SPIR-V module) in place; returns how many instructions moved (0: left as it was, also when the
/// module is not readable)
uint32_t spv_schedule_late(std::vector<uint32_t>& words, LateResult* result = nullptr, const LateOptions& options = {});

}  // namespace hsd
