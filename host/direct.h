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
    void (*present_queue)(uint64_t queue);
    /* (A-rt) prepared assets: host/direct_assets.c's loader hooks and game patches */
    int (*assets_open)(const char *dir);
    int (*asset_load)(const hsd_asset_key *k);
    void (*asset_rebind)(const void *tex);
    void (*asset_pool)(uint64_t lo, uint64_t hi);
    void (*texels)(uint32_t source, const void *tex, unsigned level, unsigned layer0, const void *region, const void *src,
                   uint64_t row, uint64_t img);
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

/* direct_assets.c (A-rt): the host's asset patches are on (direct mode, and not HOS_DBG=noassets) */
int direct_assets_on(void);
/* nn_fs.c: the overlay root after HOS_FS_OVERLAY's in direct mode (the converted terrain tiles), NULL none; a file opened
   from it (counted) */
const char *direct_assets_overlay(void);
void direct_assets_overlay_opened(const char *path);
/* nvn_native.c nvnTextureInitialize: packaged texture data the back end never reads, its copy into the storage skipped
   (1) in direct mode */
int direct_assets_skip_packaged(uint64_t bytes);
#endif
