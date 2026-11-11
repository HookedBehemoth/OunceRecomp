// (native-vk) The uniform buffer census, RX_VK_UBO_CENSUS=<file>: vk_ubo_census.cpp
#pragma once

#include "vk_internal.h"
#include "vk_state.h"

namespace mkwvk {
struct Program;
struct Stage;
/// RX_VK_UBO_CENSUS=<file> is set (read once)
bool ubo_census_on();
/// A uniform buffer bind replayed into s: its trailer (the guest site, the hash when it was recorded) if it has one
void ubo_census_bind(const NvnState* s, int stage, int index, const uint8_t* payload, uint32_t avail);
/// A draw's or dispatch's descriptors are being built (the records that follow belong to it)
void ubo_census_begin();
/// A constant buffer those descriptors read (index 0..18 as uniform_range's), as it resolved
void ubo_census_note(const NvnState& s, const Program& p, const Stage& st, uint32_t index, const VkDescriptorBufferInfo& bi);
} // namespace mkwvk
