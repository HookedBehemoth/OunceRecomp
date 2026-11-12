// (M6b) Checks that can fail (docs/direct/m6b-report.md): the validation layer's messages counted and proven heard, and rule 2's
// strict mode (docs/direct-backend-plan.md, Rules 2; Checks, "Counters").
//
// Validation. When the Khronos validation layer is in the process (the existing way of turning it on for a run: the loader's
// VK_LOADER_LAYERS_ENABLE=*validation, with VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true for synchronization validation; or
// HOS_DBG=validation, which enables it and its synchronization validation itself), a VK_EXT_debug_utils messenger counts every
// message by severity and by message id and logs each distinct id once with its first text. The start line proves the layer
// is loaded (its library mapped in the process, and its own status message heard by the instance's messenger); the
// summary line is in every 600-present block and at a clean exit. With HOS_DBG=strict any warning or error ends the run
// (exit code 87) unless its id is on the known list below.
//
// Strict (HOS_DBG=strict[:<present>]). Armed at the first present that is at least <present> (default 1200) and at least
// kStrictQuiet presents after the last program miss (a draw or dispatch skipped because its program was not read or its
// variant was being translated). Once armed, the first event of any reason not on the known list is logged with what and
// where and ends the run (exit code 86). Events of a known reason are logged (the first few of each, with the entry's
// reason) and counted, and the run goes on. HOS_DBG=strict,strictall: a survey, every reason's first events after arming logged
// and the run not stopped. Without strict every event is only counted, as before; strict_stats gives the
// events by reason, before and after arming, in the 600-present block.
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include <vulkan/vulkan.h>

namespace hsd {

// ---- validation ----
/// Before vkCreateInstance: VK_EXT_debug_utils enabled if the loader has it, the instance's own messenger (and, with
/// HOS_DBG=validation, the layer and its settings) chained into ci. The storage stays valid until validation_instance_made.
void validation_instance_info(VkInstanceCreateInfo& ci, std::vector<const char*>& exts, const std::vector<VkExtensionProperties>& all);
/// After vkCreateInstance: whether the layer is in the process (logged), and if so the messenger for the instance's life
void validation_instance_made(VkInstance instance);
/// (the 600-present block) "validation: ..." summary
void validation_stats(char* out, size_t cap);

// ---- strict ----
extern bool strict_enabled;   // (HOS_DBG=strict: read once at the start)
inline bool strict_on() { return strict_enabled; }
/// A rule-2 event. reason: a fixed string, the event's kind (the known list matches it); where: what and where, as printf.
/// Cheap when strict is off: one test (the callers count the event themselves, as before).
void strict_event(const char* reason, const char* where, ...) __attribute__((format(printf, 2, 3)));
/// A program miss (not read yet, variant being translated): the warm-up starts again (before arming); an event after
void strict_program_miss(const char* where);
/// Each present (the Presentation Thread): arming
void strict_present(uint64_t present);
bool strict_armed();
/// (the 600-present block) "strict: ..." the events by reason, before and after arming, and the known list
void strict_stats(char* out, size_t cap);

/// The known list: reasons strict reports but does not stop on, each with why it is there (logged at the start of a strict
/// run). One place; tests/test_direct_strict.py reads it.
struct StrictKnown {
    const char* reason;   // (an event reason, or "validation:<message id>")
    const char* why;
};
extern const StrictKnown kStrictKnown[];
extern const size_t kStrictKnownCount;
constexpr uint64_t kStrictQuiet = 600;   // (presents after the last program miss before strict arms: 10 s at 60 fps)

/// Vulkan objects made (the st_objects counter); after arming, an event (what names it). The dump's own copies are a debug
/// aid and are not events (DebugObjects in their scope).
void object_made(int n, const char* what);
struct DebugObjects {
    DebugObjects();
    ~DebugObjects();
};
/// (M6b) What the objects made in its scope are for, in strict's reason ("Vulkan object made in steady play: <what>"): the
/// known list can name one cause and not all objects
struct ObjectsFor {
    const char* prev;
    explicit ObjectsFor(const char* what);
    ~ObjectsFor();
};

}  // namespace hsd
