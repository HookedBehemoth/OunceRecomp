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
| runtime tables | `nx2/lift2/tables.py`, `nx2/standalone.py` | `a64_table.c` (the function table), `a64_hooks.c`, `hos_image.c`, `a64_stubs.c`, `nvn2_exports.c` in `work/sa/gen` |
| IR | `nx2/lift2/enc*.py`, `sem*.py`, `build_ir.py`, `ir.py` | the decoder's encoding tables, each form's semantics as IR, a function's IR |
| LLVM IR | `nx2/lift2/emit_llvm.py` | one textual module per partition |
| build | `nx2/lift2/game.py` `prepare`, `nx2/build.py` | partitions lifted in worker processes, piped into `clang -x ir -` (no IR on disk), cached by input key; runtime and host compiled and linked |
| runtime | `nx2/rtgen.py` from `third_party/a64-kit/rt` | `a64.h` (`GuestContext`, the guest window) and `a64_rt.c` (`a64_lookup`, `a64_icall`, `a64_ijump`, `a64_trap`, the exclusive monitor, the counter) |
| summaries | `nx2/lift2/summary.py` | whole-program register facts, for the verification build only |

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
  `writeback`). LLVM drops a store that a later one overwrites before any call or return; the last one stays.
- **Reload after calls**: after a BL, BLR, SVC or a call into host code with the context, every register read loads
  the context again (`ir.Builder.after_call` clears the block's definitions), except after a BL to a callee with a
  full summary that the host does not edit: the values of x19-x29 and SP its summary proves preserved stay SSA values
  across the call (`build_ir.build`, `Program.keeps`; `game.call_bounds`, `game.host_edited`). The registers are still
  written to the context before the call. The low halves of v8-v15 are reloaded: their upper halves are not
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
  call out of lifted code, so a thread spinning without calls shows stale registers.

### Calls and returns

- **BL**: x30 := the return address, then `call void @f_<callee>(ptr %ctx)`, then the reload of every register but
  the callee's preserved x19-x29 and SP (see above). A callee the summaries prove noreturn is followed by
  `a64_trap(ctx, <BL address>, <BL word>)` and `unreachable`, so a wrong summary stops the game; the code only its
  return reached is dropped. Host-edited callees (`game.host_edited`: HLE'd functions, entry hooks, setjmp owners,
  Value/At probes, splices, C after an instruction) reload everything and are not treated as noreturn.
- **B to another entry, or falling into one**: `musttail call void @f_<callee>(ptr %ctx)`; every function has the same
  prototype, so every tail branch is a guaranteed tail call.
- **RET**: `ret void`.
- **PLT stubs**: a call of one calls its target (`Program.callee`); a stub reached indirectly sets x16 (the GOT slot)
  and x17 (the target) and tail-calls the target.
- **setjmp owners** (the 13 functions that call `__setjmp`; their 14 return addresses are extra entries): a BL to one
  calls `sjcall_<owner>`, a stub that enters `hos_sj_call` (`host/hos_setjmp.c`). A longjmp comes back there and runs
  the owner from the setjmp return address. `__setjmp` and `longjmp` themselves run lifted.

### Indirect calls and jumps

- **BLR**: `call void @a64_icall(ptr %ctx, i64 %target)`, then the reload of every register.
- **BR without a table**: `a64_ijump(ctx, target)` and return.
- `a64_icall` and `a64_ijump` call `a64_lookup` (`third_party/a64-kit/rt/a64_rt.c:31`): one global direct-mapped
  cache of 2^18 64-bit slots keyed `(a >> 2 ^ a >> 20)`, each `address << 32 | offset`, in front of a binary search of
  the function table (every lifted entry), then the host's functions (`a64_lookup_host`); a miss is remembered too.
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
| FRECPE, FRSQRTE | ARM's estimate algorithm out of line per lane (`a64_frecpe_f32/f64`, `a64_frsqrte_f32/f64` in `host/hos_fp.c`) |
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
  imports. `f_<addr>` is the host function; the guest body is not lifted.
- **Entry hooks**: `ENTRY_HOOK(0xADDR)` and `ENTRY_HOOK_SYM(name)` in `host/*.c` (read by `nx2/entry_hooks.py`). The
  function starts with `entry_<addr>(ctx)`; 1 returns at once (the hook did the call's work), 0 runs the body. A hook
  whose address is not a lifted function, or whose symbol the dump does not export, stops the build (`game.prepare`).
  `HOS_DBG=hooks` logs each hook's first call.
- **Value probes** (`probes.PROBES`): replace an instruction's result. The frame-step cuts of the pick-up indicator and
  the water's ripples (`fcvtzs w12, s0` at 0x80a88300, `fcvtzs w9, s0` at 0x800675f8) take `hos_frame_whole`
  (`host/frame_whole.h`).
- **At rows** (`probes.AT`): `At(func, pc, word, fn, args, result)` is "after the instruction at `pc` (or before it,
  `before=True`, on the edge into it, which must not be a branch target): `result := fn(args...)`". Arguments are
  `xN`, `wN`, `sN` (a float) or `#K`; the result is `xN`, `wN`, `sN` or none. After a BL or BLR the row runs when the
  call returns. The values stay SSA values; the call is never moved or removed. The host functions
  (`host/oot_probes.c`) take plain values and guest addresses, never the context; a guest object is a typed pointer to a
  struct of the fields used at their guest offsets, each `_Static_assert`ed, `may_alias`.

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
| widescreen | `hos_oot_wide_out` (x2), `hos_oot_wide_scene`, `hos_oot_wide_ratio`, `hos_oot_wide_projection_ratio` (x4), `hos_oot_wide_movie` | `f_81222ce0`, `f_83213890`, `f_804e48c0`, `f_834a5cc0`, `f_8006ec60`, `f_8025b910`, `f_8035a8c0`, `f_83b7c710` |
| occlusion off (`HOOKSHOT_OCCLUSION_OFF=1`) | `hos_oot_occlusion_flag` (x6) | `f_811cc790` |
| `HOS_BENCH`'s "in play" | `hos_oot_player_update` | `f_82c415e0` |
| per-core ring owners | `hos_oot_ring_write` (x14: 7 pushes, 7 pops) | `f_8004ab40`, `f_800aff80`, `f_800b3ce0`, `f_800b73d0`, `f_8073a090`, `f_8073c900`, `f_83af6150`, `f_83af59c0` |

- **The per-core rings** are an assertion, not a patch. The singleton at `**0x84c9e890` holds seven rings (ring k at
  +0x20 + k * 0x30: buffer +0x10, capacity +0x18, head +0x1c, count +0x20). The game picks a ring by
  GetCurrentCoreNumber and pushes and pops without a lock or atomics, which is safe only while one thread per core
  number writes each ring. `hos_oot_ring_write` (`host/oot_probes.c:394`) records each ring's first writer and logs
  another writer once per ring and thread; the bench's end logs each ring's owner. The rows stay on in normal play.
- **The context-based sites**: C after an instruction (`probes.after_c`: `oot_wide`'s HUD in `f_80794330` and save
  picture in `f_802a46e0`, `oot_jitter`'s 3 sites in `f_806dc520`) and the `SPLICE_AFTER` bodies of `host/direct_*.c`
  and `host/rt_slots.c` at `probes.SPLICE_AT`/`SPLICE_BEFORE`. Each is a host function `lift2_after_<pc>(Ctx *c)` the
  lifted code calls with the context; the registers are reloaded after it. They exist where an At row cannot express
  the patch: 128-bit registers (the HUD, jitter), several results (the save picture, the sky LUT and assets, `rt_slots`),
  guest calls (the mesh system) or the guest stack through SP (the layer-ahead). `oot_jitter` gives a jitter only when
  the renderer sets `HOS_OPT_DLSS_JITTER_W`/`_H`, which the direct back end does not.

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
Every build has them: the lift writes the job's input beside the plan unless the cache has them
(`game.plan_summaries`; `work/sa/summaries_<key>.pickle`, the key a SHA-1 of the image, the discovery's facts and
`enc*`/`sem*`/`ir`/`build_ir`/`summary.py`), and the build's first step computes them before the lift workers and the
compilers start, in a process of its own (`game.finish_summaries`, `summary.job`: 8 workers over the components' DAG,
`HOOKSHOT_LIFT_SUMMARY_JOBS`), then packs them into the plan. Cold: 108 s (graph 17 s, fixed point 59 s, results
26 s), 1.4 GB in the summary process and 1.3 GB in its largest worker; from the cache: 5 s. The build uses them at
direct calls (above) and prints the sites (`game.call_report`): of 794,900 BL sites, 695,413 keep registers across
the call (12.0 of x19-x29 and SP a site on average); the others reload everything: to an HLE'd function 44,288, an
entry hook 27,616, a noreturn callee (trap after) 14,170, a demoted one 13,049, a probe, splice or C-after function
325, a setjmp owner 39. The 182,791 BLR sites reload everything.

A function is demoted (its interface is outside the AAPCS64) by detection, not by list:

| reason | functions | what |
|---|---|---|
| tmp_in | 6,659 | x9-x18 live at entry |
| tmp_after_call | 5,118 | x9-x18 or flags read after a call that may change them |
| cs_clobber | 2,086 | a callee-saved register not proven preserved at a return |
| unknown callee | 1,729 | calls a function with unknown facts |
| sp | 1,682 | SP not entry SP plus a constant, or not entry SP at a return |
| lr_ret | 1,575 | a return not through the entry x30 |
| flags_in | 595 | NZCV live at entry |
| split_frame | 214 | a tail call with the frame still allocated |
| save_ptr | 26 | entry x19-x30, SP or d8-d15 stored through a pointer outside the frame |
| inconsistent | 24 | a preserved register written but never read |
| lr_data | 7 | entry x30 used as data |
| fallthrough_entry | 1 | entered by falling through from another function |
| dangling_bl | 1 | a BL with nothing after it to a callee that returns |

sp, save_ptr, lr_data, lr_ret and split_frame make a function's facts unknown (its callers assume it reads and writes
everything and preserves nothing). The x9-x18 counts are upper bounds: the liveness is path-insensitive, and the
examples read (main 0x809d78c0, 0x80051080) read x18 or x12 on paths correlated branches make infeasible.

Over the whole game (294,523 functions, 17.65 M instructions): 287,463 functions (88.4 % of the instructions) have
full summaries; 708 are noreturn; 6,996 write FPCR or call something that does; 31,854 contain FP or NEON; 612,455
callee-saved save/restore pairs in 174,749 functions. Call edges (1,073,423):

| target | edges | share |
|---|---|---|
| direct to a full summary (BL 725,080, tail 68,544) | 793,624 | 73.9 % |
| indirect (BLR 182,791, BR 23,561) | 206,352 | 19.2 % |
| host function or hook (41,704 + 2,221) | 43,925 | 4.1 % |
| direct to a demoted function (13,544 + 1,405) | 14,949 | 1.4 % |
| a noreturn seed | 14,573 | 1.4 % |

**The verification build** (`HOOKSHOT_LIFT_VERIFY=1` at build time) wraps every function with a full summary
in `host/lift2_verify.c`: at entry it poisons the registers the summary says are not read (`0xDEAD`, register,
function); at exit it reports a noreturn function that returned, a preserved register changed, and a register written
that the summary says is not; then it restores what is preserved and poisons the written registers no caller reads
(`0xDEAE`). Violations are logged once per function, kind and register to the game log and `lift2_violations.txt`.
Result (2026-11-14, 262,527 functions): zero violations and no crash in the kokiri smoke scene, the market and
zoo_static benches (600 frames each) and a 100 s title boot.

## The build

- **Partitions** by guest address, about 120,000 guest instructions each (`game.PART_INSNS`); the hot list's functions
  (`HOOKSHOT_HOT_FUNCS`, default `work/hot_funcs_play.txt`, made by `scripts/hotlist.py`) in partitions of their own.
- **Optimisation levels**: hot partitions at `-O2` (`RX_HOT_OPT`), the rest at `-O1` (`nx2/build.py --opt`). Every
  partition also gets `-march` (`RX_MARCH`, else the host's: x86-64-v3 on an AVX2 machine), `-ffp-contract=off`,
  `-ffunction-sections`, `-mllvm -rotation-max-header-size=0`. No `-mno-red-zone`.
- **Caching**: a partition's object is kept in `work/objcache` by an input key (the plan's bytes, the lifter's
  sources, the flags, its functions, the hooks, `HOOKSHOT_LIFT_VERIFY`); an unchanged partition is neither lifted nor
  compiled again. Objects no build links are removed after `HOOKSHOT_OBJCACHE_DAYS` (default 3).

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
| executable | 234 MB | 338ce2c0 |
| `.text` / lifted code / bss | 232.63 MB / 196.46 MB / 40.10 MB | 338ce2c0 |
| x86 code bytes per guest instruction | 15.6 on average (with page marks and polls; not rescaled) | 860eb332 |
| relift, cold (`scripts/relift.sh`) | 414 s (lift 89 s, compile 230 s), 10.4 GB peak | 338ce2c0 |
| relift, warm (partitions from the cache) | about 100 s | 338ce2c0 |
| host-only change | about a minute | commit not recorded |
| market 600, fixed step | frame mean 9.543 ms, median 9.536, p99 10.047; CPU 16.89 s over the 600 frames | 338ce2c0 |
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
   without a full summary or one the host edits). About 31-33 % of code bytes on the sample were context accesses
   (`disp(%reg)`) at 860eb332, before the preserved registers stayed in SSA; not measured again. `nocapture` on
   callees saves 0.0 % (-O1) / 0.7 % (-O2), so the cost is the convention, not missing alias facts. Reduced further by
   dropping dead temporaries' stores ([lifter-optimisations.md](lifter-optimisations.md), C).
   *Justification:* the stores are needed before calls to code that reads the context: the callee's inputs, entry
   hooks (`emit_llvm.emit_function`), C-after and SPLICE sites, `a64_svc`, `recomp_ldxr`/`recomp_stxr`,
   `a64_icall`/`a64_ijump`, `hos_sj_call`, callees with unknown facts; the reloads after callees the summaries do not
   cover or the host edits. Elsewhere unproven.
2. **Callee-saved saves and restores, and guest frames in guest memory.** Every function keeps its x19-x30 and d8-d15
   saves and restores (612,455 pairs in 174,749 functions) and its frame in guest memory. Not measured alone.
   Removed by deleting the pairs and promoting frame slots ([lifter-optimisations.md](lifter-optimisations.md), C and
   F).
   *Justification:* host code reads lifted frames at five places: `host/direct_layer_ahead.c:213` (SP, then
   `[sp + 0x48]`), `host/direct_job_sync.c:117` (`[sp + 0x10]`, a frame slot of its caller),
   `host/direct_mesh_system.c:52-56` (builds a call frame below SP and calls guest code), `host/rt_slots.c:718` and
   `:726` (SP and `[sp + SP_ALIGN]`), and the stall dump's x29 frame walk (`host/hos_thread.c:277`, a diagnostic).
   Their functions must keep their frames. For every other function unproven: unwinding is not a reason (throws are
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
6. **Every function has a generic entry reachable through the table.** `a64_lookup` serves all 295,270 lifted entries;
   the address-taken set (relocation targets, ADR/ADRP+ADD materialisations, vtables, thread and fiber entries, exports,
   hooks) is not computed, so nothing can drop its entry or shrink an indirect site's target domain.
   *Justification:* unproven. Experiment: emit the set into `a64_table.c`, trap in `a64_lookup`'s miss path on a
   target outside it, run the smoke and the market bench.
7. **Indirect calls through one global lookup.** 182,791 BLR sites (19.2 % of call edges with BR's 23,561), 166,256 of
   them loading the target from a computed address (vtables), 1,335 from writable globals, 0 from read-only memory.
   Each goes through `a64_icall`, the global cache's hash and a slot shared by every site; no per-site cache, no
   promotion. Not measured. Removed by a per-site cache or profile-guided promotion
   ([lifter-optimisations.md](lifter-optimisations.md), D). *Justification:* none.
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
13. **-O1 for cold partitions.** -O2 is 1 % smaller on the sample. *Justification:* build time and memory; unproven
    (the cold build at -O2 was not timed).

## Open items

One line each; the queue with sources, soundness conditions and first steps is
[lifter-optimisations.md](lifter-optimisations.md).

- C: dead temporaries' stores before calls; pair deletion.
- D: per-site indirect call cache; profile-guided promotion.
- F: frame-slot promotion, leaves first.
- G: libc calls as intrinsics, CRC32C as `crc32`, noreturn as `unreachable`, CCMP chains, NEON gaps.
- STLR as release stores; the asserted address-taken set (pessimizations 5 and 6).
- Path-sensitive liveness for the summaries (the x9-x18 demotions).
