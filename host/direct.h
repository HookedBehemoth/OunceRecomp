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
    void (*texture_data_pool)(uint64_t lo, uint64_t hi);   /* (M4f) a pool of agl::TextureDataMgr, at its creation */
    void (*texels)(uint32_t source, const void *tex, unsigned level, unsigned layer0, const void *region, const void *src,
                   uint64_t row, uint64_t img);
    /* (M3a) the format table by pass key: the job recorder's question, the game's inline path */
    int (*pass_known)(uint64_t key);
    void (*pass_inline)(void *list, uint64_t key);
    /* (M4d) shots of the presented frame */
    void (*shots)(const char *dir, int every);
    void (*texture_freed)(const void *tex);   /* (M6f) the game freed the texture's storage (rt_slots.c) */
    void (*texture_held)(const void *tex);    /* (M6f) ... and took the texture again */
    void (*texture_cached)(const void *tex, int cached);   /* (M6f) a texture released to agl's LRU cache (1), taken back (0) */
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
/* direct_sky_lut.c (M4e): a file served from the prepared overlay (direct_assets_overlay_opened) */
void direct_sky_lut_opened(const char *path);
/* nvn_native.c nvnTextureInitialize: packaged texture data the back end never reads, its copy into the storage skipped
   (1) in direct mode */
int direct_assets_skip_packaged(uint64_t bytes);
/* direct_tex_dedup.c (M4g, M6f, game patch): a load with no data, td its agl::TextureData: when it is a UnifyTex alias, its
   target's NVN texture as an HSD_ASSET_SAME key (1); otherwise 0 */
int direct_tex_alias(uint64_t td, hsd_asset_key *k);
/* direct_texdata.c (M6f): agl::TextureDataMgr's texture events */
void direct_texdata_created(uint64_t td, int alias);
const void *direct_texdata_moving(void);   /* (this thread's texture being moved by compaction, else NULL) */
void direct_texdata_line(char *out, size_t cap);
int direct_assets_on(void);
/* (M4f) nvnMemoryPoolInitialize (lr its return address): agl::TextureDataMgr's pool is named to the back end */
void direct_assets_pool_made(uint64_t lr, uint64_t gpu, uint64_t size);
/* nvn_native.c nvnTextureInitialize, after the back end made its image: terrain tiles given for its storage before it */
extern volatile long direct_assets_tiles_pending;
extern volatile long direct_assets_tilelog;   /* (M6f: HOS_DBG=tilelog, a diagnosis) */
void direct_assets_texture_init(const void *nvn_texture);   /* (an nvn_tex) */
/* nvn_native.c (M3a): the back end's open list of a guest command buffer (this thread's record handed over first) */
void *direct_cmd_list(uint64_t cmd);
/* direct_passes.c (M3a): the job recorder's entry in direct mode (eng_jobs.c): 1 = take the game's early return */
int direct_passes_recorder(Ctx *c);
/* direct_patches.c (M3a): the game patches' counters, every 600 presents (nvn_native.c's present, direct mode) */
void direct_patches_frame(void);
/* direct_texwatch.c (M4d): HOS_DBG=texwatch, CPU writes into texture storage (diagnosis): nvn_native.c's texture
   initialization and release, nn_fs.c's reads into guest memory, the present's line */
int direct_texwatch_on(void);
void direct_texwatch_created(const void *nvn_texture, uint64_t init_lr);
void direct_texwatch_released(const void *nvn_texture);
void direct_texwatch_kernel_write(uint64_t guest, uint64_t n, const char *what);
void direct_texwatch_frame(void);
#endif
