# Havok in the game

This page explains how the game's physics, cloth and AI work reaches Havok, and which kernels hold most of its time.
Read it when you profile game logic (calc stage A, see [the frame loop](frame-loop.md)) or look for lifter work that
would pay in physics. The functions carry the prefix `phys_` in `games/oot/labels.txt` (section "Havok").

The game links Havok 2025.2: hknp rigid-body physics, Havok Cloth (hcl) and Havok AI (hkai), under Nintendo's Phive
layer. Havok's monitor timers are compiled in, and their strings name most of its functions.

## How the work reaches Havok

- **Physics as tasks.** The world steps through Havok's task graph, not through `hknpWorld::stepCollide` or `stepSolve`
  (never called in play). `phys_hknp_generate_collide_tasks` and `phys_hknp_generate_solve_tasks` build the graph, and
  the game's job graph runs each task on the BaseProc executors. Two worlds step each frame.
- **Cloth.** `phys_cloth_step` runs once per cloth instance a frame: as an executor job for most actors, inline in the
  player's own update for the player. Its cost per second grows with the frame rate.
- **Queries.** Actors call the world's ray, shape, point and box queries directly.
- **AI.** hkai's tasks (navigator updates, path requests, collision avoidance) run on the NavMesh workers. Collision
  avoidance's solve runs every second frame, for several milliseconds of wall time.

## Where the time goes

Of the largest Havok kernels by the executors' time, cloth (constraints, particle collision, skinning) is about a
third, the narrowphase and its geometry steps another third, and the queries' traversals a fifth. No single kernel
holds more than 4% of the Havok time. The order comes from a profile of the market; the times depend on the lifter.

Cloth settings are mostly the code's defaults: `GameParameter/GameClothParam/Player` sets only the iteration count for
the player. Havok's own cloth and solver settings are in `Phive/Cloth/*.bphcl`.
