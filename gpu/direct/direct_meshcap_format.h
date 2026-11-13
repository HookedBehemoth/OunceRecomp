// (M6e) The file HOS_DBG=meshcap:<present> writes (direct_meshcap.cpp) and the mesh harness reads (tools/meshlab): one frame's
// mesh draws with everything their mesh stage reads, so that the stage can be run outside the game. A debug aid's file, local
// only: it holds game data (the stages' SPIR-V translated from the game, the game's buffers) and is never committed.
//
// meshcap/frame.bin: a McFile, then records, each a McRecord and its payload, in this order: the programs the draws use
// (MC_PROGRAM), the memory blobs (MC_BLOB), then every draw of the frame's command buffers in recording order (MC_DRAW for
// mesh draws, MC_OTHER for the rest, so the order of a job's commands is kept). Little-endian, packed as declared (every field
// naturally aligned).
#pragma once
#include <cstdint>

namespace mcap {

constexpr uint32_t kMagic = 0x434d5348;   // ('HSMC')
constexpr uint32_t kVersion = 1;

struct McFile {
    uint32_t magic, version;
    uint64_t present;
};
enum McType : uint32_t { MC_PROGRAM = 1, MC_BLOB = 2, MC_DRAW = 3, MC_OTHER = 4, MC_VDRAW = 5 };
struct McRecord {
    uint32_t type, reserved;
    uint64_t bytes;   // (the payload's)
};

/// MC_PROGRAM: a McProgram, its McUbo-like table (McProgUbo[nubo]), its McPushed[npushed], then each stage: a McStage, its
/// SPIR-V (spirv_words * 4 bytes), its heap mappings (maps * map_size bytes: VkDescriptorSetAndBindingMappingEXT as the back
/// end made them), its c1 bytes
struct McProgram {
    uint64_t key;
    uint64_t codes[3];     // (mesh, fragment, header-only stage's code keys)
    uint64_t task_code;    // (the task stage's code key; 0: a mesh-only program)
    uint32_t task, task_threads, task_payload_words, push_bytes;
    uint32_t nubo, npushed, nstages, map_size;
};
struct McProgUbo {
    uint8_t stage, index;
    uint16_t pad;
    uint32_t bytes;
};
struct McPushed {
    uint8_t stage, index;
    uint16_t push_off;
};
struct McStage {
    uint32_t vk_stage, nvn_stage, spirv_words, maps, c1_bytes, threads, payload_words, pad;
};

/// MC_BLOB: a McBlob and its bytes. guest: the game's address of the range (0: bytes the back end made, a bank's copy)
struct McBlob {
    uint32_t id, pad;
    uint64_t guest, bytes;
};

/// MC_DRAW: a McDraw, McDrawUbo[nubo], McDrawSsbo[nssbo], then the task header's map (nmap words) and payload area (npayload
/// words) as the GPU left them
struct McDraw {
    uint64_t key, cb, lr, present;
    uint32_t seq;          // (the command's number in the frame's recording, all command buffers)
    uint32_t cb_seq;       // (its number in its command buffer)
    uint32_t list_kind, task, indirect, gated;
    int32_t x, y, z;       // (NVN's draw: first and count of mesh workgroups for a direct draw)
    uint32_t nargs, stride;
    uint64_t record;       // (an indirect mesh-only draw's argument record's guest address)
    uint32_t record_words[4];   // (and its words when the GPU had run the frame)
    uint32_t w, h, n, samples;
    uint32_t color[8], depth, depth_aspect;
    uint32_t push_bytes;
    uint8_t push[256];
    uint32_t dyn_bytes;
    uint8_t dyn[512];      // (the back end's dynamic state as set for this draw: direct_draw.cpp Dyn)
    uint32_t header[16];   // (the task header when the GPU had run the frame: emit_spirv.h TASK_HEADER_*)
    uint32_t nubo, nssbo, nmap, npayload;
};
struct McDrawUbo {
    uint8_t stage, index;
    uint16_t pad;
    uint32_t bytes;        // (what the program reads of it)
    uint64_t dev;          // (the address the table held)
    uint64_t guest;        // (a game buffer's guest address; 0: a bank copy or c1)
    uint32_t blob, pad2;   // (its bytes: a McBlob id; ~0u none)
};
struct McDrawSsbo {
    uint8_t stage, slot;
    uint16_t pad;
    uint32_t blob;         // (~0u: not read)
    uint64_t guest, size;
};
/// MC_VDRAW (M6h): a vertex draw, for the fragment harness: a McDraw (its pass, push data, dynamic state; x: the vertices or
/// indices, y: the instances, task 0) and McDrawUbo[nubo] (the program's uniform buffers, both stages'); then a McOther as for
/// MC_OTHER (its place in the frame's order)
/// MC_OTHER: a command of the frame that is not a mesh draw
struct McOther {
    uint64_t key, cb;
    uint32_t seq, cb_seq, kind, count;   // (kind: 1 draw, 2 indexed, 3 indirect, 4 task step (compute), 5 barrier)
};

}  // namespace mcap
