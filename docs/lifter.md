# The lifter

Hookshot's lifter turns the game's AArch64 code into native x86-64 code: every function of the game is decoded, built
into a typed SSA IR, emitted as LLVM IR and compiled by clang. This document is its design: the pipeline, the decisions
and the measurements behind them, the IR, how calls work, the whole-program summaries, floating point, memory, the
host interface, polls and traps, the build, how it is checked, the measured results, the open work, and a short
history.

Measurements: clang/LLVM 23.1.3 on the development PC (Zen, x86-64-v3), unless a section says otherwise.

## 1. The pipeline

| step | where | what |
|---|---|---|
| image | `nx2/image.py`, `third_party/a64-kit/hos/nx_image.py`, `nx2/nso2.py` | the modules loaded at their addresses (rtld, main, subsdk*, sdk), relocated, with the entries from symbols, unwind info and relocations; the HLE functions and stubs hooked (`nx2/standalone.py`) |
| entries | `nx2/adr_entries.py` | code addresses the code builds with ADR / ADRP+ADD (callbacks reached only through a pointer) |
| discovery | `nx2/lift2/discover.py` | function bodies (an entry plus what it reaches without crossing another entry), jump tables, PLT stubs, setjmp owners, the split of very large functions |
| plan | `nx2/lift2/game.py` `write_plan` | every function's instructions and the program's facts in `work/sa/plan.pickle`; the summaries when asked (section 7) |
| runtime tables, image | `nx2/lift2/tables.py`, `nx2/standalone.py` | `a64_table.c` (the function table `a64_lookup` searches), `a64_hooks.c` (hooked addresses), `hos_image.c`, `a64_stubs.c`, `nvn2_exports.c` in `work/sa/gen` |
| IR | `nx2/lift2/enc*.py`, `sem*.py`, `build_ir.py`, `ir.py` | the decoder (tables of the encodings), the semantics of each form as IR, the IR of a function |
| LLVM IR | `nx2/lift2/emit_llvm.py` | one textual module per partition |
| build | `nx2/lift2/game.py` `prepare`, `nx2/build.py` | partitions lifted in worker processes and compiled by clang (`-x ir -`, no IR on disk), cached by their inputs; the runtime and host compiled and linked |
| runtime | `nx2/rtgen.py` from `third_party/a64-kit/rt` | the context header (`a64.h`: `GuestContext`, guest memory) and `a64_rt.c` (`a64_lookup`, `a64_icall`, `a64_ijump`, `a64_trap`, the exclusive monitor, the counter) with the 64-bit guest window |

`nx2/lift.py` runs the steps up to the plan and the tables (`scripts/relift.sh`); `nx2/build.py` the rest. The plan
holds instructions, not lifted code: a change to the lifter needs only a build (its partitions' input keys change).

## 2. Output format: LLVM IR

### 2.1 Measurements (when the format was chosen)

The corpus is 120 random cold functions (8,450 guest instructions) from six address ranges of the image. Each variant was compiled with
the build's flags at -O1. The counts are x86 instructions in the object.

| variant | instructions |
|---|---|
| a64-kit's C (registers in the context; the lifter before this one) | 29,442 |
| all registers C locals; x0-x3, SP, ctx as arguments; x0/x1 returned; `ccc` | **24,189** |
| the same, emitted as IR by clang and compiled from `.ll` | 24,189 (identical) |
| same signature, `tailcc` | 24,699 (+2.1 %) |
| same signature, `regcall` (C attribute) | 25,753 (+6.5 %) |
| x0-x8 + SP as arguments, `preserve_none` | 26,848 |
| x0-x8 + SP as arguments, `ccc` (4 on the stack) | 30,135 |
| locals + prologue/epilogue saves of x19-x30 deleted | 23,565 (-2.6 %) |
| the same without page marks | 21,092 (-28 % against as lifted) |

Metadata, applied to the `ccc` variant's IR:

| metadata | instructions |
|---|---|
| `memory(read)` on the 27 callees that the whole lift proves store-free (53,530 of 295,270 functions are, transitively; 9 % of direct call edges reach one) | 24,204 (0 %) |
| page-mark stores made plain and put in their own `!alias.scope`, all other accesses `!noalias` to it | 24,099 (-0.4 %) |
| every callee `memory(read)`: an upper bound, not correct | 22,727 (-6 %) |

The context already does not alias guest memory: `restrict` plus `noescape` on the pointer gives LLVM that fact.
A guest store makes LLVM reload only other guest loads, and those really may alias. TBAA cannot separate guest
accesses, because guest code type-puns.

Return registers (`llc`, a callee returning N values, checked for sret demotion):

| convention | i64 results in registers | `<4 x float>` results |
|---|---|---|
| ccc, fastcc, tailcc, preserve_none, ghccc, preserve_most | 3 | 4 |
| swiftcc | 4 | 4 |
| x86_regcallcc | 11 | 16 |
| C struct return (SysV) | 2 | 1-2 (a struct of 16 bytes or less) |
| C `__attribute__((regcall))` | 6+ | 6+ |

A mixed `{i64, i64, <2 x i64>, <2 x i64>}` with `ccc` returns in rax, rdx, xmm0, xmm1. The same C struct is 48 bytes
and goes to memory.

Tail calls: guest functions end in a branch to another function's entry about once per 4.5 functions (690 in 3,138
functions of one address range). With per-function signatures these become tail calls between different prototypes:
- C `musttail` refuses them, including with `swiftasynccall`.
- IR `tailcc` with `tail call` works.
- `ccc` with `musttail` and identical prototypes works, and needs no calling convention of its own: the lifter uses
  it (section 6).

Front end: 3 MB of C from one address range at -O1 takes 15.9 s from C, 16.1 s from bitcode and 16.0 s from textual IR. The
textual IR is 42 MB against 3 MB of C. The optimiser and code generator are the cost; parsing is not.

NEON permutes: C vector extensions (`__builtin_shufflevector`) give the same code as IR `shufflevector`: EXT #8 is one
`vpalignr`, DUP one `vshufps`, FADDP one `vhaddps`.

### 2.2 Verdict

**Emit LLVM IR.** The reasons are narrower than "LLVM allows a lot of metadata":

1. **Mixed integer and vector results in registers.** A guest function's results are x0/x1 and v0/v1 (HFA and
   floating-point returns). `ccc` returns them in rax, rdx, xmm0, xmm1. In C that needs a struct over 16 bytes,
   which goes to memory, or `regcall`, which costs 6.5 % because it leaves fewer callee-saved registers.
2. **`poison` for the arguments and results a callee does not use**, under one uniform prototype (the fast entries,
   section 15). In C the same effect needs an uninitialised variable, which is undefined behaviour.
3. **Exact intrinsics** with ARM's value semantics: `llvm.fptosi.sat` (FCVTZS: NaN to 0, saturating; one
   `vcvttss2si` plus two `cmov`), `llvm.minimum`/`llvm.maximum` (FMIN/FMAX), `llvm.minimumnum`/`llvm.maximumnum`
   (FMINNM/FMAXNM), `llvm.fma`, `llvm.roundeven`/`round`/`floor`/`ceil`/`trunc` (FRINTN/A/M/P/Z), overflow intrinsics.
   No C undefined behaviour to dodge (shift amounts, signed overflow).
4. **The IR is SSA with block arguments**, which maps one to one onto LLVM `phi`. C would need an out-of-SSA step.
   This step is small, so it is a convenience, not a reason.

Claims that did **not** hold, and are not reasons:
- Alias metadata, `memory()` effects and clobber facts on declarations measured 0 to 0.4 %. The upper bound,
  declaring everything read-only, is 6 % and incorrect. The register traffic they were meant to remove is removed by
  passing registers as values (section 6), and that works in C as well.
- Front-end time: no difference.
- Guaranteed tail calls between different signatures: one prototype for every entry makes them unnecessary.

For integer code, C with a uniform prototype gets the same code: 24,189 instructions either way. The IR advantage
is in floating-point results, `poison` and the intrinsics.

**Version pin.** The emitter writes textual IR for one LLVM major. `nx2/lift2/llvm.py` refuses a clang whose major
differs. Textual IR (`.ll`) is used, not bitcode:
it is readable and diffable, and it costs nothing measurable at parse time.

## 3. Instruction coverage

Census of every word of the modules' `.text`: 18.16 M words, 2.03 M distinct. Forms are the capstone mnemonic plus
the operand shape (register class, vector arrangement, addressing mode); data in `.text` is included and is little.

| class | forms | forms seen 10+ times | words | share | mnemonics |
|---|---|---|---|---|---|
| integer data processing | 331 | 278 | 6.81 M | 37.5 % | 83 |
| integer load/store (incl. atomics) | 269 | 228 | 5.58 M | 30.7 % | 75 |
| branches | 22 | 19 | 3.36 M | 18.5 % | 13 |
| vector/FP load/store | 367 | 240 | 0.57 M | 3.1 % | 16 |
| udf/nop (padding, data) | 2 | 2 | 0.55 M | 3.1 % | 2 |
| FP scalar | 121 | 89 | 0.53 M | 2.9 % | 46 |
| NEON | 534 | 347 | 0.50 M | 2.7 % | 182 |
| system (MRS/MSR, barriers, PAC, hints, SVC) | 53 | 23 | 0.27 M | 1.5 % | 24 |
| **total** | **1,699** | **1,226** | | | **396** |

398 forms cover 99 % of words and 1,217 cover 99.99 %. All 1,699 must decode, because every reachable word must
lift; there is no fallback. They reduce to about 160 semantic templates, parametrised by width, signedness,
addressing mode and arrangement:

| templates | count |
|---|---|
| integer | ~40 |
| integer load/store and atomics | ~25 (address modes are shared) |
| branches | ~8 |
| FP scalar | ~25 |
| NEON | ~55 (lane-generic over arrangements) |
| system | ~10 |

`nx2/lift2/census.py` regenerates the list into `work/lift2/forms.txt` and reports what the lifter covers: every form
the game reaches decodes (`nx2/lift2/selfcheck.py` compares the decoder with llvm-mc on every distinct word).

## 4. Discovery

`nx2/lift2/discover.py`. A function is an entry plus everything reachable from it without crossing another entry;
a branch into another entry is a tail call. The entries come from the image (symbols, unwind info, relocations,
setjmp's return addresses) and from the code (`nx2/adr_entries.py`).

- **Jump tables.** The straight-line code before a BR is run symbolically: the target is a base plus an entry of a
  constant table shifted left; the table address may be hoisted out of the window (the constants reaching the BR
  through the function's control flow, a hoisted ADRP+ADD, or the one constant the function ever gives the register,
  ADR included), and a register reused for something else between the table load and the BR is resolved at the load.
  The bound comes from CMP plus B.HI/B.HS, CCMP, an AND mask or a copy of the index; without one, entries are read
  until one leaves the function's extent (from the unwind info). The window starts after the last return or branch,
  RETAA/RETAB and BRAA* included.
- **Pointer authentication**: RETAA/RETAB are returns, BRAA*/BLRAA* are BR/BLR through Xn (the modifier ignored).
- **PLT stubs** (`adrp x16 / ldr x17 / add x16 / br x17`) are resolved through the relocated GOT: a call of one calls
  its target.
- **setjmp owners**: the functions that call `__setjmp`, with the call's return addresses as extra entries.
- **Very large functions** get extra entries where no branch crosses the cut (`--max-func`).

Over the whole game: 294,523 functions, 1,475 jump tables (1,385 bounded), no BR without a table, 9,917 PLT stubs.
`tests/test_lift2_discover.py` runs it on a synthetic image.

## 5. The IR

One `Function` per guest function entry (`nx2/lift2/ir.py`). The IR is plain Python data.

**Types.** `i1 i8 i16 i32 i64 i128`, `v128` (canonical `<2 x i64>`; operations bitcast to lane types
`<16 x i8>` ... `<4 x float>` `<2 x double>`), `f32 f64 f16` (scalar FP values are kept in their bit patterns where
only moved), `ptr` (the context only).

**Values.** SSA values. Blocks take parameters instead of phis. Guest registers do not exist inside the IR: the
front end keeps a register-to-value map per block, and block parameters carry what is live in. X registers are
`i64`. W writes zero-extend explicitly. N, Z, C, V are four `i1` values. V registers are `v128`.

**Operations.**

| group | operations (ARM semantics stated where they differ from LLVM's defaults) |
|---|---|
| integer | add sub mul umulh smulh udiv sdiv (divisor 0: result 0, explicit select), and or xor not, shl lshr ashr ror (amount masked to the width), clz cls rbit rev rev16 rev32, extract(bitfield) insert(bitfield), sext zext trunc, select, icmp |
| flags | `addflags(a,b,carry_in,width)` -> (result, n, z, c, v) via `llvm.uadd/sadd.with.overflow`; `subflags` likewise; logical flags (n, z, c=0, v=0); `cond(code, n,z,c,v)` folded to one `icmp` when all four come from one compare (CMP + B.LT becomes `icmp slt`) |
| memory | `gaddr(i64) -> ptr` = `getelementptr i8, ptr inttoptr (i64 0x100000000000 to ptr), i64 a`; `load(ptr, w, sext, order)` with order plain, acquire; `store(ptr, v, w, order)` with order plain, seqcst; `rmw(op, ptr, v, w)` for the LSE ops; `cas(ptr, exp, new, w)`, `cas2` (CASP, `cmpxchg16b`); `excl_load`/`excl_store`; `fence(full)`, `fence(compiler)` |
| vector | lane ops on `v128` via LLVM vector ops, `shufflevector`, `insertelement`/`extractelement`, x86 intrinsics where nothing generic exists (`pshufb` for TBL/TBX, `pmaddubsw` for UDOT), saturating ops via `llvm.[us]add.sat` |
| FP | fadd fsub fmul fdiv `llvm.sqrt` `llvm.fma` fneg fabs, min/max as above, `fcmp` -> NZCV (unordered = 0011), conversions (`fptosi.sat`, `sitofp`, `fptrunc`/`fpext`, F16 via F16C), FRINT*, `frecpe/frsqrte` (calls into a table routine), `mrs_fpsr`/`msr_fpsr`/`msr_fpcr` (section 7) |
| control | br, condbr, `switch(index)` for jump tables (`(x - base) >> k`, an indexed table, not a compare tree), ret, `gcall(sym)` (BL), `icall(target)` (BLR), tail(sym) (B to an entry), ijump(target) (BR without a table), `svc(imm)`, `trap(pc, word)`, `poll` |
| host interface | probes: a host call before or after an instruction, or in place of its result (section 10) |

**Emission.** One textual LLVM module per partition (section 12). Each IR value is one LLVM value, each block one
basic block, and block parameters become `phi`s. No `alloca` is emitted except for host-stack temporaries.

## 6. Calls

Every function is `void f_<addr>(ptr noalias nocapture %ctx)`: the name the function table, the hooks and the host
use, and what every call reaches. Inside a function the guest registers are SSA values; the entry block loads the
registers the function reads before writing them from the context, and every register write is also stored to the
context where it happens (write-through: LLVM drops the stores a later one overwrites, and a return stores nothing).

- **BL**: X30 set to the return address, then `f_<callee>(ctx)`; after it every register is the context's again
  (reads load it).
- **B to another entry, or falling into one**: `musttail call f_<callee>(ctx)` (one prototype for every function).
- **BLR**: `a64_icall(ctx, target)` (the runtime's `a64_lookup`: a direct-mapped cache in front of a binary search
  of the function table).
- **BR**: a jump table's `switch` (cases inside the function are branches, the others tail calls; the default
  `a64_ijump`), else `a64_ijump(ctx, target)`.
- **PLT stubs**: a call of one calls its target; a stub reached indirectly sets x16/x17 and tail-calls the target.
- **RET**: return. BRK, HLT, UDF and words that are not code: `a64_trap`.
- **The flags** are stored to the context only where control leaves for code that may read them (a tail call, an
  indirect jump, an SVC).

The faster convention, where registers travel as values between lifted functions (`F_<addr>` with a uniform
prototype, callee-saved saves deleted), is built and parked (section 15).

## 7. Whole-program summaries and the verification build

`nx2/lift2/summary.py`, computed by the lift into the plan when the verification build or `HOOKSHOT_LIFT_SUMMARIES=1`
asks for them.
- **Per-instruction register use and definition** come from the semantic templates themselves: a template run on an
  empty block reads its block parameters and writes its written variables. They are memoised per word, so int, FP
  and NEON follow without a second model.
- **Per function, given its callees' summaries:**
  - inputs: backward liveness; a call reads its callee's inputs and kills its must-defs;
  - clobbers and must-defs: forward;
  - a frame pass: SP and every register holding entry SP plus a constant (x29, `mov x9, sp` copies); frame slots
    holding entry values of x19-x30 and d8-d15 (STR/STP/ST1 saves, LDR/LDP/LD1 restores); returns, noreturn,
    boundaries, FP use.
- **Whole program:** strongly connected components (Tarjan), callees first, iterated to a fixed point inside each.
  Then results: what callers read after a call, plus X0, X1, V0-V3 for callers it cannot see, minus preserved
  registers.
- **Noreturn** is seeded by the fatal functions' names and by the callers' evidence: a BL with nothing after it in its
  function is the compiler saying the callee does not return.

**Demotion rules** (detection, not lists; a function can have several reasons):

| reason | functions | what |
|---|---|---|
| tmp_in | 6,659 | x9-x18 live at entry |
| tmp_after_call | 5,118 | x9-x18 or flags read after a call that may change them |
| cs_clobber | 2,086 | a callee-saved register not proven preserved at a return |
| unknown callee | 1,729 | calls a function whose facts are unknown (below) |
| sp | 1,682 | SP not entry SP plus a constant, or not entry SP at a return |
| lr_ret | 1,575 | a return not through the entry x30 |
| flags_in | 595 | NZCV live at entry |
| split_frame | 214 | a tail call with the frame still allocated |
| save_ptr | 26 | entry x19-x30, SP or d8-d15 stored through a pointer outside the frame |
| inconsistent | 24 | a preserved register written but never read (the facts disagree; demoted for safety) |
| lr_data | 7 | entry x30 used as data |
| fallthrough_entry | 1 | entered by falling through from another function |
| dangling_bl | 1 | a BL with nothing after it to a callee that returns |

- sp, save_ptr, lr_data, lr_ret and split_frame make a function's facts unknown: its callers treat it as reading and
  writing everything and preserving nothing.
- The other reasons keep the facts and only mean an interface outside the AAPCS64.

**Distribution** (the whole game, 294,523 functions; 17.65 M instructions):
- Full summaries: 287,463 functions, 88.4 % of the instructions. Hot set: 688 of 852 functions, 64.2 % of its
  instructions.
- Noreturn: 708 functions. A generic-only boundary is reachable from 206,966 (anything that reaches a BLR, an
  unresolved BR, an SVC or a host function).
- FP: 6,996 functions write FPCR (or call something that does); 31,854 contain FP/NEON.
- Callee-saved save/restore pairs that a fast entry could delete: 612,455, in 174,749 functions (section 15).
- Call edges (1,073,423 BL, B and branch exits):

| target | edges | share |
|---|---|---|
| direct to a full summary (BL 725,080, tail 68,544) | 793,624 | 73.9 % |
| indirect (BLR 182,791, BR 23,561) | 206,352 | 19.2 % |
| host function or hook (41,704 + 2,221) | 43,925 | 4.1 % |
| direct to a demoted function (13,544 + 1,405) | 14,949 | 1.4 % |
| a noreturn seed | 14,573 | 1.4 % |

- **Cost.** 394 s in one process (decode and graph 67 s, the fixed point 288 s, results 35 s); peak memory 8.4 GB.
  A parallel version over the components' DAG (294,025 components, the largest 85 functions), in a process of its
  own, is on the parked branch (section 15): 130 s on 8 workers, 6.4 GB.

**Against the ABI census** ([lifter-abi-census.md](lifter-abi-census.md)). Every function on its non-benign list is demoted by
detection, except where noted:
- setjmp: save_ptr, lr_data, lr_ret. longjmp: sp, cs_clobber. DispatchFiberContext: sp, lr_data, lr_ret,
  split_frame. libunwind's jump: sp, lr_ret.
- unw_getcontext: save_ptr. UserExceptionHandler 0x85110e58: save_ptr (its d8-d15 saves).
- VAbortImpl: lr_data. BindEntry, rtld's trampolines: lr_data, sp.
- The merged bodies 0x8507fcd0, 0x81dc6f78, 0x81dcff78, 0x824e2ea8, 0x857da520: sp (and split_frame). The split-frame
  pairs in main: 158 functions demoted in 0x824c43bc-0x824f8adc; the census counted 24, and the rest are callers of
  them (unknown callee).
- The hand-written NEON functions that save d8-d15 with ST1/LD1 through a copy of SP (`mov x9, sp`): full summaries.
  One audio tap filter is demoted for tmp_in.
- **Where we differ:**
  - CallFiberFunction 0x8510d430 gets a full summary. It is AAPCS-shaped as far as its code shows; the census lists
    it as never-lift because it is reached only from the SDK's fiber assembly, which is HLE'd. That is a list fact,
    not an ABI fact; the lifter never takes it while the fiber functions are hooked.
  - Fatal callees: the census found 68 by BL-at-end-of-body. That evidence seeds noreturn here, and the fixed point
    adds what reaches them: 708 noreturn functions.
- **x9-x18 (`tmp_in` 6,659; `tmp_after_call` 5,118).** These are larger than the census' 877 and 1,382 because the
  liveness here is path-insensitive too, and counts reads through callees transitively.
  - The examples looked at (main 0x809d78c0, 0x80051080) read x18 or x12 on paths that correlated branches make
    infeasible.
  - Cross-function use of x9-x18 is not real: the verification build below never saw a non-input read.
  - The demotion stays conservative until liveness is path-sensitive (open work: on the SSA, conditions as
    values).

**The verification build.** `HOOKSHOT_LIFT_VERIFY=1` at lift time (the summaries are computed only then, or with
`HOOKSHOT_LIFT_SUMMARIES=1`: about 9 GB) and at build time wraps every function with a full summary. `host/lift2_verify.c`:
- at entry, poisons the registers the summary says are not read (`0xDEAD`, register, function);
- at exit, reports a function claimed noreturn that returned, a preserved register changed, and a register written
  that the summary says is not written;
- then restores what is preserved or not written and poisons the written registers no caller reads (`0xDEAE`);
- logs violations once per function, kind and register, to the game log and `lift2_violations.txt`.

Tail calls go past the callee's checks (to its body), so tail loops do not grow the host stack. An input holding
poison is not reported: inputs are an over-approximation (a BLR or host call counts as reading x0-x8), and a real
read of poison shows as a wrong picture or a crash naming the function.

**Verification results:**
- 262,527 functions lifted at the time (2026-11-14), those with full summaries checked; the executable 526 MB.
- **Zero violations and no crash** in the kokiri smoke scene (PASS, worst picture distance 6), the market and
  zoo_static benchmarks (600 frames each), and a 100 s title boot with the movie.
- What the first runs found, all fixed:
  - the runtime did not give back the preserved registers it had poisoned;
  - jump-table defaults counted as exits;
  - SVC dropped x30's entry value;
  - a noreturn call's fall-through into the next function counted toward clobbers.

## 8. Floating point: host FP mode

- FADD, FSUB, FMUL, FDIV, FSQRT, FMADD/FMSUB/FNMADD/FNMSUB and vector FMLA/FMLS are the native SSE/AVX/FMA3
  operations (`fadd` ... `llvm.fma`). IEEE operations and fused multiply-add are correctly rounded, so in
  round-to-nearest the values equal ARM's. `-ffp-contract=off` semantics: the emitter never fuses what the guest
  kept separate.
- **FPCR to MXCSR** (`host/hos_fp.c`). FPCR.FZ sets MXCSR.DAZ and MXCSR.FTZ; RMode sets MXCSR.RC (RN/RP/RM/RZ to
  RC 0/2/1/3); every exception stays masked. DN is not modelled (8.1).
  - MSR FPCR calls `a64_fpcr_write`, which stores the context's `fpcr` (keeping the host's bits 32 and up) and sets
    MXCSR.
  - A function with MSR FPCR is emitted `strictfp`: its FP operations are LLVM's constrained intrinsics with a dynamic
    rounding mode, which LLVM keeps in order with the FPCR write (a call). In TOTK no function with an MSR FPCR, and
    no direct caller (two levels up) of one, contains FP arithmetic, so the other functions stay plain; the whole-program
    stage is to mark a function with FP arithmetic that calls an FPCR writer strictfp too.
  - `thread_main` (`host/hos_thread.c`) sets MXCSR from the context before the guest's first instruction: a host
    thread may inherit its creator's MXCSR. `fiber_main` sets it from the FPCR the fiber inherited, and `fiber_switch`
    sets it again for `from` when `from` runs again, because not every platform's fiber switch keeps MXCSR.
  - `host/hos_setjmp.c` already saves and loads MXCSR.
  - Host code called from guest threads (HLE) runs under the guest's MXCSR (DAZ|FTZ on threads with FZ). Host code
    that depends on IEEE denormals on such a thread sets MXCSR itself; none is known.
- **What TOTK sets** (static census of every MSR/MRS FPCR/FPSR word, 31 sites in 11 functions):
  - FZ: `sys_fpcr_flush_to_zero` (f_80ec0e90 → f_80ec0ea0, main) ORs FPCR.FZ in for the main thread; f_8169d350
    (main) sets or clears FZ by its argument (3 direct callers: f_80ceb360, f_8169cd90, f_816e1944). FZ is in use.
  - RMode: only through the SDK's libc `fegetround`/`fesetround` (f_8575d1b0/f_8575d1bc) and `fegetenv`/`fesetenv`
    (f_8575d214/f_8575d228), reached by function pointer only (no direct BL in any module). Not provably unused.
  - f_851109b8 (sdk) saves FPCR/FPSR around a call and restores them.
  - Thread creation starts at FPCR 0 (`hos_thread.c` `setup_guest`); fibers inherit their first switcher's FPCR.
  - Seen in the game (HOS_FP_LOG=1, title boot): MainThread sets FPCR 0x1000000 at 0.17 s, MXCSR 0x1fa2 -> 0x9fc0;
    each guest thread then starts with FPCR 0, and its host thread, created from an FZ thread with DAZ|FTZ inherited
    (0x9fe2), is set back to 0x1f80 by `thread_main`. Without that sync every thread created after the main thread's
    FZ would have run its FP code flushing.
- **FPSR from MXCSR.**
  - MRS FPSR (8 sites) reads the context's `fpsr` and ORs in MXCSR's sticky bits: IE to IOC, ZE to DZC, OE to OFC,
    UE to UFC, PE to IXC, DE to IDC (DE only when FZ is set).
  - MSR FPSR writes the context and clears MXCSR's sticky bits.
- **Fix-ups, where x86 values differ from ARM's:**

| operation | x86 behaviour | fix-up |
|---|---|---|
| FMIN, FMAX | `minps` returns the second operand for NaN and either zero for ±0 | `llvm.minimum`/`llvm.maximum` (LLVM's lowering: a few ops) |
| FMINNM, FMAXNM | as above | `llvm.minimumnum`/`llvm.maximumnum` |
| FCVTZS/FCVTZU, all widths | out of range gives 0x80000000 | `llvm.fpto[su]i.sat` |
| FCVTNS/NU/AS/AU/MS/MU/PS/PU | as above | rounding intrinsic, then `.sat` |
| generated NaN (invalid op) | 0xFFC00000 / 0xFFF8... | not fixed by default (an allowed deviation, 7.1); a select to ARM's default NaN after each arithmetic op (2-4 instructions per op) is the fix if a reader turns out to care |
| NaN propagation with two NaN operands or a signalling NaN | x86 propagates src1 | not fixed: ARM differs only when the second operand is signalling |
| FZ: tininess | ARM flushes results tiny before rounding, x86 FTZ after | not fixed: differs only for results that round up to exactly the smallest normal |
| FRECPE, FRSQRTE | `rcpps` is a different estimate | ARM's algorithm (exact), out of line per lane: `a64_frecpe_f32/f64`, `a64_frsqrte_f32/f64` in `host/hos_fp.c`, FZ and RMode read from MXCSR |
| FRECPS, FRSQRTS | | `fma` plus the 0·∞ special case (2.0 / 1.5) |
| FCVT to and from half | | `fptrunc`/`fpext` (F16C `vcvtps2ph`/`vcvtph2ps`); FZ16/AHP not supported |
| FCMP, FCCMP | | four `fcmp`s give NZCV (unordered 0011); a following condition folds into one `fcmp` with the matching ordered/unordered predicate (`sem_int.cond`, kind `fcmp`) |
| TBL, TBX | `pshufb` uses the low 4 bits of an index | `pshufb` per table register with the index made relative to it and `uadd.sat(idx, 0x70)`, which sets bit 7 for every index past 15 |
| FABD, FACGE etc. | | compositions of the above |

### 8.1 Allowed deviations from ARM (host FP mode)

The checks (`tests/test_lift2_fp.py`, `nx2/test_lift2_leaf.py`) accept exactly these differences, count them by
class, and fail on anything else:

| class | what differs | where |
|---|---|---|
| `nan` | a NaN result where ARM's is a NaN too, with another sign or payload: x86's default NaN is negative (0xFFC00000), x86 propagates the first NaN operand where ARM prefers a signalling one, LLVM may commute operands; FPCR.DN is not modelled | every FP operation producing a NaN |
| `fz-boundary` | under FZ, a result of the smallest normal magnitude where ARM flushes to zero, or the reverse: ARM detects tininess before rounding, x86 after | FP arithmetic under FZ |
| `fz-minmax` | under FZ, a subnormal operand returned unflushed where ARM returns the signed zero (LLVM's minimum/maximum(num) lowering blends the original operand bits; DAZ only affects arithmetic) | FMIN/FMAX/FMINNM/FMAXNM, their pairwise and across-lane forms |
| `snan-minmax` | a signalling NaN operand of FMINNM/FMAXNM is treated as quiet (the number is returned, ARM returns the quieted NaN) | FMINNM/FMAXNM and their pairwise/across forms |
| FPSR | the cumulative flags are not computed per instruction: MRS FPSR sees the context's value ORed with MXCSR's sticky bits, which also record exceptions of lanes and operations the guest never had (64-bit arrangements are computed on 128-bit registers) | MRS FPSR (8 sites) |
| FRSQRTS | the halving is a separate exact multiply by 0.5 after the fused (3 - a*b): differs only when the result is below 2^-125 (single) | FRSQRTS |

Not deviations (exact): FMIN/FMAX/FMINNM/FMAXNM signed zeros and quiet NaNs, all saturating conversions and
rounding modes of FCVT*/FRINT*, FRECPE/FRSQRTE, FRECPS (0·∞ → 2), FCMP/FCCMP flags, every integer NEON operation.

## 9. Memory

- **Window.** The guest address plus the constant `0x100000000000` stays (one `movabs` per function, folded into
  addressing). There are no bounds checks; the 512 GB reservation and the host's first-touch commit stay as they
  are.
- **Plain loads and stores** are LLVM plain accesses with `align 1`. ARM's single-copy atomicity of aligned
  accesses holds on x86 for plain moves. A spin loop on a plain load stays a loop only because the back-edge poll
  (section 11) is a call that may write guest memory. That is an invariant of the poll's declaration, documented
  there.
- **Ordering under x86-TSO**, per construct:

| guest | emitted | x86 | why |
|---|---|---|---|
| LDR/STR | plain | mov | TSO orders them as ARM's dependency rules need |
| LDAR, LDAPR, LDAXR | `load atomic acquire` | mov | loads are not reordered with later accesses on x86; the IR order stops the compiler |
| STLR, STLXR | `store atomic seq_cst` | `xchg` | ARM's STLR then LDAR is the sequentially consistent idiom (RCsc); a release mov could pass the later LDAR in the store buffer. STLR cannot be told apart from a plain release store, so all are seq_cst (about 3,200 LDAR/STLR sites in the game) |
| DMB ISH, DMB SY, DSB | `fence seq_cst` | `lock or [rsp-64],0` | orders the store buffer |
| DMB ISHLD, DMB ISHST (~920 sites) | `fence acq_rel` | nothing | TSO gives load-load, load-store, store-store; a compiler barrier only |
| ISB | nothing | | no self-modifying code |
| LSE: LDADD/LDCLR/LDEOR/LDSET/SWP/CAS (all orderings) | `atomicrmw`/`cmpxchg` seq_cst | lock-prefixed | x86 locked ops are full barriers anyway |
| CASP | `cmpxchg` i128 | `lock cmpxchg16b` | |
| LDXR/STXR loops | recognised as `cmpxchg` loops when the pattern is LDXR, ALU, STXR, CBNZ back | `lock cmpxchg` | otherwise the value-compare monitor in the context, as today |

- **No write tracking.** A guest store is one plain store. Nothing records which guest pages the CPU wrote: the
  direct back end does not ask for them. Host code that writes guest memory (file reads, decompression, video
  frames) writes it plainly too.

## 10. The host interface

The lifted code is never edited after the lift: everything the host adds is named by guest address.

- **ENTRY_HOOK(0xADDR)** and **ENTRY_HOOK_SYM(name)** in `host/*.c` (read by `nx2/entry_hooks.py`): the lifter calls
  `entry_<addr>(ctx)` at the function's entry; 1 means it did the call's work (x0 holds the result), 0 runs the body.
- **The image's hooks** (HLE functions in `host/`, generated stubs for the OS-facing imports: `nx2/standalone.py`):
  `f_<addr>` is the host function (`a64_hooks.c`); the function is not lifted.
- **Probes** (`nx2/lift2/probes.py`, host side `host/oot_probes.c`): a host call at a guest instruction. `Value(...)`
  replaces an instruction's result, `At(...)` calls a host function of the named registers after (or before) an
  instruction and writes its result to a register; the instruction's word is checked at lift time, so another game
  version stops the lift.
- **C after an instruction** (`nx2/oot_wide.py`'s HUD and save picture, `oot_jitter.py`: `Discovery.after`) and the
  **SPLICE_AFTER** host functions (`host/direct_*.c`, sites named by address in `probes.SPLICE_AT`/`SPLICE_BEFORE`):
  written as host functions `lift2_after_<pc>(ctx)` the lifted code calls with the registers in the context.

### 10.1 Probes: Value

The game's counters that add the frame step cut to whole frames (the pick-up indicator, the water's ripples) take
`hos_frame_whole` (`host/frame_whole.h`) in place of the cut: `probes.PROBES`, each naming its instruction and word
(`fcvtzs w12, s0` at `0x80a88300`, `fcvtzs w9, s0` at `0x800675f8`).

### 10.2 At probes: the frame-rate and resolution patches

A patch that needs a few registers is a row of `probes.AT`: `At(func, pc, word, fn, args, result)`, "after the
instruction at `pc`: `result := fn(args...)`". Arguments are `xN`, `wN`, `sN` (Sn as a float) or an `#K` constant; the
result is `xN`, `wN` (zero-extended), `sN` (the rest of Vn zero) or none. `Program.after` runs it after the
instruction's own semantics, or when a BL / BLR returns; the values stay SSA values (no store and reload of the
context around the call), and the call is never moved or removed, since the host function may read and write guest
memory. The host functions (`host/oot_probes.c`) take plain values and guest addresses, never the context: an S
register comes as a float, and a guest object as a typed pointer (`A64_PTR` cast to a struct of the fields used at
their guest offsets, each checked with `_Static_assert`, `may_alias` since the host's C is built with strict aliasing).
Float bits stay only where the game moves a float through a W register (`mov w9, #1.0`, `mov w8, #1920.0`). The word
is checked at lift time (another game version stops the lift).

| family | rows | functions |
|---|---|---|
| dynamic resolution, the fixed scale | `hos_oot_dynres_flags`, `hos_oot_fixed_scale`, `hos_oot_scale_recompute`, `hos_oot_scale_flags` | `f_80825aa0`, `f_831f3200` |
| native scene (`HOS_SCENE_NATIVE`), `HOS_FIX_RES` | `hos_oot_scene_mode`, `hos_oot_scene_check` | `f_806702e0`, `f_831f3200` |
| frame seconds | `hos_oot_frame_step_probe`, `hos_oot_frame_step` | `f_80706be0`, `f_83b6ea90` |
| movie frames | `hos_oot_movie_count` | `f_83b7c710` |
| output size | `hos_oot_output_size`, `hos_oot_output_config`, `hos_oot_base_size` (x2), `hos_oot_base_size_bits` (x2) | `f_81222ce0`, `f_810a7b70`, `f_80ea2f30` |
| render-target pool | `hos_oot_rt_pool_config`, `hos_oot_rt_heap_size`, `hos_oot_rt_heap_use` (after a BLR), `hos_oot_rt_rest` | `f_836ce9d0`, `f_83b7b340`, `f_80b72340` |
| text speed | `hos_oot_text_time` (before an instruction: `At(..., before=True)`, on the edge into it, which must not be a branch target) | `f_80b1e140` |
| native mouse camera | `hos_mouse_camera` (`host/mouse_native.c`) | `f_80644c80` |
| widescreen | `hos_oot_wide_out` (x2), `hos_oot_wide_scene`, `hos_oot_wide_ratio`, `hos_oot_wide_projection_ratio` (x4), `hos_oot_wide_movie` | `f_81222ce0`, `f_83213890`, `f_804e48c0`, `f_834a5cc0`, `f_8006ec60`, `f_8025b910`, `f_8035a8c0`, `f_83b7c710` |
| occlusion off (`HOOKSHOT_OCCLUSION_OFF=1`) | `hos_oot_occlusion_flag` (x6) | `f_811cc790` |
| `HOS_BENCH`'s "in play" | `hos_oot_player_update` (Link updated, no map loading, no event playing) | `f_82c415e0` |
| per-core ring owners | `hos_oot_ring_write` (x14: 7 pushes, 7 pops) | `f_8004ab40`, `f_800aff80`, `f_800b3ce0`, `f_800b73d0`, `f_8073a090`, `f_8073c900`, `f_83af6150`, `f_83af59c0` |

The per-core rings are an assertion, not a patch. The singleton at `**0x84c9e890` holds seven rings, ring k at
+0x20 + k * 0x30: a buffer at +0x10, a capacity at +0x18, a head at +0x1c and a count at +0x20. The game picks a ring
by GetCurrentCoreNumber and pushes and pops it without a lock or atomics. That is safe only while one thread per core
number writes each ring. Each of the 14 sites is an At probe after the store of the count, given the ring's address.
The first thread that writes a ring is its owner. Another thread writing it is logged once per ring and thread, and
the game goes on. The probes stay on in normal play.

The output size in nnMain (`f_81222ce0`): the probe names the instruction (`mov x8, #2560; movk x8, #1440, lsl #32`, then the store at +20):

```
At(0x81222ce0, 0x81222ee4, 0xf2c0b408, "hos_oot_output_size", (), "x8"),   # movk x8, #1440, lsl #32
uint64_t hos_oot_output_size(void) { return hos_output_size(0) | (uint64_t)hos_output_size(1) << 32; }
```

Every site names its instruction and word, and a word that differs stops the lift: the At and Value rows
(`probes.at`, `probes.find`), `SPLICE_AT` and `SPLICE_BEFORE`, the C-after sites that remain (`oot_wide.install`,
`oot_jitter.install`), and every ENTRY_HOOK (one whose address is not a lifted function, or a symbol the dump does not
export, stops the build; `HOS_DBG=hooks` logs each hook's first call). Tolerances removed: the Value probes' match of the
instruction's text (now pc and word); the "main+... holds ...: left out" paths of `oot_wide`, `oot_jitter` and
`oot_occlusion` (a site skipped, the lift going on); ENTRY_HOOKs left out silently when their function was not lifted;
the frame step's no-hook fallback in `host/nn_vi.c` (`hos_step_probe` unset: the rate up to the cap, as an unpatched
game); the parked patches' "found 0 times" warnings with the patches themselves.

The Ctx-based C after an instruction (`probes.after_c`, `lift2_after_<pc>(ctx)`) stays for:
- `oot_wide`'s HUD (`f_80794330`, after `ldp q1, q4, [x2]`): it reads Q1 and a lane of Q4 and rewrites all of Q1; an At
  row has no 128-bit arguments or results yet.
- `oot_wide`'s save picture (`f_802a46e0`, before its `blr x8`): it rewrites four W registers at once (x1..x4); an At
  row has one result.
- `oot_jitter` (3 sites in `f_806dc520`): Q registers again. Nothing reaches it in this build: `hos_dlss_jitter_clip`
  gives a jitter only when the renderer's options `HOS_OPT_DLSS_JITTER_W`/`_H` are set, which only the legacy Vulkan
  back end's DLSS and FSR did; the direct back end answers no options. Proposed: removed like the parked patches, its
  record in `docs/parked-patches.md`.
- the `SPLICE_AT` / `SPLICE_BEFORE` splices of `host/direct_*.c` and `host/rt_slots.c` (`SPLICE_AFTER` in the host):
  the depth copy's and job sync's retargets (x8 or x4 := f(x8)) and the mesh lists' record (x19 + x8) would be plain
  At rows; the mesh system's (saves the registers and calls guest code), the layer-ahead's (reads the guest stack
  through SP), the sky LUT's and assets' (x0..x2 and the Z flag) and `rt_slots`' (four registers, block state) need more
  than one result or the whole context. They are the next At conversions.
### 10.3 Removed patches

The patches that were text edits of the deleted lifter's C and did not fit the probe style are removed: the game
menus' mouse entirely; heart protection, flight and its rotation, and the free camera with their host code (the Cheats
page's actions, the live `!cheat`/`!warp` commands), their record in [parked-patches.md](parked-patches.md): what each
did, the guest functions and fields, the guest calls, the logic as pseudo-code, what a probe would need. The native
mouse camera and text speed came back as At rows (section 10.2).

## 11. Polls, SVC, traps, setjmp

- **Polls.**
  - *Why they existed:* the virtual cores multiplexed guest threads, and a waiting thread asked a running one to give
    its core up through `ctx.poll`. The virtual cores are gone (`host/hos_thread.c`, "no scheduler"): nothing writes
    `ctx.poll`, `a64_poll_cur` is empty, and the polls are to be removed from the lifter.
  - *Where:* every back edge (a branch to an earlier address).
  - *Emitted form:* `load atomic i32 monotonic` of `ctx.poll` and a cold call of `a64_poll_cur()`.
  - *Declaration invariant:* `a64_poll_cur` takes no argument: it may read and write guest memory, which keeps spin
    loops on plain loads correct (section 9), but cannot touch the context, so values stay in registers across the
    loop.
- **SVC**: the flags stored, `a64_svc(ctx, imm)`; every register the context's again after it.
- **Traps** (UDF, BRK, HLT, words that are not code): `a64_trap(ctx, pc, word)`. A function the lifter cannot take
  (a word it does not decode) is a trap at its entry, listed by the lift (none in OoT 1.1.1).
- **setjmp owners**: a call of one goes through `hos_sj_call` (`host/hos_setjmp.c`), with the owner's setjmp return
  addresses; a longjmp comes back there and runs the owner's code from the return address.

## 12. Build

- **Partitions** by guest address, about 120,000 guest instructions each, the hot list's functions
  (`work/hot_funcs_play.txt`, `scripts/hotlist.py`) in their own: 147 partitions for the whole game. Lifted in worker
  processes (each holds the plan), compiled by clang from IR on its standard input: cold partitions at -O1, hot at
  -O2 (`RX_HOT_OPT`).
- **Caching**: a partition's object is kept by an input key (the plan, the lifter's sources, the flags, its
  functions, the hooks); an unchanged one is neither lifted nor compiled again.
- **Times** on the development PC: a relift about 100 s (the lift 91 s: discovery 37 s, the plan 32 s; 6 GB at its peak); a cold build about 5 minutes
  of partitions at 6 jobs plus the host and the direct back end; a host-only change about a minute. The executable is
  301 MB.
- **One LLVM major.** The emitter writes textual IR for one LLVM major; `nx2/lift2/llvm.py` refuses a clang of
  another.

## 13. Checks

- `tests/test_lift2.py` (no dump): the decoder's tables, every template on synthetic states, small programs.
- `tests/test_lift2_fp.py`: FP and NEON forms on edge values, the allowed deviations of 8.1 counted.
- `tests/test_lift2_discover.py`, `test_lift2_tables.py`, `test_lift2_program.py` (a lifted program end to end with
  the generated runtime), `test_lift2_summary.py`, `test_lift2_verify.py`.
- `nx2/lift2/selfcheck.py`: the decoder against llvm-mc on every distinct word of the game.
- `nx2/test_lift2_leaf.py` (the dump, Unicorn): random functions and call trees of the game lifted and run from random
  states, compared with Unicorn (`nx2/lift2/reference.py`).
- `nx2/lift2/dump.py`: a function's guest code, IR, LLVM IR, optimised IR and x86, to read.
- In the game: the smoke scene against the phashed golden, a title boot, the benches (`docs/development.md`).

## 14. Measured results

Against a64-kit's lifter (suyu's per-instruction C, every guest register in the context), which the game ran on before
this one and which was measured on the same functions before its deletion:

| check | result |
|---|---|
| decoder vs llvm-mc, every distinct game word | 2,032,675 words, 2,032,409 decoded, 0 differences |
| integer leaves (500), call trees (about 2,000 roots a seed) | 0 differences against Unicorn and a64-kit's lifter |
| x86 instructions at -O1, integer leaves / functions with calls | 0.73x / 0.92x a64-kit's |
| FP/NEON leaves (770) | 0 differences outside 8.1; x86 instructions 0.216x (12.47 against 2.69 per guest instruction) |
| f_8170a4e0 (physics vertices AABB, 119 instructions) | 6,666 x86 instructions before, 306 at -O1 |
| FP functions of the whole game, x86 instructions | hot set 0.128x, the rest 0.342x |
| lift speed, one core | 33-56 k guest instructions/s (decode, IR, LLVM IR) |

Benches of a build where this lifter's code ran 98.8 % of the game's instructions, FP and NEON included, against
a64-kit's lifter alone (fixed 1/60 s step, 600 frames, median frame): market 9.9 ms against 11.9 ms, zoo_static 5.0
against 6.8, zoo_anim 5.9 against 7.0, zoo_shadows 9.8 against 12.5; CPU time 11-29 % lower. The build on this lifter
alone is not benched yet: the benches stall before they are ready on the current branch.

## 15. Open work

- **Fast entries** (`agent/lift2-fast`, parked: built and tested without the dump, not run in the game). `F_<addr>`
  with one prototype for every function (`{i64, i64, <2 x i64>, <2 x i64>} (ptr ctx, i64 sp, i64 x0-x3,
  <2 x i64> v0-v3)`, `ccc`: rdi, rsi, rdx, rcx, r8, r9, xmm0-3 in, rax, rdx, xmm0, xmm1 out); registers beyond those
  through the context, by the summaries; callee-saved save/restore pairs deleted (612,455 of them); BLR through a map
  of the fast entries; `f_<addr>` a wrapper. Expected 10-12 % fewer instructions on code with calls. Needs the
  summaries on every build (130 s and 6.4 GB with the parallel pass there).
- **Frame-slot promotion.**
- **Build**: IR-level PGO, `-fsplit-machine-functions`, 2 MB text pages.
- **Path-sensitive liveness** for the summaries (x9-x18 demotions).

### 15.1 Risks not retired

- **Non-AAPCS functions** (SDK assembly, outlined helpers, veneers reading x16/x17): their number decides how much
  code keeps the generic convention. The summaries' demotion list (section 7) counts them. A static census (`nx2/lift2/abi_census.py`, [lifter-abi-census.md](lifter-abi-census.md)) lists what
  breaks the model besides setjmp and the fibers: libunwind's context pair, C++ exceptions (thrown in main and the
  SDK; a throw is a fatal stop, `host/cxx_throw.c`), about 3,500 noreturn call sites, 25 split frames, ST1/LD1 saves of d8-d15; and the rule that finds them.
- **Host FP mode divergence** in the game: NaN sign and propagation, the FZ tininess boundary, the 8 FPSR read sites.
  Replays and goldens were re-baselined for it.
- **Summary time**: the whole game's summaries take a minute or more and up to 9 GB; they are computed only for
  the verification build.
- **One LLVM major**: the textual IR follows LLVM's syntax, so a new major needs the emitter checked (`nx2/lift2/llvm.py`).
