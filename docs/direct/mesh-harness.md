# The mesh harness

This page shows how to capture the game's mesh draws and run them outside the game with `tools/meshlab`: to compare a
replacement stage with the translator's, to find register spills, or to reproduce a GPU hang without the game. Read it
before you write a replacement mesh stage or change how mesh stages are translated.

The harness links nothing of the game. Captures and everything made from them hold game data: keep them under `work/`.

## Capture a frame

Run the game with `HOS_DBG=meshcap:<present>`. At that present the back end writes `meshcap/frame.bin` into the run
folder: every task+mesh and mesh-only draw of the frame (program, push data, uniform buffers, the storage the stages
read, the task header and payload as the GPU left them, pass formats, dynamic state), every vertex draw, and the stages
of every program. A market frame is a few hundred MB.

Related switches: `meshcapprog:<key>` limits the capture to one program; `meshcaplost` keeps a rolling capture for a
frame that loses the device.

## Build the tools

```
tools/meshlab/build.sh
tools/meshlab/build_shaders.sh <out dir>
```

`build.sh` builds `meshlab` (the harness), `meshtool` (the offline translator) and `spvlate` (the register-pressure
pass as a command). `build_shaders.sh` compiles the replacement stages listed in `gpu/direct/shaders/*/codes.txt`.

## Use the harness

| Task | Command |
|---|---|
| list a frame's commands and mesh draws | `meshlab <frame.bin> list` |
| compare a replacement stage with the translator's | `meshlab <frame.bin> compare <seq...> --repl <dir>` |
| compare with the register-pressure pass applied | `meshlab <frame.bin> compare all --late mesh` |
| print one draw's geometry | `meshlab <frame.bin> dump <seq>` |
| compile the stages and print their statistics | `meshlab <frame.bin> stats <seq...>` |
| draw a job's commands again and again, with a watchdog | `meshlab <frame.bin> run --job <seq> --seconds 30 --watchdog 5` |
| time a fragment stage on a captured draw | `meshlab <frame.bin> fs ...` |

`compare` rewrites the mesh stage so that every output store also goes to a buffer, then compares the two stages'
triangles corner by corner, bit for bit. A replacement goes into `gpu/direct/shaders/mesh/swap.txt` only when every
captured draw is equal.

How a draw is reproduced: the stage is made from the captured SPIR-V and heap mappings, uniform buffers are copied into
buffers of their own, and guest memory sits at its own guest addresses in sparse buffers with only the captured pages
bound (the rest reads as zeros, as in the game's null window).

## Run anything that may hang the GPU

Run every GPU job of the harness through `tools/meshlab/gpu_run.sh <output file> <meshlab args>`. It takes the same
lock as a game run, starts only when the last GPU reset is three minutes old, holds the lock through a recovery, and
saves the kernel log after a loss.

## Find register spills

`tools/meshlab/spill_scan.py --meshlab <meshlab> <frame.bin>...` compiles every mesh program of the captures with
`RADV_DEBUG=shaders` and counts ACO's scratch spills per stage. ACO is RADV's shader compiler. A mesh stage that
spills is the kind that hung the GPU; see [task and mesh shaders](task-mesh.md). `tools/meshlab/late_gate.py` runs the
same scan before and after [the register-pressure pass](spv-late.md).

## Read a stage

`meshtool <stage file> <prefix>` translates a stage offline as the back end does and writes the IR, a structured print
and the SPIR-V. `tools/meshlab/inventory.py` groups every mesh stage of a capture into families, and
`tools/meshlab/simplify.py` folds the lowering's noise out of a flat print.
