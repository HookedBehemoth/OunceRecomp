// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <map>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/varying_state.h"

namespace Shader {

enum class AttributeType : u8 {
    Float,
    SignedInt,
    UnsignedInt,
    SignedScaled,
    UnsignedScaled,
    Disabled,
};

enum class InputTopology {
    Points,
    Lines,
    LinesAdjacency,
    Triangles,
    TrianglesAdjacency,
};

struct InputTopologyVertices {
    static u32 vertices(InputTopology input_topology) {
        switch (input_topology) {
        case InputTopology::Lines:
            return 2;
        case InputTopology::LinesAdjacency:
            return 4;
        case InputTopology::Triangles:
            return 3;
        case InputTopology::TrianglesAdjacency:
            return 6;
        case InputTopology::Points:
        default:
            return 1;
        }
    }
};

enum class CompareFunction {
    Never,
    Less,
    Equal,
    LessThanEqual,
    Greater,
    NotEqual,
    GreaterThanEqual,
    Always,
};

enum class TessPrimitive {
    Isolines,
    Triangles,
    Quads,
};

enum class TessSpacing {
    Equal,
    FractionalOdd,
    FractionalEven,
};

struct TransformFeedbackVarying {
    u32 buffer{};
    u32 stream{};
    u32 stride{};
    u32 offset{};
    u32 components{};
};

struct RuntimeInfo {
    std::array<AttributeType, 32> generic_input_types{};
    VaryingState previous_stage_stores;
    /// Fragment stage after a mesh program: generic inputs it writes per primitive.
    std::bitset<32> per_primitive_generics{};
    /// Fragment stage after a mesh program that draws points: generic inputs the hardware's point sprite
    /// replaces with the coordinate within the point (s, t, 0, 1), as NVN's do with the generics the mesh
    /// program leaves unwritten.
    std::bitset<32> point_coord_generics{};
    std::map<IR::Attribute, IR::Attribute> previous_stage_legacy_stores_mapping;

    bool convert_depth_mode{};
    bool force_early_z{};
    /// Work around drivers that mix up results across calls to the 128-bit global-load helper.
    bool inline_global_128_loads{};
    /// (fork) a mesh stage with no task stage before it: its workgroups start at the draw's first, as a task stage's do
    bool mesh_without_task{};
    /// (fork) a GPU without VK_EXT_mesh_shader: task and mesh programs are compute shaders that write their stage
    /// buffer outputs to storage buffers, and a generated vertex stage reads those records back.
    bool mesh_fallback{};
    /// (fork) the generated vertex stage of that fallback: it reads a record per position and writes the stage's outputs
    bool mesh_fallback_vertex{};
    /// (fork) a mesh program with a task program before it: the task's count in its payload bounds the mesh workgroups
    bool mesh_fallback_task{};
    /// (fork) that task program's threads: task workgroup w starts its meshlets at slots w * threads + i
    u32 mesh_fallback_task_threads{};
    /// (fork) the fallback's record slots: mesh workgroups past this one write nothing
    u32 mesh_fallback_capacity{};
    /// (fork, direct back end M4c) the task stage as a compute step, the mesh stage native: the task (with mesh_fallback
    /// set: a compute shader) writes each mesh workgroup it starts into the draw's map and its payload into the draw's payload
    /// area, both named by the draw's task header (its address pushed at MESH_FALLBACK_PAYLOAD_PUSH_OFFSET); the mesh stage
    /// (mesh_fallback not set) reads its map entry and its task's payload from there. Header words: TASK_HEADER_*
    bool task_buffer{};
    /// (fork, direct back end) a vertex stage that reads its generic inputs as fetched by same-layout UINT formats and
    /// converts them by a code per attribute in the push data (VERTEX_FORMATS_PUSH_OFFSET): generic_input_types unread
    bool vertex_formats_pushed{};
    /// (fork, direct back end) a fragment stage whose generic inputs are what it loads, whatever stage is before it: a
    /// generic the stage before does not write (its bit clear in the push data's word at FRAGMENT_INPUTS_PUSH_OFFSET) reads
    /// (0, 0, 0, 1). previous_stage_stores unread
    bool fragment_inputs_pushed{};

    TessPrimitive tess_primitive{};
    TessSpacing tess_spacing{};
    bool tess_clockwise{};

    InputTopology input_topology{};

    std::optional<float> fixed_state_point_size;
    std::optional<CompareFunction> alpha_test_func;
    float alpha_test_reference{};

    /// Static Y negate value
    bool y_negate{};
    /// Use storage buffers instead of global pointers on GLASM
    bool glasm_use_storage_buffers{};

    /// Transform feedback state for each varying
    std::array<TransformFeedbackVarying, 256> xfb_varyings{};
    u32 xfb_count{0};

    /// Output types for each color attachment
    std::array<AttributeType, 8> color_output_types{};

    /// Dual source blending
    bool dual_source_blend{};
};

} // namespace Shader
