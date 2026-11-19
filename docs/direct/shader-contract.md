# The shader contract

This page describes what a program translated by the back end expects at a draw, and what the back end provides: how
programs are identified, how buffers, textures and state reach the shader, and where the caches live. Read it before
you change `renderer/vk_shader.cpp`, `renderer/vk_draw.cpp`, `renderer/vk_compute.cpp` or the
translator fork in `third_party/eden`.

The authoritative definitions are `renderer/vk_shader.h` (the program variant a draw reads, `DrawProg`) and the
translator's push-data layout in `third_party/eden/shader_recompiler/backend/spirv/emit_spirv.h`.

## Programs and variants

- A program is a slot found by its NVN serial in a table read without a lock. Its stages are read when the game sets
  them (`nvnProgramSetShaders`), on a worker, and translated from their native Ampere instructions by the direct front
  (`third_party/eden/shader_recompiler/frontend/ampere`).
- A program has a vertex stage, optionally a geometry stage after it, and a fragment stage; or a mesh stage, optionally
  after a task stage. A geometry stage is translated for triangle input, so a draw of such a program with another
  topology is skipped and counted.
- A stage is translated from its code alone: no draw state reaches the translator. A texture's type is its
  instruction's (2D where the instruction has none), and no texture is taken for an integer one.
- A draw finds its **variant** by a key made of the stages' code and the numeric class (float, signed, unsigned) of the
  color targets the fragment stage writes. In practice there is one variant per program.
- A draw checks the textures it reads through a driver bank word against what the translation took them for. A bound
  integer, rectangle or buffer texture, or another type at a size query, is an error: logged once per program and
  texture word, counted, and it fails the smoke test.
- What else a stage's SPIR-V could depend on is data the draw pushes, so a stage translates the same whatever it is drawn
  with:
  - **Vertex inputs:** every attribute is fetched as the UINT format of its layout, which keeps every bit, and the vertex
    stage converts it (unorm, snorm, half, scaled, integer, packed) by a code per attribute.
  - **The stage before the fragment stage:** the fragment stage declares the inputs it reads. A word in the push data
    says which of them the stage before writes; one it does not write reads (0, 0, 0, 1).
  - **Bindings:** every stage reads the same descriptor set, NVN's pools: a binding per Vulkan view type for sampled
    images and for storage images, and the sampler pool twice (plain and depth compare). Uniform and storage buffers
    have no binding: the stage reads them through device addresses.
  - **A color location with no attachment** is written as a float output, and Vulkan discards the write.
- A draw with no ready variant is skipped and counted, and the translation is queued. Workers run four classes of jobs,
  most urgent first: what a load waits for, what a draw misses now, programs the game set at load, and speculative
  variants (`renderer/vk_work.h`).
- Variants are only ever added, and shader objects are never destroyed.

## What a draw provides

| What the shader reads | How the back end provides it |
|---|---|
| push data | the translator's block, then at offset 120 one address per stage that reads uniform buffers (vertex or mesh, fragment, geometry): that stage's part of the uniform-buffer address table |
| vertex attribute formats | a code byte per attribute in the translator's block (`VERTEX_FORMATS_PUSH_OFFSET`): the conversion, the component width and the component count |
| which generics the stage before the fragment stage writes | a word in the translator's block (`FRAGMENT_INPUTS_PUSH_OFFSET`) |
| c0 and c2, the driver banks (storage-buffer table, texture and sampler handles) | copied into the frame arena when the stage's binds change; reached through the address table |
| c1, the program's own constants | a buffer made when the program loads |
| c3 and up, the game's uniform buffers | the guest address, through the address table |
| bank 24, a stage's root table: its graphics push constants | the bytes the game pushed for the stage, copied into the frame arena with the banks; for a stage the list pushed nothing for, the console's blit block for the render area (texel size, size, UV scale and clamp, source rectangle) |
| storage buffers | by device address: the shader loads the address from its bank and translates it through the guest-window table |
| textures and samplers | an index in the pools' set = the NVN pool id, written at registration; the shader reads the handle from the bank and clamps its image id to the pool, whose last entry is null |
| a handle stored in a game buffer | read by the shader through that buffer's pushed address |
| deferred handle binds | the bank words are copied from the game's array at the guest's submit |

For compute, c0 is NVN's packed compute bank: grid, push constants, uniform and storage tables and handles. Compute
textures read their handles from c0, not c2. An indirect dispatch reads its grid on the GPU.

## Fixed-function state

The back end sets all dynamic state in every command buffer, because RADV inherits none into secondaries. Conversions
to know about:

- **Viewport:** y is flipped (`{x, y + h, w, -h}`) because NVN's clip space has +y up. Front faces are not inverted:
  the flip keeps the winding.
- **Depth:** NVN's -1..1 clip range is converted in the vertex stage, so the depth range is clamped to 0..1.
- **Polygons** are drawn as triangle fans. **Quads** have no Vulkan topology: their draws are skipped and counted.
  Tessellation is not translated.
- **Write masks** cover only the components the fragment stage writes.
- **Unset vertex attributes** read one 32-bit zero from a zero buffer; the vertex stage completes it to (0, 0, 0, 1).

## The caches

The caches live beside the executable. A file of another stamp, format or driver is made again from its start, never
read; you don't need to delete one.

| File | Holds | Made again when |
|---|---|---|
| `ounce_renderer_programs.bin` | translated graphics variants (SPIR-V, interface info) | the shader stamp changes (the translator, the compiler and flags that build it, the stamped regions of `vk_shader.cpp`, `vk_state.h`, the register-pressure pass), the record format, the device's subgroup answers, the depth mode, or the translation switches `nolate`, `latevertex` and `loopcap` |
| `ounce_renderer_compute.bin` | translated compute variants | the compute stamp (the translator, the compiler and flags, the stamped regions), the record version, the device's answers or `loopcap` change |
| `ounce_renderer_shader_objects.bin` | driver binaries of the shader objects | the driver's binary UUID or version changes; a binary the driver refuses is compiled again |

The build computes the stamps (`renderer/meson/shader_stamp.py`) by hashing the compiler's name, version and flags,
the translator's files (its `meson.build` files excepted) and the regions marked `// <stamp graphics>`,
`// <stamp shared>` and `// <stamp compute>`. Code that decides a translation's output belongs inside a stamped region;
editing code outside them keeps the caches. Markers that don't pair stop the build. A change to a record's layout
raises `kDiskFormat` or `kDiskVersion`.

## Debug switches

| Switch | Effect |
|---|---|
| `debug.drawlog=<present>` | logs every draw of one frame with its program and state |
| `debug.logdraw`, `logcompute`, `logmesh` | more draw, dispatch and mesh lines |
| `debug.workers=<n>` | the number of shader workers |
