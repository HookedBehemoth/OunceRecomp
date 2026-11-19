/* The direct back end's list calls (ounce_renderer.so): what runtime/nvn_native.c hands it as the game
   records, besides the object layer, sync and present of renderer/api/vk_api.h's table (version 14), which it answers too.
   C, included by the host; the host finds these by name in the library.

   A list is begun, filled and ended on one recording thread at a time (game workers, concurrently with other lists),
   released from any thread, and submitted from the guest's submitting threads (the Presentation Thread, Main). */
#ifndef OUNCE_DIRECT_API_H
#define OUNCE_DIRECT_API_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* (Windows: the library's exports are marked where they are first declared, here, when it is built) */
#if defined(_WIN32) && defined(MKWVK_BUILDING)
#define GPU_LIST_EXPORT __declspec(dllexport)
#else
#define GPU_LIST_EXPORT
#endif

enum { GPU_LIST_PACKET, GPU_LIST_LOAD, GPU_LIST_JOB, GPU_LIST_LAYER, GPU_LIST_GATE, GPU_LIST_COMPUTE, GPU_LIST_HEAD,
       GPU_LIST_IMMEDIATE, GPU_LIST_KINDS };   /* IMMEDIATE: a one-shot list the game submits itself and waits for */
typedef struct gpu_list_info {
    uint32_t kind;        /* GPU_LIST_*; PACKET = not classified (counted, its lr logged once) */
    uint32_t load_kind;   /* GPU_LIST_LOAD: 1 vertex, 2 draw, 3 shape, 4 material, 5 and 6 the measuring copies of 1 and 2 (the game's list_kinds.c) */
    uint64_t lr, fp;      /* the guest return address and frame pointer of BeginRecording */
    uint64_t cmd;         /* the guest NVNcommandBuffer object */
    uint64_t pass_key;    /* GPU_LIST_JOB: the recorder's pass key pointer (x3 of f_80084de0: docs/direct/jobs.md) */
    uint64_t view;        /* GPU_LIST_JOB: the view record (x25 at BeginRecording), for logs only */
} gpu_list_info;

/* op and payload are renderer/api/rx_nvn.h's records (NOP_*, the payload after the 8-byte header) */
GPU_LIST_EXPORT void *gpu_list_begin(const gpu_list_info *info);                         /* recording thread */
GPU_LIST_EXPORT void  gpu_emit(void *list, uint32_t op, const void *payload, uint32_t size);   /* recording thread; payload valid during the call */
GPU_LIST_EXPORT void  gpu_list_call(void *list, void *const *callees, uint32_t n);       /* CallCommands / CopyCommands, in order */
GPU_LIST_EXPORT void  gpu_list_end(void *list);                                           /* complete; valid until released */
GPU_LIST_EXPORT void  gpu_list_released(void *list);                                      /* the handle is gone (any thread) */
GPU_LIST_EXPORT void  gpu_submit(uint64_t queue, void *const *lists, uint32_t n);         /* the guest's submit, in handle order */
GPU_LIST_EXPORT void  gpu_queue_wait_fence(uint64_t queue, uint64_t fence);               /* nvnQueueWaitSync, instead of fw_record's side record */
GPU_LIST_EXPORT void  gpu_present_queue(uint64_t queue);   /* the queue of the present that follows on this thread (nvnQueuePresentTexture) */

/* Prepared assets. The host's loader hooks name each texture load's BNTX texture;
   the back end makes that texture's image from the prepared store (ounce tool textures) when it is initialized,
   filled on the initializing thread. Any thread (the game's loaders). */
typedef struct gpu_asset_key {
    const void *tex;               /* the NVNtexture the load initializes next (host pointer of the guest object) */
    const char *name;              /* the BRTI's name (UTF-8, name_len bytes); NULL: the data is not from a BNTX */
    uint32_t name_len;
    uint32_t format, width, height, layers, mips, image_size;   /* BRTI +0x1c, +0x24, +0x28, +0x30, +0x16, +0x50 */
    uint32_t index, data_offset;   /* the texture's index in its BNTX; mip 0's offset in the BNTX (0: not known, data given) */
    uint32_t path;                 /* GPU_ASSET_LOAD / _INPLACE (logs) */
    const void *data;              /* data_offset 0: mip 0's bytes in the decompressed file (host pointer): the record whose
                                      data offset back from it is its BNTX's header is the one */
    const void *same_as;           /* GPU_ASSET_SAME: the NVNtexture whose load's content this texture has (M6f: an
                                      alias's target, docs/direct/texture-streaming.md) */
} gpu_asset_key;
enum { GPU_ASSET_LOAD = 1, GPU_ASSET_INPLACE = 2, GPU_ASSET_REBIND = 3, GPU_ASSET_SAME = 4 };
/* (REBIND: a record found replaces the link; none: the link stays, armed as by gpu_asset_rebind. SAME : a load with no
   data whose texels are another texture's, same_as: it takes that texture's link (its record or its plain image) when the
   descriptors agree; the host's game patch names it (the alias's target, read from the alias's chain) */
GPU_LIST_EXPORT int  gpu_assets_open(const char *dir);              /* the store's folder (index.bin, pack_NNN.bin): its textures, 0 none */
GPU_LIST_EXPORT int  gpu_asset_load(const gpu_asset_key *k);        /* 1: a prepared record; 0: the texture gets the placeholder (named) */
GPU_LIST_EXPORT void gpu_asset_rebind(const void *tex);             /* tex is initialized again over new storage (the same record) */
GPU_LIST_EXPORT void gpu_asset_pool(uint64_t lo, uint64_t hi);      /* the asset heap's pool (pool 127): its textures need a load */
GPU_LIST_EXPORT void gpu_texture_data_pool(uint64_t lo, uint64_t hi);   /* a texture data pool, named at its creation: layer 0 leaves it unbound */
/* texels from the CPU that no NVN call carries (game patches): an NVNcopyRegion of tex's level and first layer from
   src (row bytes, layer bytes; 0: packed); src NULL: none to give (the placeholder content, counted) */
enum { GPU_TEXELS_TERRAIN = 1, GPU_TEXELS_GLYPH = 2, GPU_TEXELS_FILE = 3, GPU_TEXELS_TERRAIN_KEPT = 4 };
/* (FILE: a prepared file's linear texels, M4e. TERRAIN_KEPT : a tile's bytes the storage still holds, for a texture
   initialized there again: taken only into an image nothing has written) */
GPU_LIST_EXPORT void gpu_texels(uint32_t source, const void *tex, unsigned level, unsigned layer0, const void *region, const void *src,
                uint64_t row, uint64_t img);

/* The format table by pass key. The job recorder's entry asks whether a key's formats
   are known: 1, the job records one rendering secondary with them; 0, the recorder takes the game's own early return and the
   layer list draws the pass through the game's inline path, which gpu_pass_inline names (the list's targets set then give the
   key its formats). A layer list's call of a job notes them too */
GPU_LIST_EXPORT int  gpu_pass_known(uint64_t key);
GPU_LIST_EXPORT void gpu_pass_inline(void *list, uint64_t key);

/* The transient pool (docs/direct/transient-pool.md; games/oot/patches/rt_slots.c). The game's transient render targets (agl's
   DynamicTextureAllocator) live in one range of storage addresses that the host names once, before the first texture is
   initialized in it: the back end allocates that many bytes of device memory then, and never again for these targets. A
   texture initialized in the range gets an image bound in that memory at its storage offset; images whose ranges overlap
   share memory as the targets do on the console, and the one claimed last (initialized, or given out by the allocator)
   owns the range.
     gpu_transient_pool   the range [lo, hi) of storage addresses; 1 made
     gpu_image_need       the bytes (and alignment) an image of a texture of this nvn_texdesc needs in the pool: the host
                          sizes the game's blocks and its slots by it (0: no image of it)
     gpu_transient_slot   the slot at storage address gpu has cap bytes, told before the texture is initialized there: an
                          image that needs more is not bound (a strict event, the placeholder)
     gpu_target_given     the game's allocator gave the target out (tex: its nvn_tex): its image claims its range if
                          another has owned memory of it since
     gpu_host_event       an event of the host's for strict mode (reason: a fixed string) */
GPU_LIST_EXPORT int gpu_transient_pool(uint64_t lo, uint64_t hi);
GPU_LIST_EXPORT uint64_t gpu_image_need(const void *desc, uint64_t *align);
GPU_LIST_EXPORT void gpu_transient_slot(uint64_t gpu, uint64_t cap);
GPU_LIST_EXPORT void gpu_target_given(const void *tex);
GPU_LIST_EXPORT void gpu_host_event(const char *reason, const char *what);
int64_t gpu_vram_free(void);   /* free VRAM in bytes as the driver sees it (budget minus usage of the device-local heap,
                                  plus images already waiting to be destroyed), sampled once a present; -1 not yet */
GPU_LIST_EXPORT void gpu_texture_cached(const void *tex, int cached);   /* agl released tex to its LRU cache (1: still registered,
                                                          its texels kept) or took it back (0); counted, nothing evicted */

/* The host's upscale (runtime/upscale.h): a spatial upscale recorded into the list where the game's own upscaler would run,
   outside a rendering. src_id names a 2D texture of the texture pool (sampled at src_w x src_h, its top left), dst_id a 2D
   image of the same pool (written at dst_w x dst_h). Ordered after the work before it (the colour's rendering) and before
   the work after it, as the list's dispatches are. One filter a call; a filter the back end does not have is counted and
   records nothing (strict: an event) */
enum { GPU_UPSCALE_BILINEAR = 1, GPU_UPSCALE_CATMULL_ROM = 2 };
typedef struct gpu_upscale_args {
    uint32_t filter;                   /* GPU_UPSCALE_* */
    uint32_t src_id, src_w, src_h;
    uint32_t dst_id, dst_w, dst_h;
    float sharpness;                   /* (the game's parameter: no filter of these reads it) */
} gpu_upscale_args;
GPU_LIST_EXPORT void gpu_upscale(void *list, const gpu_upscale_args *a);

/* Shots of the presented frame: every `every` presents a 1280x720 PNG of what is shown, <dir>/vk_<present>.png (the
   host's shots / shot_every, the frozen back end's names). Called once, before the renderer starts */
GPU_LIST_EXPORT void gpu_shots(const char *dir, int every);

#ifdef __cplusplus
}
#endif
#endif
