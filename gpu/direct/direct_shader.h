// Programs (docs/direct/m2-interface.md, direct_shader; m2-shader-contract.md step A): NVN programs through Eden's
// translator (linked from the frozen back end's objects, unchanged) to shader objects with descriptor-heap mappings.
//
// A program is a slot found by its NVN serial in a table read without a lock. Its stages' code is read and lowered when the
// game sets them (nvnProgramSetShaders), on a worker; the variants kept in the direct back end's own cache file for those
// stages are made then too. A draw finds its variant by the key the frozen back end uses (the stages' code, the vertex input
// types it reads, the colour types it writes) and the bound textures' answers; a miss skips the draw (counted) and queues
// the translation on a worker. Variants are only ever added (a list per slot, prepended), so a draw reads them without a
// lock.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "direct_internal.h"
#include "../render/rx_nvn.h"

namespace hsd {

struct DState;

/// Why a draw has no program to draw with (direct_draw.cpp counts them)
enum ShaderMiss : uint8_t {
    SM_NONE, SM_NO_PROGRAM, SM_NOT_PARSED, SM_NOT_GRAPHICS, SM_TRANSLATING, SM_FAILED, SM_UNSUPPORTED, SM_TASK_MESH, SM_COUNT
};

/// A ready program variant, as a draw reads it
struct DrawProg {
    uint64_t key = 0, variant = 0;
    VkShaderEXT vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    // (M4b: a mesh-only program: its mesh stage's object, no vertex stage; drawn by DRAW_MESH only. Its stages' code keys and
    // whether a draw has used it yet, for the first-use log)
    bool mesh = false;
    VkShaderEXT ms = VK_NULL_HANDLE;
    // (M4c: a task+mesh program: its task stage as a compute shader (ts), run as a compute step before the pass that draws it,
    // its threads and payload words; the mesh stage native, reading the draw's task header)
    bool task = false;
    VkShaderEXT ts = VK_NULL_HANDLE;
    uint32_t task_threads = 0, task_payload_words = 0;
    uint64_t codes[3]{};   // (vertex or mesh, fragment, header-only)
    uint16_t ssbo_slots[8]{};   // (M4b: by NVN stage, the storage buffer slots its stages read through the driver bank's table)
    mutable std::atomic<bool> used{false};
    uint32_t vs_loads = 0;          // (bit i: the vertex stage reads generic attribute i)
    uint8_t frag_stores = 0;        // (bit i: the fragment stage writes colour output i)
    uint8_t frag_components[8]{};   // (and which of its components)
    bool uses_render_area = false;
    uint8_t direct_ubos = 0;   // (HOS_DBG=ubopush: the first ones' addresses pushed at 192)
    uint32_t push_bytes = 0;        // (the push data a draw writes: Eden's block, then the address table's and the banks')
    /// The uniform buffers in the address table's order (push data at kTablePush holds the table's address)
    struct Ubo {
        uint8_t stage;   // (NVN stage)
        uint8_t index;   // (Eden's constant buffer index: c0 / c2 the driver banks, c1 the program's constants, c3+ the game's)
        uint32_t bytes;  // (c0 / c2: the bank bytes the stage reads, the copy's size)
    };
    std::vector<Ubo> ubos;
    /// Constant buffers whose address is pushed for textures that read their handle from them (HEAP_WITH_INDIRECT_INDEX)
    struct Pushed {
        uint8_t stage, index;
        uint16_t push_off;
    };
    std::vector<Pushed> pushed;
    VkDeviceAddress c1[8]{};   // (each stage's c1 by NVN stage, made at load)
    /// The variant's texture answers a draw checks (c0 / c2 words; one in a game buffer is taken as it was translated)
    struct Answer {
        uint8_t stage;
        uint8_t bank;
        uint16_t offset;
        uint8_t want;      // (the canonical answer the variant was translated for)
        uint8_t dim_fixed; // (the instruction fixes the type: the answer's dimension is no question, but rect / buffer)
        uint8_t desc_type; // (the descriptor's own type)
        bool depth_float;  // (a depth texture that is not integer: no integer bit)
    };
    std::vector<Answer> answers;
    struct TexWord {
        uint8_t stage, bank;
        uint16_t offset;
    };
    std::vector<TexWord> tex_words;   // (the handle words its textures read: logs)
    const char* skip = nullptr;   // (a variant step A cannot draw: the reason)
    char shape[160]{};   // (M3b, logs: per stage the textures by their handle's source, images, storage buffers, global memory)
};

constexpr uint32_t kTablePush = 120;
constexpr uint32_t kMeshFirstPush = 56;   // (M4b: a mesh draw's first workgroup, a u32: Eden's MESH_FIRST_PUSH_OFFSET)   // (push data: Eden's block is 120 bytes (PUSH_CONSTANTS_END rounded to 8))

/// (the game's thread, nvnProgramSetShaders) the program's stages read and its kept variants made, on a worker
void shader_program_set(const nvn_prog* p);
/// The variant for the bound program and state; null: none ready (why says, the translation queued where it is one)
const DrawProg* shader_pick(const DState& s, const uint8_t color_types[8], ShaderMiss& why);
/// (M4b) A draw of the bound program, which has a task stage, counted by its stages (M4c: drawn or skipped)
void shader_task_mesh_draw(const DState& s, bool indirect, bool drawn);
/// (M4c) Whether the bound program has a task stage (a lookup: for a draw that has no variant to say so)
bool shader_has_task(const DState& s);
void shader_task_mesh_log(double frames);
/// (M4b, logs) the bound program's stages: kind and code keys
void shader_describe(const DState& s, char* out, size_t cap);
/// Whether the textures bound now answer what the variant was translated for
bool shader_answers_hold(const DState& s, const DrawProg& p);
void shader_stats(char* out, size_t cap, double frames);

}  // namespace hsd
