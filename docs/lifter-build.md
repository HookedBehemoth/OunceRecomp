# Build and iterate on the lifter

This page tells you what to run after a change to the lifter, the patches or the host, how long it takes, and how to
inspect and test what the lifter produced. For the general build and run loop, see the
[development guide](development.md).

## Decide between a lift and a build

| You changed | Run | Typical time |
|---|---|---|
| A file in `runtime/`, including `PATCH_AT`, `PATCH_FUNC` and `NV` declarations | `scripts/build.sh` | Seconds; plus the partitions a declaration touches |
| A file in `renderer/` | `scripts/build.sh` | The renderer's files, then the link |
| The lifter (`ounce/lift`) | `scripts/build.sh` | Every partition whose IR changed is compiled again: up to three minutes |
| The hot list, the dump or the toolchain | `scripts/relift.sh` | The same build, also when `BUILD.txt` names the commit |

Every build lifts the game first (about 8 s), so there is no separate lift step. `scripts/build.sh` builds the
checkout it is in; `scripts/relift.sh` runs it under `work/heavy.lock` and refuses to start while a game runs from
that build. Its log is `work/sa/relift.log` (in the main checkout, `work/play/relift.log`).

## How the build is organized

- **Partitions.** Functions are grouped by guest address into partitions of about 120,000 instructions, and each
  partition is one LLVM module. Functions on the hot list get partitions of their own.
- **Optimization levels.** Hot partitions build at `-O2`, the rest at `-O1` with dead-store elimination added. The hot list is the game description's `hot` file (`games/oot/hot_funcs.bin`); see
  [Game description files](lifter.md#game-description-files).
- **Object cache.** Each partition's object is kept in `work/builder-cache` under a key of its IR, its recipe and
  the LLVM version; a C file's under the files its compile read. An unchanged partition is not compiled again.
  Objects that no build used are removed after `HOOKSHOT_OBJCACHE_DAYS` days (default 3).
- **Parallelism.** The build starts a compile only when free memory holds its estimate, so memory, not core count,
  sets how many run at once. `HOOKSHOT_JOBS` or `-j` caps the number.
- **The lift's files.** The build writes `work/sa/gen/`, `work/sa/facts.tsv` and `work/sa/image/` for the tools.
  The partitions' IR (`work/sa/lift_*.ll`, which `ounce/lift/tools/diffout.py` and the dump tools read) only with
  `scripts/build.sh --gen-parts`.

## Inspect a function

To see what the lifter made of a function, run:

```sh
ounce/target/release/ounce-lift dump --game games/oot <address or label> [--tree] [--opt O1|O2] [--asm] [-o <dir>]
```

It writes the guest code, the lifter's IR, the LLVM IR and, with the options, the optimized IR and the x86 assembly to
`-o <dir>`. It prints the function's summary when the plan has one. `--tree` includes the callees.

## Check the lifter

| Check | What it tests | Needs the dump |
|---|---|---|
| `cargo test --manifest-path ounce/Cargo.toml` | The lifter's decoder and lift | No |
| The verification build | The summaries against a real run ([The lifter](lifter.md#the-verification-build)) | Yes |

In the game, the smoke test and a benchmark are the landing checks ([Test Hookshot](testing.md)).

## Lifter switches

| Switch | When | What it does |
|---|---|---|
| `--game games/<game>` | Lift, build, tools | The game description. Required. |
| `scripts/build.sh -j N`, `HOOKSHOT_JOBS` | Build | Parallel compiles at most |
| `HOOKSHOT_MEM_RESERVE_GB`, `HOOKSHOT_MEM_GATE=0` | Build | Memory the build leaves free (default 1.5); no memory gate |
| `scripts/build.sh --gen-parts` | Build | The partitions' IR in `work/sa` for diffout and the dump tools |
| `HOOKSHOT_LIFT_VERIFY=1` | Build | The verification build |
| `HOOKSHOT_LIFT_SUMMARY_JOBS` | Build | The summary job's workers (default 8) |
| `LIFT2_DEFAULT_NAN=1` | Lift | ARM's default NaN for NaNs made from non-NaN operands |
| `HOOKSHOT_LLVM_BIN` | Lift, build | Where clang and the LLVM tools are |
| `ounce-lift --max-func N` | Lift | Split functions larger than N instructions where no branch crosses |
