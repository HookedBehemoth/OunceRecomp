// (native-vk) Graphics programs as VK_EXT_shader_object stages (vk_shader_object.cpp), RX_VK_SHADER_OBJECTS=1: a draw binds its
// program variant's vertex and fragment VkShaderEXT and sets all the state a pipeline would bake (vk_draw.cpp so_state), so no
// pipeline is made per state. The driver's binaries are kept in hookshot_shader_objects.bin beside the other caches, and the
// programs the game sets (nvnProgramSetShaders) are made ahead of their first draw by a prefetch thread.
#pragma once

#include <atomic>
#include <cstdint>

#include "vk_shader.h"

namespace mkwvk {

/// Shader objects in use (the device has them, RX_VK_SHADER_OBJECTS=1)
bool so_on();
/// Native task / mesh programs as shader objects too (RX_VK_SO_MESH=0: they keep their pipelines)
bool so_mesh_on();
/// Compute programs (dispatches) as compute shader objects too (RX_VK_SO_COMPUTE=0, or RX_VK_NOOPT: their pipelines)
bool so_compute_on();
/// The mesh fallback's task and mesh stages (compute) as shader objects too: by default only where their pipelines are optimized (not
/// under the task/mesh fallback, whose pipelines take DISABLE_OPTIMIZATION, which a shader object has no flag for). RX_VK_SO_FALLBACK=0/1
bool so_fallback_on();
/// (make_module) a module's SPIR-V: what its shader objects are compiled from, until they are made
void so_module_made(VkShaderModule m, const uint32_t* words, size_t n);

/// A program variant's graphics stages as shader objects
struct SoProg {
    std::atomic<bool> ready{false};   // (made, or failed)
    std::atomic<int> state{0};        // 0 not begun, 1 being made, 2 done
    bool failed = false;
    uint64_t key = 0, variant = 0;
    VkShaderModule mods[8]{};   // (the stages it was made for: a Program made again at the same address is another one)
    uint32_t nmods = 0;
    VkShaderEXT vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, ts = VK_NULL_HANDLE, ms = VK_NULL_HANDLE;   // (task, mesh: native mesh programs)
    // (compute stages, by stage index: a compute program's stage 0, a mesh fallback program's task and mesh dispatches)
    VkShaderEXT cs[8]{};
    bool cs_failed = false;   // (a fallback's compute stage not made: its dispatches take the pipelines, its draw stays a shader object)
};
/// Every compute stage of p has its shader object in so (ready, not failed): a dispatch binds them, not pipelines
bool so_compute_made(const SoProg& so, const Program& p);

/// (p's layout made) p's shader objects: made here when every stage's binary is kept (microseconds), else queued for a worker
/// (ready later: the draw leaves itself out or waits, as for a pipeline). push_size: the push constant range's size
/// *started: this call began the making (a compute dispatch waits for it a little, as for its pipeline)
SoProg* so_for(const Program& p, uint32_t push_size, bool* started = nullptr);
/// (the frame statistics) made from kept binaries, compiled from SPIR-V, their time; prefetched programs; draws that found theirs ready
void so_stats(int64_t* from_binary, int64_t* compiled, double* compile_ms, int64_t* prefetched, double* prefetch_ms);

/// (the NVN layer, at nvnProgramSetShaders[Ext] / InitializeFromBuilder) a program the game set: its known variants' shader objects
/// are made by the prefetch thread, before the program's first draw
void so_program_set(const nvn_prog* p);

// (vk_shader.cpp) for the prefetch
/// The stage keys of an NVN program as the program picks name them (Program::codes); false: no vertex or mesh stage
bool program_stage_keys(const nvn_prog& prog, uint64_t codes[4], bool* mesh);
/// The kept program variant (key, variant), loaded from the program cache into the programs the draws pick from (null: not kept)
const Program* program_load(uint64_t key, uint64_t variant, const uint64_t codes[4]);
// (vk_draw.cpp)
void program_layout(Program& p);   // (ensure_layout)
void fallback_prefetch(Program& p);   // (a mesh fallback program's compute pipelines begun on a worker)
uint32_t so_push_size();

} // namespace mkwvk
