# The player builder

This page is the plan for `hookshot-builder` (`ounce/builder`, in the `ounce` cargo workspace with the lifter): one
Rust executable that turns a player's dump (a `.nup`, or an `exefs/` and `romfs/` folder) into a portable game folder,
with no Python and no compiler on the player's PC. Read it before you work on the builder; [builder notes](builder-notes.md) has the facts earlier builders learned.

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
| Lift and compile | `ounce-lift` as a library (`ounce/lift/src/pipeline.rs`), each partition and generated module compiled as it is made (`ounce/builder/src/lift.rs`), byte for byte `nx2/build.py`'s objects | 164 s at 16 jobs |
| Host C | `host/*.c`, libopus and the runtime by clang in this process (`ounce/builder/src/host.rs`) | 4 s |
| Link | lld in this process (`ounce/builder/src/link.rs`), against the prebuilt host and the sysroot; then the labels | 1 s |
| Textures | `prepare_textures` in Rust, with the native encoders linked statically | 440 s (Python, fast settings) |
| Shaders | optional: the game translates its programs ahead of time into the folder's caches | not written |

Terrain tiles and the sky's LUT are deswizzled by the game at load time (they are BC and RGBA, never ASTC), so the
build does not prepare them and the folder keeps the game's files.

## The compile

- Cold partitions: clang 23's printed -O1 pipeline with `dse` after `memcpyopt` (`ounce/builder/data/pipeline_cold_O1.txt`),
  -O1 code generation, as build.py's `opt | clang`.
- Hot partitions: `default<O2>` with the SLP vectorizer, as clang -O2. The pipeline text clang prints cannot stand in
  for it: it loses VectorCombine's early-folds-only setting.
- `-rotation-max-header-size` is a global LLVM reads while LoopRotate runs; build.py gives 0 to the hot partitions
  and leaves the cold ones at 16. The compile runs hot and cold partitions apart (a gate in `compile.rs`).
- The target machine has clang's options (PIC, function sections, addrsig); the module's name is `<stdin>`, so the
  object's file symbol is build.py's. `hookshot-builder compile-all <lift dir> <out> --check <obj dir>` compares every
  object (the partitions' and gen/'s) with build.py's.

## The lift in this process

- `hookshot-builder lift-compile <out> [--check <obj dir>]` runs the lift and hands every partition and generated
  module to the compile threads, so no IR is written to disk. With `--check`, every object is compared with
  build.py's.
- The lift hands the hot partitions over first and the builder holds the cold generated modules until they are out:
  hot and cold compiles never run at once (`compile.rs`'s gate), so the long hot compiles start first.
- A cold build peaks at 6.4 GB: the compiles (4.5 GB at 16 jobs) and the compressed IR waiting for them.

## The game folder

`hookshot-builder build <out> --direct <hookshot_direct.so>` makes the game folder in one
process: the host's C (`host/*.c`, libopus and `rt64/a64_rt.c`, with build.py's flags and
`-fstack-protector-strong`), the lift and its compile, the link and the labels. The folder holds `hookshot`,
`hookshot_direct.so` and `image.bin` beside it, where the game looks without an argument; `scripts/run-vk.sh` and
`scripts/play.sh` take the image from beside the executable when it is there.

- The version text is `nx2/version.py`'s, computed by the builder (`hookshot-builder version`); `--version` replaces it.
- `--gen <dir>` also writes the lift's files for the RE tools and diffout: `gen/*.ll`, `gen/functions.txt`,
  `lift_answers.ll` and `facts.tsv`.
- `--progress` prints the lines a front end reads: `@@plan`, `@@file <name> <seconds> <new|cached>` and `@@eta`,
  estimated from the last build's seconds (`seconds.txt` in the cache).
- Checks: the LLVM version the process runs; the patch declarations against the compiled host (`hos_patch`
  records); the back end's undefined symbols against the executable's exports.
- LLVM's `-mllvm` options are globals of the process, so the compiles run in phases: the host's C at LLVM's
  defaults, then the runtime at its two options, then the partitions at theirs.
- The lift hands every module over LZ4-compressed and ends within seconds; its state is freed before the compiles
  are done.
- Jobs: `-j`, else `HOOKSHOT_JOBS`, else every CPU. A memory gate holds a compile back until the memory free (the
  smaller of MemAvailable and the room in a capped cgroup), less what compiles started in the last 3 s may still
  take, keeps its need and `HOOKSHOT_MEM_RESERVE_GB` (1.5) free. A partition needs about eight times its IR (the
  largest, 42 MB of IR, peaks at 360 MB), a C file 300 MB. `HOOKSHOT_MEM_GATE=0` turns it off.
- Objects are cached in `work/builder-cache`, apart from build.py's `work/objcache` because the keys are built
  differently. A module is found by its IR, recipe and the partitions' options; a C file by a manifest of the files
  its last compile read (clang's dependency list) and their contents; both keys name the LLVM version and the
  builder's recipes. Files no build used for `HOOKSHOT_OBJCACHE_DAYS` (3) days are removed at the end of a build.
  A rebuild with nothing changed takes 8 to 10 s, most of it the lift.
- `HOOKSHOT_SYSROOT` gives the link and the C compiles a sysroot (the AppImage's glibc).

## Building the builder

- Development: `cargo build --release -p hookshot-builder` (the default `clang` feature) links the system's
  `libLLVM.so` and `libclang-cpp.so` of LLVM 23, and uses the system clang's resource headers.
- Shipping: `scripts/linux/static-llvm.sh` builds a static LLVM, clang and lld of the pinned release for
  x86-64-v3; `LLVM_STATIC_DIR=<its build> cargo build --release -p hookshot-builder --features static-llvm,clang`
  links them. LTO is not set up: the archives would hold bitcode, and the Rust link would have to run LTO.

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

## LLVM linked statically

`--features static-llvm` links LLVM and lld from a static build tree (`LLVM_STATIC_DIR`: Release, X86 only, clang
and lld projects, no tools); `--features clang` adds clang's libraries and `hookshot-builder compile-c`, a C file
compiled in this process from build.py's flags. Its objects equal the system clang's for every host C file when
`-fstack-protector-strong` is given (the distribution's clang turns it on by default). Sizes of the stripped builder
(the lifter included):

| Builder | Raw | zstd -19 | xz -9 | squashfs (zstd 19) |
|---|---|---|---|---|
| LLVM and lld | 54.5 MB | 16.2 MB | 14.7 MB | 19.9 MB |
| LLVM, lld and clang | 98.1 MB | 27.2 MB | 24.4 MB | 33.7 MB |

The static build compiles the same objects (all 154, byte for byte) 28 % slower than the distribution's libLLVM
(CachyOS, built for x86-64-v3): 215 s against 168 s for the lift and compile. A shipped static LLVM wants the same
build: x86-64-v3, LTO and profile-guided optimisation.

The host's C needs 153 of clang's resource headers (3.3 MB, 0.2 MB compressed); the whole folder is 16.4 MB
(0.8 MB compressed).

## The output folder

`hookshot`, `hookshot_direct.so`, `libSDL2-2.0.so.0`, `image.bin`, `romfs/` (a copy), `prepared/tex/`, the shader
caches and `user/`. The host needs two changes for it: the prepared store and the saves default to the game folder.

## Order of work

1. The compile, lld and the generated code as IR in this process (done).
2. `ounce-lift` as a library, the partitions handed to the compile in memory (done).
3. The builder as the development build too, in place of `nx2/build.py`'s game path.
4. CI: the host and `hookshot_direct.so` prebuilt; the sysroot.
5. The dump reader (`nx2/nup.py`, `unpack.py`, the ZBIC decompressor) in Rust.
6. The texture converter in Rust; terrain and sky deswizzled in the host.
7. The output folder, the host's defaults for it, the AppImage.
8. Shaders ahead of time.
