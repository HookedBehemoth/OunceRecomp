/* (native-vk engine cut E3) HOS_ENG_JOBS=1: the game's render-job recorder f_80084de0 hooked (host/eng_jobs.c): each job's
   recording tagged with the job (its serial, the view record it found, its item range) and, at its EndRecording on the job's
   own thread, handed to the Vulkan back end, which records the job as a secondary command buffer there (gpu/vk/eng_job.h).
   host/nvn_native.c calls in at BeginRecording and EndRecording.

   HOS_ENG_JOBS_CHECK=1: the back end also compares each job draw's state with the replay's for the same records.
   The game's recorder always runs (its NVN records stay: the replay's fallback for a job not recorded, and the check's
   reference). With HOS_NVN_LIST_REF=1 the jobs are not tagged (only the recordings' copies are handed over). */
#ifndef HOOKSHOT_ENG_JOBS_H
#define HOOKSHOT_ENG_JOBS_H
#include <stddef.h>
#include <stdint.h>
#include "hos.h"

int eng_jobs_on(void);
/* BeginRecording, its guest registers in c: the job's serial when this is the recording of a job the hook is running on this
   thread (0: not a job's) */
uint32_t eng_jobs_begin(Ctx *c);
/* (direct mode: eng_jobs_on is 2) the same recording's pass key pointer (the recorder's x3) and view record; 0: not a job's */
int eng_jobs_tag(Ctx *c, uint64_t *key, uint64_t *view);
/* EndRecording of that recording: its records, BEGIN .. END (bytes), and its handle */
void eng_jobs_end(uint32_t serial, const uint8_t *rec, size_t bytes, uint64_t handle);
/* the counts since the last call, one log line (every 600 presents) */
void eng_jobs_log(double frames);
#endif
