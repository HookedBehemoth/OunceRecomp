// (M6c) GPU breadcrumbs for a lost device (HOS_DBG=marks, a debug aid: docs/direct-backend-plan.md, Checks, "Hangs").
//
// Every draw and dispatch the back end records is bracketed by two VK_AMD_buffer_marker writes into host memory: one at the
// top of the pipe (the command processor reached it) and one at the bottom (it and everything before it finished). Each
// command gets an entry of a ring whose CPU side says what it was (program, list, present, counts). When a submit reports
// the device lost, marks_report logs the entries that were started and not finished, in recording order: the command the
// GPU hung in is among the first of them in its command buffer (later ones may have been reached by the command processor
// ahead of the pipe). Off by default; when off, one load and a branch per command.
#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace hsd {

extern bool marks_enabled;   // (HOS_DBG=marks and the device has VK_AMD_buffer_marker)
inline bool marks_on() { return marks_enabled; }
/// Before vkCreateDevice: VK_AMD_buffer_marker enabled when HOS_DBG=marks asks and the device has it
void marks_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want);
/// After vkCreateDevice: the marker buffer
void marks_init();
/// Before a command: its entry (returned) and its top-of-pipe marker. what: a fixed string
uint32_t mark_before(VkCommandBuffer cb, const char* what, uint64_t key, uint32_t kind, uint64_t lr, uint32_t a, uint32_t b);
/// After it: the bottom-of-pipe marker of the entry
void mark_after(VkCommandBuffer cb, uint32_t entry);
/// At a lost device: the commands started and not finished (logged; the log flushed by the caller)
void marks_report(const char* why);

}  // namespace hsd
