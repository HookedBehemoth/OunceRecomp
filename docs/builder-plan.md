# The player builder

This page is the plan for `hookshot-builder` (`builder/`): one Rust executable that turns a player's dump (a `.nup`,
or an `exefs/` and `romfs/` folder) into a portable game folder, with no Python and no compiler on the player's PC.
Read it before you work on the builder; [builder notes](builder-notes.md) has the facts earlier builders learned.

## What ships

- `hookshot-builder.AppImage`: the builder, `libLLVM.so.23` and lld (linked dynamically), the prebuilt host (its
  objects and `hookshot_direct.so`, built in CI), and the link's sysroot (glibc 2.35's start files and stub libraries).
  A GUI comes later over the same steps; every step reports `@@stage/@@progress/@@eta/@@error` lines for it.
- A folder of the source the lift reads: `host/*.c` (the patch declarations), `games/oot/` (`game.toml`, the labels,
  the hot list `hot_funcs.bin` and the link order `hot_order.bin`), `ounce/lift/data/horizon.toml` and
  `third_party/a64-kit/rt/`.

Everything is built for x86-64-v3; a CPU without AVX2 is refused.

## The steps and their budget

The target is 15 minutes on a 16-thread PC. Measured on a Ryzen 7 5800X3D (16 threads, 32 GB):

| Step | What | Time |
|---|---|---|
| Read the dump | `.nup` or folder; the exefs first, so the lift starts while the romfs is copied | not measured |
| Lift | `ounce-lift` as a library | 7 s |
| Compile | the partitions in this process (`builder/src/compile.rs`), byte for byte `nx2/build.py`'s objects | 173 s at 12 jobs, 3.5 GB peak |
| Generated code | `gen/*.ll` from `ounce-lift`, compiled as the cold partitions are | 4.5 s |
| Link | lld in this process (`builder/src/link.rs`), against the prebuilt host and the sysroot; then the labels | 1 s |
| Textures | `prepare_textures` in Rust, with the native encoders linked statically | 440 s (Python, fast settings) |
| Shaders | optional: the game translates its programs ahead of time into the folder's caches | not written |

Terrain tiles and the sky's LUT are deswizzled by the game at load time (they are BC and RGBA, never ASTC), so the
build does not prepare them and the folder keeps the game's files.

## The compile

- Cold partitions: clang 23's printed -O1 pipeline with `dse` after `memcpyopt` (`builder/data/pipeline_cold_O1.txt`),
  -O1 code generation, as build.py's `opt | clang`.
- Hot partitions: `default<O2>` with the SLP vectorizer, as clang -O2. The pipeline text clang prints cannot stand in
  for it: it loses VectorCombine's early-folds-only setting.
- `-rotation-max-header-size` is a global LLVM reads while LoopRotate runs; build.py gives 0 to the hot partitions
  and leaves the cold ones at 16. The compile runs hot and cold partitions apart (a gate in `compile.rs`).
- The target machine has clang's options (PIC, function sections, addrsig); the module's name is `<stdin>`, so the
  object's file symbol is build.py's. `hookshot-builder compile-all <lift dir> <out> --check <obj dir>` compares every
  object (the partitions' and gen/'s) with build.py's.

## The link

- The command line is the one clang's driver gives `ld.lld` for build.py's link: a PIE against glibc with libgcc's
  start and end files, `--gc-sections`, and the symbols `hookshot_direct.so` calls in the executable exported
  (`os_*`, `hos_mem_ledger`).
- The link order (`hot_order.bin`) becomes lld's symbol-ordering file, `hot_order.txt` beside the executable.
- The labels rename `f_<address>` to `<label>.f_<address>` in the symbol table only.
- `hookshot-builder link <out> <objs.rsp> --order <hot_order.bin> --labels <labels.txt> -- <libs>` links build.py's
  objects. Against build.py's executable, every loaded segment and the symbols are equal; only the section header
  offset and the string tables' order differ.

## The generated code

`ounce-lift` writes the runtime's tables, the image's description, the import stops and the NVN routes as IR
(`gen/*.ll`); build.py and the builder compile them as they compile `lift_answers`, so neither needs a C compiler
for them. Each object equals clang's object of the C these files replace in its loaded sections, symbols and
relocations (`hookshot-builder objeq <a.o> <b.o>`). The guest model's runtime (`rt64/a64_rt.c`) reads no dump: it is
built with the host.

## The output folder

`hookshot`, `hookshot_direct.so`, `libSDL2-2.0.so.0`, `image.bin`, `romfs/` (a copy), `prepared/tex/`, the shader
caches and `user/`. The host needs two changes for it: the prepared store and the saves default to the game folder.

## Order of work

1. The compile, lld and the generated code as IR in this process (done).
2. `ounce-lift` as a library: `lift()` out of its `main.rs`, the partitions handed over in memory.
3. CI: the host and `hookshot_direct.so` prebuilt; the sysroot.
4. The dump reader (`nx2/nup.py`, `unpack.py`, the ZBIC decompressor) in Rust.
5. The texture converter in Rust; terrain and sky deswizzled in the host.
6. The output folder, the host's defaults for it, the AppImage.
7. Shaders ahead of time.
