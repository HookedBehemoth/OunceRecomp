/* (native-vk engine cut E2) HOS_ENG_LISTS=1: host packets for the game's load-time command lists (host/eng_lists.c).

   The game records four kinds of list at load or scene start, once, and calls them by their handle every frame
   (work/assets/native/engine-materials.md): vertex lists (vertex input state and the vertex buffer), draw lists (one
   indexed or mesh draw), per-instance shape lists (uniform and storage buffers, material textures, a CallCommands of a
   vertex list) and material lists (never called in play). host/eng_lists.c tells them apart by where the game began
   the recording (the guest return address at BeginRecording) and translates each, at its EndRecording on the recording
   thread, into one packet: the state the list leaves bound, already decoded (the last value of each binding, in the
   order the replay would set them), with the nested lists it calls folded in and named (refs).

   Lifetime: a packet lives as long as its recording's bytes. Its serial is set to 0 when the game lets the handle go
   (control memory added again, or its mark overwritten: the copies' / nvn_list.h's rule 1), and its memory goes back
   to a packet-only free list when the recording's bytes are freed (after the replay passed every submit made before
   the release). The memory is never given back to the heap, so a stale reference reads a packet (with another serial),
   never freed memory.

   In the command stream: CallCommands / CopyCommands of a handle with a packet records NOP_PKT (rx_nvn.h nvn_p_pkt)
   before what the default path records for the call (the callee's records copied in, or HOS_NVN_LIST_REF's NOP_CALL);
   `skip` is that part's size. The Vulkan replay applies the packet and skips the rest (gpu/vk/vk_replay.cpp
   pkt_apply); a stale serial, or a replay that cannot use it, decodes the records after it as before. The WebGPU
   replay ignores NOP_PKT. */
#pragma once
#include <stdint.h>
#include "rx_nvn.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NVN_PK_NONE,
    NVN_PK_VERTEX,           /* the final vertex list (BeginRecording at 0x804582ac, gsys::ModelResource(DisplayList)) */
    NVN_PK_DRAW,             /* the final draw list (0x80293b14) */
    NVN_PK_SHAPE,            /* a per-instance shape list (0x806006d8, f_806002e0: model instance x pass x variant) */
    NVN_PK_MATERIAL,         /* a material list (0x80445314, f_80445020: never called in play) */
    NVN_PK_VERTEX_SCRATCH,   /* the vertex list's measuring copy (0x8044ea20, never called) */
    NVN_PK_DRAW_SCRATCH,     /* the draw list's measuring copy (0x80293460, never called) */
    NVN_PK_KINDS
};

#define NVN_PKT_MAGIC 0x54454b504e474e45ull   /* "ENGNPKET" */

enum {
    NVN_PKF_TEX = 1,         /* a texture / sampler / image bind was recorded (the replay stamps the handles, as the op does) */
    NVN_PKF_VATTR = 2,       /* vertex attributes set (vattr / nvattr replace the bound ones) */
};

/* a uniform or storage buffer binding (the op's payload as recorded) */
typedef struct { nvn_p_bind_buf b; uint32_t ssbo, pad_; } nvn_pk_buf;
/* a texture (0), sampler (1) or image (2) handle at a stage's slot: the raw NVN handle word (E1 makes it a heap slot) */
typedef struct { uint8_t what, stage; uint16_t slot; uint32_t pad_; uint64_t h; } nvn_pk_handle;
/* vertex input: attributes (whole, when NVN_PKF_VATTR), streams' layouts and vertex buffers by slot (masks: set ones) */
typedef struct {
    uint32_t nvattr;
    uint16_t vstream_mask, vbuf_mask;
    uint32_t vattr[16];
    nvn_vs_st vstream[16];
    NVNbufferRange vbuf[16];
} nvn_pk_vertex;
/* the list's draw, last in it (op 0: none): the op's payload as recorded */
typedef struct {
    int32_t op, pad_;
    union { nvn_p_draw d; nvn_p_draw_idx di; nvn_p_draw_mesh dm; } u;
} nvn_pk_draw;
/* a list the list calls (folded in above), by its packet */
typedef struct { const struct nvn_pkt *pkt; uint64_t handle; uint32_t serial, kind; } nvn_pk_ref;

typedef struct nvn_pkt {
    uint64_t magic;
    volatile uint32_t serial;    /* (0: released; a stream's NOP_PKT is used only while it holds the serial it names) */
    uint8_t kind, flags;
    uint16_t cls;                /* (host: its size class) */
    uint64_t handle;             /* the game's handle */
    uint16_t nbuf, nhandle, nref, nops;   /* (nops: the records it was made from) */
    uint32_t bytes, raw_bytes;   /* its own size; the recording's, BEGIN .. END */
    uint64_t meta[4];            /* shape: the material-shape binding (f_806002e0's x0), its program entry, the variant k,
                                    the pass; others: 0 */
    nvn_pk_draw draw;
    const nvn_pk_vertex *vtx;    /* NULL: the list sets no vertex input */
    const nvn_pk_buf *buf;
    const nvn_pk_handle *hnd;
    const nvn_pk_ref *ref;
    struct nvn_pkt *next;        /* (host: its free list) */
} nvn_pkt;

/* the packet a NOP_PKT names, when it still holds that serial (NULL: decode the records after it) */
static inline const nvn_pkt *nvn_pkt_live(const nvn_p_pkt *r) {
    const nvn_pkt *k = r->pkt;
    if (!k || !r->serial) return 0;
    return __atomic_load_n(&k->serial, __ATOMIC_ACQUIRE) == r->serial ? k : 0;
}

#ifdef __cplusplus
}
#endif
