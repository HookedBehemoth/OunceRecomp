// A list's Vulkan commands at M2a: passes, clears, barriers, copies (direct_pass.cpp has the rules).
#pragma once

#include "direct_lists.h"

namespace hsd {

/// One record of the list (the recording thread)
void pass_emit(List* l, uint32_t op, const void* data, uint32_t size);
/// A callee joins the list at a CallCommands (ended; before it is noted in the list's ops)
void pass_call(List* l, const List* callee);
/// (M4a) An event's signal or wait: the list's command buffer ends here (its rendering too; the targets stay noted)
void pass_cut(List* l);
/// The list's end: its pass ended, its command buffer too
void pass_list_end(List* l);
void pass_stats(char* out, size_t cap, double frames);
/// (M3a) A job begun: its pass key's formats known, it is one rendering secondary of its thread's slot (l->secondary)
void pass_job_begin(List* l);
/// (M3a) The jobs' part of the 600-present line: recorded, executed, the format table, the keys drawn inline
void jobs_stats(char* out, size_t cap, double frames);

}  // namespace hsd
