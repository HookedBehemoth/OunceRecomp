# Renderer pages

These pages explain the direct back end, the Vulkan renderer in `renderer`, and the parts of the game it depends on.
Start with [the direct back end plan](../direct-backend-plan.md) for the rules the back end keeps and the open work,
then read [the architecture](architecture.md).

Function names in these pages are labels from `games/oot/labels.txt`, which holds each function's guest address. Nothing
made from the game belongs in these pages or in the repository; it stays under `work/`.

## The back end

- [Architecture](architecture.md): how NVN commands become Vulkan work, from the seam to the submit.
- [Render jobs](jobs.md): pass keys, formats, the inline path and the job patches.
- [The shader contract](shader-contract.md): what a translated program expects at a draw, and the shader caches.
- [Task and mesh shaders](task-mesh.md): the task stage as compute and the defenses against a mesh stage that hangs the GPU.
- [The register-pressure pass](spv-late.md): reordering translated mesh stages, and its gate.
- [Render targets](render-targets.md): sizes, switches, the allocator hook, shadows and upscaling.
- [The transient pool](transient-pool.md): one allocation for the transient render targets.
- [Pool claims](pool-claims.md): what backs each memory pool on the GPU, and the plan that removes layer 0.

## Textures and assets

- [Texture streaming](texture-streaming.md): how the game loads, caches and frees textures, and how the back end follows.
- [Terrain tiles and CPU texels](terrain-texels.md): texels that do not come from the texture store.
- [The asset converters](converters.md): the build-time tools that prepare textures, terrain tiles and sky tables.

## The game

- [From process start to the main loop](boot-to-main-loop.md): the host's set-up and the game's initialization.
- [The frame loop](frame-loop.md): the threads, one frame's phases, and where the render work is recorded.
- [The movie player](movie-player.md): how movies are read, paced and drawn.
- [Havok](havok.md): how physics, cloth and AI reach Havok.

## Tools

- [The shader census](shader-costs.md): what the game's shaders cost, offline and in the game, which programs a scene
  needs, and what making them ahead of time costs.
