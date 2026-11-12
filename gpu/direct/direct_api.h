/* The direct back end's list calls (hookshot_direct.so, RX_BACKEND=direct): what host/nvn_native.c hands it as the game
   records, besides the object layer, sync and present of gpu/vk/vk_api.h's table (version 14), which it answers too.
   Fixed by docs/direct/m1-interface.md. C, included by the host; the host finds these by name in the library.

   A list is begun, filled and ended on one recording thread at a time (game workers, concurrently with other lists),
   released from any thread, and submitted from the guest's submitting threads (the Presentation Thread, Main). */
#ifndef HOOKSHOT_DIRECT_API_H
#define HOOKSHOT_DIRECT_API_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HSD_LIST_PACKET, HSD_LIST_LOAD, HSD_LIST_JOB, HSD_LIST_LAYER, HSD_LIST_GATE, HSD_LIST_COMPUTE, HSD_LIST_HEAD,
       HSD_LIST_KINDS };
typedef struct hsd_list_info {
    uint32_t kind;        /* HSD_LIST_*; PACKET = not classified (counted, its lr logged once) */
    uint32_t load_kind;   /* HSD_LIST_LOAD: eng_lists_kind's value */
    uint64_t lr, fp;      /* the guest return address and frame pointer of BeginRecording */
    uint64_t cmd;         /* the guest NVNcommandBuffer object */
    uint64_t pass_key;    /* HSD_LIST_JOB: the recorder's pass key pointer (x3 of f_80084de0: m0-job-formats.md) */
    uint64_t view;        /* HSD_LIST_JOB: the view record (x25 at BeginRecording), for logs only */
} hsd_list_info;

/* op and payload are gpu/render/rx_nvn.h's records (NOP_*, the payload after the 8-byte header) */
void *hsd_list_begin(const hsd_list_info *info);                         /* recording thread */
void  hsd_emit(void *list, uint32_t op, const void *payload, uint32_t size);   /* recording thread; payload valid during the call */
void  hsd_list_call(void *list, void *const *callees, uint32_t n);       /* CallCommands / CopyCommands, in order */
void  hsd_list_end(void *list);                                           /* complete; valid until released */
void  hsd_list_released(void *list);                                      /* the handle is gone (any thread) */
void  hsd_submit(uint64_t queue, void *const *lists, uint32_t n);         /* the guest's submit, in handle order */
void  hsd_queue_wait_fence(uint64_t queue, uint64_t fence);               /* nvnQueueWaitSync, instead of fw_record's side record */

#ifdef __cplusplus
}
#endif
#endif
