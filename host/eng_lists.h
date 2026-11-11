/* (native-vk engine cut E2) HOS_ENG_LISTS: host packets for the game's load-time command lists (gpu/render/nvn_pkt.h has
   the format and the lifetime rules). Used by host/nvn_native.c; no guest code and no renderer state here, so
   tests/eng_lists_test.c builds this file as it is.

   HOS_ENG_LISTS=1: packets built at EndRecording and recorded at CallCommands (NOP_PKT), the Vulkan replay applies them.
   HOS_ENG_LISTS=2: packets built only (their cost and counts; the stream is as without the switch).
   HOS_ENG_LISTS_CHECK=1 (with 1): the replay also decodes the records each packet stands for and compares the state.
   HOS_ENG_LISTS_DUMP=<file>: every packet built, one line each (game-derived: keep it under work/). */
#ifndef HOOKSHOT_ENG_LISTS_H
#define HOOKSHOT_ENG_LISTS_H
#include <stddef.h>
#include <stdint.h>
#include "../gpu/render/nvn_pkt.h"

/* once, before any other call. on: 0 off, 1 used, 2 built only; dump: a file name or NULL */
void eng_lists_init(int on, const char *dump, void (*log)(const char *fmt, ...), int64_t (*now_ns)(void));
int eng_lists_on(void);              /* 0, 1 or 2 */

/* the kind of a recording begun from guest return address lr (NVN_PK_NONE: not one of the game's load-time lists) */
int eng_lists_kind(uint64_t lr);
/* EndRecording of a recording of that kind, BEGIN .. END at rec (bytes): its packet, or NULL (refused: counted by why).
   meta: the kind's guest values taken at BeginRecording (nvn_pkt.meta) */
nvn_pkt *eng_lists_build(int kind, const uint8_t *rec, size_t bytes, const uint64_t meta[4]);
void eng_lists_set_handle(nvn_pkt *k, uint64_t handle);   /* (after the handle is known; the dump's line is written here) */
/* the handle was let go: no NOP_PKT recorded from now on names it, the ones recorded before are not used */
void eng_lists_released(nvn_pkt *k);
/* the recording's bytes are gone (after the replay passed every submit made before the release): its memory reused */
void eng_lists_freed(nvn_pkt *k);
/* a CallCommands / CopyCommands of it: 1 when a NOP_PKT is to be recorded (HOS_ENG_LISTS=1 and it is live) */
int eng_lists_called(const nvn_pkt *k);
void eng_lists_call_plain(void);      /* (a call of a handle with no packet: counted) */
void eng_lists_begun(int kind);       /* (a recording of that kind begun: counted) */

typedef struct {
    uint64_t begun[NVN_PK_KINDS], built[NVN_PK_KINDS], refused[NVN_PK_KINDS], build_ns[NVN_PK_KINDS];
    uint64_t calls[NVN_PK_KINDS], calls_plain, shape_k_calls[4];
    uint64_t released, freed, live, live_bytes, raw_bytes, pool_bytes;
    uint64_t why[NOP_COUNT_ + 8];     /* refusals: by the op that was not taken, then ENG_WHY_* */
} eng_lists_stats;
enum { ENG_WHY_DEFERRED = NOP_COUNT_, ENG_WHY_AFTER_DRAW, ENG_WHY_TWO_DRAWS, ENG_WHY_FULL, ENG_WHY_CENSUS, ENG_WHY_DEEP,
       ENG_WHY_CORRUPT };
void eng_lists_get_stats(eng_lists_stats *s);
/* the counts since the last call, as a log line (every few hundred presents) */
void eng_lists_log(const char *why, double frames);
#endif
