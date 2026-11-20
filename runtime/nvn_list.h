/* Command lists by reference (runtime/nvn_list.c): the recordings runtime/nvn_native.c makes, their handles and
   lifetimes. The records themselves go to the back end as they are made (runtime/render_lists.c); a recording here holds
   the accounting and the back end's list (user).

   - A recording is written once, into an allocation of its own (size classes, cached per thread, no lock while the
     thread's cache has one), with its header just before its BEGIN record.
   - Its handle is an id in a table that needs no lock to read (chunks never freed), with a generation: a handle the
     game passes back is used only when its slot still holds it, never by what memory at some address holds.
   - Recordings are filed by their control memory position in buckets of 64 KiB blocks, each run of 256 buckets under
     a spinlock of its own; AddControlMemory releases the range's ones and touches only those buckets.

   Lifetime rules:
   1. A recording's handle lives until the game adds its control memory position again (AddControlMemory over it), or
      overwrites the mark written there (the sweep, every 25,000 recordings): then the handle stops working at once.
   2. Its bytes live on until the back end is past every submit handed in before that release (after: the pushed count)
      and no recording that calls it is alive. */
#ifndef OUNCE_NVN_LIST_H
#define OUNCE_NVN_LIST_H
#include <stddef.h>
#include <stdint.h>

#define NVN_LIST_MAX_DEPTH 7   /* calls below a recording, at most (the replay's stack has 8 places) */
#define NVN_LIST_SLACK 32      /* bytes always left after a reserve (the default path's OP_RESERVE is 24) */
#define NVN_LREC_MAGIC 0x4345524c4e564e48ull   /* "HNVNLREC" */

enum {
    NVN_LREC_OWN_DEF = 1,   /* it has deferred binds of its own (read from the game's arrays at each submit) */
    NVN_LREC_SUB_DEF = 2,   /* one of its callees, or theirs, has */
    NVN_LREC_PRIVATE = 4,   /* a private copy a submit made (no handle, not filed) */
    NVN_LREC_RELEASED = 8,  /* its handle is gone (control memory added again, or its mark overwritten) */
    NVN_LREC_MARKED = 16,   /* a mark was written at its control memory position (the sweep looks at it) */
};

typedef struct nvn_lrec nvn_lrec;
struct nvn_lrec {   /* a recording: this header, then BEGIN ... END */
    uint64_t magic;
    uint64_t bytes;          /* BEGIN .. END (0 while it is being recorded) */
    uint64_t cap;            /* bytes from BEGIN to the allocation's end */
    uint64_t flat;           /* BEGIN .. END with every callee copied in (what the default path's copy holds) */
    uint64_t ctl;            /* its control memory position (~0: none, never released by a range) */
    nvn_lrec *next;          /* (the control index's chain; the dead list's while it waits) */
    int32_t refs;            /* its handle until released and replayed (1), and one for each caller alive */
    uint32_t id;             /* its handle's slot (0: none) */
    uint32_t cls;            /* its allocation's size class */
    uint32_t flags;          /* NVN_LREC_* */
    uint32_t first_call;     /* offset of its first NOP_CALL from BEGIN (0: none); the next one in each call */
    uint32_t first_def, last_def;   /* the first and the last deferred bind (offsets from BEGIN; 0: none) */
    uint32_t depth;          /* calls below it, deepest */
    uint64_t after;          /* when it was released: the submit the replay must be past */
    void *user;              /* (HOS_ENG_LISTS, runtime/eng_lists.c) its packet: told when the handle goes and when the bytes go */
};

/* a command buffer's recording place (in runtime/nvn_native.c's cmd_rec): seg the open recording's BEGIN, cur where the
   next record goes, end its allocation's end; rec its header (NULL: none open) */
typedef struct nvn_lw {
    uint8_t *seg, *cur, *end;
    nvn_lrec *rec;
    uint64_t key;            /* the command buffer's guest address: its last recording's size, the next one's guess */
    uint32_t last_call, first_call, first_def, last_def;
    uint32_t flags, depth;
    uint64_t flat_extra;     /* flat - bytes so far: what the callees add, less their call records */
    void *user;              /* (HOS_ENG_LISTS) the recording's user pointer, set by nvn_list_end before its handle is published */
} nvn_lw;

typedef struct {
    uint64_t made, live, live_bytes, calls, released, freed;
    uint64_t grown, shrunk, swept, copied;   /* (copied: bytes moved by growing and shrinking) */
} nvn_list_stats;

/* before any other call. fatal does not return */
void nvn_list_init(void (*log)(const char *fmt, ...), void (*fatal)(const char *fmt, ...));
/* (HOS_ENG_LISTS) told, for a recording with a user pointer, when its handle is released (any thread, a bucket run's lock
   may be held) and when its bytes are freed */
void nvn_list_set_user_hooks(void (*released)(nvn_lrec *r), void (*freed)(nvn_lrec *r));

/* ---- recording (one thread at a time for a command buffer, as NVN has it) ---- */
/* BeginRecording: an allocation (the last one's guess, or the one open), cur at its start (what was recorded since
   is dropped, as the default path's BeginRecording does) */
void nvn_list_open(nvn_lw *w, uint64_t key);
/* EndRecording (the END record written): the recording filed at ctl (~0: none); its handle in *handle. marked: the
   caller writes {mark, handle} at ctl (nvn_list_sweep) */
nvn_lrec *nvn_list_end(nvn_lw *w, uint64_t ctl, int marked, uint64_t *handle);
/* Finalize / Initialize again: the open recording (if any) dropped */
void nvn_list_drop(nvn_lw *w);

/* ---- handles ---- */
nvn_lrec *nvn_list_lookup(uint64_t handle);   /* NULL: not live */
static inline const uint8_t *nvn_lrec_begin(const nvn_lrec *r) { return (const uint8_t *)(r + 1); }

/* ---- lifetimes ---- */
/* AddControlMemory: every recording filed in [lo, hi) released; its bytes go once the replay is past after */
void nvn_list_release(uint64_t lo, uint64_t hi, uint64_t after);
/* the released recordings (and private copies) the replay is past (done: rx_nvn_replayed) let go */
void nvn_list_drain(uint64_t done);
/* recordings whose mark in their control memory (at ctl: {mark, handle}) was overwritten released; returns how many.
   guest: the host address of a guest range, NULL when outside the window */
size_t nvn_list_sweep(uint64_t mark, uint64_t after, void *(*guest)(uint64_t a, uint64_t n));

void nvn_list_get_stats(nvn_list_stats *s);
#endif
