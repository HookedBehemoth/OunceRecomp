# The shader census

This page describes the tools that measure what the game's shaders cost, offline and in the game, and what their
results tell you about where shader work would pay. Read it before you rewrite a shader by hand or change the
translator for speed.

## What the numbers say

The figures are indicative: they come from a few scenes and from ACO's static estimates. ACO is the shader compiler in
RADV, Mesa's Vulkan driver for AMD GPUs.

- **The game has about 43,000 shader stages** in about 39,000 programs, and the translator takes all but a few dozen. A
  scene uses a few hundred programs: Kokiri about 350, Hyrule Field about 460.
- **RADV removes most of the translation's bulk.** A median fragment stage compiles to about 1.6 times its native
  instruction count.
- **Most of the GPU's frame is not shader ALU work.** ACO's estimate explains under a third of the measured command
  time. The rest is memory latency, fixed function, and many small draws and dispatches with barriers between them.
- **Smaller is not faster.** Hand-written mesh stages that compile much smaller, and a rewritten fog composite, run
  exactly as fast as the translator's in the harness. The limit is outside the stage's ALU and occupancy.

So the order that pays is: per-command overhead first, then translator-wide costs that RADV
cannot clean up (sub-word shared stores, the mesh stage buffer in shared memory, duplicated NaN guards). Rewriting
stages one by one does not pay.

## Measure one frame in the game

Run with `HOS_DBG=pstats:<present>` (or `pstats:bench`, the first frame a benchmark measures). At that present the back end puts a pipeline-statistics query and two timestamps
around every command and writes the results; `tools/shadercensus/scene.py` turns them into a per-program table.
`HOS_DBG=gputime` logs the GPU's busy time a frame.

Take timings with the frame cap off, on an idle machine, with one game process. Under other load, the numbers are not
a result.

## Run the offline census

1. Build the tool: `tools/shadercensus/build.sh`.
2. Make the shader seed, `ounce_shaders.bin`, from the romfs. No tool in the tree makes it.
3. Run the census, which is sharded and resumable:

   ```
   python -I tools/shadercensus/census.py <seed> --out <dir> --compile
   ```

4. Aggregate it with `tools/shadercensus/report.py`.

The census translates each stage as the back end does and, with `--compile`, has RADV compile it and records its
statistics. It draws nothing and needs no GPU lock. Its output is made from the game's code and stays under `work/`.
