// A list's Vulkan commands at M2a: passes, clears, barriers, copies (direct_pass.cpp has the rules).
#pragma once

#include "direct_lists.h"

namespace hsd {

/// One record of the list (the recording thread)
void pass_emit(List* l, uint32_t op, const void* data, uint32_t size);
/// A callee joins the list at a CallCommands (ended; before it is noted in the list's ops)
void pass_call(List* l, const List* callee);
/// The list's end: its pass ended, its command buffer too
void pass_list_end(List* l);
void pass_stats(char* out, size_t cap, double frames);

}  // namespace hsd
