# The lift export: what the Rust lifter reads

`nx2/lift_export.py` writes everything `nx2/lift2/game.py`'s `lift_part` needs to produce a partition's LLVM IR: the
partitions as `prepare()` makes them, every function's instructions, the facts of the patches, and what the
whole-program summaries (nx2/lift2/summary.py) and the call-boundary records (nx2/lift2/bounds.py) are computed from,
as plain data. The Rust lifter computes the summaries and the records itself (src/summary.rs, src/bounds.rs) and emits
the same IR text, byte for byte. It needs no Python, no plan pickle and no host C scan.

    work/venv/bin/python -I nx2/lift_export.py --game games/oot work/ounce/export

The export is made from the game: it stays under `work/` and never enters git (CONTRIBUTING.md).

The export is of the build without `HOOKSHOT_LIFT_VERIFY` (`verify` off: no `body_`/`hchk_`/`sum_` wrappers, no
`sitepoison`) and with `omit` on (`omit_stores` runs), as `prepare()` lifts.

## Files

    <out>/meta.json                 the export's facts, the summaries' inputs and the partitions' list
    <out>/parts/<name>.json         one per partition: its functions
    <out>/check/summaries.json      Python's summaries and records: the diagnostic's reference (ounce-lift --check),
    <out>/check/bounds.json         read by nothing else

All files are UTF-8 JSON (one object, no comments). Numbers are JSON integers (all fit in `u64`; addresses are guest
virtual addresses, decimal). The only values wider than 64 bits, the register masks, are hex strings (see *Masks*).
Lists keep the order given here; where an order matters it says so.

## meta.json

```json
{
  "format": 3,                      // 2 since 5bd7beba (answers, stops); 3: the summaries' inputs, no records
  "commit": "1e054a23...",          // git HEAD of the Python lifter the export was made at ("+dirty" if the tree was)
  "input_key": "…",                 // lift_export.input_key: every input of the IR (the golden's stamp is the same key)
  "code_key": "…",                  // game.code_key(): the lifter's code as the build keys it
  "facts_key": "…",                 // the plan's facts_key
  "summary_cache": "summaries_….pickle",
  "verify": false,
  "omit": true,
  "code_targets": [2147614720, …],  // sorted: code_ptrs.targets(plan) (every function-table entry less the modules' bases)
  "sj_rets": [[entry, r0, r1, r2, r3], …],   // sorted by entry: the setjmp owners' sjcall_ stub constants
  "answers": [[entry, "symbol", value], …],   // sorted by entry: the answered functions (new at 5bd7beba, below)
  "partitions": [
    {"name": "lift_c000", "tag": "c", "index": 0, "functions": 1234, "insns": 120017},
    …
  ],
  // the summaries' and records' inputs (format 3)
  "entries": [a, …],                // sorted: plan["entries"], every function entry (lifted and hooked)
  "plt": [[stub, target, slot], …], // sorted by stub: plan["plt"]; callee(t) follows it, at most 4 steps
  "names": [[entry, "symbol"], …],  // the image's symbols (the summary input's names): the noreturn seeds match them
  "noreturn_names": ["abort", …], "noreturn_parts": ["AbortImpl", …],   // nx2/horizon NORETURN_NAMES, _PARTS
  "hle": [[addr, "symbol", stops, answer | null, clobbering, "reads"], …],   // by addr: the HLE'd functions
  "row_effects": [[pc, "bu", "au", "bd", "ad", guest, replace], …],         // by pc: summary.rows()
  "patch_funcs": [[entry, "reads", "result", orig, [guest, …]], …],         // by entry: summary.func_facts
  "summary_bodies": [{"entry": e, "pcs": [[pc, n], …], "words": "…"}, …]
}
```

- `hle`: plan["hle"] (equal to plan["hooks"] and to gen/a64_hooks.c's map: the export checks) with each symbol's
  declaration: `host_effects.stops`, `.answer` (u64), `.clobbers(...) is not None`, and the mask of `.hle_reads`.
- `row_effects`: per pc, every Value probe's and At row's declared reads and writes (masks) before and after the
  instruction, the guest functions their host functions call (a list, or `"computed"`), and whether a row replaces the
  instruction. They apply to every body holding the pc, not only the row's function.
- `patch_funcs`: each PATCH_FUNC's operands read, its result (a mask of one register or 0), `orig`, its guest callees.
- `summary_bodies`: the functions whose body the summaries see differs from the lifted one (`pcs`/`words` of the
  Function): the summary input holds the discovered body, the plan's body a UDF trap (game.write_plan: a module's
  first word, not an instruction). Four in OOT, the modules' headers. An inconsistency of the Python lifter kept to stay
  identical: one body per function is the fix, after the port.

- `partitions` is `prepare()`'s list in its order: first the cold partitions (`tag` "c", functions not on the hot
  list), then the hot ones (`tag` "h"), each group by ascending entry, cut after the function that brings the
  partition to `PART_INSNS` (120000) instructions or more. `index` is the position in this list (`k` in `prepare`),
  `name` is `"lift_%s%03d" % (tag, index)` (the object `prepare` writes is `<name>.o`). The cold level is the build's
  `-O1`, the hot one `-O2`; that is the compile's business, not the IR's.
- `code_targets`: `Program.code_targets`. The IR reads it in two places: a PLT stub's `x17` (see `plt` below, already
  resolved per function) and the check in `build_ir.build` that a constant formed by `ADR`/`ADRP+ADD`/... outside a
  code pointer site is not an entry (`ValueError`, "not a code_ptrs.sites shape"). Only that check needs the global
  set.
- `sj_rets`: `plan["sj_rets"]`: for a setjmp owner `e`, the four integers `lift_part` passes to `hos_sj_call` in the
  `sjcall_%08x` stub it appends. A partition needs those of the owners whose `sjcall_` symbol its IR calls (a record
  whose `symbol` is `sjcall_<addr>`).

## parts/<name>.json

```json
{
  "name": "lift_c000",
  "tag": "c",
  "index": 0,
  "functions": [ Function, … ]      // the partition's functions in lift order (ascending entry)
}
```

### Function

```json
{
  "entry": 2147614720,
  "pcs": [[2147614720, 12], [2147614800, 3]],   // runs [first pc, count]: pc, pc+4, … (the body's pcs, ascending)
  "words": "d10083ffa9017bfd…",                 // the instruction words, 8 lowercase hex digits each, in pcs order
  "plt": null | {"target": t, "slot": s, "target_is_code": bool},
  "body_entries": [a, …],           // sorted: the body's pcs that are function entries (plan["entries"]), entry included
  "jt": [[br, [t0, t1, …]], …],     // sorted by br: the jump tables of the BRs in the body, targets sorted, unique
  "jt_index": [[br, load, reg, opt, [t0, …]], …],   // sorted by br: plan["jt_index"] for the BRs in the body
  "code_sites": [[pc, entry, page | null], …],      // sorted by pc: code_ptrs.sites (page null: an ADR site)
  "values": [ValueRow, …],          // its Value probes, in declaration order
  "rows": [AtRow, …],               // its At rows, in declaration order
  "func": null | {"name": "hos_func_…", "orig_name": "…"}    // a PATCH_FUNC at this entry
}
```

What `lift_part` / `build_ir.build` do with these, as `Program`:

- `pcs`, `words`: `plan["bodies"][entry]`. `word(pc)` is the word at pc. A body may have gaps (runs).
- `plt`: non-null when `entry in plan["plt"]` (a PLT stub). Then `build` emits: `x16 = slot`; `x17 =
  codeaddr(target)` if `target_is_code` else `const(target)`; the block's terminator `("tail", bound(callee(target)).tail)`.
- `body_entries`: the `t in prog.entries` test of `inside()`: a branch to `t` stays in the function iff `t` is in the
  body and (`t == entry` or `t` not in `body_entries`). Only pcs of the body are ever asked, so this is the whole of
  `prog.entries` the lift reads.
- `jt`: `prog.jt` restricted to the body's BRs (`build` only looks up BRs at body pcs). `jt_index`: `prog.jt_index`
  restricted to `br in body` (the loop in `build` skips the others); `build` itself still checks `load in body` and
  that no probe, before row or after row lies in `load..br`. `opt`: 2 UXTW, 6 SXTW, other: none.
- `code_sites`: `prog.code_sites` (`code_ptrs.sites` of this body).
- `values`, `rows`: `prog.probes`, `prog.before`, `prog.after`, `prog.replace` (below). The Python side has already
  run every check `probes.find` / `probes.at` make (word matches, Value result is the destination, ...): an export
  exists only if they all passed.
- `func`: a PATCH_FUNC (`patch_scan.check_funcs`): the lifted function is named `orig_name` instead of
  `f_%08x`, and `lift_part` appends `FUNC_STUB` with `fn = name`.
- the record omit_stores takes for the function itself is `bound.own(entry)`: equal to `bound(entry)` except for a
  PATCH_FUNC (its body's own record).

### callee(t) and bound(c)

`prog.callee(t)` follows the PLT stubs (`plt`, at most 4 steps). `prog.bound(c)` is `bounds.call_bounds` (verify off)
over the summaries the Rust side computes from the inputs above (summary.summarise_parallel's fixed point, the results
and the fall-through demotion, then bounds.make / func_bound). `ounce-lift --check` compares every function's summary
(`check/summaries.json`: `summary.pack` with the masks as hex and the demotion reasons as a list) and every record the
partitions read (`check/bounds.json`: `{"bounds": [Bound…], "own": [Bound… of the PATCH_FUNCs]}`) with Python's, field
by field.

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

`ounce/lift/tools/golden.py` writes the Python lifter's IR of every partition, `lift_part`'s text exactly (verify off):

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
    work/ounce/golden/gen/<file>.zst     the lift's generated files (nx2/lift.py --standalone --write): a64_table.c,
                                         a64_hooks.c, a64_code_slots.c, a64_stubs.c, hos_image.c, nvn2_exports.c,
                                         functions.txt
    work/ounce/golden/rt64/<file>.zst    a64.h, a64_rt.c (nx2/rtgen.py: the build writes them before compiling)
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
