/* The direct back end's view of the game's render-job recorder f_80084de0 (games/oot/patches/eng_jobs.c): which recording a job's is,
   its pass key and its view record, and what the job being recorded on a thread is (for games/oot/patches/job_sync.c and
   games/oot/patches/mesh_system.c). */
#ifndef OUNCE_ENG_JOBS_H
#define OUNCE_ENG_JOBS_H
#include <stdint.h>
#include "hos.h"

/* BeginRecording, its guest registers in c: 1 and the recorder's pass key pointer (its x3) and view record when this is the
   recording of a job the hook is running on this thread; 0: not a job's */
int game_job_list(uint64_t cmd, uint64_t *key, uint64_t *view);   /* (a JOB list's BeginRecording: its x0) */
#endif
