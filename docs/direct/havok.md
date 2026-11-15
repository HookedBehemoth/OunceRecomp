# Havok: where the game's physics, cloth and AI work runs

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

## The kernels

The largest Havok kernels by the executors' time (a profile of the market with an earlier lifter; their order, not
their times, is what still holds): cloth (constraints, collision, skinning) about a third of the top 25, the
narrowphase and its geometry steps another third, the queries' traversals and leaves a fifth; no kernel held more than
4% of the Havok time.

| function | guest instructions | what |
|---|---|---|
| `phys_cloth_link_constraints` `f_81c86720` | 401 | hclStandardLinkConstraintSet::apply |
| `phys_cloth_collide_particles` `f_81b5a3c0` | 4803 | cloth particles against a collidable |
| `phys_cloth_update_aabbs` `f_81b38800` | 385 | hclSimClothInstance::updateAabbs (vector min/max) |
| `phys_hknp_raycast_leaf_kernel` `f_81a4a540` | 2350 | under hknpWorld::castRay |
| `phys_vertices_aabb` `f_81746440` | 124 | transformed vertices into a min/max box |
| `phys_hknp_np_convex_kernel` `f_81730760` | 2040 | the largest narrowphase kernel |
| `phys_hknp_np_composite_kernel` `f_81779980` | 1413 | narrowphase, convex against a composite |
| `phys_hknp_raycast_leaf` `f_818623a0` | 1145 | under hknpWorld::castRay |
| `phys_cloth_collide_kernel` `f_81b8c000` | 17427 | under `phys_cloth_collide_particles` |
| `phys_gsk_step` `f_8173b340` | 800 | Common/Base geometry under the narrowphase |

The hkai work is uneven: collision avoidance's solve task runs every second frame, for several milliseconds of wall
time on a NavMesh worker.
