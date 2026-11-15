# The lifter

Hookshot's lifter turns the game's AArch64 code into x86-64 code at build time: every function is decoded, built into
a typed SSA IR, emitted as textual LLVM IR and compiled by clang. This document says what the lifter emits, how the
build runs, where the emitted code is knowingly worse than it could be and why that is kept. The queue of changes is
[lifter-optimisations.md](lifter-optimisations.md); the census of non-AAPCS64 functions is
[lifter-abi-census.md](lifter-abi-census.md).

Measurements are on the development PC (Zen, x86-64-v3) with clang/LLVM 23.1.3. "The sample" is a size-weighted
sample of 120 functions of the play binary at 860eb332, recompiled from edited IR; its savings were the same at -O1 and
-O2. That binary still had page marks and polls, so shares of it are shares of that binary, not rescaled.

## Pipeline and files

| step | files | what |
|---|---|---|
| image | `nx2/image.py`, `third_party/a64-kit/hos/nx_image.py`, `nx2/nso2.py`, `nx2/standalone.py` | the modules (rtld, main, subsdk*, sdk) loaded at their addresses and relocated; entries from symbols, unwind info and relocations; the HLE functions and stubs hooked |
| entries | `nx2/adr_entries.py` | code addresses the code builds with ADR / ADRP+ADD (callbacks reached only through a pointer) |
| discovery | `nx2/lift2/discover.py` | function bodies (an entry plus what it reaches without crossing another entry), jump tables, PLT stubs, setjmp owners, the split of very large functions (`--max-func`) |
| plan | `nx2/lift2/game.py` `write_plan` | every function's instructions and the program's facts in `work/sa/plan.pickle`; the summaries when asked |
| runtime tables | `nx2/lift2/tables.py`, `nx2/standalone.py` | `a64_table.c` (the function table), `a64_hooks.c`, `a64_code_slots.c` (the relocated slots holding a function), `hos_image.c`, `a64_stubs.c`, `nvn2_exports.c` in `work/sa/gen` |
| IR | `nx2/lift2/enc*.py`, `sem*.py`, `build_ir.py`, `ir.py` | the decoder's encoding tables, each form's semantics as IR, a function's IR |
| LLVM IR | `nx2/lift2/emit_llvm.py` | one textual module per partition |
| build | `nx2/lift2/game.py` `prepare`, `nx2/build.py` | partitions lifted in worker processes, piped into `clang -x ir -` (cold ones through `opt` first; no IR on disk), cached by input key; runtime and host compiled and linked |
| runtime | `nx2/rtgen.py` from `third_party/a64-kit/rt` | `a64.h` (`GuestContext`, the guest window) and `a64_rt.c` (`a64_lookup`, `a64_icall`, `a64_ijump`, `a64_trap`, the exclusive monitor, the counter) |
| summaries | `nx2/lift2/summary.py` | whole-program register facts: call boundaries, omitted stores, the verification build |

`nx2/lift.py` runs the steps up to the plan and the tables (`scripts/relift.sh`); `nx2/build.py` runs the rest. The
plan holds instructions, not lifted code, so a change to the lifter needs only a build.

Coverage: the modules' `.text` has 18.16 M words, 2.03 M distinct, in 1,699 forms (mnemonic plus operand shape). Every
form decodes; there is no fallback. `nx2/lift2/selfcheck.py` compares the decoder with llvm-mc on every distinct word
(2,032,675 words, 0 differences); `nx2/lift2/census.py` writes the form list to `work/lift2/forms.txt`. A function
with a word that does not decode is lifted as a trap at its entry and listed by the lift (none in OoT 1.1.1).

| class | forms | words | share |
|---|---|---|---|
| integer data processing | 331 | 6.81 M | 37.5 % |
| integer load/store (atomics included) | 269 | 5.58 M | 30.7 % |
| branches | 22 | 3.36 M | 18.5 % |
| vector/FP load/store | 367 | 0.57 M | 3.1 % |
| udf/nop (padding, data) | 2 | 0.55 M | 3.1 % |
| FP scalar | 121 | 0.53 M | 2.9 % |
| NEON | 534 | 0.50 M | 2.7 % |
| system (MRS/MSR, barriers, PAC, hints, SVC) | 53 | 0.27 M | 1.5 % |

Discovery over the whole game: 294,523 functions, 1,475 jump tables (1,385 with a bound from CMP + B.HI/B.HS, CCMP,
an AND mask or a copy of the index; the rest read until an entry leaves the function's unwind extent), no BR without a
table, 9,917 PLT stubs. RETAA/RETAB are returns and BRAA*/BLRAA* are BR/BLR (the modifier ignored).

The output format is LLVM IR rather than C for its exact intrinsics (`llvm.fptosi.sat`, `llvm.minimum`/`maximum`,
`llvm.minimumnum`/`maximumnum`, `llvm.fma`, the rounding intrinsics, the overflow intrinsics), `poison`, and block
parameters that map one to one onto `phi`. For integer code C gives the same machine code (24,189 x86 instructions over
the same 120 functions either way). `nx2/lift2/llvm.py` refuses a clang of another LLVM major than the one the emitter
writes for.

## What is emitted

### Functions, registers and the context

- Every function is `void f_<addr>(ptr noalias nocapture %ctx)` (`emit_llvm.emit_function`): the symbol the function
  table, the hooks and the host use, and what every call reaches. `%ctx` is the thread's `GuestContext`: x0-x30 and SP
  at 8 * n, N Z C V as bytes at 0x108-0x10B, V0-V31 at 0x110 + 16 * n.
- Inside a function guest registers are SSA values; blocks take parameters, emitted as `phi`. X registers are `i64`,
  W writes zero-extend, the flags are four `i1`, V registers `<2 x i64>` bitcast to lane types per operation.
- The entry block loads from the context the registers the function reads before writing them.
- **Write-through**: every register write is also stored to the context where it happens (`ir.Builder.set` emits a
  `writeback`). LLVM drops a store that a later one overwrites before any call or return; the last one stays. For
  that it must see that nothing in between reads the context: every declaration taking it says `ptr captures(none)`
  (`emit_llvm.no_capture`; the callees keep no copy, `A64_NOESC`), and the partitions are compiled with capture
  tracking and DSE limits raised (`game.DSE_FLAGS`) and with DSE at both levels (The build). Without them the
  context escapes at the first call, any guest load may read it, and a large function's thousands of uses of it
  exceed the default limits: on the market's five hottest functions 40-47 % more context stores and 10-14 % more
  code.
- **Stores no reader reads are not emitted** (`build_ir.omit_stores`, a backward pass over each function's IR after
  it is built): a register's store is dropped when no reader can read it before the register's next store. What
  each call reads and writes is its callee's call-boundary record (`bounds.call_bounds`, below), the same record the
  lift keeps registers by. Readers: a load of the register after a call; a BL's callee, which reads the record's
  reads and passes on what it does not write on every path (the record's must-writes; the summaries and the lift
  resolve a BL through a PLT stub to the same target, `Program.callee`) to the caller's later loads:
  a lifted callee its summary's inputs and must-writes, an entry-hooked one its summary's too (the hook's reads and
  its "return 1" path are in it: must-writes none), an HLE'd one its declared reads (`host_effects.hle_reads`, checked
  against the host's IR by `tests/test_host_context_reads.py`; must-writes none); a return, which reads the record's
  results (the summary's: what its callers read after it), x19-x29, SP and v8-v15; a call taking the context, which
  reads every register unless the template that made it says which (`regs=0` on the runtime's monitor, counter and
  FPCR/FPSR helpers, `recomp_*`, `a64_fp*`, the estimate helpers: they read only the context's other fields). Every
  register is read by BLR, SVC, an At row calling guest code, a BL to a callee whose record is `bounds.unknown` (no
  summary, demoted, setjmp owner, HLE'd and declared clobbering), tail calls, indirect jumps, traps, and the returns
  of a function without a full summary. The flags are not affected. On 6 cold partitions after LLVM's DSE: context stores -20.9 %, instructions
  -6.6 %, `.text` -6.7 %; on the market's five hottest functions -18.2 %, -5.5 %, -5.9 %.
- **Reload after calls**: after a BL, BLR, SVC or a call into host code with the context, every register read loads
  the context again (`ir.Builder.after_call` clears the block's definitions), except after a BL to a callee with a
  full summary that the host does not edit, or to a host callee with a declared effect: the values of x19-x29 and SP
  the callee keeps stay SSA values across the call (`build_ir.build`; the callee's call-boundary record, below). The registers are still written to the context
  before the call. The low halves of v8-v15 are reloaded: their upper halves are not
  preserved, so the register needs its load anyway.
- No `alloca` is emitted except for host-stack temporaries of runtime helpers (`lift2_ldxp_lo`).

### Flags

- N, Z, C, V are computed with `llvm.[us]{add,sub}.with.overflow`; a condition whose four flags come from one compare
  folds to one `icmp` (`sem_int.cond`), an FCMP's to one `fcmp` with the matching ordered/unordered predicate.
- Flags are not written through. They are stored to the context only before an SVC and in the exit blocks that leave
  the function for other code (a tail call, `a64_ijump`), when the function writes flags (`build_ir.build`, the
  `flagsout` loop). A RET stores no flags: the AAPCS64 leaves them undefined at a return.

### Memory and ordering

- **Window.** A guest address `a` is `getelementptr i8, ptr inttoptr (i64 0x100000000000 to ptr), i64 a`: one
  `movabs` per function, folded into addressing. No bounds checks.
- **Plain accesses** are LLVM plain loads and stores with `align 1` (`emit_llvm.emit_op`, `load`/`store`). A guest
  store is one plain store: nothing records which guest pages were written. Host code writes guest memory plainly too.
- **Ordering under x86-TSO:**

| guest | emitted | x86 |
|---|---|---|
| LDR/STR | plain | mov |
| LDAR, LDAPR | `load atomic acquire` | mov |
| STLR | `store atomic seq_cst` (`sem_int.t_ldstord`) | xchg |
| DMB/DSB ISH, SY, OSH, NSH | `fence seq_cst` | `lock or [rsp-64],0` |
| DMB/DSB ISHLD, ISHST and other load-only or store-only kinds | `fence acq_rel` | nothing (compiler barrier) |
| ISB | nothing | |
| LDADD/LDCLR/LDEOR/LDSET/SWP and the signed/unsigned max/min, every ordering | `atomicrmw seq_cst` | lock-prefixed |
| CAS | `cmpxchg seq_cst` | `lock cmpxchg` |
| CASP | `cmpxchg` i128 | `lock cmpxchg16b` (`-mcx16`) |
| LDXR/LDAXR, STXR/STLXR | calls of `recomp_ldxr`/`recomp_stxr` (`sem_int.t_ldstex`): the runtime's monitor keeps the address, size and value LDXR saw; STXR compare-and-swaps against that value | call |
| LDXP/STXP | `recomp_ldxp`/`recomp_stxp` | call |
| CLREX | a store of 0 to the context's monitor | mov |

The whole game has 3 LDXR/LDAXR and 2 STXR against 10,721 CAS and 25,802 LDADD-family LSE operations, so the
exclusives' runtime calls cost nothing measurable.

### Loops

- **The cycle barrier.** A block with an edge that closes a cycle of the function's CFG gets the IR op `barrier`,
  emitted as `call void asm sideeffect "", "~{memory}"()` (with `strictfp` appended in a `strictfp` function).
  Placement is `build_ir.cycle_edges`: a depth-first search from the entry, then from any block not reached; a block
  gets the barrier when one of its edges goes to a block still on the search's stack. So every cycle has one: natural
  loops, irreducible regions, self-loops, cycles through a jump table's switch, a branch to the function's own entry.
  LLVM treats the barrier as reading and writing any memory, so a guest load inside a loop is done each time round.
  It emits no instruction: `ldr w0, [x1]; cbz w0, back` compiles at -O1 and -O2 to a loop that reloads each
  iteration with no call (`tests/test_lift2_barrier.py`). Whole game: 142,299 barrier blocks in 43,613 of 294,523
  functions.
- YIELD and WFE are `pause` (`llvm.x86.sse2.pause`, `sem_int.t_hint`). NOP, SEV and the PAC hints are nothing.
- **No polls.** The lifted code never checks for a request from another thread, and nothing depends on a lifted
  thread calling into the host periodically. Every guest thread runs freely on its own host thread
  (`host/hos_thread.c`, "no scheduler"); process exit is `_exit`; the profiler samples by signal
  (`host/posix/prof.c`); fiber switches are host calls. The stall dump reads each thread's context as of its last
  call out of lifted code, so a thread spinning without calls shows stale registers. The stall dump, the crash
  handler and the handle dump read a context from outside the thread: a register whose store `omit_stores` dropped
  shows an older value there (a temporary, x30 or an argument register before a BL to a callee that does not read
  it, a register that is not a result at a return); the registers read at BLR, SVC and calls into the host are
  current.

### Calls and returns

- **BL**: x30 := the return address, then `call void @f_<callee>(ptr %ctx)`, then the reload of every register but
  the callee's preserved x19-x29 and SP (see above). A callee the summaries prove noreturn is followed by
  `a64_trap(ctx, <BL address>, <BL word>)` and `unreachable`, so a wrong summary stops the game; the code only its
  return reached is dropped.
- **The call-boundary record** (`nx2/lift2/bounds.py`, `bounds.call_bounds`): every direct callee has one record,
  computed once per build worker from the plan's summaries and the host's declarations, and the lift (`Program.bound`),
  the verification build, the build's call report (`bounds.call_report`) and `nx2/lift2/dump.py` all read it. It
  gives the kind, the registers the callee may read, may write and writes on every returning path, those its callers
  read after it, the x19-x29 and SP bits a BL keeps in SSA, noreturn, and the symbols of its call and tail call. A
  callee without a declaration or a full summary (no summary, demoted, setjmp owner, an HLE'd function declared
  clobbering) gets `bounds.unknown`: it reads and writes every register, keeps nothing and returns, so a callee no rule
  classifies costs speed, never correctness. Only a lifted callee whose summary proves it (demoted or not) is
  noreturn; HLE'd, entry-hooked and setjmp-owner callees never are. A function with Value probes or At rows is lifted as any other: each row
  declares what it reads and writes, and the summaries count it as its instruction's (`probes.row_effects`,
  `summary.rows`). What a call of a host callee keeps is its declared effect (`nx2/lift2/host_effects.py`):
  - an HLE'd function keeps x19-x29 and SP unless `host_effects.CLOBBERS` names it with the reason (the three fiber
    functions: a context switch or a host fiber deleted; nine sdk NVN exports with no host proc: not audited);
  - an entry-hooked function keeps what its lifted body's full summary preserves (the hook declines and the body runs,
    or the hook handled the call, writing no callee-saved register, or ran the body itself through `a64_lookup`), and
    for a hook that calls other guest code what every callee in `host_effects.CALLS` keeps too (the three mesh drawer
    hooks, `host/direct_mesh_binds.c`; the render-target alloc and free, `host/rt_slots.c`, which call the free); a
    body without a full summary keeps nothing;
  - setjmp owners keep nothing.
  A name in `CLOBBERS` that is no HLE'd function, or a `CALLS` key that is no entry hook, stops the build. Every guest
  function a hook or a row's host function enters, on any path through the host's own calls, is declared:
  `tests/test_host_context_reads.py` follows the host's LLVM IR from each hook and row function to every
  `hook_call_guest`, `guest_call`, `hos_call_guest`, `a64_icall`, `a64_ijump`, `f_<addr>` call and `hook_run` (constant
  targets followed through arguments) and fails on a target missing from `CALLS` or the row's `guest`. The default
  rests on an audit of the host's C (2026-11-15, its LLVM IR: stores into a context's x19-x31 and v8-v15, calls of
  guest code, fiber switches, through direct calls and signature-matched indirect ones): stores to x19-x31 are only in
  thread and fiber setup, `hos_call_guest` (x30) and the guest calls of hooks and rows, which put every register back
  (`host/hos_hostcall.c` `call_restoring`); no HLE'd function reaches one, a guest call or a switch except those
  declared. What the host reads is declared too (`host_effects.HLE_ARGS`, `HLE_READS`, `HOOK_ARGS`, `HOOK_READS`) and
  checked against the host's IR by `tests/test_host_context_reads.py`, and the summaries count every declaration
  (Whole-program summaries, "The host's patches").
- **B to another entry, or falling into one**: `musttail call void @f_<callee>(ptr %ctx)`; every function has the same
  prototype, so every tail branch is a guaranteed tail call.
- **RET**: `ret void`.
- **PLT stubs**: a call of one calls its target (`Program.callee`); a stub reached indirectly sets x16 (the GOT slot)
  and x17 (the target) and tail-calls the target.
- **setjmp owners** (the 13 functions that call `__setjmp`; their 14 return addresses are extra entries): a BL to one
  calls `sjcall_<owner>`, a stub that enters `hos_sj_call` (`host/hos_setjmp.c`). A longjmp comes back there and runs
  the owner from the setjmp return address. `__setjmp` and `longjmp` themselves run lifted.

### Indirect calls and jumps

Code pointers are host function pointers, translated where they originate (`nx2/lift2/code_ptrs.py`):

- **Relocated slots.** Every relocated slot of the image whose value is a function of the table (453,943 slots:
  RELATIVE/RELR 437,540, JUMP_SLOT 11,697, ABS64 4,542, GLOB_DAT 164; vtables and pointer tables in `.data`, the GOT,
  `init_array`) is listed in `gen/a64_code_slots.c` (slot, function index; `nx2/lift.py`, `tables.write_code_slots`).
  The host writes the function's host pointer there right after `image.bin` is loaded (`host/code_ptrs.c`
  `hos_code_ptrs_init`, which stops when a slot does not hold the lift's value). `image.bin` keeps guest values.
- **Addresses the code forms.** At `ADR Xd, <entry>` and at the `ADD Xd, Xn, #imm12` of an `ADRP Xn` + `ADD` pair
  (within 4 instructions, one block, Xn not written between) that forms an entry, the lifter emits
  `ptrtoint (ptr @f_<entry> to i64)` (IR value `codeaddr`). An entry is a function of the table except the modules'
  bases: every module's DT_INIT passes its own base to `__nnmusl_init_dso` as a range bound (main's at 0x81237808), and
  `f_80020000` (main's MOD0 header, taken for code) is a table entry. The census (`code_ptrs.sites`) finds 8,180 ADR
  and 11,067 ADD sites; the lift translates exactly those (`build_ir.build` stops when a site's ADD does not see the
  page as a constant, or when an ADR or a 64-bit ADD/SUB immediate forms an entry's address anywhere else; MOVZ/MOVK
  and ORR make integers in this position-independent code, and some equal an entry: 0x80000100 at 0x81701194), and
  the build stops when a partition's translated count differs from the census (`game.prepare`; the count of a cached
  partition is kept beside its object, `.cp`). `code_ptrs.audit` accounts for every other code address the code forms
  and the build stops on one of no known kind: 1,419 jump-table bases, 20 module bases (14 at the base, 6 at base + 1),
  zstd_decompress_sequences' 2 local continuations (`adr x7` ... `br x7`), and 11,075 ADRPs of a text page each
  consumed in its block by the entry-forming ADD (11,067) or a module base's (8). An indirectly reached PLT stub's
  x17 (its GOT value) is the target's `codeaddr` too.
- **The host's hand-outs.** A host call without user data is handed out as the handler itself (`hos_hostcall`: every
  NVN proc, the substituted job-sync and depth-copy procs); the 16 guest functions the host calls by address are named
  by their `f_` symbols (`CODE_PTR`).

What is emitted:

- **BLR**: `call void @lift2_icall(ptr %ctx, i64 %target, i64 <pc>)`, an always-inlined helper: a target with any of
  its high 32 bits set (host code; guest code addresses are below 4 GB) is called directly, else
  `a64_icall_guest(ctx, target, pc)`; then the reload of every register. A site's code is `mov; shr $32; je; call *%r`
  (it was `call a64_icall`, then the global cache's hash, load and compare, then the call); LLVM turns a BLR of a
  known `codeaddr` into a direct call.
- **BR without a table**: the same test, then a `musttail` call of the host pointer, else `a64_ijump_guest(ctx, target,
  pc)` and return.
- **The guest-address path** (`third_party/a64-kit/rt/a64_rt.c`): `a64_icall_guest` / `a64_ijump_guest` count every
  call, log each site once (`host/code_ptrs.c`), then `a64_icall` / `a64_ijump` look the address up: one global
  direct-mapped cache of 2^18 64-bit slots keyed `(a >> 2 ^ a >> 20)`, each `address << 32 | offset`, in front of a
  binary search of the function table, then the host's functions (`a64_lookup_host`). It takes setjmp return
  addresses (a longjmp's BR, `host/hos_setjmp.c`), host calls with user data (the unimplemented NVN procs) and any
  origin the translation missed. `hos_code_ptr_report` logs `code pointers: N sites and M calls took the
  guest-address path; K guest-valued code addresses reached the host` every 1800 presents and at a bench's exit.
- **The host's receiving paths**: thread entries, fiber functions and the TLS hooks take either kind (`a64_fn_of`,
  `a64_call_received`: a guest-valued one is looked up and counted). `a64_guest_of` maps a host pointer back to its
  guest address (the crash report, the thread dump, the thread and fiber logs).
- **The game's own checks**: `__nnmusl_init_dso` (0x85056160) tests whether nnMain, loaded from its relocated slot,
  lies inside the module's range; an At row after the load (0x850565b0) gives it nnMain's guest address
  (`hos_code_guest_value`). `f_823a6140` (`net_grpc_authority_table_arg_cmp`), the compare of a gRPC pointer
  channel argument ("grpc.target_authority_table", vtable 0x84978ed8 set at 0x823a608c): three-way, by the function
  pointer at +0x10, then +0x18. It orders channel arguments (gRPC sorts and compares them to tell channels apart);
  with host pointers that order differs from the guest's and between runs (ASLR), which nothing observes; left as it
  is.
- A guest load of a host-valued code pointer as data faults: the guest window is 2^39 bytes at 0x1000_0000_0000 and a
  host pointer is above 2^39.
- `a64_rt.c` also holds a per-site two-slot cache (`A64_SITE_CALL`, `site_call`) and a site profiler
  (`A64_SITE_PROFILE`); the lifter emits neither.

### Jump tables

A BR with a table the discovery resolved is a `switch` on the index the guest loaded the entry with, cases `0..n-1`
(dense: LLVM lowers a table), when the discovery proves the table's facts (`Discovery.index_tables`): one table, its
address at the load and the ADD's base each one ADRP/ADR(+ADD) constant on every path of the function's CFG
(`exact_constant`), the table in `.text`/`.rodata`, the LDR's index scaled by the entry size, the ADD's extend computed
exactly, and the load to the BR one straight-line block (with a probe in it, the offset switch). `build_ir.build`
reads the index, extended as the LDR extends it, as an SSA value at the load (the register may be reused before the
BR). Targets inside the function are branches,
the others tail calls; the default, which an index out of range reaches, is `a64_ijump` of the BR's register. Any other
table (`Discovery.jt_offset` holds why; the lift prints the count) is a `switch` on `fshr(t - lo, t - lo, 2)`, `lo` the
lowest target, cases `(target - lo) >> 2`, as sparse as its targets are spread.

### FP and NEON

- **Arithmetic** is native: `fadd`, `fsub`, `fmul`, `fdiv`, `llvm.sqrt`, `llvm.fma` for FMADD/FMSUB/FNMADD/FNMSUB and
  vector FMLA/FMLS. Correctly rounded, so equal to ARM's in every rounding mode. The emitter writes no `contract`
  flags and the build passes `-ffp-contract=off` (`nx2/build.py:403`): nothing the guest kept separate is fused.
- **FPCR to MXCSR** (`host/hos_fp.c`): FPCR.FZ sets MXCSR.DAZ and FTZ, RMode sets RC (RN/RP/RM/RZ to 0/2/1/3), every
  exception stays masked.
  - MSR FPCR calls `a64_fpcr_write`, which stores the context's `fpcr` and sets MXCSR.
  - A function with an MSR FPCR is emitted `strictfp` (`sem_fp.fp_sysreg_write`, `emit_llvm.emit_strict`): its
    rounding-dependent FP operations are constrained intrinsics with a dynamic rounding mode, kept in order with the
    write. No function with an MSR FPCR, and no direct caller of one two levels up, contains FP arithmetic.
  - MXCSR is set from the context before a thread's first guest instruction (`host/hos_thread.c:396`), at a fiber's
    start (`:964`) and when a fiber switch returns (`:999`). `host/hos_setjmp.c` saves and loads MXCSR.
  - HLE host code on a guest thread runs under the guest's MXCSR.
- **What the game sets**: 31 MSR/MRS FPCR/FPSR sites in 11 functions. `sys_fpcr_flush_to_zero` (f_80ec0e90 →
  f_80ec0ea0) sets FZ on the main thread (seen in the game with `HOS_FP_LOG=1`: FPCR 0x1000000 at 0.17 s); f_8169d350
  sets or clears FZ by its argument. RMode changes only through the SDK's `fesetround`/`fesetenv`, reached by function
  pointer only. A new thread starts at FPCR 0; a fiber inherits its first switcher's FPCR.
- **Why MXCSR follows FPCR**: FZ because the game sets it (above) and results differ without it; RMode because it
  costs nothing outside the `strictfp` functions, though no caller of `fesetround`/`fesetenv` was found to run.
  Contraction off costs nothing: LLVM does not fuse operations without `contract` flags.
- **FPSR**: MRS FPSR (8 sites) reads the context's `fpsr` ORed with MXCSR's sticky bits; MSR FPSR writes the context
  and clears them.
- **Fix-ups where x86 differs:**

| operation | fix-up |
|---|---|
| FMIN, FMAX | `llvm.minimum`/`llvm.maximum` |
| FMINNM, FMAXNM | `llvm.minimumnum`/`llvm.maximumnum` |
| FCVTZS/FCVTZU, every width | `llvm.fpto[su]i.sat` (NaN to 0, saturating) |
| FCVTNS/NU/AS/AU/MS/MU/PS/PU | rounding intrinsic, then `.sat` |
| FRINTN/A/M/P/Z | `llvm.roundeven`/`round`/`floor`/`ceil`/`trunc` |
| FRECPE, FRSQRTE | ARM's estimate algorithm inline per lane, bit-exact (`nx2/lift2/fp_estimate.py`: the estimate table as a constant computed from RecipEstimate/RecipSqrtEstimate, integer arithmetic and selects, FZ and RMode from the context's FPCR); the game has 904 FRSQRTE and 130 FRECPE sites, all single precision, 2 or 4 lanes |
| FRECPS, FRSQRTS | `fma` plus the 0·∞ special case |
| FCVT to and from half | `fptrunc`/`fpext` (F16C); FZ16 and AHP not supported |
| FCMP, FCCMP | four `fcmp`s give NZCV (unordered 0011) |
| TBL, TBX | `pshufb` per table register, index made relative and `uadd.sat(idx, 0x70)` to set bit 7 past 15 |
| AESE/AESD/AESMC/AESIMC, PMULL 1Q | AES-NI and PCLMUL, by a per-function `target-features` (`sem_crypto.features`) |
| SHA1*, SHA256* | the Arm ARM's pseudocode in scalar code, lane by lane (`sem_crypto.py`) |
| SQSHL/UQSHL/SQRSHL/UQRSHL by register | one lane at a time (`sem_neon.lanewise`) |
| CRC32*, CRC32C* | a bit loop (`emit_llvm.HELPERS["lift2_crc32"]`) |

- **Allowed deviations from ARM.** `tests/test_lift2_fp.py` and `nx2/test_lift2_leaf.py` accept exactly these,
  count them by class and fail on anything else:

| class | what differs | where |
|---|---|---|
| `nan` | a NaN result with another sign or payload than ARM's: x86's default NaN is negative (0xFFC00000), x86 propagates the first NaN operand where ARM prefers a signalling one, LLVM may commute operands; FPCR.DN is not modelled. `LIFT2_DEFAULT_NAN=1` at lift time gives ARM's default NaN for a NaN made from non-NaN operands | every FP operation producing a NaN |
| `fz-boundary` | under FZ, a result of the smallest normal magnitude: ARM detects tininess before rounding, x86 after | FP arithmetic under FZ |
| `fz-minmax` | under FZ, a subnormal operand returned unflushed where ARM returns the signed zero | FMIN/FMAX/FMINNM/FMAXNM and their pairwise and across-lane forms |
| `snan-minmax` | a signalling NaN operand treated as quiet (the number returned) | FMINNM/FMAXNM and their pairwise/across forms |
| FPSR | MRS FPSR sees MXCSR's sticky bits, which also record lanes and operations the guest never had | MRS FPSR (8 sites) |
| FRSQRTS | the halving is a separate exact multiply by 0.5 after the fused (3 - a*b) | results below 2^-125 (single) |

  Exact: FMIN/FMAX/FMINNM/FMAXNM signed zeros and quiet NaNs, every saturating conversion and rounding mode of
  FCVT*/FRINT*, FRECPE/FRSQRTE, FRECPS, FCMP/FCCMP flags, every integer NEON operation.

### System calls and traps

- **SVC**: the flags stored, `a64_svc(ctx, imm)`, then the reload of every register.
- **UDF, BRK, HLT** and words that are not code: `a64_trap(ctx, pc, word)`.
- MRS/MSR TPIDR_EL0 and MRS TPIDRRO_EL0 read and write the context; MRS CNTPCT_EL0/CNTVCT_EL0 call `recomp_cntpct`
  (the host's `a64_cntpct_host`, `host/mkw_glue.c`: 31.25 MHz), CNTFRQ_EL0 is the constant 31,250,000.

### Hooks and probes

The lifted code is never edited after the lift: everything the host adds is named by guest address, and every site
names its instruction word, so another game version stops the lift or the build.

- **Image hooks** (`nx2/standalone.py`, `a64_hooks.c`): HLE functions in `host/` and generated stubs for the OS-facing
  imports. `f_<addr>` is the host function; the guest body is not lifted. A BL to one keeps x19-x29 and SP unless
  `host_effects.CLOBBERS` names it (Calls and returns).
- **Entry hooks**: `ENTRY_HOOK(0xADDR)` and `ENTRY_HOOK_SYM(name)` in `host/*.c` (read by `nx2/entry_hooks.py`). The
  function starts with `entry_<addr>(ctx)`; 1 returns at once (the hook did the call's work), 0 runs the body. A hook
  whose address is not a lifted function, or whose symbol the dump does not export, stops the build (`game.prepare`).
  The hook's C never sees the context: `host/hos.h`'s macro hands it x0-x7 and x30 as plain values (`HookArgs`) and an
  opaque `HookCall` through which alone it acts on the call: `hook_return` (x0), `hook_set_arg` (x0-x7 before the
  body), `hook_run` (the lifted body now), `hook_call_guest` (guest code on the call's stack, every register put back).
  Its declared reads are x0-x7 and x30 (`host_effects.HOOK_ARGS`), plus SP for the five that call guest code
  (`HOOK_READS`); a hook that runs the body reads what the body reads. A BL to an entry-hooked function keeps what its
  body's summary (and `host_effects.CALLS`) keeps. `HOS_DBG=hooks` logs each hook's first call.
- **Patch declarations** (`host/patch.h`): Value probes and At rows are declared in the host's C, next to their bodies:
  `PATCH_AT(pc, word(W), mode, result, fn, operand...) { body }` and, for each further site of the same function,
  `PATCH_AT_SITE(...)`. The lifter reads them with a token scanner of a fixed grammar (`nx2/lift2/patch_scan.py`:
  comments, strings, continuations and nested parentheses handled; no C parser, no regular expression over the
  source); the compiler expands the same text into the function's prototype (a further site whose operand types differ
  is a compile error) and into a record in the object section `hos_patch`, which the build compares with the scan
  (`patch_scan.check_records`: a declaration one of them sees and the other does not stops the build). A declaration
  stands at file scope, outside `#if`, and is never made by another macro; a malformed one, a bare word where
  `word(...)` belongs, an unknown register, mode or result, a second `PATCH_AT` of one function or a duplicate site
  stops the scan with file:line. The containing function is not written: the build finds it from the plan
  (`probes.resolve`), and a site in no lifted function or in more than one stops it with the declaration's file:line.
  Rows at one instruction run in the order of their declarations (the host's files in the build's order, each file's
  in its order).
  - mode `after`: after the instruction (after a BL or BLR when the call returns); `before`: on the edge into it, which
    must not be a branch target; `instead`: in place of the BL or BLR (the call is not made, x30 set as the call
    would; an At row with `replace`), or in place of any other instruction's result (a Value probe: the result is the
    instruction's destination, an i32, as the frame-step cuts of the pick-up indicator and the water's ripples,
    `fcvtzs w12, s0` at 0x80a88300 and `fcvtzs w9, s0` at 0x800675f8, take `hos_frame_whole`).
  - result: `none`, `xN` (`uint64_t`), `wN` (`uint32_t`, zero-extended), `sN` (`float`, the rest of Vn zero), `qN`
    (`hos_q`). Operands, one parameter each: `xN`, `wN`, `sN`, `qN`, `sp`, `ld(xN or sp, K)` (the 64-bit guest word
    there, loaded by the lifted code: a frame slot the host needs is handed over as a value, the host does not read
    the frame), `add(xN, K)`, `k(K)` (a constant); `as(operand, name)` names the parameter. `guest(callees...)` or
    `guest(computed)` first: the host function calls guest code, its first parameter a `GuestCall` whose `guest_call`
    runs it on a given SP and puts every register back, so the row writes nothing else. `io(regs...)` last: registers
    (`xN`, `wN`, `qN`) the host function rewrites through a struct (its last parameter; the lifter allocates it in the
    entry block).
  The values stay SSA values; the call is never moved or removed unless the row replaces it. A row's result or io
  register must be caller-saved or written by the instruction itself, so a row never changes what its function
  preserves (checked at the build). The host functions take plain values and guest addresses, never the context; a
  guest object is a typed pointer to a struct of the fields used at their guest offsets, each `_Static_assert`ed,
  `may_alias`.

| family | host functions | guest functions |
|---|---|---|
| dynamic resolution, fixed scale | `hos_oot_dynres_flags`, `hos_oot_fixed_scale`, `hos_oot_scale_recompute`, `hos_oot_scale_flags` | `f_80825aa0`, `f_831f3200` |
| native scene (`HOS_SCENE_NATIVE`), `HOS_FIX_RES` | `hos_oot_scene_mode`, `hos_oot_scene_check` | `f_806702e0`, `f_831f3200` |
| frame seconds | `hos_oot_frame_step_probe`, `hos_oot_frame_step` | `f_80706be0`, `f_83b6ea90` |
| movie frames | `hos_oot_movie_count` | `f_83b7c710` |
| output size | `hos_oot_output_size`, `hos_oot_output_config`, `hos_oot_base_size` (x2), `hos_oot_base_size_bits` (x2) | `f_81222ce0`, `f_810a7b70`, `f_80ea2f30` |
| render-target pool | `hos_oot_rt_pool_config`, `hos_oot_rt_heap_size`, `hos_oot_rt_heap_use` (after a BLR), `hos_oot_rt_rest` | `f_836ce9d0`, `f_83b7b340`, `f_80b72340` |
| text speed | `hos_oot_text_time` (before) | `f_80b1e140` |
| native mouse camera | `hos_mouse_camera` (`host/mouse_native.c`) | `f_80644c80` |
| widescreen | `hos_oot_wide_out` (x2), `hos_oot_wide_scene`, `hos_oot_wide_ratio`, `hos_oot_wide_projection_ratio` (x4), `hos_oot_wide_movie`, `hos_oot_wide_hud` (q1, q4 -> q1), `hos_oot_wide_save_picture` (io w1-w4) | `f_81222ce0`, `f_83213890`, `f_804e48c0`, `f_834a5cc0`, `f_8006ec60`, `f_8025b910`, `f_8035a8c0`, `f_83b7c710`, `f_80794330`, `f_802a46e0` |
| occlusion off (`HOOKSHOT_OCCLUSION_OFF=1`) | `hos_oot_occlusion_flag` (x6) | `f_811cc790` |
| `HOS_BENCH`'s "in play" | `hos_oot_player_update` | `f_82c415e0` |
| per-core ring owners | `hos_oot_ring_write` (x14: 7 pushes, 7 pops) | `f_8004ab40`, `f_800aff80`, `f_800b3ce0`, `f_800b73d0`, `f_8073a090`, `f_8073c900`, `f_83af6150`, `f_83af59c0` |
| renderer patches (`host/direct_*.c`, `host/rt_slots.c`) | depth copy and job sync retargets (x8 / x4), job call (x8, x21, `[sp+0x10]`), mesh list resets, mesh args, sky LUT and terrain tiles (x2: the memcpy's size), render-target slot (x10, `[sp+192]`), mesh system binds (guest call), layer ahead (guest call, `[sp+0x48]`), inline passes (two calls of `f_80681320`), the job view (x25), list kinds (13 recorders) | see the table below |

The patches a caller makes of its own call (no host function is told who called it; a row at the call site says it):

| caller | site | what the caller does, and why it is patched |
|---|---|---|
| `f_8348c680` frame_body | `bl` GetSystemTick at 0x8348c76c, replaced | its frame-end clock read; the replacement also notes x19, the graphics module, for `host/nn_vi.c`'s real-time step |
| `f_800998a0` job_calc_runner | three `bl` SleepThread (0x8009a474, 0x8009afec, 0x8009b230), replaced | polls its sub-stages with SleepThread(20 us); a timed 20 us sleep with 1 us timer slack (`hos_oot_calc_sleep`) where SleepThread spins, ~3.9 ms of CPU a frame less (`HOS_CALC_NAP=0`: SleepThread's own) |
| `f_805902c0` frame_present_submit | `bl` SleepThread at 0x8059051c, replaced | polls its frame's NVN event between sleeps; `HOS_PRESENT_POLL=1` (an experiment, off) looks at the event between yields |
| `f_80667d20` system_texture_table_write | `bl` f_800634b0 at 0x806680d8, replaced (guest call) | re-registers one system texture's descriptor every frame; the call is skipped while the entry, view and texture are unchanged |
| `f_80f6c9d0` agl_memory_pool_create | after `ldr x8, [x8]` at 0x80f6cb18 | agl's pool wrapper; names the pool its nvnMemoryPoolInitialize makes, so the texture heap (made inside f_80f6c580's run) is told to the back end |
| `f_80289590`, `f_80666d20` | the depth-copy retargets (#0, #1) | the two depth copies that need barriers around them (their calls at 0x80289cc0, 0x806675d8) |
| the 13 list recorders (`host/direct.c`) | after `ldr x8, [x8]` before BeginRecording's blr | the function beginning a command list names its kind (load lists 1-6, job, layer, gate, compute, head) |
| `f_80084de0` render_job_record | after `ldr x8, [x8]` at 0x80085334 | its BeginRecording's list is the job's; x25 is the job's view record |
| `f_802640a0` pass_local_shadow_depth | after `str w8, [sp, #0x38]` at 0x80264128 | its alloc of the local shadow map, whose size is a class (`host/rt_slots.c`) |
| `f_806688c0`, `f_8028a2a0` | before their calls of `f_80681320` | the layer list drawing a recorded pass inline; the pass's key and draw context handed over (`host/direct_passes.c`) |

`f_80365f00` (report_reader_a) marks its own run with an entry hook that runs its body, so the occlusion grid slots' invalidates
inside it do not wait (`host/nvn_native.c` `readback_stamped`).

- **The per-core rings** are an assertion, not a patch. The singleton at `**0x84c9e890` holds seven rings (ring k at
  +0x20 + k * 0x30: buffer +0x10, capacity +0x18, head +0x1c, count +0x20). The game picks a ring by
  GetCurrentCoreNumber and pushes and pops without a lock or atomics, which is safe only while one thread per core
  number writes each ring. `hos_oot_ring_write` (`host/oot_probes.c:394`) records each ring's first writer and logs
  another writer once per ring and thread; the bench's end logs each ring's owner. The rows stay on in normal play.
- **No site takes the whole context.** The former C after an instruction (widescreen's HUD and save picture) and the
  `SPLICE_AFTER` bodies of the renderer patches are At rows; the camera jitter for temporal upscalers is parked
  ([parked-patches.md](parked-patches.md)).

### Fibers and C++ exceptions

- **Fibers**: `nn::os::InitializeFiber`, `SwitchToFiber`, `FinalizeFiber`, `GetCurrentFiber` are HLE'd
  (`host/hos_thread.c`): a guest fiber is a host fiber with its own guest context. The SDK's fiber assembly is never
  reached.
- **C++ throws are a fatal stop** (`host/cxx_throw.c`): `__cxa_throw` (with the type_info's name), `__cxa_rethrow`,
  `_Unwind_RaiseException` and `_Unwind_ForcedUnwind` stop the game with the caller's guest address. Unwinding through
  lifted frames is not supported: the guest unwinder's resume would return into host frames. The game has 24
  `__cxa_throw` call sites (main 6, sdk 18) and 9 real catchers (main 1, sdk 8); the one in main (0x823b01c0) is
  reached only from libc++'s length checks. The kokiri smoke and a 600-frame market bench throw nothing.

## Whole-program summaries and the verification build

`nx2/lift2/summary.py` computes per function, over the call graph's strongly connected components (callees first, a
fixed point inside each): inputs (backward liveness), clobbers and must-defs, the preserved callee-saved registers (a
frame pass tracking SP, x29 and copies as entry SP plus a constant, and which frame slots hold entry values of
x19-x30 and d8-d15), results, noreturn (seeded by the fatal functions' names and BLs with nothing after them), FP use.
Every build has them: the lift writes the job's input beside the plan (`game.plan_summaries`;
`work/sa/summary_input_<key>.pickle`, kept until the next lift) and the cache's key: `work/sa/summaries_<key>.pickle`,
a SHA-1 of the image, the discovery's facts, the HLE'd functions' symbols, the entry hooks, the code of
`enc*`/`sem*`/`ir`/`bounds`/`build_ir`/`summary`/`probes`/`host_effects.py` (`game.source_key`: the syntax tree without
comments, docstrings and the patch tables) and the patches' declared effects (`game.summary_effects`: every row's reads,
writes, guest callees and replace; every entry hook's reads, writes and guest callees; every HLE'd function's summary).
A build whose patches declare other effects than the summaries in the plan count computes them again from the input,
without a lift; a row whose effects are the same (its host function renamed, say) reruns nothing. The build's first
step computes them before the lift workers and the
compilers start, in a process of its own (`game.finish_summaries`, `summary.job`: 8 workers over the components' DAG,
`HOOKSHOT_LIFT_SUMMARY_JOBS`), then packs them into the plan. Cold: 108 s (graph 17 s, fixed point 59 s, results
26 s), 1.4 GB in the summary process and 1.3 GB in its largest worker; from the cache: 5 s. The build uses them at
direct calls (above) and prints the sites (`bounds.call_report`): of 794,900 BL sites, 766,648 keep registers across
the call (12.0 of x19-x29 and SP a site on average), 71,491 of them to host callees; the others reload everything: a
noreturn callee (trap after) 14,170, a demoted one 13,693, an entry-hooked function whose body has no full summary 327
(192 of them to `f_8005ed00`, the render-target allocator), a setjmp owner 39, an HLE'd function declared clobbering 23.
The 182,791 BLR sites reload everything. (Before the probes declared their effects, 2026-11-15, a call of a function
with a probe, splice or C after an instruction reloaded everything: 551 sites.)

**The host's patches** are part of the facts, each by its declaration, so a function's summary covers the host code
that runs inside it or in its callees:
- Value probes and At rows (`probes.row_effects`, `summary.rows`): their declared reads and writes are their
  instruction's; a row whose host function calls guest code (`guest`, through `guest_call`, which puts every register
  back) also reads those functions' inputs there (a call-graph edge to each), and every register for `"computed"`; a
  row in place of a BL or BLR (`replace`) makes that instruction no call: it writes x30 and the row's result.
- An ENTRY_HOOK (`summary.ENTRY`, `entry_hooks`): at the entry it reads `host_effects.hook_reads` (x0-x7 and x30, SP
  for the five hooks that call guest code) and the inputs of the guest functions `host_effects.CALLS` lists; its
  "return 1" path is a return that may write x0-x7 (`hook_return`, `hook_set_arg`), so the function returns (never
  noreturn), writes nothing on every path (must-writes 0) and may write x0-x7. `hook_run` runs the function's own
  body, whose facts the summary has. These reads are not the entry instruction's: x30 read by the hook is a label, not
  the function's LR as data.
- An HLE'd callee (`summary.host_summary`): AAPCS64 facts with the declared reads (`host_effects.hle_reads`:
  x0-x8, v0-v7, SP, x30 and `HLE_READS`); one declared clobbering (`CLOBBERS`) or without a known symbol reads every
  register. The lift's record of an HLE'd callee takes the same facts (`bounds.call_bounds`).
The build stops when the ENTRY_HOOKs it finds differ from those the plan's summaries count (lift again).

A function is demoted (its interface is outside the AAPCS64) by detection, not by list:

| reason | functions | what |
|---|---|---|
| tmp_in | 7,110 | x9-x18 live at entry |
| tmp_after_call | 5,391 | x9-x18 or flags read after a call that may change them |
| cs_clobber | 2,086 | a callee-saved register not proven preserved at a return |
| unknown callee | 1,729 | calls a function with unknown facts |
| sp | 1,682 | SP not entry SP plus a constant, or not entry SP at a return |
| lr_ret | 1,575 | a return not through the entry x30 |
| flags_in | 692 | NZCV live at entry |
| split_frame | 214 | a tail call with the frame still allocated |
| save_ptr | 26 | entry x19-x30, SP or d8-d15 stored through a pointer outside the frame |
| inconsistent | 20 | a preserved register written but never read |
| lr_data | 7 | entry x30 used as data |
| fallthrough_entry | 1 | entered by falling through from another function |
| dangling_bl | 1 | a BL with nothing after it to a callee that returns |

sp, save_ptr, lr_data, lr_ret and split_frame make a function's facts unknown (its callers assume it reads and writes
everything and preserves nothing). The x9-x18 counts are upper bounds: the liveness is path-insensitive, and the
examples read (main 0x809d78c0, 0x80051080) read x18 or x12 on paths correlated branches make infeasible.

Over the whole game (294,523 functions, 17.65 M instructions): 287,064 functions (88.1 % of the instructions) have
full summaries; 669 are noreturn; 6,996 write FPCR or call something that does; 31,854 contain FP or NEON; 610,321
callee-saved save/restore pairs in 174,408 functions. Call edges (1,073,417; a noreturn seed counted as its target):

| target | edges | share |
|---|---|---|
| direct to a full summary (BL 735,355, tail 68,519) | 803,874 | 74.9 % |
| indirect (BLR 182,791, BR 23,561) | 206,352 | 19.2 % |
| an HLE'd function (44,283 + 2,221) | 46,504 | 4.3 % |
| direct to a demoted function (15,256 + 1,431) | 16,687 | 1.6 % |

**The verification build** (`HOOKSHOT_LIFT_VERIFY=1` at build time) wraps every function with a full summary
in `host/lift2_verify.c`: at entry it poisons the registers the summary says are not read (`0xDEAD`, register,
function); at exit it reports a noreturn function that returned, a preserved register changed, and a register written
that the summary says is not; then it restores what is preserved and poisons the written registers no caller reads
(`0xDEAE`). Violations are logged once per function, kind and register to the game log and `lift2_violations.txt`.
Result (2026-11-14, 262,527 functions): zero violations and no crash in the kokiri smoke scene, the market and
zoo_static benches (600 frames each) and a 100 s title boot.
Before every BL it fills the context fields no reader reads there (`omit_stores`: neither the callee's record reads
them nor the caller reloads them before the callee must write them) with `0xDEAF`, the register and the BL's address
(IR `sitepoison`; only registers the function itself may write).
The exit check reports a result the summary says the function writes on every path still holding it ("result not
written, store omitted at a call", naming the BL); an input holding it at entry is counted, not reported, as inputs
over-approximate (an indirect jump's target is assumed to read the AAPCS64's arguments, not the x29 and x30 a callee
saves; host code calls guest functions with fewer arguments than their summaries list). The log counts entries
meeting the site poison and exits poisoning registers (`lift2 verify: N ...` at powers of two).
It also routes every BL to an HLE'd function declared clean through a stub (`hchk_<addr>`, `game.HOST_CHECK`) that
reports x19-x29 or SP changed by the host (`lift2_host_enter`/`lift2_host_exit`). Result (2026-11-15, with the host
checks, the omitted stores and the patches in the summaries): zero violations in the kokiri smoke scene (40 s), with
2^28 entries meeting a site poison, 2^20 of them in an input, and 2^27 exits poisoning a register.

## The build

- **Partitions** by guest address, about 120,000 guest instructions each (`game.PART_INSNS`); the hot list's functions
  (`HOOKSHOT_HOT_FUNCS`, default `work/hot_funcs_play.txt`, made by `scripts/hotlist.py`) in partitions of their own.
- **Optimisation levels**: hot partitions at `-O2` (`RX_HOT_OPT`), the rest at `-O1` (`nx2/build.py --opt`). clang's
  -O1 pipeline has no DSE, so a level without it runs as `opt` with clang's own pipeline for that level and `dse` after
  `memcpyopt`, where -O2 has it, then `clang -Xclang -disable-llvm-passes` for the code generation at that level
  (`game.opt_argv`, `game.compile_ir`; `dump.py` the same). On 6 cold partitions against plain -O1: `.text` -13.3 %,
  context stores in the optimised IR -32.8 %, compile time the same (59 s); -O2 gave -15.9 % and -36.7 % in 133 s.
  Every partition also gets `-march` (`RX_MARCH`, else the host's: x86-64-v3 on an AVX2 machine), `-ffp-contract=off`,
  `-ffunction-sections`, `-mllvm -rotation-max-header-size=0` and `game.DSE_FLAGS` (capture tracking up to 100,000
  uses, DSE's per-block definitions 100,000, scan and walk 1,000, other-block cost 1). No `-mno-red-zone`.
- **Caching**: a partition's object is kept in `work/objcache` by an input key: the lifter's code (`game.code_key`:
  every `nx2/lift2` source as `game.source_key` takes it, so comments and the patch tables are not in it), the
  discovery's facts (`game.facts_key`: the plan as the lift wrote it, the summaries left out), the flags,
  `HOOKSHOT_LIFT_VERIFY`, its functions, and for each its rows, its entry hook, its summary and the call-boundary record
  of every function it may branch, call or fall to (`game.part_key`, `game.branch_targets`). The code pointer translation
  (`code_ptrs.targets`, `code_ptrs.sites`) reads only the plan (`code_excluded`, the bodies, the jump tables) and its
  own code, both in the key; the count of sites a partition translated is kept beside its object under the same key
  (`.cp`). An unchanged partition is
  neither lifted nor compiled again: editing a row recompiles the partition holding its function (and those whose
  callees' records its declared effects change); editing a semantic template, the IR, the emitter, `bounds.py` or the
  lowering code of `probes.py` recompiles every partition, as every function's IR may change with them. Objects no build links are removed after `HOOKSHOT_OBJCACHE_DAYS` (default 3).

| switch | when | what |
|---|---|---|
| `nx2/build.py --jobs N` (`HOOKSHOT_JOBS`), `--opt O1` | build | parallel jobs (default: by the memory, `build.default_jobs`); the cold partitions' level |
| `RX_HOT_OPT` | build | the hot partitions' level (default `O2`) |
| `RX_MARCH` | build | the target CPU (default: the host's) |
| `HOOKSHOT_HOT_FUNCS` | lift | the hot list |
| `HOOKSHOT_LIFT_VERIFY=1` | build | the verification build |
| `HOOKSHOT_LIFT_SUMMARY_JOBS` | build | the summaries' workers (8) |
| `LIFT2_DEFAULT_NAN=1` | lift | ARM's default NaN for generated NaNs |
| `HOOKSHOT_LLVM_BIN` | lift and build | where clang is |
| `nx2/lift.py --max-func N` | lift | cut functions larger than N instructions where no branch crosses |
| `HOOKSHOT_OBJCACHE_DAYS` | build | object cache retention |

| measure | value | at |
|---|---|---|
| executable | 209.24 MB | stores no reader reads omitted, by the call-boundary records |
| `.text` / lifted code (hot / cold partitions) / bss | 172.33 MB / 169.35 MB (7.72 / 161.63) / 40.05 MB | stores no reader reads omitted, by the call-boundary records |
| x86 code bytes per guest instruction | 15.6 on average (with page marks and polls; not rescaled) | 860eb332 |
| relift, cold (`scripts/relift.sh`) | 556 s with the summaries computed (lift 95 s; build: summaries 110 s, partitions lifted 106 s, compiled 237 s with 7 jobs), 13.07 GB peak (16 GB scope); from the summaries' cache about 110 s less | stores no reader reads omitted, by the call-boundary records |
| relift, warm (partitions from the cache) | about 100 s | 338ce2c0 |
| host-only change | about a minute | commit not recorded |
| market 600, fixed step | frame mean 9.43-9.63 ms (one run on each of this change's last three builds), median 9.42-9.60, p99 10.02-10.34; CPU 14.04-14.41 s over the 600 frames | stores no reader reads omitted |
| lift speed, one core | 33-56 k guest instructions/s (decode, IR, LLVM IR) | commit not recorded |

## Checks

| check | what |
|---|---|
| `tests/test_lift2.py` | the decoder's tables, every template on synthetic states, small programs (no dump) |
| `tests/test_lift2_fp.py` | FP and NEON forms on edge values; the allowed deviations counted |
| `tests/test_lift2_discover.py`, `test_lift2_tables.py`, `test_lift2_program.py`, `test_lift2_summary.py`, `test_lift2_verify.py` | discovery on a synthetic image, tables, a lifted program with the generated runtime, summaries, the verification wrapper |
| `nx2/lift2/selfcheck.py` | the decoder against llvm-mc on every distinct word of the game |
| `nx2/test_lift2_leaf.py` | random functions and call trees of the game run from random states against Unicorn (`nx2/lift2/reference.py`; the dump) |
| `nx2/lift2/dump.py` | a function's guest code, IR, LLVM IR, optimised IR and x86, to read |
| in the game | the smoke scene against the phashed golden, a title boot, the benches (`docs/development.md`) |

## Pessimizations and their justification

Each entry: what is emitted, what it costs, what would remove it, and why it is kept. "Unproven" means nothing in this
tree shows the constraint is needed; the experiment named decides it.

1. **Context traffic.** Write-through of every register before calls and exits, and a reload after every BLR and
   SVC, and after a BL of every register but the callee's preserved x19-x29 and SP (all of them after a callee
   without a full summary, an HLE'd function declared clobbering, an entry-hooked function whose body has no full
   summary, a setjmp owner). About 31-33 % of code bytes on the sample were context accesses
   (`disp(%reg)`) at 860eb332, before the preserved registers stayed in SSA; not measured again. Since `captures(none)`
   on the declarations and `game.DSE_FLAGS`, a store overwritten before anything reads the context is dropped. What
   stays (2026-11-15, the lifter's IR over the whole game): of 8.45 M stores reaching a reader, 29 % are read (callee
   inputs, reloads, results), 25 % dead (a temporary the callee does not read and the caller does not reload), 21 %
   kept only for a BLR, SVC, unknown callee or exit, 14 % callee-saved registers and x30 the callee only saves, 9 %
   callee-saved registers at a return (7 % epilogue restores). `omit_stores` now drops the dead ones (overwritten,
   dead at a BL whose callee does not read them, not a result at a return); the stores kept only for a BLR, SVC,
   unknown callee or exit, and the callee-saved registers a callee only saves, remain
   ([lifter-optimisations.md](lifter-optimisations.md), C).
   *Justification:* the stores are needed before calls to code that reads the context: the callee's inputs, entry
   hooks (`emit_llvm.emit_function`: x0-x7, x30 and `host_effects.HOOK_READS`), `a64_svc`, `recomp_ldxr`/`recomp_stxr`,
   `a64_icall`/`a64_ijump`, `hos_sj_call`, callees with unknown facts, HLE'd functions (they read their arguments
   from the context); the reloads after callees the summaries do not cover and the host callees listed above.
   Elsewhere unproven.
2. **Callee-saved saves and restores, and guest frames in guest memory.** Every function keeps its x19-x30 and d8-d15
   saves and restores (612,455 pairs in 174,749 functions) and its frame in guest memory. Not measured alone.
   Removed by deleting the pairs and promoting frame slots ([lifter-optimisations.md](lifter-optimisations.md), C and
   F).
   *Justification:* the frame slots the host needs are loaded by the lifted code and handed over by At rows
   (`[sp+0x48]` in `f_8007fe60`, `[sp+0x10]` in `f_806688c0`, `[sp+192]` in `f_8005ed00`), so a promotion sees them as
   guest loads. The host still reads frames through x29: `pool_creation_key` (`host/nvn_native.c`, the return
   addresses of a pool's creation hashed into its placement key; `host_effects.HLE_READS`), the NVN logs' chain walks
   and the stall dump (diagnostics); and the guest calls of rows and hooks (`guest_call`, `hook_call_guest`) run below
   the SP they are given (`host/direct_mesh_system.c` builds a 0x100-byte block there). Those must keep x29 chains. For every other function unproven: unwinding is not a reason (throws are
   fatal). The experiment is the pair deletion for full-summary functions with a verification run.
3. **Host-stack spills.** 5.8 % of code bytes on the sample (size-weighted) are spills to the host stack. Cause not
   analysed. *Justification:* none; unproven what removes it (read the spills of the largest sample functions with
   `nx2/lift2/dump.py`).
4. **Sparse jump-table switches.** The tables the discovery cannot prove (278 of 1,475: the table's address not one
   constant on every path 223, no table load (a BR to ADR candidates 39, to a constant 1), a join between the load
   and the BR 15) switch on the target's offset, which LLVM lowers to compare chains when sparse. *Justification:*
   their index switch would rest on an unproven fact.
5. **STLR as `xchg`.** Every STLR is a `seq_cst` store (1,546 sites, 52 in hot functions; LDAR 1,478), so an STLR
   followed by an LDAR of another address keeps ARM's RCsc order. A plain x86 store is already a release store.
   *Justification:* unproven: no store-then-load-acquire pattern was looked for. Experiment: STLR as
   `store atomic release` (a `mov`) behind a lift-time switch, then the smoke and the market bench.
6. **Every function has a generic entry reachable through the table, and a guest-address path behind every BLR.**
   `a64_lookup` still serves all 295,270 table entries, though 102,243 of them are in no origin (no relocation, no
   code-formed address, no export, no init/fini entry); the BLR keeps its `a64_icall_guest` branch.
   *Justification:* evidence, not yet proof. The origins of code pointers are closed in this census: every relocated
   value that is code lands on a function entry (453,943 slots, 0 elsewhere), every code address formed in code is an
   entry or one of the kinds `code_ptrs.audit` accounts for, and in a kokiri smoke and a market bench (agent/addr-taken,
   HOS_DBG=addrtaken) no called target came from outside that set, apart from host calls and the host's own constants;
   with the translation, a kokiri smoke and a market bench logged `0 sites and 0 calls took the guest-address path; 0
   guest-valued code addresses reached the host`.
   Not proven: uses of vtable-loaded pointers other than calls (a static scan sees only the build-time origins), and
   coverage beyond two scenes. A non-call use found: one ordered compare (`net_grpc_authority_table_arg_cmp`), whose
   order now follows host addresses. Experiment that closes it: the summary line over a long play session
   (`code pointers: 0 sites ... 0 guest-valued`); then the guest path, the lookup's cache and the unreached entries go.
7. **Indirect calls through a pointer, opaque.** 182,791 BLR sites (19.2 % of call edges with BR's 23,561), 166,256 of
   them loading the target from a computed address (vtables), 1,335 from writable globals, 0 from read-only memory.
   Each calls a host pointer through a register: no per-site cache, no promotion, the callee unknown to LLVM.
   Removed by profile-guided promotion ([lifter-optimisations.md](lifter-optimisations.md), D). *Justification:* none.
8. **The cycle barrier.** A guest load in a loop is not hoisted out of it or merged across iterations (142,299 barrier
   blocks in 43,613 functions). Zero bytes; speed not measured. *Justification:* evidence: 8 count-bounded YIELD spin-waits on a plain byte load in the sdk
   (0x850f5640-0x850fac30, `subs w20, w20, #1; b.eq out; yield; ldrb w8, [x19, #0x40]; tbnz w8, #0, loop`) would wait
   out their count instead of seeing the flag if LLVM hoisted the load.
9. **Flags stored at exits.** A function that writes flags stores all four before every tail call and indirect jump.
   Not measured. *Justification:* evidence: 595 functions read NZCV at entry (`flags_in`), reached by split functions
   and shared tails; with summaries the store could be limited to targets with `flags_in`.
10. **SQRSHL and the other saturating shifts by register, lane by lane** (34 SQRSHL sites): an extract, a 128-bit
    computation and an insert per lane. Not measured. *Justification:* AVX2 has variable shifts for 32/64-bit lanes
    only; whether the sites are hot is unproven.
11. **SHA-256 in scalar code** (392 sites). Not measured. *Justification:* the build targets x86-64-v3, which has no
    SHA-NI (`nx2/build.py:403`); a per-function `target-features="+sha"` with a CPUID check is possible as for AES.
    Whether the sites are hot is unproven.
12. **CRC32 as a bit loop.** The C (Castagnoli) variants equal SSE4.2's `crc32`, one instruction. Sites not counted.
    *Justification:* none.
13. **-O1 for cold partitions** (with DSE added, The build). On 6 cold partitions -O2 is 3.0 % smaller (`.text`
    6.24 MB against 6.43 MB) and takes 2.25 times the compile time (133 s against 59 s). *Justification:* build time:
    the cold build's compile would go from about 4 to about 9 minutes for 3 % of cold code no profile shows hot.

## Open items

One line each; the queue with sources, soundness conditions and first steps is
[lifter-optimisations.md](lifter-optimisations.md).

- C: dead temporaries' stores before calls; pair deletion.
- D: per-site indirect call cache; profile-guided promotion.
- F: frame-slot promotion, leaves first.
- G: libc calls as intrinsics, CRC32C as `crc32`, noreturn as `unreachable`, CCMP chains, NEON gaps.
- STLR as release stores; the asserted address-taken set (pessimizations 5 and 6).
- Path-sensitive liveness for the summaries (the x9-x18 demotions).
