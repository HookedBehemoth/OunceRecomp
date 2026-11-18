// What vk_shader.cpp shares with the compute programs (vk_compute.cpp): reading a stage's package,
// the translator's profile and host settings, the programs' constants (c1) in device memory, the cache records' Info format,
// the device's part of the caches' stamps, the driver binaries' file, the bound textures' answers. Thin calls into vk_shader.cpp's own (one copy of each).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/shader_info.h"

#include "vk_internal.h"
#include "rx_nvn.h"

namespace hsd::shader_parts {

/// The stage's control section (0xd8 bytes) and code area
bool read_stage(const nvn_stage_data& sd, uint8_t ctl[0xd8], std::vector<uint8_t>& area);
/// The profile of a stage (pinned to 32 lanes where the device runs it so)
const Shader::Profile& stage_profile(VkShaderStageFlags stage);
Shader::HostTranslateInfo translate_info();
VkDeviceAddress put_c1(const std::vector<uint8_t>& bytes, uint32_t used);
void info_put(std::vector<uint8_t>& o, const Shader::Info& i);
bool info_get(const uint8_t*& p, const uint8_t* end, Shader::Info& i);
/// What translated code depends on besides the translator's own code: the device's subgroup answers, the record layout
uint64_t device_stamp();
/// The driver binaries of shader objects (hookshot_direct_shader_objects.bin), by a hash of the create info
uint64_t object_key(const VkShaderCreateInfoEXT& ci, const VkShaderDescriptorSetAndBindingMappingInfoEXT& mi, bool size32);
/// The binary kept for key, or empty
std::vector<uint8_t> object_binary(uint64_t key);
/// Keeps a shader object made from SPIR-V
void object_keep(uint64_t key, VkShaderEXT obj);
std::string program_dir();
/// A texture handle's answer (heap_answer; not registered: Color2D)
uint8_t answer_of(uint32_t handle);
uint8_t answer_canonical(uint8_t type, bool dim_fixed, uint8_t desc_type, bool depth_float);

}  // namespace hsd::shader_parts
