# The lift export: what the Rust lifter reads

`nx2/lift_export.py` writes everything `nx2/lift2/game.py`'s `lift_part` needs to produce a partition's LLVM IR,
already resolved: the partitions as `prepare()` makes them, every function's instructions, and every fact of other
functions, of the patches and of the summaries the lift reads, as plain data. The Rust lifter needs no Python, no plan
pickle, no host C scan and no summaries: it reads this and emits the same IR text, byte for byte (phase 1).

    work/venv/bin/python -I nx2/lift_export.py --game games/oot work/ounce/export

The export is made from the game: it stays under `work/` and never enters git (CONTRIBUTING.md).

The export is of the build without `HOOKSHOT_LIFT_VERIFY` (`verify` off: no `body_`/`hchk_`/`sum_` wrappers, no
`sitepoison`) and with `omit` on (`omit_stores` runs), as `prepare()` lifts.

## Files

    <out>/meta.json                 the export's facts and the partitions' list
    <out>/parts/<name>.json         one per partition: its functions and the records they read

All files are UTF-8 JSON (one object, no comments). Numbers are JSON integers (all fit in `u64`; addresses are guest
virtual addresses, decimal). The only values wider than 64 bits, the register masks, are hex strings (see *Masks*).
Lists keep the order given here; where an order matters it says so.

## meta.json

```json
{
  "format": 1,
  "commit": "1e054a23...",          // git HEAD of the Python lifter the export was made at ("+dirty" if the tree was)
  "input_key": "…",                 // lift_export.input_key: every input of the IR (the golden's stamp is the same key)
  "code_key": "…",                  // game.code_key(): the lifter's code as the build keys it
  "facts_key": "…",                 // the plan's facts_key
  "summary_cache": "summaries_….pickle",
  "verify": false,
  "omit": true,
  "code_targets": [2147614720, …],  // sorted: code_ptrs.targets(plan) (every function-table entry less the modules' bases)
  "sj_rets": [[entry, r0, r1, r2, r3], …],   // sorted by entry: the setjmp owners' sjcall_ stub constants
  "partitions": [
    {"name": "lift_c000", "tag": "c", "index": 0, "functions": 1234, "insns": 120017},
    …
  ]
}
```

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
  "functions": [ Function, … ],     // the partition's functions in lift order (ascending entry)
  "bounds": [ Bound, … ],           // every call-boundary record these functions read, sorted by addr
  "redirect": [[t, c], …]           // sorted by t: callee(t) = c for every t the functions reach where c != t
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
  "func": null | {"name": "hos_func_…", "orig_name": "…"},   // a PATCH_FUNC at this entry
  "own": Bound                       // bound.own(entry): the record omit_stores takes for the function itself
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
- `own`: `prog.bound.own(entry)`: equal to `bound(entry)` except for a PATCH_FUNC (its body's own record).

### callee(t) and bound(c)

`prog.callee(t)` (PLT stubs followed, at most 4 steps) is resolved: `callee(t)` is the `c` of the partition's
`redirect` pair for `t`, else `t` itself. `prog.bound(c)` is the partition's `bounds` entry with `addr == c`. The
partition carries the record of `callee(t)` for every `t` whose record its functions' lift reads: every BL target;
every B/B.cond/CB(N)Z/TB(N)Z target and every `pc + 4` that is not `inside()` (not in the body, or another
function's entry in it); every jump-table case of a BR in the body (`jt` and `jt_index` targets) that is not
`inside()`; a PLT stub's `target`; plus every function's own entry. A lookup that finds no record is a bug of the export: stop.

### Bound

```json
{"addr": 2147614720, "kind": "lifted", "reads": "…", "clobbers": "…", "keeps": "…", "must": "…", "results": "…",
 "noreturn": false, "symbol": "f_80020000", "tail": "f_80020000"}
```

`bounds.Bound` field for field. `kind` is a reason ("lifted", "noreturn", "HLE", "PATCH_FUNC", "setjmp owner",
"no summary", "demoted", "HLE declared clobbering", "PATCH_FUNC with orig, body not proven"); it does not enter the
IR. `symbol` is the symbol a BL calls (`gcall`), `tail` the one a tail branch jumps to. In `own`, `addr` is the entry.

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

## Golden (work/ounce/golden)

`ounce/lift/tools/golden.py` writes the Python lifter's IR of every partition, `lift_part`'s text exactly (verify off):

    work/ounce/golden/meta.json          {"commit", "input_key", "code_key", "facts_key", "partitions": [names],
                                         "seconds"}: made again only when input_key differs (or --force)
    work/ounce/golden/<name>.ll.zst      the partition's IR text, zstd-compressed
    work/ounce/golden/<name>.idx.json    {"f_80020000": [start, end, sha1], …}: each function's byte range in the
                                         uncompressed text (its `define` line to its closing `}` line, inclusive) and
                                         the sha1 of those bytes; keyed by the name it is defined as

`ounce/lift/tools/compare.py <dir of Rust .ll>` compares `<dir>/<name>.ll` against the golden, per partition.
