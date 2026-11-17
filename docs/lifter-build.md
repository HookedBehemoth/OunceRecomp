# Build and iterate on the lifter

This page tells you what to run after a change to the lifter, the patches or the host, how long it takes, and how to
inspect and test what the lifter produced. For the general build and run loop, see the
[development guide](development.md).

## Decide between a lift and a build

| You changed | Run | Typical time |
|---|---|---|
| A file in `host/` or `gpu/direct/`, including `PATCH_AT`, `PATCH_FUNC` and `NV` declarations | `nx2/build.py` | About a minute; plus the partitions a declaration touches |
| The lifter (`nx2/lift2`, except discovery) | `nx2/build.py` | Every partition is lifted and compiled again: several minutes |
| `nx2/lift.py`, `nx2/lift2/discover.py`, an `HLE` or `PATCH_ANSWER` added or removed, the hot list, or the dump | `scripts/relift.sh` | About 1.5 minutes of lift (peak about 6 GB), then the build |

To build, run:

```sh
python -I nx2/build.py --game games/oot --standalone
```

To lift and build in one step, run `scripts/relift.sh` from the checkout you want to rebuild. It refuses to start while
a game runs from that build. Its log is `work/sa/relift.log` (in the main checkout, `work/play/relift.log`).
`scripts/play-refresh.sh` lifts again by itself when the lift's inputs changed (`nx2/lift_mark.py` decides).

## How the build is organized

- **Partitions.** Functions are grouped by guest address into partitions of about 120,000 instructions, and each
  partition is one LLVM module. Functions on the hot list get partitions of their own.
- **Optimization levels.** Hot partitions build at `-O2` (`RX_HOT_OPT`), the rest at `-O1` with dead-store elimination
  added. The hot list is the game description's `hot` file (`games/oot/hot_funcs.bin`); see
  [Game description files](lifter.md#game-description-files).
- **Object cache.** Each partition's object is kept in `work/objcache` under a key of everything that shaped it: the
  lifter's code (without comments), the plan, the compiler flags, its functions, their patches, their summaries, and
  the call-boundary records of every function they call. An unchanged partition is neither lifted nor compiled again.
  Objects that no build links are removed after `HOOKSHOT_OBJCACHE_DAYS` days (default 3).
- **Parallelism.** The build starts a compiler only when free memory holds its estimate, so memory, not core count,
  sets the number of jobs. The build prints the number and why.

## Inspect a function

To see what the lifter made of a function, run:

```sh
python -I nx2/lift2/dump.py --game games/oot <address or label> [--tree] [--opt O1|O2] [--asm]
```

It writes the guest code, the lifter's IR, the LLVM IR and, with the options, the optimized IR and the x86 assembly to
`work/lift2/dump/<address>/`. It prints the function's summary when the plan has one. `--tree` includes the callees.

## Check the lifter

| Check | What it tests | Needs the dump |
|---|---|---|
| `tests/test_lift2*.py` | The decoder, every semantic template on synthetic states, FP edge values, discovery, summaries, the verification wrapper | No |
| `python -I nx2/lift2/selfcheck.py --game` | The decoder against `llvm-mc` on every distinct word of the game | Yes |
| `python -I nx2/test_lift2_leaf.py [--calls]` | Random functions (or call trees) of the game, lifted and run against Unicorn from random states | Yes |
| `python -I nx2/lift2/census.py` | The instruction forms of the game and which class each belongs to | Yes |
| `python -I nx2/lift2/abi_census.py --game games/oot` | Functions that do not follow the AAPCS64 (see [constraints](lifter-costs.md#functions-outside-the-aapcs64)) | Yes |
| The verification build | The summaries against a real run ([The lifter](lifter.md#the-verification-build)) | Yes |

In the game, the smoke test and a benchmark are the landing checks ([Test Hookshot](testing.md)).

## Lifter switches

| Switch | When | What it does |
|---|---|---|
| `--game games/<game>` | Lift, build, tools | The game description. Required. |
| `nx2/build.py --jobs N`, `HOOKSHOT_JOBS` | Build | Parallel jobs, instead of the memory-based default |
| `nx2/build.py --opt O1` | Build | The cold partitions' level |
| `RX_HOT_OPT` | Build | The hot partitions' level (default `O2`) |
| `RX_MARCH` | Build | The target CPU (default: this PC's, `x86-64-v3` on an AVX2 machine) |
| `HOOKSHOT_LIFT_VERIFY=1` | Build | The verification build |
| `HOOKSHOT_LIFT_SUMMARY_JOBS` | Build | The summary job's workers (default 8) |
| `LIFT2_DEFAULT_NAN=1` | Lift | ARM's default NaN for NaNs made from non-NaN operands |
| `HOOKSHOT_LLVM_BIN` | Lift, build | Where clang and the LLVM tools are |
| `nx2/lift.py --max-func N` | Lift | Split functions larger than N instructions where no branch crosses |
