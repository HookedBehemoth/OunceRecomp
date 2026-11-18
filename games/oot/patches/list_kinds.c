/* The kinds of the game's command lists for the direct back end (runtime/render_lists.c hsd_name_next_list): the function that
   begins a recording names it, by an At row just before its BeginRecording call.
   1-6: the game's load-time lists (the list's load kind): 1 vertex list (f_80457e60, at its call of agl_cmdbuf_begin
   f_804581b0), 2 draw list (f_80293890 <- f_804566e0), 3 per-instance shape list (f_806002e0 <- f_8009bb80), 4 material
   list (f_80445020 <- f_80443000), 5 the vertex list's measuring copy (f_8044e8e0 <- f_8044e020), 6 the draw list's
   measuring copy (f_80293190 <- f_8044cb30); 16 JOB (f_80084de0, the render-job recorder), 17 LAYER (f_8006cd60, an
   agl::lyr layer's list; f_8007ad40; f_8007b520, a layer job's too: 5 a frame, never called, M1 run), 18 GATE
   (f_802eb390, the gate primary), 19 COMPUTE (f_806650d0, an async-compute list), 20 HEAD (f_80665980, a queue's head,
   command-lists.md), 21 IMMEDIATE (a one-shot list the game submits itself and waits for: agl_cmdbuf_begin's other
   callers, the save picture's copy f_80bd22b0, the immediate context's begin f_834bb830 and f_83a6d390's second
   command buffer; f_812bd720, the begin of the effect noise upload: vfx_curl_noise_staging submits and finishes it).
   f_804581b0 is shared, so its callers name the kind at their call, not a row inside it. */
#include "patches.h"

PATCH_AT(0x80458044, word(0x9400005b), before, none, direct_list_kind, as(k(1), kind)) {
    if (kind >= 1 && kind <= 6) {
        hsd_name_next_list(HSD_LIST_LOAD, kind);
        return;
    }
    switch (kind) {
    case 16: hsd_name_next_list(HSD_LIST_JOB, 0); break;
    case 17: hsd_name_next_list(HSD_LIST_LAYER, 0); break;
    case 18: hsd_name_next_list(HSD_LIST_GATE, 0); break;
    case 19: hsd_name_next_list(HSD_LIST_COMPUTE, 0); break;
    case 20: hsd_name_next_list(HSD_LIST_HEAD, 0); break;
    case 21: hsd_name_next_list(HSD_LIST_IMMEDIATE, 0); break;
    }
}
PATCH_AT_SITE(0x80293b0c, word(0xf9400108), after, none, direct_list_kind, k(2));
PATCH_AT_SITE(0x806006d0, word(0xf9400108), after, none, direct_list_kind, k(3));
PATCH_AT_SITE(0x8044530c, word(0xf9400108), after, none, direct_list_kind, k(4));
PATCH_AT_SITE(0x8044ea18, word(0xf9400108), after, none, direct_list_kind, k(5));
PATCH_AT_SITE(0x80293458, word(0xf9400108), after, none, direct_list_kind, k(6));
PATCH_AT_SITE(0x80085334, word(0xf9400108), after, none, direct_list_kind, k(16));
PATCH_AT_SITE(0x8006d250, word(0xf9400108), after, none, direct_list_kind, k(17));
PATCH_AT_SITE(0x8007b0c0, word(0xf9400108), after, none, direct_list_kind, k(17));
PATCH_AT_SITE(0x8007b9f8, word(0xf9400108), after, none, direct_list_kind, k(17));
PATCH_AT_SITE(0x802eb3f4, word(0xf94002e8), after, none, direct_list_kind, k(18));
PATCH_AT_SITE(0x80665578, word(0xf9400108), after, none, direct_list_kind, k(19));
PATCH_AT_SITE(0x80665ff4, word(0xf9400108), after, none, direct_list_kind, k(20));
PATCH_AT_SITE(0x80bd2490, word(0x97e21748), before, none, direct_list_kind, k(21));
PATCH_AT_SITE(0x834bb874, word(0x973e724f), before, none, direct_list_kind, k(21));
PATCH_AT_SITE(0x83a6d3f4, word(0x9727ab6f), before, none, direct_list_kind, k(21));
PATCH_AT_SITE(0x812bd764, word(0xf9400108), after, none, direct_list_kind, k(21));
