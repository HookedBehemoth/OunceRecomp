// (native-vk engine cut E3, HOS_ENG_JOBS=1) The game's render jobs (f_80084de0, ~102 a frame: 17 passes x 6 chunks) as Vulkan
// secondary command buffers recorded on the game's own job threads (ModuleSystemWorker1-5, sead::MainThread), from the job's
// NVN records at its EndRecording (host/eng_jobs.c hooks the recorder and tags the recording; gpu/vk/eng_job.cpp records).
//
// The game's per-item walk (the 0x80 specialised draw loops, the material binder f_8008fc00, the occlusion gating, the
// virtual draws) is kept as it is: it is the whole of the recorder, and its NVN calls are host code already. What is
// replaced is what its records become: translated where they are made, with the pass known (the view record), instead of
// at the submit on the Presentation Thread. Nothing here touches the replay's single-threaded state (texture map, uploads,
// residency, hazards): textures are NVN handle words (descriptor-heap slots, E1), buffers guest addresses in layer 0's
// windows, programs the ones the replay picked for the same job draws before (published by the replay, never compiled or
// waited for here).
//
// Interface with E4 (engine-cut-plan.md "E3 / E4 interface"): EngJob, eng_job_lookup, eng_job_retire_through. E4 owns
// eng_pass.h (eng_pass_formats); until it is in the tree the formats are learned from the replay's own targets at the
// job's first draw (eng_job.cpp, HOS_ENG_JOBS_SELFPASS).
#pragma once

#include <cstdint>
#include <vector>

#include "vk_internal.h"
#include "vk_state.h"

struct rx_eng_job_info;

namespace mkwvk {

struct Program;
struct SoProg;
struct EngPassFormats { uint32_t ncolor; VkFormat color[8]; VkFormat depth, stencil; VkSampleCountFlagBits samples; };
/// (HOS_ENG_JOBS_SPLIT=1) a job split around the draws its thread could not record: a run of its draw ops (every DRAW, DRAW_IDX,
/// DRAW_INDIRECT, DRAW_MESH in record order, counted from 0 as its records hold them) recorded into one secondary; the draw ops
/// between the runs are the replay's, drawn inline in the same rendering
struct EngSeg { uint32_t first, n; VkCommandBuffer cb; uint32_t ndraws; VkCommandBuffer pre = VK_NULL_HANDLE; };   // (pre: HOS_ENG_JOBS_FALLBACK, a
                                                                                               // fallback draw's dispatches before the run)
/// (HOS_ENG_JOBS_PREWORK=1, fallbacks.md 4-5) work a job needs before the rendering it is executed in: copies of GPU-made indirect
/// mesh arguments into the job's own slots (NVN's {count, ...} records -> Vulkan's {x, 1, 1}), and the cross-queue waits its
/// records hold (their order is the job-call site's)
struct EngPreCopy { VkBuffer src; uint64_t src_off; VkBuffer dst; uint64_t dst_off; uint32_t bytes; };
struct EngWait { uint64_t gpu; uint32_t mode, value; };
struct EngJob {
    uint64_t view_key;            // the job's view record (guest address): the pass identity
    EngPassFormats fmt;           // the formats it was recorded for (from eng_pass_formats at record time)
    VkCommandBuffer cb;           // secondary, VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT + inheritance(fmt); null: not
                                  // recordable (eng_job.cpp counts why): use the job's NVN records
    uint32_t ndraws;
    const uint32_t *tex; uint32_t ntex;   // every NVN texture/image handle word it binds (residency + hazards, at submit)
    const uint64_t *deferred; uint32_t ndeferred;  // guest addresses of deferred handle words it binds (system table)
    uint32_t serial;              // 0 = retired
    // (HOS_ENG_JOBS_SPLIT) nsegs 0: cb holds all the job's draws. Else the runs in order (cb = segs[0].cb), ndrawops the job's draw
    // ops in all (the call site checks its own count against it)
    const EngSeg* segs; uint32_t nsegs; uint32_t ndrawops;
    const uint32_t* dsig;          // (split: per draw op, a hash of its payload: the call site checks it meets the same draws)
    // (E6) the uniform ranges its draws read from guest memory by address, (address, size) pairs: the replay's own uniform updates
    // earlier in the same recording are written to guest memory only at the recording's end (flush_ub_updates), so a job whose
    // ranges one of them overlaps would read the value from before it
    const uint64_t* ubo; uint32_t nubo;
    // (HOS_ENG_JOBS_PREWORK) before the job's rendering: these copies, a full barrier if pre_barrier (the job's own barrier
    // before its first draw, e.g. 0x28 after the culling waits), and its waits met in replay order
    const EngPreCopy* pre; uint32_t npre;
    const EngWait* waits; uint32_t nwaits;
    bool pre_barrier;
    // (HOS_ENG_JOBS_WRITES) the storage ranges its draws write, (address, size) pairs: noted as the GPU's at the call
    const uint64_t* writes; uint32_t nwrites;
};
/// (E6) the hash of a draw op's payload both sides make
uint32_t eng_draw_sig(int op, const void* payload, size_t n);
/// The fake slot handle E3 writes back (while the game's recorder runs: the job recording's own NVN handle) -> its job, or NULL.
/// On the replay thread: the job is noted as used by the recording being made (ub_recording_value), retired by
/// eng_job_retire_through once the GPU passed that value
const EngJob* eng_job_lookup(uint64_t slot_handle);
/// Command buffers reused after the GPU passed them
void eng_job_retire_through(uint64_t replay_value);
/// (E6's measurement, HOS_ENG_PROF) why the job under a slot handle has no command buffer: its verdict (0 = recorded), -1 none
/// there; the verdict's name
int eng_job_why(uint64_t slot_handle);
const char* eng_job_why_name(int why);

// ---- the rest: E3's own (the host's entry, the replay's hooks, vk_draw.cpp's state for a job's draw) ----
bool eng_job_on();        // HOS_ENG_JOBS=1 (with RX_VK_DESC_HEAP=1 and shader objects)
bool eng_job_check();     // HOS_ENG_JOBS_CHECK=1: every job draw's state compared with the replay's for the same records

/// (vk_replay.cpp) NOP_JOB: a job's records begin (end 0) / end (1) in the stream being replayed
void eng_job_replay_mark(uint32_t serial, uint32_t end);
/// (vk_replay.cpp) inside a job's records: a draw op (before its render-enable test), with the state the replay has for it
bool eng_job_replay_active();
void eng_job_replay_op(int op);
/// (vk_replay.cpp) the replay stops inside a job's records at a wait not met (at), and goes on there later
void eng_job_replay_suspend(const uint8_t* at);
void eng_job_replay_resume(const uint8_t* at);   // (while active: an op met, for the check's log)
void eng_job_replay_draw_op(const NvnState& s, int op, const void* payload, size_t n);
/// (vk_replay.cpp) inside a job's records: a texture / sampler / image bind (deferred: src, the game's array), or a packet's
void eng_job_replay_bind(int what, int stage, int first, int n, uint64_t src);
/// (vk_draw.cpp draw()) the program the replay picked for the job draw just traced, and the targets it draws into
struct EngTargets { uint32_t width, height; int n; VkFormat color[8]; VkFormat depth; VkSampleCountFlagBits samples; };
void eng_job_replay_program(const NvnState& s, const Program& p, const struct SoProg* so, const uint8_t color_types[8],
                            const EngTargets& at);
/// (vk_replay.cpp, every 120 presents) the frame log's line
void eng_job_report(double frames);

// (vk_draw.cpp, for eng_job.cpp: a job draw's fixed-function state, its vertex input and its stages on a job's command buffer,
// through the draw path's own functions and the calling thread's caches. reset: a new command buffer, all set again)
bool eng_draw_topology(int prim, VkPrimitiveTopology* t);
void eng_draw_state(VkCommandBuffer cmd, const NvnState& s, const Program& p, const struct SoProg& so, const EngTargets& at,
                    VkPrimitiveTopology topo, bool reset, bool mesh = false);
VkBuffer eng_draw_zero_buffer();   // (the stream unset attributes read: binding 15)

// ---- (E3 step 2) a job draw's descriptors for the heap, made on the job thread (vk_draw.cpp eng_draw_descriptors), and the
// replay's for the same draw (the check) ----
/// A bank word made from a deferred handle (the game's array is read at the submit): rewritten then (eng_job.cpp, from the
/// submitted records: the job's deferred bind number bind, its handle index)
struct EngDescPatch { uint32_t* word; uint32_t bind; uint32_t index; };
/// One buffer element of the draw (build_descriptors' order): a descriptor of a buffer range, or the bytes of an upload (a driver
/// bank, c1), compared by their bytes; or none (a null descriptor, a storage buffer read through its address)
struct EngDescEl { uint8_t kind; VkBuffer buffer; uint64_t offset, range; const uint8_t* bytes; uint32_t n; uint8_t stage = 0, index = 0; };
enum EngDescWhy { EDW_OK, EDW_NO_HEAP, EDW_UBO_MEM, EDW_UBO_UNALIGNED, EDW_SSBO, EDW_SSBO_WRITTEN, EDW_SSBO_CLASSIC, EDW_TEXEL, EDW_IMAGE, EDW_HANDLE_PROGRAM,
                  EDW_USER_HANDLE, EDW_DEFERRED_PUSH, EDW_RING, EDW_STREAM, EDW_N };
struct EngDrawDesc {
    uint32_t why = EDW_OK;
    const Program* prog = nullptr;
    uint8_t push[256];
    uint32_t push_bytes = 0;
    std::vector<EngDescEl> els;
    std::vector<uint32_t> handles;   // (per texture element: the pushed handle, 0 for the others)
    std::vector<std::pair<const uint8_t*, uint32_t>> banks;   // (per HeapProgram::banks: its bytes)
    std::vector<EngDescPatch> patches;
    std::vector<uint64_t> ubos;   // (E6: the user uniform ranges it reads by address, guest address << 0 and size, in pairs)
    std::vector<uint64_t> writes; // (HOS_ENG_JOBS_WRITES: the storage ranges it writes, (address, size) pairs)
    void clear() { why = EDW_OK, push_bytes = 0, els.clear(), handles.clear(), banks.clear(), patches.clear(), ubos.clear(), writes.clear(); }
};
/// The deferred binds a job's state came from: per (texture 0, sampler 1, image 2), stage, slot: (bind number + 1) << 8 | index
/// (0: an immediate bind)
using EngBindRefs = uint32_t[3][8][128];
/// A job draw's descriptors (ring block in the job threads' ring, banks and c1 in the job stream) and its push data; false: why set
struct HeapProgram;
bool eng_draw_descriptors(const NvnState& s, const Program& p, const HeapProgram& hp, const EngTargets& at, const EngBindRefs& refs,
                          EngDrawDesc& out);
/// (HOS_ENG_JOBS_FALLBACK, fallbacks.md 3) a mesh fallback program's task and mesh dispatches for a job draw into cb (a secondary outside
/// any rendering, executed by the job-call site before the run that draws it); *records: the buffer the draw's command is the header of
bool eng_draw_fallback(VkCommandBuffer cb, const Program& p, const SoProg& so, EngDrawDesc& dd, uint32_t groups, uint32_t first,
                       VkBuffer conv, uint64_t conv_off, uint64_t input, VkBuffer* records);
/// (fallbacks.md 2b) a direct mesh draw's first task: a 16-byte slot of the job stream ({0, 0, 0, first}) whose word 3 the task stage
/// reads, its address in the draw's push data (as the replay's mesh_args entry); false: no stream
bool eng_draw_mesh_first(EngDrawDesc& dd, uint32_t first);
/// (E6 R1) BdaStage's refusals for the program's stages, worked out from their Info ("" none)
std::string eng_bda_refusal(const Program& p);
/// (E6) every vertex stream the program loads has a buffer in the job's own state (else it is inherited from the caller)
bool eng_draw_vertex_bound(const NvnState& s, const Program& p, bool strict);
/// (HOS_ENG_JOBS_SAMPLED) the handle words a job draw's program reads (textures, images), deferred ones left out
void eng_draw_sampled(const NvnState& s, const Program& p, const EngBindRefs& refs, std::vector<uint32_t>& out);
/// (vk_draw.cpp heap_draw, inside a job's records) the replay's descriptors for the job draw it is drawing
void eng_job_replay_desc(const EngDrawDesc& d, const NvnState& s);
/// The job threads' upload memory (banks, c1): n bytes at align, its buffer, offset, mapping and device address; false: none
bool eng_job_stream_alloc(uint32_t n, uint64_t align, VkBuffer* buf, uint64_t* off, uint8_t** map, VkDeviceAddress* addr);

} // namespace mkwvk

extern "C" {
/// (the host, host/eng_jobs.c through host/rx_backend.c, on the game's job thread) a job's recording BEGIN .. END, its handle
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void mkwvk_eng_job_recorded(const struct rx_eng_job_info* j, const uint8_t* rec, uint64_t bytes, uint64_t handle);
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void mkwvk_eng_job_deferred(uint32_t serial, uint32_t bind, const uint64_t* h, int n);
}
