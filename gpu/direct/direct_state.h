// The NVN state of a list (docs/direct/m2-interface.md, direct_state; m2-shader-contract.md section 3, "Minimal state"):
// what the emitted ops set, in the layout the translator's questions and the driver banks are read from. A list starts
// from NVN's defaults (it inherits nothing from the list before it: m0-jobs-state.md A); the recording thread's alone.
// The generation counters say what changed since a draw last read it (direct_draw.cpp keeps its copies by them).
#pragma once

#include <cstdint>

#include "direct_internal.h"
#include "../render/rx_nvn.h"

namespace hsd {

/// NVN's stages: vertex 0, fragment 1, geometry 2, tessellation 3 / 4, compute 5; NVN2's task 6 and mesh 7
constexpr int kStages = 8;
/// The graphics stages a bank is kept for (vertex, fragment)
constexpr int kBankStages = 2;

struct DState {
    nvn_prog prog{};
    bool has_prog = false;
    nvn_p_bind_buf ubo[kStages][16]{}, ssbo[kStages][16]{};
    // (the handles' low words, which the banks hold: the graphics stages')
    uint32_t tex[kBankStages][128]{}, samp[kBankStages][128]{}, img[kBankStages][32]{};
    // (a deferred bind's source: the guest address of the slot's handle in the game's array, read at the guest's submit; 0:
    // immediate. Kept for the stages whose banks a draw makes)
    uint64_t tex_src[kBankStages][128]{}, samp_src[kBankStages][128]{}, img_src[kBankStages][32]{};
    uint32_t deferred[kBankStages]{};   // (slots of the stage bound deferred now)
    nvn_blend_st blend[8]{};
    nvn_color_st color{};
    nvn_chmask_st chmask{0xffffffffu};
    nvn_ms_st ms{};
    nvn_poly_st poly{};
    nvn_ds_st ds{};
    uint32_t vattr[16]{};
    int nvattr = 0;
    nvn_vs_st vstream[16]{};
    NVNbufferRange vbuf[16]{};
    float viewport[4]{};
    float depth_near = 0.0f, depth_far = 1.0f;
    int32_t scissor[4]{};
    uint32_t stencil_value_mask[2]{0xff, 0xff}, stencil_mask[2]{0xff, 0xff}, stencil_ref[2]{0, 0};
    float poly_offset[3]{};
    float blend_color[4]{};
    bool restart = false;
    bool depth_clamp = false, raster_discard = false, render_enabled = true;
    bool conditional = false;   // (a render condition is set: the draws after it are skipped until RENDER_ENABLE)
    // (what changed: bumped by the ops)
    uint32_t bank_gen[kBankStages]{};   // (a stage's textures, samplers, images or storage buffers: its c0 / c2 banks)
    uint32_t ubo_gen = 0;               // (any uniform buffer)
    uint32_t vtx_gen = 0;               // (vertex attributes: what the pick reads of the vertex state)
    uint32_t prog_gen = 0;
};

/// One op on the list's state (the recording thread); false: not a state op
bool state_emit(DState& s, uint32_t op, const void* data, uint32_t size);
/// A driver bank word (c0 / c2) as the NVN driver lays it out for the graphics stage (m2-shader-contract.md section 2,
/// "Banks"; vk_bank.cpp driver_bank_word)
uint32_t bank_word(const DState& s, int bank, int stage, uint32_t offset);
/// The first n bytes of a graphics stage's driver bank
void bank_fill(const DState& s, int bank, int stage, uint8_t* d, uint32_t n);

void state_stats(char* out, size_t cap, double frames);

}  // namespace hsd
