# Havok: where the game's physics, cloth and AI work runs, and what it costs

The game links Havok 2025.2 (`hk2025_2_10_PhysicsClothNavigation`): hknp rigid-body physics, Havok Cloth (hcl) and Havok
AI (hkai), under Nintendo's Phive layer. Its code lies in four regions of the lift, by the source paths in its asserts:
Common/Base `0x8169xxxx-0x8173xxxx`, hknp physics `0x8174xxxx-0x81a5xxxx`, cloth `0x81b0xxxx-0x81dexxxx`, navigation
`0x81e0xxxx-0x8210xxxx`. Havok's own monitor timers are compiled in (`hkMonitorStream`, with `__PRETTY_FUNCTION__`
strings), which names most of the functions below; the labels are in `nx2/oot_labels.txt` (section "Havok").

## How the work reaches Havok

- **hknp tasks.** The world steps through Havok's task graph, not through `hknpWorld::stepCollide` / `stepSolve`
  (never called in play): `phys_hknp_generate_collide_tasks` (`f_80372ad4`) and `phys_hknp_generate_solve_tasks`
  (`f_817f3ce0`) build the graph, and the game's job graph runs each task's `hkTask::process` straight from the
  BaseProc executors (`f_8004ab40`, an indirect call at one of its job sites). Each task takes a simulation thread context
  from the world's pool (`f_819212c0`) and gives it back (`f_81921420`). Two worlds step a frame (two collide and two
  solve generations).
- **Cloth.** `phys_cloth_step` (`f_8016b620`, `hclWorld::stepSingleThreaded`) once per cloth instance a frame, from the
  executor job `phys_cloth_step_job` (`f_8016b320`) or inside an actor's own update (`actor_update_b`); everything Havok
  Cloth does in a frame is under it.
- **Queries.** Actors call the world's queries directly: `hknpWorld::castRay`, `castShape`, `getClosestPoints`,
  `queryPoint`, `queryAabb`, `castAabb` (`f_818f4f28` ... `f_818f8c7c`).
- **AI.** hkai's tasks (navigator update, nav data update, path requests, collision avoidance) run on the NavMesh
  workers, mostly `GatherBoundariesAndSolveCharactersTask` (`f_820da77c`).

## The per-frame record: HOS_DBG=havok

`host/eng_havok.c` hooks the entry of 95 functions: every task `process` seen in the market (by stack samples of the
executors) or named by a `Task::process` string, the two generators, `phys_step_phase_job`, `phys_cloth_step` and the
world's queries. A hook calls the lifted body itself (`a64_lookup`, then the body; the hook lets its own call through)
and times it. With `HOS_DBG` lacking `havok` a hook is a load and a branch.

- `havok.csv` (the run folder): a row per frame, from `moduleCalc_`'s entry to the next (`host/direct_layer_ahead.c`'s
  hook calls `havok_stats_frame`). Columns: `t` (s since start), `present`, `frame_ms`, per kind (`collide_gen`,
  `solve_gen`, `step_phase`, `task`, `cloth`, `query`, `ai`) the calls, wall ms and thread-CPU ms, `cloth_max_ms`, the
  outermost spans' wall and CPU over all threads (`havok_ms`, `havok_cpu_ms`), the outermost wall per executor
  (`x0_ms`..`x5_ms`) and on any other thread (`other_ms`). A span inside one of the same kind counts once; the queries'
  CPU time is taken only when a query is not inside another Havok span.
- `havok_funcs.csv`: every 600 frames, per hooked function its calls and wall ms a frame.

Coverage: of the stack samples of one executor that fall in Havok code, 97% are under a hooked function (the rest:
small calls from game code into Common/Base and hknp utilities). Cost with the switch on: the frame median rises by
0.05-0.07 ms of 11.3 (two alternating pairs of 60 s market runs, uncapped), about 0.5%.

Market overlay, uncapped (87 fps), 2600 frames from 32 s, means (medians where they differ):

| kind | calls a frame | wall ms | CPU ms |
|---|---|---|---|
| hknp tasks | 90 | 2.91 | 2.96 |
| of which `hknpProcessNarrowPhasePairsTask` | 13.5 | 2.27 | |
| `phys_step_phase_job` | 1 | 0.46 | 0.45 |
| collide / solve task generation | 2 / 2 | 0.04 / 0.02 | |
| cloth (`phys_cloth_step`) | 23 | 2.26 (longest 0.54) | 2.27 |
| world queries | 43 | 1.49 | 1.17 |
| of which castShape / castRay / getClosestPoints | 11.8 / 21.7 / 9.0 | 0.66 / 0.59 / 0.25 | |
| hkai (NavMesh workers) | 13.5 | 5.6 (median 1.1) | 2.25 (median 0.28) |
| all Havok, outermost spans | | 12.4 (median 8.3) | 9.2 (median 7.8) |

Per executor 1.0-1.3 ms of Havok a frame. The hkai work is uneven: collision avoidance's solve task runs every second
frame for about 10 ms of wall time, most of it waiting for a virtual core (its CPU time is less than half its wall).
