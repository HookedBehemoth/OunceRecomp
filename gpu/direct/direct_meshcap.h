// HOS_DBG=meshcap:<present>, a debug aid: one frame's mesh draws captured for the mesh harness (tools/meshlab,
// docs/direct/mesh-harness.md). Every mesh program's stages (SPIR-V, heap mappings, c1) are kept as they are made; in the frame
// after that present every mesh draw (task+mesh and mesh-only) is recorded with its push data, its uniform buffers' bytes, its
// mesh stage's storage buffers and, once the GPU has run the frame, its task header, map and payload; the frame's other draws
// and task steps are listed in their order. Written to meshcap/frame.bin in the working directory (a run's folder: local, never
// committed; it holds game data). Off by default: one load and a branch per mesh program made and per draw.
#pragma once

#include <cstdint>
#include <vector>

#include "direct_meshcap_format.h"

namespace hsd {

struct CmdSlot;
struct DrawProg;

/// HOS_DBG=meshcap:<present> is set (the programs are kept from the start)
bool meshcap_on();
/// The frame being recorded is one to capture, and (the roll: meshcap:<first>-<last>, meshcapprog:<key>, meshcaplost) this
/// program's draws are wanted
bool meshcap_now(uint64_t key = 0);

/// (direct_shader.cpp finish) a mesh program's stage, as made
struct McStageIn {
    uint32_t vk_stage, nvn_stage, threads, payload_words;
    const std::vector<uint32_t>* spirv;
    const void* maps;
    uint32_t nmaps, map_size;
    const std::vector<uint8_t>* c1;
};
void meshcap_program(const DrawProg& d, uint64_t task_code, const McStageIn* stages, uint32_t n);

/// (direct_draw.cpp) a mesh draw of the frame. ubo_bytes[i]: table entry i's bytes when the CPU made them (bank copies, the
/// list's updated copies), empty when it is a game buffer read at the frame's end (ubos[i].guest) or c1 (the program's).
/// header_host / header_dev: a task+mesh draw's header in the arena (the chunk holding it: chunk_*); the task pool (indirect)
struct MeshCapDraw {
    bool vertex = false;   // (M6h: a vertex draw: its uniform buffers only)
    mcap::McOther other{};  // (a vertex draw's place: kind and count as meshcap_other)
    mcap::McDraw d{};
    std::vector<mcap::McDrawUbo> ubos;
    std::vector<std::vector<uint8_t>> ubo_bytes;
    std::vector<mcap::McDrawSsbo> ssbos;
    const uint8_t* chunk_host = nullptr;
    uint64_t chunk_dev = 0, chunk_bytes = 0, header_dev = 0;
    const uint8_t* pool_host = nullptr;
    uint64_t pool_dev = 0, pool_bytes = 0;
};
void meshcap_draw(MeshCapDraw&& d);   // (M6h: a vertex draw too, d.vertex)
/// Another command of the frame (kind: mcap::McOther)
void meshcap_other(uint64_t cb, uint32_t kind, uint64_t key, uint32_t count);
/// (present_run) at the present after the captured one: the GPU's words read, the file written
void meshcap_after();
/// (HOS_DBG=meshcaplost) a submit reported the device lost: the kept draws around the first unfinished command's present
/// (0: not known, the newest) written to meshcap/roll.bin, with the task headers as the GPU left them
void meshcap_lost(uint64_t first_open_present);

}  // namespace hsd
