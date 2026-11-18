/* The kinds of DKB's command lists for the direct back end (runtime/render_lists.c gpu_name_next_list): the function
   that begins a recording names it, by an At row just before its BeginRecording call (BeginRecording's seven users:
   render_job_record, the gate primary, layer_list_record_b, shape_list_build_instance, gfx_immediate_submit_rebegin,
   and the shared helpers agl_cmdbuf_begin f_801e7380 and agl_cmdbuf_begin_with_memory f_802c6300, whose callers name the
   kind at their call: a 130 s run logged every caller's return address).
   1-7: the load-time lists (the list's load kind; the pairs by their equal counts, which of a pair is the measuring copy
   a guess): 1 vertex list (shape_list_vertex_driver), 5 its measuring copy (f_802c6be0), 2 draw list (f_802bf7cc),
   6 its measuring copy (shape_list_build_scratch), 3 per-instance shape list (shape_list_build_instance), 4 material
   list (shape_list_build_material), 7 the other load-time lists (model_shared_resource_list, f_8019ec00, f_8019fb7c,
   f_803f9cec, f_803fa2c8 and f_801e67bc's three, on al::InitializeThread); 16 JOB (render_job_record), 17 LAYER
   (layer_list_record_b), 18 GATE (f_80224008, the gate primary), 21 IMMEDIATE (a one-shot list the game submits itself:
   f_8030ef44, two a frame; gfx_immediate_begin, the movie thread's and texdata_copy_to_buffer's; and
   gfx_immediate_submit_rebegin's begin after its submit). */
#include "patches.h"

PATCH_AT(0x802bfdb4, word(0x97fc9d73), before, none, list_kind, as(k(1), kind)) {
    if (kind >= 1 && kind <= 7) {
        gpu_name_next_list(GPU_LIST_LOAD, kind);
        return;
    }
    switch (kind) {
    case 16: gpu_name_next_list(GPU_LIST_JOB, 0); break;
    case 17: gpu_name_next_list(GPU_LIST_LAYER, 0); break;
    case 18: gpu_name_next_list(GPU_LIST_GATE, 0); break;
    case 21: gpu_name_next_list(GPU_LIST_IMMEDIATE, 0); break;
    }
}
PATCH_AT_SITE(0x802c6fd8, word(0x97fffcca), before, none, list_kind, k(5));
PATCH_AT_SITE(0x802bf998, word(0x97fc9e7a), before, none, list_kind, k(2));
PATCH_AT_SITE(0x802c1c7c, word(0x940011a1), before, none, list_kind, k(6));
PATCH_AT_SITE(0x800b0d80, word(0xf9400108), after, none, list_kind, k(3));
PATCH_AT_SITE(0x802c443c, word(0x940007b1), before, none, list_kind, k(4));
PATCH_AT_SITE(0x807343e8, word(0x97ee47c6), before, none, list_kind, k(7));
PATCH_AT_SITE(0x8019f694, word(0x94049b1b), before, none, list_kind, k(7));
PATCH_AT_SITE(0x8019fc9c, word(0x94011db9), before, none, list_kind, k(7));
PATCH_AT_SITE(0x803f9e80, word(0x97fb3120), before, none, list_kind, k(7));
PATCH_AT_SITE(0x803fa464, word(0x97fb2fa7), before, none, list_kind, k(7));
PATCH_AT_SITE(0x801e6900, word(0x940002a0), before, none, list_kind, k(7));
PATCH_AT_SITE(0x801e6ca8, word(0x940001b6), before, none, list_kind, k(7));
PATCH_AT_SITE(0x801e6e9c, word(0x94000139), before, none, list_kind, k(7));
PATCH_AT_SITE(0x8008162c, word(0xf9400108), after, none, list_kind, k(16));
PATCH_AT_SITE(0x803406ac, word(0xf9400108), after, none, list_kind, k(17));
PATCH_AT_SITE(0x80224068, word(0xf94002e8), after, none, list_kind, k(18));
PATCH_AT_SITE(0x8030f0a0, word(0x97fb60b8), before, none, list_kind, k(21));
PATCH_AT_SITE(0x815b28c0, word(0x97b0d2b0), before, none, list_kind, k(21));
PATCH_AT_SITE(0x815b3368, word(0xf9400108), after, none, list_kind, k(21));
