# The lift export: the Rust lifter's reference

`ounce-lift` needs no export: it lifts the game from the dump's exefs, the host's C (its patch declarations: src/patch.rs,
patch_scan.py, probes.py, host_effects.py) and the game description (games/oot/game.toml; its binary files: docs/lifter.md, "Game description files"), with the platform's facts in
ounce/lift/data/horizon.toml (kept equal to nx2/horizon by tests/test_horizon_data.py). In order: the scan, the modules (src/nso.rs), the HLE'd and stubbed symbols and the image (src/eden.rs), the ADR /
ADRP+ADD entries (src/image.rs), the discovery (src/discover.rs), the plan (src/plan.rs), the rows resolved and checked
and the PATCH_FUNCs checked against the plan, the generated sources (src/gen.rs), the summaries and records
(src/summary.rs, src/bounds.rs) and each partition's IR, byte for byte the Python lifter's.

`ounce-lift <export> <out> --check` compares the scan's products (meta.json), the image, the discovery and plan, the
summaries and the records with an export of the Python lifter's results. The Python lifter is gone from the tree, so
no tool writes that export now; an export made earlier still works.

    ounce/target/release/ounce-lift <out dir> [--game games/oot] [--work work] [--exefs <dir>] [--root .]
        [--golden work/ounce/golden]
    ounce/target/release/ounce-lift work/ounce/export <out dir> --check [--golden work/ounce/golden]

The exefs is the dump's (`--exefs`, else OOT_EXEFS, OOT_DUMP/exefs, or the first line of `<work>/dump.txt`, as
nx2/kit.py), checked against games/oot/game.toml's identity first. `--root` is the repository (a64-kit's rt/ sources
for rt64/). The output holds `<name>.ll` per partition, `lift_answers.ll`, `gen/`, `rt64/` and `image/image.bin`;
`--golden` checks gen/, rt64/ and image.bin against the golden's sha256 (its meta.json `files`).

The export is made from the game: it stays under `work/` and never enters git (CONTRIBUTING.md). It is of the build
without `HOOKSHOT_LIFT_VERIFY` and with `omit` on.

## Files

    <out>/meta.json                 Python's patch scan results (rows with their functions, effects, PATCH_FUNC facts),
                                    the platform lists, keys
    <out>/check/plan.json           Python's discovery and plan,
    <out>/check/image.json          the lift's image.json and
    <out>/check/image_entries.json  its entry candidates (before the ADR ones),
    <out>/check/summaries.json      summaries and records: the diagnostic's reference (ounce-lift --check), read by
    <out>/check/bounds.json         nothing else

## meta.json

```json
{
  "format": 5,                      // 3: summaries' inputs; 4: discovery's inputs, no parts; 5: no image, from the exefs
  "commit": "…", "input_key": "…", "code_key": "…", "facts_key": "…", "summary_cache": "…",
  "verify": false, "omit": true,
  "image_sha256": "…",              // the lift's image.bin (the Rust side's must equal it: --check)
  "hle_symbols": ["…", …],          // patch_scan.patches().hle_symbols(): HLE(...) and PATCH_ANSWER(...) symbols
  "os_namespaces": ["account", …],  // nx2/horizon OS_NAMESPACES: an imported free function of one is stubbed
  "setjmp_names": ["__setjmp", …],  // nx2/horizon SETJMP_NAMES
  "noreturn_names": ["abort", …], "noreturn_parts": ["AbortImpl", …],
  "part_insns": 120000,             // game.PART_INSNS
  "answers": [["symbol", value], …],          // the PATCH_ANSWER values, by symbol
  "hle": [["symbol", stops, answer | null, clobbering, "reads"], …],   // each hooked symbol's declared facts
  "values": [ValueRow + "func", …], // every Value probe, in declaration order, with its function (probes.resolve)
  "rows": [AtRow + "func", …],      // every At row, likewise
  "func_names": [[entry, "hos_func_…", "hos_orig_…"], …],   // the PATCH_FUNCs' adapter and lifted body names
  "row_effects": [[pc, "bu", "au", "bd", "ad", guest, replace], …],
  "patch_funcs": [[entry, "reads", "result", orig, [guest, …]], …]
}
```

- `hle`: for every hooked symbol (the hook's C name less `imp_`, as host_effects is asked), its declaration:
  `host_effects.stops`, `.answer` (u64), `.clobbers(...) is not None`, and the mask of `.hle_reads`.
- `row_effects`: per pc, every Value probe's and At row's declared reads and writes (masks) before and after the
  instruction, the guest functions their host functions call (a list, or `"computed"`), and whether a row replaces the
  instruction. They apply to every body holding the pc, not only the row's function.
- `patch_funcs`: each PATCH_FUNC's operands read, its result (a mask of one register or 0), `orig`, its guest callees.
- The partitions, each function's body, jump tables, PLT stub, entries in its body, code pointer sites and the setjmp
  owners' stub constants are the Rust side's now (`plan::parts` makes them as the export's parts were made).

## What the Rust side derives per function

As `lift_part` / `build_ir.build` read them (`Program`): the plan's body (pcs, words); `plt` (target, slot, whether the
target is a code target); the body's pcs that are entries (`inside()`); `jt` restricted to the body's BRs (targets
sorted, unique) and `jt_index` likewise; `code_sites` (`code_ptrs.sites`); its Value and At rows (by `func`); its
PATCH_FUNC names. The summaries see the discovered body (summary.job's input) where the plan traps or cuts it.

### callee(t) and bound(c)

`prog.callee(t)` follows the PLT stubs (at most 4 steps). `prog.bound(c)` is `bounds.call_bounds` (verify off) over the
summaries the Rust side computes. `ounce-lift --check` compares the discovery and the plan (`check/plan.json`: entries,
every body as pc runs and a key of its words, the discovered bodies that differ, insns, jt, jt_index, plt, setjmp
owners, hooks, traps by reason, the hot set, the partitions and each one's census), every function's summary
(`check/summaries.json`: `summary.pack`, masks as hex) and every record the partitions read (`check/bounds.json`) with
Python's, field by field.

### Bound

```json
{"addr": 2147614720, "kind": "lifted", "reads": "…", "clobbers": "…", "keeps": "…", "must": "…", "results": "…",
 "noreturn": false, "symbol": "f_80020000", "tail": "f_80020000", "answer": null}
```

`bounds.Bound` field for field (in check/bounds.json). `kind` is a reason ("lifted", "noreturn", "HLE", "PATCH_FUNC", "setjmp owner",
"no summary", "demoted", "HLE declared clobbering", "PATCH_FUNC with orig, body not proven", and since 5bd7beba
"answer" and "stop"); only "stop" enters the IR (see below). `answer`: null, or the u64 a BL to it answers. `symbol` is the symbol a BL calls (`gcall`), `tail` the one a tail branch jumps to. In `own`, `addr` is the entry.

### Masks

`reads`, `clobbers`, `keeps`, `must`, `results` are register masks over `ir.VARS` (68 bits): bit `i` is `VARS[i]`,
`VARS = x0..x30 (bits 0-30), sp (31), n z c v (32-35), v0..v31 (36-67)`. Written as lowercase hex without prefix or
leading zeros ("0" for none); read with `u128::from_str_radix(s, 16)`.

### ValueRow

```json
{"pc": 2147650000, "word": 3573751839, "fn": "hos_value_…", "args": ["x0", "x1+16", "s0"], "result": "w8"}
```

`probes.Value`: the instruction at `pc` is replaced by `result := fn(args)` (`Value.lower`): `args` are `xN` (i64),
`xN+K` (xN + K, i64) or `sN` (Sn as f32); the call's type is i32 if `result` starts with `w`, else i64; the result is
set with `setx`. In `prog.probes` at `pc`.

### AtRow

```json
{"pc": 2147650000, "word": 2483028004, "fn": "hos_at_…", "mode": "after", "args": ["x0", "#3", "[sp+16]"],
 "result": null, "io": ["x1", "q0"], "guest": null}
```

`probes.At` (`At.post`). `mode`: "before" (`prog.before`), "after" (`prog.after`) or "instead" (`prog.replace`).
Several rows at one `pc` and mode run in the order of this list. `args`: `xN`, `wN`, `sN`, `qN`, `sp`, `#K` (i32
constant; K decimal or 0x hex), `[xN+K]` / `[sp+K]` (the i64 loaded there); `result`: null, `xN`, `wN`, `sN`, `qN`;
`io`: registers passed through a struct (`rowio`/`rowout`); `guest`: null, `"computed"`, or the list of guest
addresses the host function calls: the IR only reads whether it is null (`ctx = guest is not None`).

## Changes at 5bd7beba

Commit 68927513 ("answers and stops lowered at their call sites; the retarget rows become instead rows on the calls")
changed what the lift emits. What the export adds for it (format 2):

1. **`Bound.answer`** (new field; null, or a u64). Set on a record of kind `"answer"`: the callee is a PATCH_ANSWER
   (an SDK function answered by a constant). Its masks are reads 0, clobbers and must x0|x30, keeps x19-x29|SP,
   results x0, noreturn false. `build_ir.build` then:
   - **BL to it is no call**: in place of the `gcall`, `set x30 := const(pc + 4)`, then `set x0 := const(answer)`
     (in that order), then the instruction's `after` rows; then, as for any call, if `pc + 4` is not in the body the
     block ends with `br goto(pc + 4)`. No `held`, no `after_call`, no `gcall` op (so `omit_stores` sees no call).
   - **A tail branch to it** (any `goto(t)` whose `t` is not `inside()` and whose `bound(callee(t)).answer` is not
     null) goes to an exit block keyed `("a", t)` (one per `t`, created on first use, appended to `exits` like the
     `("t", t)` blocks: it gets the flags-out of an exit) holding `set x0 := const(answer)` and terminator `("ret",)`;
     the builder's current block and `cmp` are restored after filling it. Other tail targets keep the `("t", t)` exit.
2. **`kind == "stop"`** (an import the host neither implements nor answers, or an NVN export without a proc): the
   record is noreturn (reads/clobbers/results ALL, keeps 0, must 0, noreturn true), and the `gcall` op carries
   `stop=True`: the emitter writes `  call void @<sym>(ptr %ctx) noreturn` (the ` noreturn` suffix is new); the block
   then ends as any noreturn call (`("noreturn", pc, word)`: a64_trap, unreachable). Tail branches to a stop are
   ordinary tail calls of `tail`.
3. **Instead rows on a BR** (an indirect tail call): an `AtRow` with `mode` "instead" may now sit on a BR (never a
   jump table's BR: the export ran `probes.at(replace=True, jt=...)`'s check). `build` handles it before the B/BR
   terminators: if the instruction is a BR and `pc` has instead rows, run them in order (`At.post`), the block's
   terminator is `("ret",)`, and nothing else of the BR is lowered (no switch, no ijump block). Unlike a BL/BLR
   instead row, no `x30` is set and no `after` rows run.
4. **`answers`** in meta.json and the **answers module**: `game.answers_ir(hle)` is a module of its own (the build's
   `lift_answers.o`), the generic entries of the answered functions, for calls through a code pointer or a BLR. Its
   text, exactly (`%d` the decimal byte offset of x0 in the context, 0; the value printed signed 64-bit, as
   `emit_llvm.int_text(v, 64)`):

       target triple = "x86_64-pc-linux-gnu"
       define void @f_<entry %08x>(ptr noalias nocapture %ctx) {   ; <symbol>
         %x0 = getelementptr i8, ptr %ctx, i64 0
         store i64 <value as signed i64>, ptr %x0, align 8
         ret void
       }
       … one per answer, ascending entry, joined by "\n", the text ending in "\n"

   The golden has it as `lift_answers` (below); the Rust side writes `<dir>/lift_answers.ll`.

## Golden (work/ounce/golden)

The golden is the Python lifter's IR of every partition, `lift_part`'s text exactly (verify off). The tool that wrote it is gone from the tree; a golden made earlier is still read (`ounce-lift --golden`, `ounce/lift/tools/compare.py`):

    work/ounce/golden/meta.json          {"commit", "input_key", "code_key", "facts_key", "partitions": [names],
                                         "seconds"}: made again only when input_key differs (or --force)
    work/ounce/golden/<name>.ll.zst      the partition's IR text, zstd-compressed
    work/ounce/golden/<name>.idx.json    {"f_80020000": [start, end, sha1], …}: each function's byte range in the
                                         uncompressed text (its `define` line to its closing `}` line, inclusive) and
                                         the sha1 of those bytes; keyed by the name it is defined as

    work/ounce/golden/lift_answers.ll.zst, .idx.json   the answers module (game.answers_ir), listed last in partitions
    work/ounce/golden/compile.json       each partition's (and the answers module's) clang flags and `opt` command
                                         (game.part_flags, game.opt_argv: cold O1, hot O2), and the generated C's
                                         flags (build.flags)
    work/ounce/golden/gen/<file>.zst     the lift's generated files : a64_table.c,
                                         a64_hooks.c, a64_code_slots.c, a64_stubs.c, hos_image.c, nvn2_exports.c,
                                         functions.txt
    work/ounce/golden/rt64/<file>.zst    a64.h, a64_rt.c (the build writes them before compiling)
    work/ounce/golden/reference/image/<file>.zst   image.json, functions.json: not compile inputs (below)

The golden is everything generated before the compile of a standalone lift and build, of the lift whose LIFT_OK
meta.json records (`lift`) and the dump in `dump` (game.json: build id, main's sha256). `files` gates the compile
inputs: the sha256 (uncompressed) of every file under gen/ and rt64/, and of `image/image.bin`, the run-time image
hookshot loads (by its sha256 alone: the file is not in the golden). `reference` lists reference/image/image.json and
functions.json with their sha256, not gated: the image stage's intermediate data, which nothing reads at run time (the
Rust pipeline keeps it in memory), kept to check that stage against. Not included at all: the plan and summary pickles
(the lift's hand-off to the build: their content is in the IR), the host's C, a64-kit's, the direct back end's and
third-party sources (checked in, not generated), and the version stamp `build.py` passes to hos_main.c alone.

`ounce/lift/tools/compare.py <dir of Rust .ll>` compares `<dir>/<name>.ll` against the golden, per partition (and
`lift_answers`).
