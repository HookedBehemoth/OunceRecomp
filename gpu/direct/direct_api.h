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
void  hsd_present_queue(uint64_t queue);   /* (M2a) the queue of the present that follows on this thread (nvnQueuePresentTexture) */

/* (A-rt) Prepared assets (docs/direct/a-rt-report.md). The host's loader hooks name each texture load's BNTX texture;
   the back end makes that texture's image from the prepared store (nx2/prepare_textures.py) when it is initialized,
   filled on the initializing thread. Any thread (the game's loaders). */
typedef struct hsd_asset_key {
    const void *tex;               /* the NVNtexture the load initializes next (host pointer of the guest object) */
    const char *name;              /* the BRTI's name (UTF-8, name_len bytes); NULL: the data is not from a BNTX */
    uint32_t name_len;
    uint32_t format, width, height, layers, mips, image_size;   /* BRTI +0x1c, +0x24, +0x28, +0x30, +0x16, +0x50 */
    uint32_t index, data_offset;   /* the texture's index in its BNTX; mip 0's offset in the BNTX (0: not known, data given) */
    uint32_t path;                 /* HSD_ASSET_LOAD / _INPLACE (logs) */
    const void *data;              /* data_offset 0: mip 0's bytes in the decompressed file (host pointer): the record whose
                                      data offset back from it is its BNTX's header is the one */
} hsd_asset_key;
enum { HSD_ASSET_LOAD = 1, HSD_ASSET_INPLACE = 2, HSD_ASSET_REBIND = 3 };   /* (REBIND: a record found replaces the link; none:
                                                                              the link stays, armed as by hsd_asset_rebind) */
int  hsd_assets_open(const char *dir);              /* the store's folder (index.bin, pack_NNN.bin): its textures, 0 none */
int  hsd_asset_load(const hsd_asset_key *k);        /* 1: a prepared record; 0: the texture gets the placeholder (named) */
void hsd_asset_rebind(const void *tex);             /* tex is initialized again over new storage (the same record) */
void hsd_asset_pool(uint64_t lo, uint64_t hi);      /* the asset heap's pool (pool 127): its textures need a load */
/* texels from the CPU that no NVN call carries (game patches): an NVNcopyRegion of tex's level and first layer from
   src (row bytes, layer bytes; 0: packed); src NULL: none to give (the placeholder content, counted) */
enum { HSD_TEXELS_TERRAIN = 1, HSD_TEXELS_GLYPH = 2 };
void hsd_texels(uint32_t source, const void *tex, unsigned level, unsigned layer0, const void *region, const void *src,
                uint64_t row, uint64_t img);

/* (M3a) The format table by pass key (docs/direct/m3-interface.md, M3a). The job recorder's entry asks whether a key's formats
   are known: 1, the job records one rendering secondary with them; 0, the recorder takes the game's own early return and the
   layer list draws the pass through the game's inline path, which hsd_pass_inline names (the list's targets set then give the
   key its formats). A layer list's call of a job notes them too */
int  hsd_pass_known(uint64_t key);
void hsd_pass_inline(void *list, uint64_t key);

/* (M4d) Shots of the presented frame: every `every` presents a 1280x720 PNG of what is shown, <dir>/vk_<present>.png (the
   host's RX_VK_SHOTS / RX_VK_SHOT_EVERY, the frozen back end's names). Called once, before the renderer starts */
void hsd_shots(const char *dir, int every);

#ifdef __cplusplus
}
#endif
#endif
