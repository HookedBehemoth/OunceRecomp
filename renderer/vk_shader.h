// Programs: NVN programs through Eden's
// translator (linked from the frozen back end's objects, unchanged) to shader objects on the pools' set (vk_pool.h).
//
// A program is a slot found by its NVN serial in a table read without a lock. Its stages' code is read and lowered when the
// game sets them (nvnProgramSetShaders), on a worker; the variants kept in the direct back end's own cache file for those
// stages are made then too. A stage is translated from its code alone: what else it reads (vertex formats, what the stage
// before writes) is the draw's push data. A draw finds its variant by the stages' code and the colour types it writes, and
// checks its bound textures against the translation; a miss skips the draw (counted) and queues the translation on a worker. Variants are only ever added (a list per slot, prepended), so a draw reads them without a
// lock.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "vk_internal.h"
#include "rx_nvn.h"

namespace hsd {

struct DState;

/// Why a draw has no program to draw with (vk_draw.cpp counts them)
enum ShaderMiss : uint8_t {
    SM_NONE, SM_NO_PROGRAM, SM_NOT_PARSED, SM_NOT_GRAPHICS, SM_TRANSLATING, SM_FAILED, SM_UNSUPPORTED, SM_TASK_MESH, SM_COUNT
};

/// A ready program variant, as a draw reads it
struct DrawProg {
    uint64_t key = 0;
    VkShaderEXT vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderEXT gs = VK_NULL_HANDLE;   // (a geometry stage between them: drawn with triangle input only)
    // (M4b: a mesh-only program: its mesh stage's object, no vertex stage; drawn by DRAW_MESH only. Its stages' code keys and
    // whether a draw has used it yet, for the first-use log)
    bool mesh = false;
    VkShaderEXT ms = VK_NULL_HANDLE;
    // (M4c: a task+mesh program: its task stage as a compute shader (ts), run as a compute step before the pass that draws it,
    // its threads and payload words; the mesh stage native, reading the draw's task header)
    bool task = false;
    VkShaderEXT ts = VK_NULL_HANDLE;
    uint32_t task_threads = 0, task_payload_words = 0;
    uint64_t codes[3]{};   // (vertex or mesh, fragment, header-only or geometry)
    uint64_t task_code = 0;   // (a task+mesh program's task stage code key)
    uint16_t ssbo_slots[8]{};   // (M4b: by NVN stage, the storage buffer slots its stages read through the driver bank's table)
    mutable std::atomic<bool> used{false};
    uint32_t vs_loads = 0;          // (bit i: the vertex stage reads generic attribute i)
    uint32_t frag_inputs = 0;       // (bit i: the stage before the fragment stage writes generic i; pushed for it to read)
    uint8_t frag_stores = 0;        // (bit i: the fragment stage writes colour output i)
    uint8_t frag_components[8]{};   // (and which of its components)
    bool uses_render_area = false;
    uint32_t push_bytes = 0;        // (the push data a draw writes: Eden's block, then each stage's part of the address table)
    /// The uniform buffers in the address table's order: each stage's in its descriptors' order, the stage's part from
    /// table_first[slot], its address pushed at kTablePush + 8 * slot (the translator's UniformTableSlot)
    uint8_t table_first[3]{};
    uint8_t table_slots = 0;   // (bit slot: a stage there reads uniform buffers)
    struct Ubo {
        uint8_t stage;   // (NVN stage)
        uint8_t index;   // (Eden's constant buffer index: c0 / c2 the driver banks, c1 the program's constants, c3+ the game's)
        uint32_t bytes;  // (c0 / c2: the bank bytes the stage reads, the copy's size)
    };
    std::vector<Ubo> ubos;
    VkDeviceAddress c1[8]{};   // (each stage's c1 by NVN stage, made at load)
    /// The textures a draw checks against the translation from the code alone (c0 / c2 words; one in a game buffer it does
    /// not see): vk_shader.cpp check_textures
    struct Answer {
        uint8_t stage;
        uint8_t bank;
        uint16_t offset;
        uint8_t want;      // (the canonical answer the translation took: Color2D as the descriptor has it, not integer)
        uint8_t dim_fixed; // (the instruction fixes the type: the answer's dimension is no question, but rect / buffer)
        uint8_t desc_type; // (the descriptor's own type)
        bool depth_float;  // (a depth texture that is not integer: no integer bit)
    };
    std::vector<Answer> answers;
    struct TexWord {
        uint8_t stage, bank;
        uint16_t offset;
        // (logs: a two-word handle's second word and the shifts, the descriptor's type and count; computed: no word)
        uint8_t sbank = 0, shift = 0, sshift = 0, type = 0, count = 1;
        uint16_t soffset = 0;
        bool secondary = false, computed = false;
    };
    std::vector<TexWord> tex_words;   // (the handle words its textures read: logs)
    const char* skip = nullptr;   // (a variant step A cannot draw: the reason)
    char shape[160]{};   // (logs: per stage the textures by their handle's source, images, storage buffers, global memory)
};

constexpr uint32_t kTablePush = 120;   // (Eden's UNIFORM_TABLE_PUSH_OFFSET: after its block)
constexpr uint32_t kMeshFirstPush = 56;   // (M4b: a mesh draw's first workgroup, a u32: Eden's MESH_FIRST_PUSH_OFFSET)

/// (the game's thread, nvnProgramSetShaders) the program's stages read and its kept variants made, on a worker
void shader_program_set(const nvn_prog* p);
/// The variant for the bound program and state; null: none ready (why says, the translation queued where it is one)
const DrawProg* shader_pick(const DState& s, const uint8_t color_types[8], ShaderMiss& why);
/// A draw of the bound program, which has a task stage, counted by its stages (M4c: drawn or skipped)
void shader_task_mesh_draw(const DState& s, bool indirect, bool drawn);
/// Whether the bound program has a task stage (a lookup: for a draw that has no variant to say so)
bool shader_has_task(const DState& s);
void shader_task_mesh_log(double frames);
/// (logs) the bound program's stages: kind and code keys
void shader_describe(const DState& s, char* out, size_t cap);
void shader_stats(char* out, size_t cap, double frames);
/// A stage of a program made ahead of its draws: its control section (0xd8 bytes) and code area
struct PreStage {
    const uint8_t* ctl;
    const uint8_t* area;
    uint32_t area_n;
};
enum PreResult { PRE_MADE, PRE_KEPT, PRE_FAILED, PRE_OTHER };
/// (any thread) A graphics program made from its stages' bytes as a draw into float targets makes it: translated into
/// the program cache unless it holds it (KEPT), its shader objects made (their driver binaries kept) and destroyed
/// again. OTHER: a compute program, or one the draws do not draw
PreResult shader_precompile(const PreStage* st, int n);
/// The stamp of the caches a precompile filled: the program cache's and the driver binaries' (another driver, another
/// translator or another device: another stamp)
uint64_t shader_cache_stamp();
/// The caches' index files written for what they hold now (the precompile's end, so the next start reads no record head)
void shader_cache_index();
/// (the start, vk_precompile.cpp) the shader pack's programs made again on the background workers when the caches are
/// not the ones its last complete precompile filled
void shader_warm();
/// The shader objects and variants alive, in counts (none is ever destroyed)
void shader_held_stats(char* out, size_t cap);

}  // namespace hsd
