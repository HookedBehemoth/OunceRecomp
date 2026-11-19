// A list's Vulkan commands at M2a: passes, clears, barriers, copies (vk_pass.cpp has the rules).
#pragma once

#include "vk_lists.h"

namespace hsd {

/// One record of the list (the recording thread)
void pass_emit(List* l, uint32_t op, const void* data, uint32_t size);
/// A callee joins the list at a CallCommands (ended; before it is noted in the list's ops)
void pass_call(List* l, const List* callee);
/// An event's signal or wait: the list's command buffer ends here (its rendering too; the targets stay noted)
void pass_cut(List* l);
/// The list's end: its pass ended, its command buffer too
void pass_list_end(List* l);
/// The list's command buffer outside any rendering: a begun rendering of its own ended (its targets stay noted)
VkCommandBuffer pass_outside(List* l);
/// The list's command buffer for a host dispatch (vk_upscale.cpp), outside its rendering: before and the waiting
/// dependencies recorded first, a dispatch's own left waiting
VkCommandBuffer pass_dispatch(List* l, const Dep& before);
/// Its rendering begun again (loading) after pass_outside, with the draw's query and condition
void pass_resume(List* l);
void pass_stats(char* out, size_t cap, double frames);
/// Where a list is, for strict's events: its kind and return address, its pass (SET_RT number, size, first target)
void pass_where(const List* l, char* out, size_t cap);
/// A job begun: its pass key's formats known, it is one rendering secondary of its thread's slot (l->secondary)
void pass_job_begin(List* l);
/// The jobs' part of the 600-present line: recorded, executed, the format table, the keys drawn inline
void jobs_stats(char* out, size_t cap, double frames);

}  // namespace hsd
