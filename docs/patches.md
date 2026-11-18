# Write a patch

This page shows you how to change what the game does: run host code at a guest instruction, replace a guest function,
or stand in for an SDK function. Read it before you add or change a declaration in `runtime/`.

A patch is a C declaration at file scope in a host source file, next to the host function it runs. The lifter reads
the declarations with a fixed-grammar scanner (`ounce/lift/src/patch.rs`); the compiler expands the same text into the
host function's prototype. The authoritative description of every macro, mode and operand is the comment at the top of
`runtime/patch.h`.

## Choose the kind of patch

| To do this | Declare | Needs a new lift |
|---|---|---|
| Run host code after, before, or instead of one instruction, including an indirect call | `PATCH_AT(pc, word(W), mode, result, fn, operand...) { ... }`, plus `PATCH_AT_SITE(...)` for further sites | No |
| Watch every call of a function without changing it | `PATCH_AT` at the function's entry with mode `before` and result `none` | No |
| Replace a guest function | `PATCH_FUNC(addr, word(W), body(H), aapcs, result, fn, [orig,] [guest(...),] operand...) { ... }` | No |
| Replace a function the image exports by name | `PATCH_FUNC_SYM(symbol, aapcs, result, fn, ...) { ... }` | No |
| Implement an SDK function the game imports | `HLE(symbol, effect...) { ... }` | Yes |
| Answer an SDK function with a constant (no host code: each direct call becomes the constant) | `PATCH_ANSWER(symbol, value, "why");` | Yes |
| Implement an NVN proc | `NV(name, effect...) { ... }` in `runtime/nvn_native.c` | No |

A few real declarations, one per kind:

```c
PATCH_AT(0x8009a474, word(0x94ef225b), instead, none, hos_oot_calc_sleep, as(x0, ns), as(k(0x8009a478), lr)) { ... }
PATCH_AT(0x802640a0, word(0xd503237f), before, none, rt_slots_shadow, as(x0, obj)) { shadow_obj = obj; }
PATCH_FUNC(0x8005ed00, word(0xd503237f), body(0xa0d86942ab307816), aapcs, x0, rt_slots_alloc,
           orig, guest(0x802737a0), as(x0, dta), as(x2, desc), as(x3, flags), as(x30, lr), sp) { ... }
PATCH_FUNC_SYM(strlen, aapcs, x0, hos_strlen, as(x0, s)) { return strlen((const char *)GP(s)); }
PATCH_ANSWER(_ZN2nn3aoc17CountAddOnContentEv, 0, "0: no add-on content");
```

## Write the declaration

1. Read the function first. To see its guest code and lifted output, run:

   ```sh
   ounce/target/release/ounce-lift dump --game games/oot <address or label>
   ```

2. Print the site's facts and a declaration to start from:

   ```sh
   ounce/target/release/ounce-lift site --game games/oot <address or label>
   ```

   It prints the instruction and its neighbors, every path into the instruction (branches, jump tables, other
   entries), the registers live in and out, the function's call-boundary record, the declarations already in that
   function, and a `PATCH_AT` with its `word(...)` filled in. At a function's entry it also prints the `PATCH_FUNC`
   form with its `body(...)` hash. The command takes a few seconds.

3. Paste the declaration next to its host function and write the body. Declare only the operands the body uses: each
   one widens what the function's callers must keep in the context.

4. If the file is new, add it to the `patches` list in `games/oot/game.toml`. A patch file holds the game's addresses;
   the runtime holds none. Where the runtime's own work needs the game, it calls a hook point of
   `runtime/game_hooks.h`, and the game's `patches/hooks.c` fills it.

5. Check every declaration against the plan:

   ```sh
   ounce/target/release/ounce-lift check --game games/oot --all
   ```

   It runs the checks the build runs and prints each error with its `file:line`. When everything fits, it ends with
   `N declarations checked against the plan, 0 errors`.

6. Build ([Build and iterate](lifter-build.md)). Only the partitions whose functions the declaration touches are
   lifted and compiled again.

To see what a declaration costs, run `check <file:line>`, or `check -` with a proposed declaration on standard input.
It recomputes the summaries without the declaration and lists the functions and call sites whose kept registers
change. It takes about two minutes and several GB of memory, so run it like any other heavy job. `report` prints the
local cost of every declaration, costliest first, in about half a minute, and each answer's direct call sites.

## Rules the scanner and the build enforce

- `word(...)` is the instruction's or the function's first word, and `body(...)` is a hash of the function's body. Both
  are checked at the build, so a different game version stops the build instead of patching the wrong code. The
  build's error names the expected values.
- A declaration stands at file scope, outside `#if`, and is never produced by another macro.
- A `before` row cannot sit on a branch target, a jump-table target, or another function's entry. Its own function's
  entry is allowed: the row then runs on every call.
- An `instead` row replaces a BL or BLR (the call is not made), or replaces another instruction's result. The
  declared result must then be that instruction's destination register.
- A row's result and `io(...)` registers must be caller-saved or written by the instruction itself, so a row never
  changes what its function preserves.
- The build compares the compiled host's declarations with the scan. A declaration that one of them sees and the other
  does not stops the build.

## Patch an indirect call

To change what a BLR (or a BR that is a tail call) does, put an `instead` row on that instruction. Pass the target
register as an operand, and assert in the host function that it is the proc you expect. To keep the original behavior,
call the target through `guest_call`, declared with `guest(computed)`. A row never rewrites a code pointer, and no
host-call slot is handed to the game for a patch. `games/oot/patches/job_sync.c` and `games/oot/patches/depth_copy.c` are worked
examples.

`guest(computed)` counts as reading every register, so such a row costs more than one that names its guest callees;
`report` shows it.

## What the host function gets

- Host functions take plain values and guest addresses, never the guest context. To use a guest object, define a
  struct of the fields you use at their guest offsets, with a `_Static_assert` for each offset.
- A frame slot the host needs is handed over as an operand, `ld(sp, K)`; the host does not read guest frames itself.
- To call guest code from a host function, declare `guest(<address>, ...)` and call through `guest_call`. The call runs
  on the stack you give it and puts every register back afterwards.
- A `PATCH_FUNC` with `orig` can run the function's own lifted body with `guest_orig(orig, ...)`. Arguments you pass
  go to `x0` and up; every other register is the call's own.
- An `HLE` or `NV` function that reads registers beyond the AAPCS64 arguments declares `reads(reg, "why")`. One that
  does not preserve the callee-saved registers declares `clobbers("why")`. Every effect needs its reason.

The check `host_reads` (`cargo test`) compiles the host and fails if a host function calls guest code that its
declaration does not name, or reads registers it does not declare.

## Calls that stop the game

- A call of an SDK import that the host neither implements nor answers stops the game and names the function.
- A call of an NVN proc the host does not implement stops the game and names the proc.
- An `HLE` or `PATCH_ANSWER` of a symbol that no module of the image has stops the lift.

A declaration of a symbol that only some SDK versions have names them: `sdk(22)` (22 and later) or `sdk(19, 20)`,
an effect of an `HLE` or the last operand of a `PATCH_ANSWER`. The lift reads the image's SDK version from its `sdk`
module and leaves out the declarations of other versions. Inside the range, a missing symbol still stops the lift.

If one of these stops your run, implement the function or add a `PATCH_ANSWER` with the reason the constant is enough.
