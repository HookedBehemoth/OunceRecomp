// What direct_shader.cpp shares with the compute programs (direct_compute.cpp): reading a stage's package,
// the translator's profile and host settings, the programs' constants (c1) in device memory, the cache records' Info format
// and stamp, the bound textures' answers. Thin calls into direct_shader.cpp's own (one copy of each).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/shader_info.h"

#include "direct_internal.h"
#include "../api/rx_nvn.h"

namespace hsd::shader_parts {

/// The stage's control section (0xd8 bytes) and code area
bool read_stage(const nvn_stage_data& sd, uint8_t ctl[0xd8], std::vector<uint8_t>& area);
/// The profile of a stage (pinned to 32 lanes where the device runs it so)
const Shader::Profile& stage_profile(VkShaderStageFlags stage);
Shader::HostTranslateInfo translate_info();
VkDeviceAddress put_c1(const std::vector<uint8_t>& bytes, uint32_t used);
void info_put(std::vector<uint8_t>& o, const Shader::Info& i);
bool info_get(const uint8_t*& p, const uint8_t* end, Shader::Info& i);
uint64_t cache_stamp();
std::string program_dir();
/// A texture handle's answer (heap_answer; not registered: Color2D)
uint8_t answer_of(uint32_t handle);
uint8_t answer_canonical(uint8_t type, bool dim_fixed, uint8_t desc_type, bool depth_float);

}  // namespace hsd::shader_parts
