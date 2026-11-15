/* HOS_DBG=havok: a per-frame account of the game's Havok work (hknp physics, Havok Cloth, Havok AI) in havok.csv in the
   working directory (the run folder), and the time per hooked function every 600 frames in havok_funcs.csv
   (docs/direct/havok.md). Measurement only; with the switch off each hook below costs a load and a branch.

   The hooks wrap the functions the executors and the game call into Havok with: the hkTask::process of every hknp,
   particle and hkai task type (the BaseProc executors call them straight from their job graph), the world's task
   generation and the step-phase job, phys_cloth_step (one cloth instance's step, all of Havok Cloth's work), and the
   world's collision queries (castRay, castShape, getClosestPoints, queryPoint, queryAabb, castAabb). A hook calls the
   lifted body itself and times it: wall time always, the thread's CPU time for every kind but the queries (they are
   many and short). A span inside another span of the same kind counts once (the outer one); "havok" columns count
   only the spans not inside any other.

   havok.csv, a row per frame (moduleCalc_'s entry to the next): t (s since start), present, frame_ms, then per kind
   n, ms and cpu_ms, cloth_max_ms (the longest phys_cloth_step), havok_ms and havok_cpu_ms (outermost spans, all
   threads), x0_ms..x5_ms (outermost spans on BaseProcExecutor0-5) and other_ms (on every other thread). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

enum { K_COLLIDE_GEN, K_SOLVE_GEN, K_STEP_PHASE, K_TASK, K_CLOTH, K_QUERY, K_AI, KINDS };
static const char *const kind_name[KINDS] = {"collide_gen", "solve_gen", "step_phase", "task", "cloth", "query", "ai"};

/* the hooked functions, by address (sorted): kind and what it is (Havok's names from its monitor and assert strings) */
static const struct { uint64_t addr; int kind; const char *name; } fn[] = {
    {0x8016b620, K_CLOTH, "phys_cloth_step (hclWorld::stepSingleThreaded)"},
    {0x80372ad4, K_COLLIDE_GEN, "hknpWorld::generateCollideTasks"},
    {0x8073a4a0, K_STEP_PHASE, "phys_step_phase_job"},
    {0x817f3ce0, K_SOLVE_GEN, "hknpWorldEx::generateSolveTasks"},
    {0x817f4aa0, K_TASK, "hknpWorldEx::postSolve"},
    {0x818337b4, K_TASK, "hknpParticlesColliderManager::PreStepTask"},
    {0x81834588, K_TASK, "hknpParticlesColliderManager::PostStepTask"},
    {0x818b5df8, K_TASK, "hknpParticlesTasksImpl::FireSignalTask"},
    {0x818b6800, K_TASK, "hknpParticlesTasksImpl::ParticleParticleProcessEarlyOutStateTask"},
    {0x818b7720, K_TASK, "hknpParticlesTasksImpl::PreparePartitionTreeBuffers"},
    {0x818b8640, K_TASK, "hknpParticlesTasksImpl::GenerateMappingToPointsBufferTask"},
    {0x818b9140, K_TASK, "hknpParticlesTasksImpl::UpdateBodyCachesTask"},
    {0x818b9be0, K_TASK, "hknpParticlesTasksImpl::FindDynamicNeighborsTask"},
    {0x818ba6e0, K_TASK, "hknpParticlesTasksImpl::ParticleParticleBranchSelfCollisionsTask"},
    {0x818bb580, K_TASK, "hknpParticlesTasksImpl::ParticleParticlePhasedMultithreadedInterBranchSolveTask"},
    {0x818bc530, K_TASK, "hknpParticlesTasksImpl::ParticleParticleGeneratePhasedPairsTask"},
    {0x818bd320, K_TASK, "hknpParticlesTasksImpl::ParticleParticleInterBranchCollisionsTask"},
    {0x818be1c0, K_TASK, "hknpParticlesTasksImpl::BodyCollisionsTask"},
    {0x818f4f28, K_QUERY, "hknpWorld::castRay"},
    {0x818f5450, K_QUERY, "hknpWorld::castShape"},
    {0x818f5998, K_QUERY, "hknpWorld::queryPoint"},
    {0x818f5ec8, K_QUERY, "hknpWorld::getClosestPoints"},
    {0x818f64cc, K_QUERY, "hknpWorld::queryAabb (body ids)"},
    {0x818f6a08, K_QUERY, "hknpWorld::queryAabb (broadphase collector)"},
    {0x818f6f44, K_QUERY, "hknpWorld::queryAabbNmp (body ids)"},
    {0x818f74b0, K_QUERY, "hknpWorld::queryAabbNmp (broadphase collector)"},
    {0x818f7a20, K_QUERY, "hknpWorld::queryAabb (collector)"},
    {0x818f8740, K_QUERY, "hknpWorld::castAabb (body ids)"},
    {0x818f8c7c, K_QUERY, "hknpWorld::castAabb (collector)"},
    {0x8191ee00, K_TASK, "hknpProcessNarrowPhasePairsTask"},
    {0x81994b48, K_TASK, "hknp post-collide task (hknpWorldEx::postCollide)"},
    {0x81995360, K_TASK, "hknpProcessFullCastsTask"},
    {0x81995fc4, K_TASK, "hknpSolverFactoryTask"},
    {0x81996780, K_TASK, "hknpFinalizeNarrowPhaseGridTask"},
    {0x81997f00, K_TASK, "hknpGenSolverTasksTask"},
    {0x819989dc, K_TASK, "hknp task (process in the vtable slot at 0x848d95c8)"},
    {0x8199c860, K_TASK, "hknpCollideSyncTask"},
    {0x8199d5b8, K_TASK, "hknpBroadphaseOptimizeTask"},
    {0x8199e040, K_TASK, "hknpAddActiveBodyPairsTask"},
    {0x8199e714, K_TASK, "hknpDeactivationMaintenanceTask"},
    {0x8199f7c0, K_TASK, "hknpPrepareNarrowPhaseTask"},
    {0x819a3d20, K_TASK, "hknpProcessNarrowPhaseGridTask"},
    {0x819a8fac, K_TASK, "hknp task (process in the vtable slot at 0x848d9a40)"},
    {0x819a9348, K_TASK, "hknpPrepareConstraintsTask"},
    {0x819aa220, K_TASK, "hknpBuildConstraintJacobiansTask"},
    {0x819ab300, K_TASK, "hknpNarrowPhasePairsPrepareGatherTask"},
    {0x819ad000, K_TASK, "hknpNarrowPhasePairsGatherThreadOutputsTask"},
    {0x819aeae8, K_TASK, "hknpNarrowPhasePairsBuildJacobians"},
    {0x819affa0, K_TASK, "hknpNarrowPhasePairsSortThreadOutputs"},
    {0x819b25e0, K_TASK, "hknpNarrowPhasePairsFinalize"},
    {0x819b8e20, K_TASK, "hknp solver step task (gatherSolverVelocities)"},
    {0x819c67a0, K_TASK, "ShiftMotionsTask"},
    {0x819c6d20, K_TASK, "ShiftBodiesTask"},
    {0x819c7628, K_TASK, "ShiftBroadphaseTask"},
    {0x819c7b40, K_TASK, "FinalizeShiftWorldTask"},
    {0x81a14200, K_TASK, "hknpSimdTreeBroadPhaseTasks::CopyAabbsAndMarkActiveBodiesRecollide"},
    {0x81a191a0, K_TASK, "hknpSimdTreeBroadPhaseTasks::CollideTrees"},
    {0x81a19f80, K_TASK, "hknpSimdTreeBroadPhaseTasks::Finalize"},
    {0x81a42cd8, K_TASK, "hknp callback task (calls a task's process)"},
    {0x81f18280, K_AI, "hkaiNavigatorUpdateTasks::UpdateNavigatorsTask"},
    {0x81f19b60, K_AI, "hkaiNavigatorUpdateTasks::UpdateVolumeNavigatorsTask"},
    {0x81f1b2e0, K_AI, "hkaiNavigatorUpdateTasks::FireEventsTask"},
    {0x81f363d0, K_AI, "hkaiWorldProcessingTasks::FireProcessingRunStartingTask"},
    {0x81f36c0c, K_AI, "hkaiWorldProcessingTasks::FireProcessingRunCommittedTask"},
    {0x81f38900, K_AI, "hkaiUpdateNavDataTasks::ApplyAsyncWorldUpdatesTask"},
    {0x81f39944, K_AI, "hkaiUpdateNavDataTasks::UpdateOverlapManagerTask"},
    {0x81f3a8d8, K_AI, "hkaiUpdateNavDataTasks::UpdateCutConfigTask"},
    {0x81f3b8a0, K_AI, "hkaiUpdateNavDataTasks::DisconnectExternalEdgesTask"},
    {0x81f3c840, K_AI, "hkaiUpdateNavDataTasks::ResetFacesTask"},
    {0x81f3d7d0, K_AI, "hkaiUpdateNavDataTasks::ApplyCutsTask"},
    {0x81f3e774, K_AI, "hkaiUpdateNavDataTasks::ResetClearanceAllTask"},
    {0x81f3f634, K_AI, "hkaiUpdateNavDataTasks::ResetClearanceMediatorTask"},
    {0x81f405dc, K_AI, "hkaiUpdateNavDataTasks::ResetClearanceFloodFillTask"},
    {0x81f41684, K_AI, "hkaiUpdateNavDataTasks::MatchStreamingFacesTask"},
    {0x81f42660, K_AI, "hkaiUpdateNavDataTasks::CompactTask"},
    {0x81f4370c, K_AI, "hkaiUpdateNavDataTasks::GetAndClearModifiedFacesTask"},
    {0x81f446dc, K_AI, "hkaiUpdateNavDataTasks::GetAndClearModifiedAabbsTask"},
    {0x81f45674, K_AI, "hkaiUpdateNavDataTasks::FireNavDataJustModifiedTask"},
    {0x81f4660c, K_AI, "hkaiUpdateNavDataTasks::CommitUpdatesTask"},
    {0x81f4761c, K_AI, "hkaiUpdateNavDataTasks::FireEventsTask"},
    {0x81f525c0, K_AI, "hkaiNavMeshClearanceCacheFillingTasks::GetUncalculatedClearanceFacesTask"},
    {0x81f57c3c, K_AI, "hkaiNavMeshClearanceCacheFillingTasks::CalculateClearanceTask"},
    {0x81f59a60, K_AI, "hkaiNavMeshClearanceCacheFillingTasks::FireEventsTask"},
    {0x81f5cff8, K_AI, "hkaiPathRequestsTasks::IncrementRequestPrioritiesTask"},
    {0x81f5dd60, K_AI, "hkaiPathRequestsTasks::FireEventsTask"},
    {0x81f621ec, K_AI, "hkaiPathRequestsTasks::ProcessPathRequestsTask"},
    {0x820d2180, K_AI, "hkaiCollisionAvoidance::Tasks::InitContextTask"},
    {0x820d33d8, K_AI, "hkaiCollisionAvoidance::Tasks::SortAabbsTask"},
    {0x820d41c0, K_AI, "hkaiCollisionAvoidance::Tasks::ScanCharacterAabbsTask"},
    {0x820d5140, K_AI, "hkaiCollisionAvoidance::Tasks::ScanObstacleGeneratorAabbsTask"},
    {0x820d6e20, K_AI, "hkaiCollisionAvoidance::Tasks::ClearNeighborCollectorsTask"},
    {0x820d81dc, K_AI, "hkaiCollisionAvoidance::Tasks::GatherCharacterAabbsTask"},
    {0x820d94ac, K_AI, "hkaiCollisionAvoidance::Tasks::UpdateAndGatherObstacleAabbsTask"},
    {0x820da77c, K_AI, "hkaiCollisionAvoidance::Tasks::GatherBoundariesAndSolveCharactersTask"},
    {0x820dba4c, K_AI, "hkaiCollisionAvoidance::Tasks::ApplySolverOutputTask"},
};
enum { FNS = sizeof fn / sizeof fn[0], SLOTS = 256, EXECUTORS = 6, FUNCS_EVERY = 600 };

/* a thread's sums; written by its thread only, read by the frame's writer (a torn read is off by one span) */
typedef struct {
    int64_t n[KINDS], wall[KINDS], cpu[KINDS];
    int64_t outer_wall, outer_cpu;
    int64_t max_wall, max_frame;   /* the longest cloth step of frame max_frame */
    int64_t fn_n[FNS], fn_wall[FNS];
    int exec;                      /* BaseProcExecutor<exec>, else -1 */
} Slot;

static int on = -1;
static Slot *slots[SLOTS];
static volatile LONG nslots;
static volatile int64_t frame_no;
static _Thread_local Slot *my;
static _Thread_local uint64_t tl_enter;     /* the address the hook calls the body of now (its own call) */
static _Thread_local int tl_depth[KINDS], tl_any;
static int64_t t_start;
__attribute__((constructor)) static void havok_t0(void) { t_start = hos_now_ns(); }

static int havok_on(void) {
    if (on < 0) on = hos_dbg_item("havok");
    return on;
}

static int64_t thread_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static Slot *slot(void) {
    if (my) return my;
    Slot *s = (Slot *)calloc(1, sizeof *s);
    if (!s) return NULL;
    HosThread *t = hos_self();
    const char *name = t ? hos_thread_name(t) : "";
    s->exec = !strncmp(name, "BaseProcExecutor", 16) && name[16] >= '0' && name[16] < '0' + EXECUTORS ? name[16] - '0' : -1;
    const LONG i = InterlockedIncrement(&nslots) - 1;
    if (i < SLOTS) slots[i] = s;
    return my = s;
}

static int fn_index(uint64_t addr) {
    int lo = 0, hi = FNS - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (fn[m].addr == addr) return m;
        if (fn[m].addr < addr) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

/* the hook: 0 runs the lifted body as usual (switch off, or the hook's own call of it); else the body run and timed */
static int span(Ctx *c, uint64_t addr) {
    if (tl_enter == addr) { tl_enter = 0; return 0; }
    if (!havok_on()) return 0;
    const int i = fn_index(addr);
    A64Fn body = i >= 0 ? a64_lookup(addr) : NULL;
    Slot *s = body ? slot() : NULL;
    if (!s) return 0;
    const int k = fn[i].kind, outer_kind = tl_depth[k]++ == 0, outer = tl_any++ == 0;
    const int cpu = k != K_QUERY || outer;
    const int64_t c0 = cpu ? thread_cpu_ns() : 0, t0 = hos_now_ns();
    tl_enter = addr;
    body(c);
    const int64_t t1 = hos_now_ns(), c1 = cpu ? thread_cpu_ns() : 0;
    tl_depth[k]--, tl_any--;
    const int64_t wall = t1 - t0;
    s->fn_n[i]++, s->fn_wall[i] += wall;
    if (outer_kind) {
        s->n[k]++, s->wall[k] += wall, s->cpu[k] += c1 - c0;
        if (k == K_CLOTH) {
            const int64_t f = frame_no;
            if (s->max_frame != f) s->max_frame = f, s->max_wall = 0;
            if (wall > s->max_wall) s->max_wall = wall;
        }
    }
    if (outer) s->outer_wall += wall, s->outer_cpu += c1 - c0;
    return 1;
}

/* ---- the writer, at moduleCalc_'s entry on the main thread ---- */
static FILE *csv, *fcsv;
static int64_t last_t, prev_n[KINDS], prev_wall[KINDS], prev_cpu[KINDS], prev_ow, prev_oc, prev_exec[EXECUTORS], prev_other;
static int64_t prev_fn_n[FNS], prev_fn_wall[FNS], fn_frames;

static void write_funcs(double t, long long present) {
    if (!fcsv && !(fcsv = fopen("havok_funcs.csv", "w"))) return;
    if (!ftell(fcsv)) fprintf(fcsv, "t,present,frames,addr,kind,calls_per_frame,ms_per_frame,name\n");
    for (int i = 0; i < FNS; i++) {
        int64_t n = 0, w = 0;
        for (LONG j = 0; j < nslots && j < SLOTS; j++)
            if (slots[j]) n += slots[j]->fn_n[i], w += slots[j]->fn_wall[i];
        if (n != prev_fn_n[i])
            fprintf(fcsv, "%.3f,%lld,%lld,%08llx,%s,%.2f,%.4f,\"%s\"\n", t, present, (long long)fn_frames,
                    (unsigned long long)fn[i].addr, kind_name[fn[i].kind], (double)(n - prev_fn_n[i]) / (double)fn_frames,
                    (double)(w - prev_fn_wall[i]) / 1e6 / (double)fn_frames, fn[i].name);
        prev_fn_n[i] = n, prev_fn_wall[i] = w;
    }
    fflush(fcsv);
    fn_frames = 0;
}

void havok_stats_frame(void) {
    if (!havok_on()) return;
    const int64_t now = hos_now_ns();
    if (!csv) {
        if (!(csv = fopen("havok.csv", "w"))) { on = 0; return; }
        fprintf(csv, "t,present,frame_ms");
        for (int k = 0; k < KINDS; k++) fprintf(csv, ",%s_n,%s_ms,%s_cpu_ms", kind_name[k], kind_name[k], kind_name[k]);
        fprintf(csv, ",cloth_max_ms,havok_ms,havok_cpu_ms");
        for (int e = 0; e < EXECUTORS; e++) fprintf(csv, ",x%d_ms", e);
        fprintf(csv, ",other_ms\n");
        hos_log("havok: a row per frame in havok.csv, the hooked functions every %d frames in havok_funcs.csv (HOS_DBG=havok)",
                FUNCS_EVERY);
        last_t = now;
    }
    int64_t n[KINDS] = {0}, wall[KINDS] = {0}, cpu[KINDS] = {0}, ow = 0, oc = 0, ex[EXECUTORS] = {0}, other = 0, mx = 0;
    const int64_t f = frame_no;
    for (LONG j = 0; j < nslots && j < SLOTS; j++) {
        const Slot *s = slots[j];
        if (!s) continue;
        for (int k = 0; k < KINDS; k++) n[k] += s->n[k], wall[k] += s->wall[k], cpu[k] += s->cpu[k];
        ow += s->outer_wall, oc += s->outer_cpu;
        if (s->exec >= 0) ex[s->exec] += s->outer_wall; else other += s->outer_wall;
        if (s->max_frame == f && s->max_wall > mx) mx = s->max_wall;
    }
    const double t = (double)(now - t_start) / 1e9;
    const long long present = hos_vi_present_number();
    fprintf(csv, "%.3f,%lld,%.3f", t, present, (double)(now - last_t) / 1e6);
    for (int k = 0; k < KINDS; k++)
        fprintf(csv, ",%lld,%.3f,%.3f", (long long)(n[k] - prev_n[k]), (double)(wall[k] - prev_wall[k]) / 1e6,
                (double)(cpu[k] - prev_cpu[k]) / 1e6);
    fprintf(csv, ",%.3f,%.3f,%.3f", (double)mx / 1e6, (double)(ow - prev_ow) / 1e6, (double)(oc - prev_oc) / 1e6);
    for (int e = 0; e < EXECUTORS; e++) fprintf(csv, ",%.3f", (double)(ex[e] - prev_exec[e]) / 1e6), prev_exec[e] = ex[e];
    fprintf(csv, ",%.3f\n", (double)(other - prev_other) / 1e6);
    for (int k = 0; k < KINDS; k++) prev_n[k] = n[k], prev_wall[k] = wall[k], prev_cpu[k] = cpu[k];
    prev_ow = ow, prev_oc = oc, prev_other = other, last_t = now;
    frame_no = f + 1;
    if (++fn_frames >= FUNCS_EVERY) write_funcs(t, present), fflush(csv);
}

ENTRY_HOOK(0x8016b620) { return span(c, 0x8016b620ull); }
ENTRY_HOOK(0x80372ad4) { return span(c, 0x80372ad4ull); }
ENTRY_HOOK(0x8073a4a0) { return span(c, 0x8073a4a0ull); }
ENTRY_HOOK(0x817f3ce0) { return span(c, 0x817f3ce0ull); }
ENTRY_HOOK(0x817f4aa0) { return span(c, 0x817f4aa0ull); }
ENTRY_HOOK(0x818337b4) { return span(c, 0x818337b4ull); }
ENTRY_HOOK(0x81834588) { return span(c, 0x81834588ull); }
ENTRY_HOOK(0x818b5df8) { return span(c, 0x818b5df8ull); }
ENTRY_HOOK(0x818b6800) { return span(c, 0x818b6800ull); }
ENTRY_HOOK(0x818b7720) { return span(c, 0x818b7720ull); }
ENTRY_HOOK(0x818b8640) { return span(c, 0x818b8640ull); }
ENTRY_HOOK(0x818b9140) { return span(c, 0x818b9140ull); }
ENTRY_HOOK(0x818b9be0) { return span(c, 0x818b9be0ull); }
ENTRY_HOOK(0x818ba6e0) { return span(c, 0x818ba6e0ull); }
ENTRY_HOOK(0x818bb580) { return span(c, 0x818bb580ull); }
ENTRY_HOOK(0x818bc530) { return span(c, 0x818bc530ull); }
ENTRY_HOOK(0x818bd320) { return span(c, 0x818bd320ull); }
ENTRY_HOOK(0x818be1c0) { return span(c, 0x818be1c0ull); }
ENTRY_HOOK(0x818f4f28) { return span(c, 0x818f4f28ull); }
ENTRY_HOOK(0x818f5450) { return span(c, 0x818f5450ull); }
ENTRY_HOOK(0x818f5998) { return span(c, 0x818f5998ull); }
ENTRY_HOOK(0x818f5ec8) { return span(c, 0x818f5ec8ull); }
ENTRY_HOOK(0x818f64cc) { return span(c, 0x818f64ccull); }
ENTRY_HOOK(0x818f6a08) { return span(c, 0x818f6a08ull); }
ENTRY_HOOK(0x818f6f44) { return span(c, 0x818f6f44ull); }
ENTRY_HOOK(0x818f74b0) { return span(c, 0x818f74b0ull); }
ENTRY_HOOK(0x818f7a20) { return span(c, 0x818f7a20ull); }
ENTRY_HOOK(0x818f8740) { return span(c, 0x818f8740ull); }
ENTRY_HOOK(0x818f8c7c) { return span(c, 0x818f8c7cull); }
ENTRY_HOOK(0x8191ee00) { return span(c, 0x8191ee00ull); }
ENTRY_HOOK(0x81994b48) { return span(c, 0x81994b48ull); }
ENTRY_HOOK(0x81995360) { return span(c, 0x81995360ull); }
ENTRY_HOOK(0x81995fc4) { return span(c, 0x81995fc4ull); }
ENTRY_HOOK(0x81996780) { return span(c, 0x81996780ull); }
ENTRY_HOOK(0x81997f00) { return span(c, 0x81997f00ull); }
ENTRY_HOOK(0x819989dc) { return span(c, 0x819989dcull); }
ENTRY_HOOK(0x8199c860) { return span(c, 0x8199c860ull); }
ENTRY_HOOK(0x8199d5b8) { return span(c, 0x8199d5b8ull); }
ENTRY_HOOK(0x8199e040) { return span(c, 0x8199e040ull); }
ENTRY_HOOK(0x8199e714) { return span(c, 0x8199e714ull); }
ENTRY_HOOK(0x8199f7c0) { return span(c, 0x8199f7c0ull); }
ENTRY_HOOK(0x819a3d20) { return span(c, 0x819a3d20ull); }
ENTRY_HOOK(0x819a8fac) { return span(c, 0x819a8facull); }
ENTRY_HOOK(0x819a9348) { return span(c, 0x819a9348ull); }
ENTRY_HOOK(0x819aa220) { return span(c, 0x819aa220ull); }
ENTRY_HOOK(0x819ab300) { return span(c, 0x819ab300ull); }
ENTRY_HOOK(0x819ad000) { return span(c, 0x819ad000ull); }
ENTRY_HOOK(0x819aeae8) { return span(c, 0x819aeae8ull); }
ENTRY_HOOK(0x819affa0) { return span(c, 0x819affa0ull); }
ENTRY_HOOK(0x819b25e0) { return span(c, 0x819b25e0ull); }
ENTRY_HOOK(0x819b8e20) { return span(c, 0x819b8e20ull); }
ENTRY_HOOK(0x819c67a0) { return span(c, 0x819c67a0ull); }
ENTRY_HOOK(0x819c6d20) { return span(c, 0x819c6d20ull); }
ENTRY_HOOK(0x819c7628) { return span(c, 0x819c7628ull); }
ENTRY_HOOK(0x819c7b40) { return span(c, 0x819c7b40ull); }
ENTRY_HOOK(0x81a14200) { return span(c, 0x81a14200ull); }
ENTRY_HOOK(0x81a191a0) { return span(c, 0x81a191a0ull); }
ENTRY_HOOK(0x81a19f80) { return span(c, 0x81a19f80ull); }
ENTRY_HOOK(0x81a42cd8) { return span(c, 0x81a42cd8ull); }
ENTRY_HOOK(0x81f18280) { return span(c, 0x81f18280ull); }
ENTRY_HOOK(0x81f19b60) { return span(c, 0x81f19b60ull); }
ENTRY_HOOK(0x81f1b2e0) { return span(c, 0x81f1b2e0ull); }
ENTRY_HOOK(0x81f363d0) { return span(c, 0x81f363d0ull); }
ENTRY_HOOK(0x81f36c0c) { return span(c, 0x81f36c0cull); }
ENTRY_HOOK(0x81f38900) { return span(c, 0x81f38900ull); }
ENTRY_HOOK(0x81f39944) { return span(c, 0x81f39944ull); }
ENTRY_HOOK(0x81f3a8d8) { return span(c, 0x81f3a8d8ull); }
ENTRY_HOOK(0x81f3b8a0) { return span(c, 0x81f3b8a0ull); }
ENTRY_HOOK(0x81f3c840) { return span(c, 0x81f3c840ull); }
ENTRY_HOOK(0x81f3d7d0) { return span(c, 0x81f3d7d0ull); }
ENTRY_HOOK(0x81f3e774) { return span(c, 0x81f3e774ull); }
ENTRY_HOOK(0x81f3f634) { return span(c, 0x81f3f634ull); }
ENTRY_HOOK(0x81f405dc) { return span(c, 0x81f405dcull); }
ENTRY_HOOK(0x81f41684) { return span(c, 0x81f41684ull); }
ENTRY_HOOK(0x81f42660) { return span(c, 0x81f42660ull); }
ENTRY_HOOK(0x81f4370c) { return span(c, 0x81f4370cull); }
ENTRY_HOOK(0x81f446dc) { return span(c, 0x81f446dcull); }
ENTRY_HOOK(0x81f45674) { return span(c, 0x81f45674ull); }
ENTRY_HOOK(0x81f4660c) { return span(c, 0x81f4660cull); }
ENTRY_HOOK(0x81f4761c) { return span(c, 0x81f4761cull); }
ENTRY_HOOK(0x81f525c0) { return span(c, 0x81f525c0ull); }
ENTRY_HOOK(0x81f57c3c) { return span(c, 0x81f57c3cull); }
ENTRY_HOOK(0x81f59a60) { return span(c, 0x81f59a60ull); }
ENTRY_HOOK(0x81f5cff8) { return span(c, 0x81f5cff8ull); }
ENTRY_HOOK(0x81f5dd60) { return span(c, 0x81f5dd60ull); }
ENTRY_HOOK(0x81f621ec) { return span(c, 0x81f621ecull); }
ENTRY_HOOK(0x820d2180) { return span(c, 0x820d2180ull); }
ENTRY_HOOK(0x820d33d8) { return span(c, 0x820d33d8ull); }
ENTRY_HOOK(0x820d41c0) { return span(c, 0x820d41c0ull); }
ENTRY_HOOK(0x820d5140) { return span(c, 0x820d5140ull); }
ENTRY_HOOK(0x820d6e20) { return span(c, 0x820d6e20ull); }
ENTRY_HOOK(0x820d81dc) { return span(c, 0x820d81dcull); }
ENTRY_HOOK(0x820d94ac) { return span(c, 0x820d94acull); }
ENTRY_HOOK(0x820da77c) { return span(c, 0x820da77cull); }
ENTRY_HOOK(0x820dba4c) { return span(c, 0x820dba4cull); }
