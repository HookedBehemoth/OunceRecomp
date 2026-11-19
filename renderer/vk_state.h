// The NVN state of a list:
// what the emitted ops set, in the layout the translator's questions and the driver banks are read from. A list starts
// from NVN's defaults (it inherits nothing from the list before it: jobs-state.md A); the recording thread's alone.
// The generation counters say what changed since a draw last read it (vk_draw.cpp keeps its copies by them).
#pragma once

#include <cstdint>

#include "vk_internal.h"
#include "rx_nvn.h"

namespace hsd {

/// NVN's stages: vertex 0, fragment 1, geometry 2, tessellation 3 / 4, compute 5; NVN2's task 6 and mesh 7
constexpr int kStages = 8;
/// The stages a bank is kept for, by NVN stage (M4b: every one; the mesh stage's draws read theirs)
constexpr int kBankStages = kStages;
/// A stage's push constants (nvnCommandBufferPushGraphicsConstants), at most: the NVN2 driver sends no more. They are root
/// table 0, which Ampere shaders read as bank 24
constexpr uint32_t kPushBytes = 0x100;
constexpr int kRootBank = 24;

constexpr nvn_blend_st kBlendDefault{0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO,
                                      NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};

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
    uint8_t push[kBankStages][kPushBytes]{};   // (the graphics stages' push constants: their root tables)
    uint32_t pushed = 0;                       // (the stages the list pushed constants for, by bit)
    // (the state objects as their SetDefaults make them, runtime/nvn_native.c: a list that binds none draws with these)
    nvn_blend_st blend[8]{kBlendDefault, kBlendDefault, kBlendDefault, kBlendDefault, kBlendDefault, kBlendDefault,
                          kBlendDefault, kBlendDefault};
    nvn_color_st color{0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    nvn_chmask_st chmask{0xffffffffu};
    nvn_ms_st ms{1};
    nvn_poly_st poly{NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    nvn_ds_st ds{0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_OP_KEEP,
                 NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
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
    uint32_t bank_gen[kBankStages]{};   // (a stage's textures, samplers, images, storage buffers or push constants: its banks)
    uint32_t ubo_gen = 0;               // (any uniform buffer)
    uint32_t prog_gen = 0;
    uint32_t vi_gen = 0;    // (M3b: attributes or streams: the vertex input a draw sets)
    uint32_t dyn_gen = 0;   // (M3b: any state the dynamic state is made from)
};

/// One op on the list's state (the recording thread); false: not a state op
bool state_emit(DState& s, uint32_t op, const void* data, uint32_t size);
/// A driver bank word (c0 / c2) as the NVN driver lays it out for the graphics stage (shader-contract.md section 2,
/// "Banks"; vk_bank.cpp driver_bank_word)
uint32_t bank_word(const DState& s, int bank, int stage, uint32_t offset);
/// The first n bytes of a graphics stage's driver bank (c0, c2) or root table (kRootBank). A stage the list pushed no
/// constants for has the console's blit block there, for a render area of w x h (shader-contract.md, "Banks")
void bank_fill(const DState& s, int bank, int stage, uint8_t* d, uint32_t n, uint32_t w, uint32_t h);

void state_stats(char* out, size_t cap, double frames);

}  // namespace hsd
