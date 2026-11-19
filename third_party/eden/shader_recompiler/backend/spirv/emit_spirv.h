// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

constexpr u32 NUM_TEXTURE_SCALING_WORDS = 4;
constexpr u32 NUM_IMAGE_SCALING_WORDS = 2;
constexpr u32 NUM_TEXTURE_AND_IMAGE_SCALING_WORDS =
    NUM_TEXTURE_SCALING_WORDS + NUM_IMAGE_SCALING_WORDS;

struct RescalingLayout {
    alignas(16) std::array<u32, NUM_TEXTURE_SCALING_WORDS> rescaling_textures;
    alignas(16) std::array<u32, NUM_IMAGE_SCALING_WORDS> rescaling_images;
    u32 down_factor;
};
struct RenderAreaLayout {
    std::array<f32, 4> render_area;
};
constexpr u32 RESCALING_LAYOUT_WORDS_OFFSET = offsetof(RescalingLayout, rescaling_textures);
constexpr u32 RESCALING_LAYOUT_DOWN_FACTOR_OFFSET = offsetof(RescalingLayout, down_factor);
constexpr u32 RENDERAREA_LAYOUT_OFFSET = offsetof(RenderAreaLayout, render_area);
// (fork) guest memory's page table, a u64 device address pushed to every compute program that reads global memory, after the blocks
// above. It was a constant in the SPIR-V, and the address changes from run to run: the drivers' pipeline caches never hit
constexpr u32 GUEST_PAGES_PUSH_OFFSET = sizeof(RescalingLayout) + sizeof(RenderAreaLayout);
// (fork, direct back end, RuntimeInfo::vertex_formats_pushed) a vertex stage's attribute codes, a byte per generic input
// (16 bytes, in the render area block's room, which the back end does not push): bits 3:0 the conversion (VERTEX_CONVERT_*),
// 5:4 the component width (0: 8, 1: 16, 2: 32, 3: 10:10:10:2), 7:6 the components less one (the rest read (0, 0, 0, 1))
constexpr u32 VERTEX_FORMATS_PUSH_OFFSET = sizeof(RescalingLayout);
static_assert(VERTEX_FORMATS_PUSH_OFFSET + 16 <= GUEST_PAGES_PUSH_OFFSET, "the codes lie before the page table's address");
enum : u32 {
    VERTEX_CONVERT_RAW = 0,       // (32-bit float or integer: the bits as fetched)
    VERTEX_CONVERT_UNORM = 1,
    VERTEX_CONVERT_SNORM = 2,
    VERTEX_CONVERT_UINT = 3,      // (zero-extended by the fetch: as fetched)
    VERTEX_CONVERT_SINT = 4,      // (sign-extended from the width)
    VERTEX_CONVERT_USCALED = 5,
    VERTEX_CONVERT_SSCALED = 6,
    VERTEX_CONVERT_HALF = 7,
    VERTEX_CONVERT_R11G11B10F = 8,  // (one 32-bit word, fetched as R32_UINT: components 0-2 decoded from it)
    VERTEX_CONVERT_UNORM_BGRA = 9,  // (four 8-bit components in B, G, R, A order)
};
// (fork, direct back end, RuntimeInfo::fragment_inputs_pushed) a fragment stage's word of the generics the stage before it
// writes (bit i: generic i), after the down factor
constexpr u32 FRAGMENT_INPUTS_PUSH_OFFSET = offsetof(RescalingLayout, down_factor) + 4;
static_assert(FRAGMENT_INPUTS_PUSH_OFFSET + 4 <= VERTEX_FORMATS_PUSH_OFFSET, "the word lies in the down factor's padding");
// (fork) a task program's first workgroup: NVN draws tasks [first, first + count), Vulkan from 0. The address of the draw's first
// (a u32, 16 bytes per draw of an indirect draw, read at + DrawIndex * 16) is pushed here
constexpr u32 MESH_FIRST_PUSH_OFFSET = GUEST_PAGES_PUSH_OFFSET + 8;
// (fork) the mesh shader fallback's buffers, after it: the task payloads and the vertex records (u64 device addresses).
// Paths without the fallback never push them.
constexpr u32 MESH_FALLBACK_PUSH_OFFSET = MESH_FIRST_PUSH_OFFSET + 8;
constexpr u32 MESH_FALLBACK_PAYLOAD_PUSH_OFFSET = MESH_FALLBACK_PUSH_OFFSET;
constexpr u32 MESH_FALLBACK_RECORDS_PUSH_OFFSET = MESH_FALLBACK_PUSH_OFFSET + 8;
constexpr u32 MESH_FALLBACK_INDIRECT_INPUT_PUSH_OFFSET = MESH_FALLBACK_PUSH_OFFSET + 16;
constexpr u32 MESH_FALLBACK_PUSH_END = MESH_FALLBACK_PUSH_OFFSET + 24;
// (fork, RX_VK_DESC_HEAP_BDA) the layer-0 window table's device address and the draw's virtual range (bda_emit.h Pushed: four
// u64), for the stages whose storage buffers are read through device addresses (Info::ssbo_bda). Pushed to every program; the
// end of the push constants
constexpr u32 WINDOW_TABLE_PUSH_OFFSET = MESH_FALLBACK_PUSH_END;
constexpr u32 PUSH_CONSTANTS_END = WINDOW_TABLE_PUSH_OFFSET + 8 * 4;
// (direct back end, Profile::pool_handles) a stage's uniform buffers by device address: constant buffer descriptor i (in
// Info::constant_buffer_descriptors order) is at the address in entry i of a table of u64 addresses, whose own address is
// pushed at UNIFORM_TABLE_PUSH_OFFSET + 8 * UniformTableSlot(stage). Stages drawn together have a slot each (a task stage's
// compute step is pushed the same data as its mesh draw; a program has a geometry or a task stage, not both)
constexpr u32 UNIFORM_TABLE_PUSH_OFFSET = PUSH_CONSTANTS_END;
constexpr u32 UNIFORM_TABLE_SLOTS = 3;
constexpr u32 UniformTableSlot(Stage stage) {
    return stage == Stage::Fragment ? 1U : stage == Stage::Geometry || stage == Stage::Task ? 2U : 0U;
}

// (direct back end, Profile::pool_handles) the one descriptor set: NVN's texture pool as kPoolSlots sampled images per
// Vulkan view type (binding kPoolSampledBinding + the view type), the same as storage images (kPoolStorageBinding + the
// view type), and the sampler pool twice, plain and depth compare. A handle's low 20 bits index the images, clamped to
// kPoolNullSlot (which stays null), its high 12 bits the samplers
constexpr u32 kPoolSlots = 65536;
constexpr u32 kPoolNullSlot = kPoolSlots - 1;
constexpr u32 kPoolViewTypes = 7;   // (VkImageViewType 1D .. CUBE_ARRAY)
constexpr u32 kPoolSamplers = 4096;
constexpr u32 kPoolSampledBinding = 0;
constexpr u32 kPoolStorageBinding = kPoolSampledBinding + kPoolViewTypes;
constexpr u32 kPoolSamplerBinding = kPoolStorageBinding + kPoolViewTypes;
constexpr u32 kPoolCompareSamplerBinding = kPoolSamplerBinding + 1;
constexpr u32 kPoolBindings = kPoolCompareSamplerBinding + 1;
/// The Vulkan view type (VkImageViewType's value) whose bindings a texture or image of this type reads
constexpr u32 PoolViewType(TextureType type) {
    switch (type) {
    case TextureType::Color1D: return 0;
    case TextureType::Color3D: return 2;
    case TextureType::ColorCube: return 3;
    case TextureType::ColorArray1D: return 4;
    case TextureType::ColorArray2D: return 5;
    case TextureType::ColorArrayCube: return 6;
    default: return 1;   // (2D, rectangle, buffer)
    }
}
// (fork) the fallback's mesh workgroups a draw can run: each a slot, a word of the payload buffer saying whether its task
// started it; then each task workgroup's payload. It was 1024, a record block each, and Ocarina's floor grass (433 tasks of
// 32 slots) lost 13 of 14 meshlets. At 16384, Hyrule Field's far trees (3cb76827/4f0c0b40: 721 tasks of 32) lost the tasks
// past 512, a different set each frame as the culling's list order changes: they flickered. 65535: Vulkan's least
// maxComputeWorkGroupCount, which the mesh dispatch can reach; the payload buffer is ~6.5 MB a recording thread at 800 words.
constexpr u32 MESH_FALLBACK_CAPACITY = 65535;
// (fork) the records buffer: a VkDrawIndirectCommand the vertex stage's draw reads (its vertex count the records written),
// the allocator's word, then the records. A mesh workgroup takes records for its primitives' corners only, so a draw covers
// what was drawn, not every slot's maximum
constexpr u32 MESH_FALLBACK_DRAW_WORD = 0;        // (vertexCount; instanceCount at word 1, set to 1 by the host)
constexpr u32 MESH_FALLBACK_ALLOC_WORD = 4;       // (records taken, past the end too: those that do not fit are not drawn)
// (a VkDispatchIndirectCommand for the mesh workgroups a task program started (y and z 1, set by the host): each started task
// thread takes an index of it and writes its slot there in the payload buffer's map, so the mesh dispatch runs only those. With
// a count from the GPU (DrawMeshTasksIndirect, the floor grass) the host knew none, and dispatched no mesh workgroup)
constexpr u32 MESH_FALLBACK_DISPATCH_WORD = 5;
constexpr u32 MESH_FALLBACK_HEADER_WORDS = 8;
constexpr u32 MESH_FALLBACK_RECORD_WORDS = 16u << 20;   // (64 MB: ~47k grass primitives of 31 words are 17 MB)
// (fork, direct back end M4c, RuntimeInfo::task_buffer) a task+mesh draw's task header, 64 bytes the host writes before the
// draw (renderer/vk_task.cpp): words 0-2 the mesh draw's VkDrawMeshTasksIndirectCommandEXT {groups, 1, 1}, the groups
// raised by the task workgroups as they place their mesh workgroups; then what the task and mesh stages read
constexpr u32 TASK_HEADER_GROUPS = 0;     // (the mesh workgroups placed in the map: atomicMax of each task's end)
constexpr u32 TASK_HEADER_TAKEN = 3;      // (the mesh workgroups asked for: atomicAdd; past TASK_HEADER_MAP_CAP not placed)
constexpr u32 TASK_HEADER_MAP = 4;        // (u64: the map, a word a mesh workgroup: its task workgroup << 8 | its index in it)
constexpr u32 TASK_HEADER_PAYLOAD = 6;    // (u64: the payloads, one a task workgroup, TASK_HEADER_PAYLOAD_WORDS apart)
constexpr u32 TASK_HEADER_MAP_CAP = 8;    // (the map's entries)
constexpr u32 TASK_HEADER_TASKS = 9;      // (the task workgroups the payload area holds)
constexpr u32 TASK_HEADER_DISPATCH = 10;  // (words 10-12: the task dispatch {tasks, 1, 1} of an indirect draw)
constexpr u32 TASK_HEADER_PAYLOAD_WORDS = 13;   // (the task's payload words: the stride of the payload area)
constexpr u32 TASK_HEADER_WORDS = 16;

/// (fork, RX_VK_BDA_CHECK) the id the checked storage accesses of the program being translated record, this thread's
extern thread_local u32 bda_check_id;

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info, IR::Program& program, Bindings& bindings);
[[nodiscard]] inline std::vector<u32> EmitSPIRV(const Profile& profile, IR::Program& program) {
    Bindings binding;
    return EmitSPIRV(profile, {}, program, binding);
}

} // namespace Shader::Backend::SPIRV
