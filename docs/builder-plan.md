# The player builder

This page is the plan for the player's build in `ounce` (`ounce/builder`, in the `ounce` cargo workspace with the lifter): one
Rust executable that turns a player's dump (a `.nup`, or an `exefs/` and `romfs/` folder) into a portable game folder,
with no Python and no compiler on the player's PC. Read it before you work on the builder; [builder notes](builder-notes.md) has the facts earlier builders learned.

## What ships

- `ounce.AppImage`: the builder, `libLLVM.so.23` and lld (linked dynamically), the prebuilt host (its
  objects and `ounce_renderer.so`, built in CI), and the link's sysroot (glibc 2.35's start files and stub libraries).
  `ounce-gui` (`ounce/gui`) is its window: it runs the builder as a child process and reads its `@@` lines
  ([the window](#the-window)).
- A folder of the source the lift reads: `runtime/` (its C files and `sources.txt`), `games/oot/` (`game.toml`, the labels,
  the hot list `hot_funcs.bin` and the link order `hot_order.bin`), `ounce/lift/data/horizon.toml` and
  `third_party/a64-kit/rt/`.

Everything is built for x86-64-v3; a CPU without AVX2 is refused.

## The steps and their budget

The target is 15 minutes on a 16-thread PC. Measured on a Ryzen 7 5800X3D (16 threads, 32 GB):

| Step | What | Time |
|---|---|---|
| Read the dump | `.nup` or folder (`ounce/dump`); the exefs into memory first, then the romfs into the game folder on a thread while the lift runs | exefs under 1 s; romfs 30 s (folder) |
| Lift and compile | `ounce-lift` as a library (`ounce/lift/src/pipeline.rs`), each partition and generated module compiled as it is made (`ounce/builder/src/lift.rs`), byte for byte the objects of `opt \| clang` with the same flags | 164 s at 16 jobs |
| Host C | `runtime/*.c`, libopus and the runtime by clang in this process (`ounce/builder/src/host.rs`) | 4 s |
| Link | lld in this process (`ounce/builder/src/link.rs`), against the prebuilt host and the sysroot; then the labels | 1 s |
| Textures | `ounce/tex`: every BNTX texture deswizzled, ASTC re-encoded to BC7 and BC5, packed (`ounce tool textures`); the romfs copy's texture files written with their texels zeroed | 258 s (fast, the default); 1055 s with `--quality high` (16,000 CPU-s, 2.6 GB peak) |
| Shaders | optional: the game translates its programs ahead of time into the folder's caches | not written |

## The prepared assets

`ounce/tex` makes the store the direct back end maps (`prepared/tex`). [The converters](direct/converters.md) describes
it; `ounce/tex/src/store.rs` the format.

- `ounce tool textures <out>` writes a store; every build writes `<out>/prepared/tex` after the link (a
  developer's build only when its inputs changed, copied from the main checkout's when that one matches).
- The game description's `[assets]` names what is the game's: the romfs file of its zstd dictionaries and the folders
  the texture step skips.
- The encoders are bc7enc_rdo's, built into the builder: bc7e (ISPC) for BC7, and for BC5 (the ASTC normal maps) an
  AVX2 search that gives rgbcx's blocks. The sources are `third_party/bc7enc_rdo`; ISPC, a tool, comes from
  the `PATH` or `ISPC`, and its version is part of the store's key (`OUNCE_BC7ENC` overrides the sources).
- zstd is `ounce/dump`'s (1.5.7, its symbols local). A frame's bytes depend on the zstd version, so a store made with
  another version differs in its packs but not in its content.
- Files are converted in parallel, largest first, each admitted by the memory gate; the textures, images and bands of
  block rows inside a file run on the same threads.
- The default settings (fast) are BC7 level 1, BC5 radius 1 and zstd 3, and a texture below 35 dB is encoded again
  at BC7 level 4 or radius 5; `--quality high` is BC7 level 4, radius 5 and zstd 19 (again at level 6 or radius 8),
  with packs smaller in four times the time.
- ASTC is always re-encoded: desktop GPUs do not sample it, so the builder does not look for a GPU that does. The records and frames depend on nothing but the settings and the romfs;
  where a frame lies in the packs follows the order files finish. `ounce tool textures --compare <a> <b>` checks
  two stores field by field and frame by frame.
- The index and every texture record are held until the end; files are written to the packs as they finish.

## The dump

`ounce/dump` reads a `.nup` package or a folder that holds `exefs/` and `romfs/`. The lift reads the exefs in memory;
the romfs is copied into the game folder. Nothing at run time reads the exefs: the game runs from `image.bin`, which
holds everything the lift took from it (the modules, and `main.npdm`'s program id, stack and priority).

- `ounce dump <file.nup|folder>` lists the titles; `--check [--game <dir>]` reads the exefs in memory and
  checks it against `game.toml`'s identity, as a build does first (`@@dump`, `@@identity` lines); `--verify` checks the package (its table of contents,
  every member's extent and SHA-256, the game's zstd header of each `.zs` file) without writing; `--extract <dir>`
  writes the newest title (`--title` picks another, `--only` one part).
- `ounce tool build <out> --dump <file.nup|folder>` reads the exefs into memory (its modules made whole and
  plain, as an extraction does), checks it against `game.toml`'s identity, and hands it to the lift; the romfs is
  copied into `<out>/romfs` on a thread while the host's C and the lift run.
- `--dump-dir <dir>` also writes the dump there whole (`exefs/`, `romfs/`), for the developer tools that read
  `work/dump`; the game folder's romfs is then copied from it.
- A folder's files are copied with `copy_file_range`: on a file system that shares extents (btrfs, XFS) the copy
  takes no space. When the folder is the output, nothing is copied.
- ZBIC modules (firmware 22 and later) are rewritten as plain NSOs. The decoder is `third_party/zbic`'s zstd, built
  twice (with and without ZBIC) with every symbol but the reader's own made local, so it never meets the `libzstd`
  that LLVM loads.
- An interrupted extraction resumes: a file already there with its size is kept. Paths that could leave the folder,
  links on the way and a temporary with other hard links are refused.

## The compile

- Cold partitions: clang 23's printed -O1 pipeline with `dse` after `memcpyopt` (`ounce/builder/data/pipeline_cold_O1.txt`),
  -O1 code generation, as `opt | clang`.
- Hot partitions: `default<O2>` with the SLP vectorizer, as clang -O2. The pipeline text clang prints cannot stand in
  for it: it loses VectorCombine's early-folds-only setting.
- `-rotation-max-header-size` is a global LLVM reads while LoopRotate runs: 0 for the hot partitions, 16 (the
  default) for the cold ones. The compile runs hot and cold partitions apart (a gate in `compile.rs`).
- The target machine has clang's options (PIC, function sections, addrsig); the module's name is `<stdin>`, so the
  object's file symbol is the one of IR piped to clang. `ounce tool compile-all <lift dir> <out> --check <obj
  dir>` compares every object (the partitions' and gen/'s) with the folder's.

## The lift in this process

- `ounce tool lift-compile <out> [--check <obj dir>]` runs the lift and hands every partition and generated
  module to the compile threads, so no IR is written to disk. With `--check`, every object is compared with
  the folder's.
- The lift hands the hot partitions over first and the builder holds the cold generated modules until they are out:
  hot and cold compiles never run at once (`compile.rs`'s gate), so the long hot compiles start first.
- A cold build peaks at 6.4 GB: the compiles (4.5 GB at 16 jobs) and the compressed IR waiting for them.

## The build

`ounce tool build <out> --direct <ounce_renderer.so>` builds the game in one process: the host's C
(`runtime/*.c`, libopus and `rt64/a64_rt.c`, each with its flags and `-fstack-protector-strong`), the lift and its
compile, the link and the labels. `<out>` holds the game's executable (game.toml's `exe`, `Ocarina of Time`), `ounce_renderer.so` and `image.bin`, where the game
looks without an argument; `ounce run` takes the image from beside the executable. With `--dump`, `<out>` is a whole game folder (below).

- The version text (the release name and a fingerprint of the sources) is computed by the builder
  (`ounce tool version`); `--version` replaces it.
- `--gen <dir>` also writes the lift's files for the RE tools and diffout: `gen/*.ll`, `gen/functions.txt`,
  `lift_answers.ll` and `facts.tsv`.
- `--progress` prints the lines a front end reads ([the window](#the-window) lists them).
- Checks: the LLVM version the process runs; the patch declarations against the compiled host (`hos_patch`
  records); the back end's undefined symbols against the executable's exports.
- LLVM's `-mllvm` options are globals of the process, so the compiles run in phases: the host's C at LLVM's
  defaults, then the runtime at its two options, then the partitions at theirs.
- The lift hands every module over LZ4-compressed and ends within seconds; its state is freed before the compiles
  are done.
- Jobs: `-j`, else `OUNCE_JOBS`, else every CPU. A memory gate holds a compile back until the memory free (the
  smaller of MemAvailable and the room in a capped cgroup), less what compiles started in the last 3 s may still
  take, keeps its need and `OUNCE_MEM_RESERVE_GB` (1.5) free. A partition needs about eight times its IR (the
  largest, 42 MB of IR, peaks at 360 MB), a C file 300 MB. `OUNCE_MEM_GATE=0` turns it off.
- Objects are cached in the object cache (`~/.cache/ounce`). A module is found by its IR, recipe and the partitions' options; a C file by a manifest of the files
  its last compile read (clang's dependency list) and their contents; both keys name the LLVM version and the
  builder's recipes. Files no build used for `OUNCE_OBJCACHE_DAYS` (3) days are removed at the end of a build.
  A rebuild with nothing changed takes 8 to 10 s, most of it the lift.
- `OUNCE_SYSROOT` gives the link and the C compiles a sysroot (the AppImage's glibc).

## Building the builder

- Development: `cargo build --release -p ounce` (the default `clang` feature) links the system's
  `libLLVM.so` and `libclang-cpp.so` of LLVM 23, and uses the system clang's resource headers.
- Shipping: `ounce tool static-llvm` builds a static LLVM, clang and lld of the pinned release for
  x86-64-v3; `LLVM_STATIC_DIR=<its build> cargo build --release -p ounce --features static-llvm,clang`
  links them. LTO is not set up: the archives would hold bitcode, and the Rust link would have to run LTO.

## The link

- The command line is the one clang's driver gives `ld.lld` for `clang -fuse-ld=lld`: a PIE against glibc with libgcc's
  start and end files, `--gc-sections`, and the symbols `ounce_renderer.so` calls in the executable exported
  (`os_*`, `hos_mem_ledger`).
- The link order (`hot_order.bin`) becomes lld's symbol-ordering file, `hot_order.txt` beside the executable.
- The labels rename `f_<address>` to `<label>.f_<address>` in the symbol table only.
- `ounce tool link <out> <objs.rsp> --order <hot_order.bin> --labels <labels.txt> -- <libs>` links a list of
  objects alone.

## The generated code

`ounce-lift` writes the runtime's tables, the image's description, the import stops and the NVN routes as IR
(`gen/*.ll`); the builder compiles them as it compiles `lift_answers`. Each object equals clang's object of the C these files replace in its loaded sections, symbols and
relocations (`ounce tool objeq <a.o> <b.o>`). The guest model's runtime (`rt64/a64_rt.c`) reads no dump: it is
built with the host.

## LLVM linked statically

`--features static-llvm` links LLVM and lld from a static build tree (`LLVM_STATIC_DIR`: Release, X86 only, clang
and lld projects, no tools); `--features clang` adds clang's libraries and `ounce tool compile-c`, a C file
compiled in this process. Its objects equal the system clang's for every host C file when
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

## The game folder

`ounce tool build <out> --direct <ounce_renderer.so> --dump <file.nup|folder>` makes a folder that holds the
whole game:

| Path | What |
|---|---|
| the game's executable (game.toml's `exe`, `Ocarina of Time`), `ounce_renderer.so`, `image.bin` | the game, its renderer and its image |
| `romfs/` | the dump's romfs; its texture files hold their headers, their texels are zeroed |
| `prepared/tex/` | the texture store; `prepared/hollowed.txt` lists the romfs files whose texels it holds |
| `ounce_renderer_*.bin` | the shader caches, written by the game beside the executable |
| `user/` | the saves |

- The host takes everything from beside the executable when it starts with no arguments: the image, `romfs/`,
  `prepared/` and `user/`. A folder without `user/` keeps its saves in `~/.local/share/ounce`. `ounce run`
  passes the image, the romfs and the saves folder on the command line.
- The build's own files (objects, the map, the link order) go to `--obj`, not into the folder.
- `--quality high` uses the slower texture settings; `--no-textures` leaves the store and the romfs's texels as
  they are.

### The romfs copy

The game reads a texture file's headers and hands its texels to `nvnTextureInitialize`; the host gives the back
end the texture's prepared record instead, and the back end never reads the texels in guest memory
([texture streaming](direct/texture-streaming.md)). So the texture step writes each romfs file that holds BNTX
textures again with its texels zeroed: the file is decompressed, each BNTX's texel data (from its BRTD section to its
relocation table) is zeroed, and the file is compressed again with its own dictionary and checksum flag at zstd 9.
The game's texture files are all zstd, so the zeroed texels take next to no space in the frame; sparse holes would
not help.

For this game, 21,658 files (9.6 GB, 18.1 GB of texels decompressed) become 10 MB, in the texture step's own time:
the romfs copy is 15.9 GB instead of 25.0 GB, and the folder about 30 GB with its 13.8 GB store.

- `TerrainArc/` stays whole: the terrain manager reads its tiles at run time and the host's terrain patch makes
  their texels linear from the loaded file ([terrain tiles](direct/terrain-texels.md)).
- A BNTX inside a compressed member of an archive is left whole.
- A rebuild writes the files `prepared/hollowed.txt` lists whole again before the texture step reads the romfs.
  `ounce tool textures` refuses a game folder's romfs: make a store from the dump.

## The window

`ounce-gui` picks the dump and the game folder, runs `dump --check` and `probe`, then `build --dump --progress`,
each as a child process, so a step that crashes never takes the window down. It runs `ounce build <game> --out <dir> --dump <dump>`
from beside itself or `ounce/target/release`, and lists the games under the checkout's `games/`.
It is built on its own, `cargo build --profile gui -p ounce-gui`, without LTO (34 s cold, 7.0 MB); the
workspace's default members leave it out. eframe has only glow, X11 and its default fonts (Wayland desktops run it
through XWayland; the `wayland` feature adds 7 s and 1.4 MB). The file dialogs are rfd's desktop portal over a
`dlopen`ed libdbus, with zenity as the fallback; the path fields take a typed path too.

The lines it reads, one per line on standard output:

| Line | When |
|---|---|
| `@@dump <what>`, `@@identity <pass\|fail> <build id or why>` | `dump --check` |
| `@@probe <key> <state> <value>`, `@@probe verdict <pass\|fail> <keys>` | `probe` |
| `@@stages <key>...` | `build --progress`: the steps this build runs, of `dump host lift link romfs textures` |
| `@@stage <key> <what>` | a step begins; `@@stage done <folder>` ends a build |
| `@@plan <modules> <modules>`, `@@file <name> <seconds> <new\|cached>`, `@@eta <seconds>` | the lift's compiles, the estimate from the last build's seconds (`seconds.txt` in the cache) |
| `@@tex <files done> <files> <bytes done> <bytes> <path>` | the texture step, at most four a second |
| `@@error <message>` | the build failed (the message on one line; the log has the rest) |

Stopping a build kills the process, and a build run again over the same folder finishes it:

- the game's executable (game.toml's `exe`, `Ocarina of Time`), `ounce_renderer.so` and `image.bin` are installed by a rename; objects and cached files are written
  beside and renamed.
- The romfs copy keeps a file only when its size is the dump's, and writes through a `.part` file. A texture file
  whose texels were zeroed is smaller than the dump's, so it is written whole again.
- The texture store's packs and index are written again from the start; a `.hollow.tmp` file left in the romfs is
  not read.

## The device check

`ounce probe` checks the GPU the back end would pick (the first discrete GPU with Vulkan 1.3, else the first GPU
with 1.3), with libvulkan loaded at run time and no Vulkan crate. It prints one line per item, `@@probe <key>
<pass|fail|warn|info> <value>`, and last `@@probe verdict <pass|fail> <failed keys>`; it exits 1 when an item fails.

An item should fail only when the back end does not start without it, and warn when the back end starts without it.

| Key | Fails | Warns | The back end without it |
|---|---|---|---|
| `loader`, `api` | under Vulkan 1.3 | | does not start |
| `swapchain` | missing | | does not start |
| `core_features` | no dynamic rendering, synchronization2, timeline semaphores or buffer device addresses | | does not start |
| `graphics_queue` | no family with graphics and compute | | does not start |
| `shader_object`, `robustness2`, `extended_dynamic_state3` and their `_feature(s)` | missing | | does not start |
| `descriptor_indexing` | no runtime descriptor arrays, non-uniform image indexing, update after bind for images, or 64-bit integers | | does not start |
| `descriptor_limits` | update-after-bind limits below the pools' set: 65,536 sampled and 65,536 storage images per view type (7 types), 8,192 samplers | | does not start |
| `push_constants` | under 144 bytes | | does not start |
| `external_memory_fd`, `dma_buf` | | missing | starts; the game's memory is not imported: Linux hands it over as dma-bufs only |
| `sparse_queue` | | no sparse-binding family of its own, or none at all | starts; binds on another queue with sparse binding, and with none layer 0 is off |
| `maintenance7` | | missing | starts; the game's command jobs are not executed |
| `sparse_residency`, `udmabuf` | | missing | starts; layer 0 is off: the game's memory is not mapped on the GPU |
| `vram` | | under 10 GB; under 8 GB the textures may not fit | starts |

When `/dev/udmabuf` cannot be opened, the `udmabuf` line tells the player how to fix it: load the module and let the
user open the device (the group that owns it, often `kvm`).

`gpu`, `device` and `driver` lines name the GPU and its driver; `gpu` warns when the GPU is not discrete.

## Order of work

1. The compile, lld and the generated code as IR in this process (done).
2. `ounce-lift` as a library, the partitions handed to the compile in memory (done).
3. The builder as the development build too (done: `ounce build`).
4. CI: the host and `ounce_renderer.so` prebuilt; the sysroot.
5. The dump reader in Rust (done: `ounce/dump`).
6. The texture converter in Rust (done: `ounce/tex`).
7. The game folder and the host's defaults for it (done); the AppImage.
8. Shaders ahead of time.
