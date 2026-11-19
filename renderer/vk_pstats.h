// What each draw and dispatch of one frame made the GPU run (debug.pstats=<present>, a debug aid: the shader census,
// docs/direct/shader-costs.md).
//
// Every draw, mesh draw, task step and dispatch recorded while the present count is <present> is bracketed by a pipeline
// statistics query (input vertices and primitives, vertex shader invocations, clipping invocations and primitives, fragment
// shader invocations, compute shader invocations, and mesh shader invocations: meshShaderQueries is enabled for it). Ten
// presents later the results the GPU made available are written to pstats.csv in the working directory (the run's
// folder), a line a command: what it was (program key, its stages' code keys, the list's kind and return address, the pass's
// size and first target format), its counts on the CPU side, the GPU's statistics, and two timestamps: when all the work before
// it had finished (written before it) and when it and all before it had (after it); the difference is the time the command
// added to its queue's timeline. Off by default: one load and a branch per command.
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace hsd {

extern bool pstats_enabled;   // (debug.pstats=<present>, or pstats:bench: the present after bench's measured frames begin)
inline bool pstats_on() { return pstats_enabled; }
/// Before vkCreateDevice (after the mesh shader features are read): whether mesh shader queries are wanted
bool pstats_wants_mesh_queries();
/// After vkCreateDevice: the query pool
void pstats_init(bool mesh_queries);
struct PstatWhat {
    const char* what = "";      // (a fixed string: draw, draw indexed, mesh draw, task step, dispatch, ...)
    uint64_t key = 0;           // (the program's key; a dispatch's code key)
    uint64_t codes[3]{};        // (its stages' code keys: vertex or mesh, fragment, header; task code in codes[2] for task+mesh)
    uint32_t kind = 0;          // (the list's kind)
    uint64_t lr = 0;            // (the list's return address)
    uint32_t w = 0, h = 0;      // (the pass's size)
    int32_t format = -1;        // (its first colour target's format)
    uint32_t targets = 0;       // (its colour targets)
    uint32_t count = 0, instances = 0;   // (vertices or indices, instances; mesh: groups; dispatch: grid x, y)
    uint32_t extra = 0;         // (dispatch: grid z; draw: gated by a render condition)
    uint64_t pass = 0;          // (its pass: a job's pass key, else its list's SetRenderTargets number)
};
/// Before the command: its query begun when this frame is the one measured (returns its index, or ~0u)
uint32_t pstat_before(VkCommandBuffer cb, const PstatWhat& w);
/// After it
void pstat_after(VkCommandBuffer cb, uint32_t index);
/// debug.pstats=bench: the present measured, named now (gpu_measure)
void pstats_arm(uint64_t present);
/// At each present: the results written ten presents after the measured one
void pstats_present(uint64_t present);

}  // namespace hsd
