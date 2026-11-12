/* (native-vk) The direct back end's list calls on the host's side (gpu/direct/direct_api.h; docs/direct/m1-interface.md).
   Direct mode is RX_BACKEND=direct, read once (rx_backend.c): with it off nothing here is called and the host behaves as
   before. host/nvn_native.c hands every record to the back end through hsd_scratch / hsd_flush, the list's kind from
   hsd_classify. */
#ifndef HOOKSHOT_HOST_DIRECT_H
#define HOOKSHOT_HOST_DIRECT_H
#include <stdint.h>
#include "hos.h"
#include "../gpu/direct/direct_api.h"

typedef struct {
    void *(*list_begin)(const hsd_list_info *info);
    void (*emit)(void *list, uint32_t op, const void *payload, uint32_t size);
    void (*list_call)(void *list, void *const *callees, uint32_t n);
    void (*list_end)(void *list);
    void (*list_released)(void *list);
    void (*submit)(uint64_t queue, void *const *lists, uint32_t n);
    void (*queue_wait_fence)(uint64_t queue, uint64_t fence);
} hsd_fns;

/* rx_backend.c: the library's list calls in direct mode (loaded at the first call, from any thread), NULL otherwise */
const hsd_fns *rx_direct(void);

/* direct.c: the kind of a recording BeginRecording begins, from its caller (c: the guest's registers there); fills kind,
   load_kind, lr and fp */
void hsd_classify(Ctx *c, hsd_list_info *info);
/* direct.c: room for a record of size bytes for the command buffer whose open list is *list (NULL: none open), on this
   thread; the record handed out before it on this thread goes to the back end first. *pending is set while the record
   waits on this thread for its flush (an EndRecording on another thread finds it set); a record whose command buffer's
   list changed before its flush is dropped, counted */
uint8_t *hsd_scratch(void *volatile *list, volatile int32_t *pending, uint32_t size);
/* direct.c: this thread's record that waits, to the back end now (EndRecording, CallCommands, CopyCommands, a submit) */
void hsd_flush(void);
#endif
