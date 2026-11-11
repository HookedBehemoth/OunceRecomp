/* (native-vk layer 1, stage S1) HOS_NVN_LIST_REF=1: command lists by reference (host/nvn_list.c).

   What it replaces (host/nvn_native.c, the default path): a recording written into its command buffer's segment and
   copied whole at EndRecording (`flatten`), filed by a malloc'd node in a table under one exclusive lock (`copy_lock`),
   every CallCommands copying the callee into the caller (`inline_recording`), and AddControlMemory walking that table
   under the same lock (`release_copies`). Here:

   - A recording is written once, into an allocation of its own (size classes, cached per thread, no lock while the
     thread's cache has one), with its header just before its BEGIN record. It is never copied again (except when its
     guess of a size was wrong: grown by doubling, or shrunk once at its end when it used less than a quarter).
   - Its handle is an id in a table that needs no lock to read (chunks never freed), with a generation: a handle the
     game passes back is used only when its slot still holds it, never by what memory at some address holds.
   - CallCommands and CopyCommands record NOP_CALL (gpu/render/rx_nvn.h nvn_p_call) naming the callee, and take a
     reference on it. The replay walks into the callee and back (gpu/vk/vk_replay.cpp replay(): a call stack, saved
     with every place it stops at or cuts a segment at).
   - Recordings are filed by their control memory position in buckets of 64 KiB blocks, each run of 256 buckets under
     a spinlock of its own; AddControlMemory releases the range's ones and touches only those buckets.

   Lifetime rules (what may be freed when):
   1. A recording's handle lives until the game adds its control memory position again (AddControlMemory over it), or
      overwrites the mark written there (the sweep, every 25,000 recordings): then the handle stops working at once.
   2. Its bytes live on until (a) the replay is past every submit handed in before that release (`after`: the pushed
      count plus the held ones, as for the copies) and (b) no recording that calls it is alive: each caller holds a
      reference from its CallCommands until its own bytes are freed. So a callee retired by the game before its caller
      is submitted, or while the caller waits at an event, is still there when the replay walks into it.
   3. A recording's bytes never change after EndRecording, except its deferred binds' arrays, which each submit reads
      from the game's memory (as resolve_deferred did on the copies). A recording a submit not yet replayed has read
      (`def_after`, `def_busy`) is not written again: a later submit that reads other values gets private copies of it
      and of the recordings that call it, up to its own handle (copy on conflict), freed after that submit.
   4. A recording the replay still walks is never freed: (2a) and (2b) together give it. HOS_NVN_LIST_REF_CHECK=1
      checks this at every free (the newest submit that reached it must be replayed), checks that the bytes did not
      change (a hash taken at EndRecording), and compares at every submit the stream the replay will walk with the
      copy the default path would have made, op by op.

   For layer 1's next stages: the header is where record-time translation (S2) keeps a recording's packets (made once
   per recording, kept as long as its bytes: the precompiled lists' per-handle translation), and NOP_CALL is the
   reference to a callee's packets; the replay's call stack is what parallel emission (S4) saves at each segment cut. */
#ifndef HOOKSHOT_NVN_LIST_H
#define HOOKSHOT_NVN_LIST_H
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
    int32_t def_busy;        /* submits reading its deferred binds right now */
    uint32_t pad_;
    uint64_t def_after;      /* the newest submit (pushed count) that read its deferred binds */
    uint64_t visit;          /* the submit that last read them (nvn_list_submit's serial): once a submit */
    uint64_t after;          /* when it was released: the submit the replay must be past */
    uint64_t reached;        /* (check) the newest submit that walks it */
    uint64_t hash;           /* (check) its bytes at EndRecording, the deferred arrays left out */
    uint8_t *shadow;         /* (check) the default path's copy: BEGIN .. END, callees copied in */
};

/* a command buffer's recording place (in host/nvn_native.c's cmd_rec): seg the open recording's BEGIN, cur where the
   next record goes, end its allocation's end; rec its header (NULL: none open) */
typedef struct nvn_lw {
    uint8_t *seg, *cur, *end;
    nvn_lrec *rec;
    uint64_t key;            /* the command buffer's guest address: its last recording's size, the next one's guess */
    uint32_t last_call, first_call, first_def, last_def;
    uint32_t flags, depth;
    uint64_t flat_extra;     /* flat - bytes so far: what the callees add, less their call records */
} nvn_lw;

typedef struct {
    uint64_t made, live, live_bytes, calls, calls_expanded, released, freed, clones, conflicts, resolved_in_place;
    uint64_t grown, shrunk, swept, copied;   /* (copied: bytes moved by growing and shrinking) */
    uint64_t check_handles, check_bytes, check_mismatch, check_early_free, check_changed;
} nvn_list_stats;

/* once, before any other call. check: HOS_NVN_LIST_REF_CHECK. fatal does not return */
void nvn_list_init(int check, void (*log)(const char *fmt, ...), void (*fatal)(const char *fmt, ...));
int nvn_list_check_on(void);

/* ---- recording (one thread at a time for a command buffer, as NVN has it) ---- */
/* BeginRecording: an allocation (the last one's guess, or the one open), cur at its start (what was recorded since
   is dropped, as the default path's BeginRecording does) */
void nvn_list_open(nvn_lw *w, uint64_t key);
/* room for need bytes at cur (and NVN_LIST_SLACK): grows the open recording, opening one when none is */
void nvn_list_reserve(nvn_lw *w, size_t need);
/* a deferred bind was just recorded at op (its header) */
void nvn_list_note_deferred(nvn_lw *w, const uint8_t *op);
/* CallCommands / CopyCommands of one handle: a NOP_CALL into it (or, past NVN_LIST_MAX_DEPTH, its records copied in).
   0: not a live handle. *flat: the callee's size with its callees (the default path's accounting of the copy) */
int nvn_list_call(nvn_lw *w, uint64_t handle, uint64_t *flat);
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

/* ---- submits (the two calls on one thread, around the hand-over to the replay) ---- */
/* the handles' BEGIN records in out (the replay's handles), their deferred binds read from the game (guest: as for
   the sweep, but dies outside the window). 0, or 1 + the index of a handle that is not live */
int nvn_list_submit(const uint64_t *in, uint64_t *out, int n, uint64_t done, void *(*guest)(uint64_t a, uint64_t n));
/* after the hand-over: after names this submit (pushed count plus held ones, read after it was handed over) */
void nvn_list_submitted(uint64_t after);

void nvn_list_get_stats(nvn_list_stats *s);

/* (tests, check) BEGIN .. END with every callee copied in, as the default path's copy: into out (r->flat bytes) */
void nvn_list_expand(const nvn_lrec *r, uint8_t *out);
#endif
