// (native-vk engine cut E4, HOS_ENG_PASSES=1) The pass table and the job consumer at the layer lists' job-call sites
// (work/assets/native/engine-cut-plan.md, E4; e4-impl.md). E4 owns this file; E3 only includes it (eng_pass_formats,
// eng_pass_note: the fixed interface).
//
// host/nvn_native.c marks each handle the job-call sites (0x80668c84 f_806688c0, 0x8028a538 f_8028a2a0, 0x807cd418
// f_807cd380) call with NOP_ENG CALL_BEGIN / CALL_END, and puts a NOP_ENG JOB with the job's view record first in every
// render job's recording (f_80084de0). The replay (vk_replay.cpp, NOP_ENG) then knows at each job call the job's pass
// (its view record) and the targets in force: the pass table. With a host-recorded job (eng_job_lookup) whose formats are
// the rendering's, no hazard and its textures resident, it executes the job's secondary in the pass instead of the NVN
// records; HOS_ENG_PASS_SELFTEST=1 records such a secondary itself on the replay thread from the job's NVN records (today's
// draw path) and executes it, to prove the consumer without E3.
#pragma once

#include <cstdint>
#include <vector>

#include "eng_job.h"

namespace mkwvk {

// ---- the fixed interface (E3) ----
bool eng_pass_formats(uint64_t view_key, EngPassFormats* out);   // thread-safe read; false = unknown yet
void eng_pass_note(uint64_t view_key, const EngPassFormats& f);   // E4: at a job-call site, the current targets

// ---- E4's own ----
bool eng_passes_on();        // HOS_ENG_PASSES=1 (read once)
bool eng_pass_selftest();    // HOS_ENG_PASS_SELFTEST=1 (implies HOS_ENG_PASSES)
/// The device takes inline draws and secondaries in one rendering (VK_KHR_maintenance7 / VK_EXT_nested_command_buffer):
/// else each execute is in a rendering of its own (counted as restarts)
bool eng_pass_mixed();
/// The flags every rendering the replay begins carries (0 when HOS_ENG_PASSES is off)
VkRenderingFlags eng_rendering_flags();
/// (vk_main.cpp create_device) VK_KHR_maintenance7 or VK_EXT_nested_command_buffer asked for, with their feature
void eng_pass_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail);
bool eng_formats_equal(const EngPassFormats& a, const EngPassFormats& b);
/// The pass table's note with what is not part of the formats: the site, the size and the targets' identities (what varies)
void eng_pass_note_ex(uint64_t view_key, const EngPassFormats& f, uint32_t site, uint32_t w, uint32_t h, uint64_t color0,
                      uint64_t depth);

// counters (the frame log's "eng passes:" line)
enum EngCount {
    EC_CALLS,          // job-call site handles seen (CALL_BEGIN)
    EC_LAYER_CALLS,    // of them, f_807cd380's (layer lists, not jobs)
    EC_JOBS,           // with a job's view (the job's own JOB mark found)
    EC_NO_VIEW,        // a job-call handle whose recording has no JOB mark
    EC_EXECUTED,       // secondaries executed (E3's or the self-test's)
    EC_EXEC_DRAWS,     // their draws
    EC_FB_NO_JOB,      // fallen back: no host job (E3 not there and no self-test)
    EC_FB_FORMATS,     // the job's formats are not the rendering's
    EC_FB_HAZARD,      // a texture it binds is an attachment of the pass
    EC_FB_RESIDENT,    // a texture it binds not resident
    EC_FB_OP,          // (self-test) an op a secondary cannot hold (barrier, dispatch, copy, event, wait, clear, targets)
    EC_FB_EMPTY,       // (self-test) no draws
    EC_FB_MODE,        // (self-test) the parallel replay's passes, a dump, the heap
    EC_FB_NOPASS,      // (self-test) no rendering could be opened for it
    EC_FB_ABORT,       // (self-test) the recording left the pass (a hazard split, a clear, a fallback dispatch): replayed inline
    EC_FB_MALFORMED,   // no CALL_END after a CALL_BEGIN
    EC_RESTARTS,       // renderings ended and begun again around an execute (no mixing)
    EC_STATE_WALKS,    // E3 jobs executed: their NVN records walked for state only
    EC_N
};
void eng_count(int c, int64_t n = 1);
void eng_abort_reason(const char* why);   // (self-test aborts by what left the pass, logged with the counters)
void eng_pass_report(double frames);      // (every 120 presents, with the frame statistics)

// ---- a secondary being recorded on this thread by today's draw path (the self-test): what would leave the pass breaks it ----
bool eng_sec_active();
const char* eng_sec_broken();          // (null: not broken)
void eng_sec_set(bool active);
/// Called where the draw path would end the pass or record outside it: true while a secondary records (the caller does
/// nothing then, the secondary is dropped and the job replayed inline)
bool eng_sec_trap(const char* why);

} // namespace mkwvk
