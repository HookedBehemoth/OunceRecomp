# What we know about the game and the direct back end

Reference notes, each cited from the code that depends on it. Addresses are guest addresses of the supported build
(`f_<address>` is the lifted function; `nx2/oot_labels.txt` names the ones we know). Everything made from the game stays in
`work/`; these files hold analysis, not data. The back end's design and open problems are in
[../direct-backend-plan.md](../direct-backend-plan.md).

The game

- [boot-to-main-loop.md](boot-to-main-loop.md): the path from the host's `main` to the steady frame loop.
- [texture-streaming.md](texture-streaming.md): how the game loads, registers, evicts and frees textures.
- [shadow-maps.md](shadow-maps.md): how the shadow maps live and die, and what pins them.
- [jobs-state.md](jobs-state.md): what a render job inherits, and the work recorded inside jobs.
- [job-formats.md](job-formats.md): where a job gets its target formats (the pass key).
- [render-targets.md](render-targets.md): every render target's size and format, where they come from, and where to take control.
- [terrain-texels.md](terrain-texels.md): terrain tiles and the other CPU texel paths.
- [task-mesh.md](task-mesh.md): the game's task and mesh shaders.
- [movie-player.md](movie-player.md): the movie player, its frame chooser, the NDVE network and how the movie file reaches it.
- [havok.md](havok.md): how the physics, cloth and AI work reaches Havok, its kernels.

The back end

- [shader-contract.md](shader-contract.md): what a translated program expects at a draw.
- [patch-sites.md](patch-sites.md): the game-patch sites, and the causes found for picture bugs.
- [converters.md](converters.md): the build-time texture and terrain converters.
- [transient-pool.md](transient-pool.md): the one allocation for transient render targets.
- [spv-late.md](spv-late.md): the register-pressure pass in the translator.

Mesh-stage hang and tools

- [hang-cause.md](hang-cause.md): the Hyrule Field device loss and what was ruled out.
- [hang-fix.md](hang-fix.md): the hand-written stage, the spill scan, the market's result.
- [mesh-program.md](mesh-program.md): the hanging program, read statically.
- [mesh-harness.md](mesh-harness.md): running the game's mesh stages outside the game (`tools/meshlab`).
- [shader-costs.md](shader-costs.md): the census of every shader and what rewriting would pay.
