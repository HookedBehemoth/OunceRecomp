# Continuous integration

This page describes the GitHub Actions workflows of the repository, how their caches are kept, and how to set up the
lift profile, which needs the game's exefs. Read it before you change a workflow.

The repository is public, and so is everything a workflow shows: its logs, its artifacts, and its caches, which any
run of a workflow in the repository can restore, including a run for a fork's pull request. No game data, no code
made from it (lifted IR, objects, disassembly) and no text from the game may reach any of them. Only numbers may:
timings, counts and sizes.

## The workflows

| Workflow | Runs on | What it does |
|---|---|---|
| `ci.yml`, job `checks` | pushes and pull requests to `master` | The game-free checks ([Test OunceRecomp](testing.md#run-the-game-free-checks)), on a runner without a GPU |
| `toolchain.yml` | called by `ci.yml` and `release.yml`, or by hand | The static LLVM, the sysroot, FFmpeg and SDL, each built only when its cache is missing |
| `ci.yml`, job `lift-profile` | pushes and this repository's pull requests to `master`, once it is set up | The lift and compile of each game, measured and compared with the newest `master` run |
| `ci.yml`, job `lift-history` | pushes to `master`, after the lift profile | The lift profile's numbers added to the history on the `gh-pages` branch |
| `release.yml` | tags `v*` | The AppImage (`ounce tool appdir`), attached to the tag's release, or to a new draft release |

The game-free checks need no GPU: the renderer's checks compile parts of the renderer and check SPIR-V with
`spirv-val`, and nothing opens a Vulkan device.

Every step calls `cargo` or an `ounce` command. Give a step that needs more than a few lines of shell a documented
`ounce` command instead. The shared setup, `.github/actions/setup`, installs LLVM 23 from apt.llvm.org, Rust, ISPC
and sccache, each pinned. Every action is pinned by its commit.

## The caches

A repository has 10 GB of caches, and the oldest unused cache is removed first. A cache that a run on `master`
saves is seen by every branch, pull request and tag; a cache that another ref saves is seen by that ref alone. So
the workflows save caches on `master` only, and a recipe change is built once on `master`.

| Cache | Key | Size |
|---|---|---|
| The static LLVM, pruned (`ounce tool static-llvm --prune`) | the recipe's name (`ounce tool cache-keys`) | about 120 MB |
| The sysroot, FFmpeg and SDL | their recipes' names | about 25 MB |
| sccache's folder and the cargo registry, one per kind of job | the Rust version, `Cargo.lock`, the run | at most 1.5 GB each |
| ISPC and sccache | their versions | about 50 MB |
| The checks' C++ objects (Sirit, the translator) | a hash of their sources | about 100 MB |
| The Python packages | `requirements.txt` | about 50 MB |
| The lift profile's baseline, numbers only | the game and the run | 1 KB |

Rust and C compiles go through sccache. The target folders are not cached: a fresh checkout gives every file a new
time, so cargo and Ninja would compile everything again anyway, while sccache knows a compile by its inputs'
contents. Nothing made from the game is cached: the lift profile compiles without the object cache.

## Measure the lift

`ounce tool lift-stats <game> --exefs <folder> --out <file>` lifts and compiles a game's exefs, as a build does, with
no object cache. The lift's own output, which names the game's functions, goes to a log that is never printed, and
the log and the objects are removed at the end. What is printed and written to `<file>` is numbers: the seconds of
each stage of the lift, of the compiles, wall and CPU, the peak memory, the counts of functions and partitions, and
the objects' sizes. The lift reads the exefs alone, not the romfs.

`ounce tool lift-stats --compare <baseline> <file>` compares two measurements. A time or the peak memory that grew by
more than 25 % fails, by more than 10 % warns, and a size that grew by more than 10 % warns. Times are judged only
when both runs had the same CPU and the same number of jobs, and only when they are longer than 2 s. On GitHub the
comparison also goes to the job's summary.

On `master`, every run's numbers become the next run's baseline, after a regression too: the push that brings it in
fails once.

## See the history

Each push to `master` adds its lift profile's numbers to `lift/data.js` on the `gh-pages` branch, with
[github-action-benchmark](https://github.com/benchmark-action/github-action-benchmark), which also keeps a chart page
there: one chart a number, a point a commit, with the CPU and the jobs of the run beside each point. To see it,
enable GitHub Pages once: **Settings > Pages > Build and deployment**, source **Deploy from a branch**, branch
`gh-pages`, folder `/ (root)`. The charts are then at `https://<owner>.github.io/<repository>/lift/`. The first push
to `master` after the setup creates the branch.

Only CI writes `gh-pages`, and the branch holds the charts only: `lift/data.js`, `lift/index.html` and an empty
`.nojekyll`, in a history of its own that shares no commit with `master`. A missing branch, or one holding anything
else, is made again by `lift-history` before it adds the numbers, keeping the charts' data it had.

Every lift profile run, a pull request's too, also writes its numbers, and the comparison with the baseline, to the
job's summary.

## Set up the lift profile

The exefs is about 86 MB for OoT, far over the 48 KB a secret holds. It lives in a private repository, which the job
reads with a read-only deploy key. To set it up:

1. Create a private repository, for example `HookedBehemoth/OunceRecomp-ci-data`.
2. In a new folder, pack each game's exefs, from the dump folder that `ounce config get <game>.dump` names:

   ```sh
   mkdir -p oot dkb
   tar -C <OoT dump>/exefs -cf - . | zstd -19 -T0 > oot/exefs.tar.zst
   tar -C <DKB dump>/exefs -cf - . | zstd -19 -T0 > dkb/exefs.tar.zst
   ```

   The files are about 29 MB (OoT) and 20 MB (DKB), under GitHub's 100 MB limit for a file. Commit them with plain
   Git, not Git LFS: LFS has a monthly bandwidth limit, and every run downloads a file.
3. Commit both files with a message that names nothing of the game, such as `data`, and push them to the private
   repository.
4. Make a key pair with no passphrase: `ssh-keygen -t ed25519 -N '' -C ci-data -f ci-data`.
5. In the private repository, open **Settings > Deploy keys > Add deploy key**, paste `ci-data.pub`, and leave
   **Allow write access** off.
6. In the public repository, open **Settings > Secrets and variables > Actions**:
   - Under **Secrets**, add `OUNCE_CI_DATA_KEY` with the contents of `ci-data`, the private key.
   - Under **Variables**, add `OUNCE_CI_DATA_REPO` with the private repository's name,
     `HookedBehemoth/OunceRecomp-ci-data`.
7. Delete `ci-data` and `ci-data.pub` from your PC.

The job runs once the variable is set. A fork's pull request gets no secrets, so the job does not run for it. When a
game's version changes, replace its file in the private repository.

## Check a workflow locally

Lint the workflows with [actionlint](https://github.com/rhysd/actionlint) before you push one.
