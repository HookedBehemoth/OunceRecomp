# Lifter 2: design

A new code generator for the game's AArch64 code. It replaces `third_party/a64-kit/lift` (suyu's per-instruction C
text plus regex passes) and the parts of `nx2/` that edit the lifted C. This document holds the decisions, the
measurements behind them, the IR, the calling convention, the host interface, the validation scheme and the
milestones. The present lifter stays the oracle until the last milestone and is then deleted: there is no
permanent fallback.

Measurements: clang/LLVM 23.1.3 on this PC (Zen, x86-64-v3). The experiment sources are not in the repository
(they are cut from the lifted game); each figure below says how it was made so that it can be repeated.

## 1. Why a new generator

The present lifter keeps every guest register, flag and vector in `GuestContext` and makes every BL an opaque
`f_X(c)` call. It lowers each FP op to a native op plus a proof that the result and the FPSR cumulative flags equal
ARM's, and builds NEON permutes as byte loops. Every optimisation is a regex over C text. Measured on the current
build:

| | value |
|---|---|
| guest code / lifted x86 code | 72.6 MB / 446 MB (6.1x) |
| x86 instructions per guest instruction, cold chunks (-O1) | 4.49 |
| context register loads+stores per guest instruction (cold) | 0.89 (20 % of all x86 instructions) |
| hot region: integer-SIMD check ops vs real FP arithmetic ops | 3.13 M vs 126 k (25 : 1, 17 % of hot instructions) |
| guest MRS FPSR sites that read those flags, whole game | 8 |
| vector kernel f_817472e0, x86 instructions: as lifted / plain SSE | 4,888 / 2,416 |

## 2. Output format: LLVM IR, with a uniform calling convention

The question was whether to emit C (as now) or LLVM IR. Each claim for IR was tested.

### 2.1 Measurements

The corpus is 120 random cold functions (8,450 guest instructions) from six chunks. Each variant was compiled with
the build's flags at -O1. The counts are x86 instructions in the object.

| variant | instructions |
|---|---|
| as lifted (registers in the context) | 29,442 |
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
functions of one chunk). With per-function signatures these become tail calls between different prototypes:
- C `musttail` refuses them, including with `swiftasynccall`.
- IR `tailcc` with `tail call` works on Linux.
- On Win64, LLVM 23 aborts on every guaranteed tail call under `tailcc` or `swifttailcc` ("Can't handle guaranteed
  tail call under win64 yet").
- `ccc` with `musttail` and identical prototypes works on both targets.

Front end: a 3 MB piece of one chunk at -O1 takes 15.9 s from C, 16.1 s from bitcode and 16.0 s from textual IR. The
textual IR is 42 MB against 3 MB of C. The optimiser and code generator are the cost; parsing is not.

NEON permutes: C vector extensions (`__builtin_shufflevector`) give the same code as IR `shufflevector`: EXT #8 is one
`vpalignr`, DUP one `vshufps`, FADDP one `vhaddps`.

### 2.2 Verdict

**Emit LLVM IR.** The reasons are narrower than "LLVM allows a lot of metadata":

1. **Mixed integer and vector results in registers.** A guest function's results are x0/x1 and v0/v1 (HFA and
   floating-point returns). `ccc` returns them in rax, rdx, xmm0, xmm1. In C that needs a struct over 16 bytes,
   which goes to memory, or `regcall`, which costs 6.5 % because it leaves fewer callee-saved registers.
2. **`poison` for the arguments and results a callee does not use**, under one uniform prototype (section 5). An
   argument the callee's summary says it does not read costs nothing. In C the same effect needs an uninitialised
   variable, which is undefined behaviour.
3. **Exact intrinsics** with ARM's value semantics: `llvm.fptosi.sat` (FCVTZS: NaN to 0, saturating; one
   `vcvttss2si` plus two `cmov`), `llvm.minimum`/`llvm.maximum` (FMIN/FMAX), `llvm.minimumnum`/`llvm.maximumnum`
   (FMINNM/FMAXNM), `llvm.fma`, `llvm.roundeven`/`round`/`floor`/`ceil`/`trunc` (FRINTN/A/M/P/Z), overflow intrinsics.
   No C undefined behaviour to dodge (shift amounts, signed overflow).
4. **The IR is SSA with block arguments**, which maps one to one onto LLVM `phi`. C would need an out-of-SSA step.
   This step is small, so it is a convenience, not a reason.

Claims that did **not** hold, and are not reasons:
- Alias metadata, `memory()` effects and clobber facts on declarations measured 0 to 0.4 %. The upper bound,
  declaring everything read-only, is 6 % and incorrect. The register traffic they were meant to remove is removed by
  passing registers as values (section 5), and that works in C as well.
- Front-end time: no difference.
- Guaranteed tail calls between different signatures: not portable to Win64, so the design does not use them.

For integer code, C with a uniform prototype gets the same code: 24,189 instructions either way. The IR advantage
is in floating-point results, `poison` and the intrinsics.

**Version pin.** The emitter writes textual IR for one LLVM major. `nx2/lift2/llvm.py` refuses a clang whose major
differs: the Linux clang and the pinned llvm-mingw must be the same major. Textual IR (`.ll`) is used, not bitcode:
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

`nx2/lift2/census.py` regenerates the list into `work/lift2/forms.txt` and marks the forms each milestone covers.

## 4. The IR

One `Function` per guest function entry, as the present discovery finds it (`Lifter.walk`, jump tables,
`lift_fixes`, `split_large`). The IR is plain Python data, so a port of the passes to a compiled language stays
possible if the lift exceeds about 10 minutes.

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
| memory | `gaddr(i64) -> ptr` = `getelementptr i8, ptr inttoptr (i64 0x100000000000 to ptr), i64 a`; `load(ptr, w, sext, order)` with order plain, acquire; `store(ptr, v, w, order, mark)` with order plain, seqcst; `rmw(op, ptr, v, w)` for the LSE ops; `cas(ptr, exp, new, w)`, `cas2` (CASP, `cmpxchg16b`); `excl_load`/`excl_store`; `fence(full)`, `fence(compiler)` |
| vector | lane ops on `v128` via LLVM vector ops, `shufflevector`, `insertelement`/`extractelement`, x86 intrinsics where nothing generic exists (`pshufb` for TBL/TBX, `pmaddubsw` for UDOT), saturating ops via `llvm.[us]add.sat` |
| FP | fadd fsub fmul fdiv `llvm.sqrt` `llvm.fma` fneg fabs, min/max as above, `fcmp` -> NZCV (unordered = 0011), conversions (`fptosi.sat`, `sitofp`, `fptrunc`/`fpext`, F16 via F16C), FRINT*, `frecpe/frsqrte` (calls into a table routine), `mrs_fpsr`/`msr_fpsr`/`msr_fpcr` (section 7) |
| control | br, condbr, `switch(index)` for jump tables (`(x - base) >> k`, an indexed table, not a compare tree), ret(results), `call_fast(f, args) -> results`, `tail_fast`, `call_generic(target)`, `tail_generic(target)`, `svc(imm)`, `trap(pc, word)`, `poll` |
| host interface | `probe(id, reads, writes)` (section 9) |

**Emission.** One textual LLVM module per partition (section 11). Each IR value is one LLVM value, each block one
basic block, and block parameters become `phi`s. No `alloca` is emitted except for host-stack temporaries.

## 5. The calling convention

### 5.1 Two entries per function

- **Fast entry** `f_<addr>`, for direct lifted-to-lifted BL and B-to-entry. One uniform prototype for every
  function in the program:

  ```
  %R = type { i64, i64, <2 x i64>, <2 x i64> }          ; x0, x1, v0, v1 out
  define %R @f_<addr>(ptr noalias nocapture %ctx, i64 %sp,
                      i64 %x0, i64 %x1, i64 %x2, i64 %x3,
                      <2 x i64> %v0, <2 x i64> %v1, <2 x i64> %v2, <2 x i64> %v3)
  ```

  - Convention `ccc`. On SysV, ctx, sp and x0-x3 are rdi, rsi, rdx, rcx, r8, r9; v0-v3 are xmm0-3; results come
    back in rax, rdx, xmm0, xmm1.
  - Callee-saved host registers (rbx, rbp, r12-r15) keep the caller's values across calls. That matters: going
    from 31 guest to 16 host registers, `preserve_none` and `regcall` lost 6-11 %.
  - The prototype is uniform because tail calls must be guaranteed (`musttail` with identical prototypes is the one
    form that works on Linux and Win64), and because a BLR site cannot know its callee's signature.
  - What one function needs goes into poison, not the prototype. A caller passes `poison` for every argument the
    callee's summary does not read. A callee returns `poison` in every result slot no caller reads.
  - x4-x8 and v4-v7, when a callee reads them (about 2 % and 4 % of functions, estimated from 11,307 functions), go
    through the context: the caller stores what the callee's summary reads and the callee loads it.
  - Results beyond x0, x1, v0, v1 that some caller reads go back through the context the same way.
- **Generic entry** `g_<addr>`: `void (ptr %ctx)`, today's protocol. All guest state is in the context: it loads
  the callee's live-ins, calls `f_<addr>`, and stores the results and every register the function may change
  (its clobber set) back. It serves:
  - BLR, and BR to a register that is not a jump table;
  - the host: thread first frames, `a64_icall`, HLE callbacks;
  - `ENTRY_HOOK`, setjmp return points;
  - old-lift code during bring-up (section 12).

### 5.2 What crosses a call

| guest state | treatment at a call |
|---|---|
| SP | an argument; never returned (preserved; the summary checks it) |
| x19-x29, d8-d15 | never passed, never returned: they live in the caller's SSA values. The callee's prologue saves and epilogue restores of them are deleted when the summary proves the slot is only read by the matching restore. Functions that are not proven preserving return them through the context (rare; listed by the lift). |
| x30 | not materialised. A function that reads x30 as data (`__builtin_return_address`, PAC signing with a modifier, a backtrace) gets it stored in the context by its callers, a summary fact. PACIBSP/AUTIBSP/RETAB are no-ops (the host return stack gives the return). |
| NZCV | never across calls (AAPCS64; the summary checks it); a function that reads flags at entry is lifted with the generic convention only |
| x9-x18 | dead at calls and returns unless the summary says a callee reads or a caller expects them (outlined helpers, veneers: x16/x17) |
| x8 | the indirect-result pointer: passed through the context when the callee reads it |

Indirect calls (BLR, 182,791 sites) call `g_<target>`. The per-site cache stays (`A64_SITE_CALL` semantics). Before
the call the caller stores what the AAPCS64 lets a callee read (x0-x8, SP, v0-v7, only those it has written since
the last sync); after it, it reloads x0, x1, v0-v3. A site whose usual target is known (`A64_DEVIRT` profile) calls
`f_<target>` directly after the compare.

## 6. The whole-program stage

Inputs: the image, the function entries and bodies (present discovery and `lift_fixes`), jump tables, PLT stubs
and imports, the hook and probe tables.

1. **Call graph**: BL edges, tail edges (B and fall-through to an entry), PLT resolution, BLR and register BR as
   edges to "unknown, AAPCS64". Functions whose address is taken (ADRP+ADD of an entry; relocations and pointers in
   `.data`, vtables) form the "may be called indirectly" set.
2. **Per-function local facts**, from the function's own IR with callees' summaries:
   - live-in GPR/V/NZCV;
   - may-write set;
   - SP balance;
   - callee-saved preservation (a save pattern: entry value stored to a frame slot, the slot read only by the
     restore into the same register on every exit);
   - x30 read as data;
   - FP mode use (MSR FPCR, MRS FPSR);
   - frame size and slots whose address never escapes (frame-slot promotion to SSA, and marks elided);
   - stores that may reach memory other than the own frame (for `memory(read)` declarations, kept because they are
     free).
3. **Summaries** over SCCs (Tarjan), iterated to a fixed point inside each SCC:
   - inputs(f) = live-in(f) ∪ inputs of callees not covered;
   - clobbers(f);
   - preserved(f), proven not assumed;
   - results(f) = registers written by f that some caller reads before redefining, plus x0, x1, v0-v3 when f is
     in the indirect set.
4. **Contract checks**:
   - a function in the indirect set must preserve x19-x29, SP and d8-d15 and read only x0-x8, SP and v0-v7 at
     entry; one that does not is listed and gets the generic convention for its indirect callers;
   - a function reading NZCV at entry, or x9-x18 outside a known veneer pattern, is listed.
   The list is printed by the lift and is expected to be short. The SDK's hand-written assembly is the likely
   member.
5. **Runtime verification build** (`LIFT2_VERIFY=1`): every fast entry checks at return that SP and x19-x29 are
   unchanged and records a violation. One game run must report none before the convention is trusted.

## 7. Floating point: host FP mode

- FADD, FSUB, FMUL, FDIV, FSQRT, FMADD/FMSUB/FNMADD/FNMSUB and vector FMLA/FMLS are the native SSE/AVX/FMA3
  operations (`fadd` ... `llvm.fma`). IEEE operations and fused multiply-add are correctly rounded, so in
  round-to-nearest the values equal ARM's. `-ffp-contract=off` semantics: the emitter never fuses what the guest
  kept separate.
- **FPCR to MXCSR** (`host/hos_fp.c`). FPCR.FZ sets MXCSR.DAZ and MXCSR.FTZ; RMode sets MXCSR.RC (RN/RP/RM/RZ to
  RC 0/2/1/3); every exception stays masked. DN is not modelled (7.1).
  - Lift 2's MSR FPCR calls `a64_fpcr_write`, which stores the context's `fpcr` (keeping the host's bit 32,
    RECOMP_FPX_INHIBIT) and sets MXCSR. The present lifter's MSR FPCR text is followed by `a64_fpcr_sync(c)`
    (`a64_lift.py` `fp_mode_sync`; its MSR FPSR by `a64_fpsr_sync(c)`).
  - A function with MSR FPCR is emitted `strictfp`: its FP operations are LLVM's constrained intrinsics with a dynamic
    rounding mode, which LLVM keeps in order with the FPCR write (a call). In TOTK no function with an MSR FPCR, and
    no direct caller (two levels up) of one, contains FP arithmetic, so the other functions stay plain; the whole-program
    stage (milestone 13) is to mark a function with FP arithmetic that calls an FPCR writer strictfp too.
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
    FZ would have run lift 2's FP code flushing.
- **The present lifter under this MXCSR** (the mixed build): its FP stays value-exact. Its native fast paths
  (FPX1 helpers in `a64_suyu.h`, `a64_fpx.h` wrappers, the A64_VFPX vector paths, `a64_fcmp*`, `a64_vfcmp`) run only
  inside RECOMP_FPX_OPEN (RMode RN, no DN), and under FZ they keep a result only when no operand is subnormal and the
  result is normal, zero, or flushed by its own code, which equals what FTZ gives; with RN and FZ clear MXCSR is the
  default it was written for. Its exact bodies and its conversions (`recomp_fp_to_int`, `recomp_fixed_to_fp`, the
  estimate tables) are integer code. What differs is FPSR only: under FTZ a tiny sum the old fast path keeps is
  flushed by the hardware without its UFC, and each side's MRS FPSR misses the other side's flags.
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
| generated NaN (invalid op) | 0xFFC00000 / 0xFFF8... | not fixed by default (an allowed deviation, 7.1); `LIFT2_DEFAULT_NAN=1` at lift time adds a select to ARM's default NaN after each arithmetic op (2-4 instructions per op) if the shadow run shows a reader cares |
| NaN propagation with two NaN operands or a signalling NaN | x86 propagates src1 | not fixed: ARM differs only when the second operand is signalling; the shadow run counts it |
| FZ: tininess | ARM flushes results tiny before rounding, x86 FTZ after | not fixed: differs only for results that round up to exactly the smallest normal; the shadow run counts it |
| FRECPE, FRSQRTE | `rcpps` is a different estimate | ARM's algorithm (exact), out of line per lane: `a64_frecpe_f32/f64`, `a64_frsqrte_f32/f64` in `host/hos_fp.c`, FZ and RMode read from MXCSR |
| FRECPS, FRSQRTS | | `fma` plus the 0·∞ special case (2.0 / 1.5) |
| FCVT to and from half | | `fptrunc`/`fpext` (F16C `vcvtps2ph`/`vcvtph2ps`); FZ16/AHP not supported |
| FCMP, FCCMP | | four `fcmp`s give NZCV (unordered 0011); a following condition folds into one `fcmp` with the matching ordered/unordered predicate (`sem_int.cond`, kind `fcmp`) |
| TBL, TBX | `pshufb` uses the low 4 bits of an index | `pshufb` per table register with the index made relative to it and `uadd.sat(idx, 0x70)`, which sets bit 7 for every index past 15 |
| FABD, FACGE etc. | | compositions of the above |

### 7.1 Allowed deviations from ARM (host FP mode)

The checks (`tests/test_lift2_fp.py`, `nx2/test_lift2_fpleaf.py`) accept exactly these differences, count them by
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

- A **shadow build** (`LIFT2_FP_SHADOW=1`) runs the present exact FPX1 helper next to each host-mode op and counts
  mismatches by class (NaN sign, NaN propagation, FZ boundary, FPSR at read sites). One game run, then goldens are
  re-baselined.

## 8. Memory

- **Window.** The guest address plus the constant `0x100000000000` stays (one `movabs` per function, folded into
  addressing). There are no bounds checks; the 512 GB reservation and the host's first-touch commit stay as they
  are.
- **Plain loads and stores** are LLVM plain accesses with `align 1`. ARM's single-copy atomicity of aligned
  accesses holds on x86 for plain moves. A spin loop on a plain load stays a loop only because the back-edge poll
  (section 10) is a call that may write guest memory. That is an invariant of the poll's declaration, documented
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

- **Page marks.**
  - *What they are for:* the direct back end reads them to find guest pages the CPU wrote since its last scan
    (vertex, index, uniform, texture uploads) without an OS write watch.
  - *Where they stay:* every store whose address is not provably in the own frame. Provably in the own frame means
    derived from SP, or from x29 set from SP, with the address not escaping.
  - *What is fixed:* the second half of an `STP Q, Q, [SP]` is no longer marked (6,050 sites today).
  - *Cost:* about 8 % of cold instructions (measured).
  - *A cheaper mechanism* exists on Linux 6.7+: `PAGEMAP_SCAN` over userfaultfd `UFFD_FEATURE_WP_ASYNC` ranges, which
    exists to emulate GetWriteWatch. Its cost is a minor fault on the first write to a tracked page after each reset,
    which can exceed the marks for pages written every frame. It is a separate experiment on the GPU pools only, not
    part of this design.

## 9. The host interface, at guest-address level

Today several tools find places in the lifted C by text and edit it. All of them move to a declarative table keyed by
guest addresses, read by the lifter, and to host C functions with fixed signatures. The lifted code is never edited
after the lift.

| consumer of the lifted C text today | what it does | replacement |
|---|---|---|
| `nx2/entry_hooks.py` `ENTRY_HOOK(0xADDR)`, `ENTRY_HOOK_SYM` (137 in `host/*.c`) | `if (entry_<addr>(c)) return;` at the function's top | Unchanged in `host/`. The lifter puts the call at the fast entry: store the live-ins and x30 to the context, call, on 1 load x0, x1, v0, v1 and return. A hook may also be declared `REPLACE`: a host function with the fast prototype, called directly. |
| `nx2/entry_hooks.py` `SPLICE_AFTER(0xADDR, "anchor")`, `_EACH` (15, `host/direct_*.c`) | inserts `splice_<addr>(c);` after a text anchor | `SPLICE_AT(0xFUNC, 0xPC)` naming the guest instruction. The lifter stores every live register to the context before the call and reloads after it (a sync point). The anchors are re-expressed as instruction addresses once with the old lift's `/* pc */` comments. |
| `nx2/oot_patches.py`: dynres, life, movement, flight rotation, view, mouse camera, menu focus, menu tick, text speed, frame seconds (`STEP_SITES`), movie frames (`MOVIE`), whole frames (`WHOLE_SITES`), output size, RT pool size (`OUT_EDITS`) | replace or insert C at text anchors in about 20 functions | Probes in `nx2/lift2/probes.py`: `After(pc, fn, reads, writes)` (a host function of the named registers returning the written ones), `Before(pc, ...)`, `CondOr(pc, fn)` (a conditional branch also taken when `fn()` is true), `Value(pc, reg, fn)` (the instruction's result replaced), `CallSite(pc, fn)` (a BL at pc calls a host function instead). Each patch's C moves into a host file as a function; the table names it. |
| `nx2/oot_native_resolution.py` (f_806702e0, f_831f3200) | text edits | probes, as above |
| `nx2/oot_wide.py` (11 sites), `nx2/oot_jitter.py` (3), `nx2/oot_occlusion.py` (6) | C after an instruction (`Lifter.after`) | `After` probes; the snippets become host functions |
| `nx2/step_readers.py` | reads the lifted C to classify readers of the frame step | reads the IR dump (`work/lift2/ir/<addr>.txt`) |
| `nx2/chunks.py` (stable boundaries, `edit_c`), `nx2/whole_write.py` | chunk files of C | partitions by guest address (kept: `work/image/chunks.txt`); written whole once |
| `nx2/split_hot.py`, `nx2/pgo_pieces.py` | splits `code_hot.c`, maps profiles | the hot partition is emitted as N modules directly; IR-level PGO (`-fprofile-generate`/`-fprofile-use`: clang's front-end instrumentation does not apply to IR input) |
| `nx2/lifted_c.py` | finds definitions in C text | not needed |
| `nx2/poll.py` | inserts `A64_POLL` before backward gotos | the `poll` IR op on back edges (section 10) |
| `nx2/rtgen.py`, `a64_rtgen.py`, `a64_suyu.*`, `a64_fpx.h` | generated runtime headers | `nx2/lift2/rt.c` (host-side runtime: generic call, traps, SVC, FP tables) |
| `nx2/build.py` | compiles `code_*.c`, hot flags, memory gate | compiles `*.ll` per partition (section 11) |
| `nx2/label_syms.py` | renames `f_<addr>` symbols | renames `f_<addr>` and `g_<addr>` |
| `nx2/test_leaf.py`, `nx2/test_insn.py` | lift with the old lifter | parametrised by lifter (section 12) |
| tests: `test_entry_hooks`, `test_mark_groups`, `test_fpx_*`, `test_vfpx_elem`, `test_vfminmax`, `test_frame_whole`, `test_movie_frame_count`, `test_menu_hooks`, `test_oot_jitter`, `test_free_view_anchor`, `test_frame_rate_cap_step`, `test_stale_hot_pieces`, `test_chunk_split` | cut the old lifter's or patches' source | rewritten against the probe table and the new modules in the milestone that replaces each piece; the old ones are deleted with the old lifter |

The game's text patches are about 20 functions. Each becomes one probe-table entry plus a host function, with its
test checking the table, not C text.

## 10. Polls, SVC, traps, setjmp

- **Polls.**
  - *Why they exist:* the virtual cores (`HOS_VCORES`, `host/hos_thread.c`) multiplex guest threads so that a
    waiting higher-priority thread can take a core from a running one. A guest loop with no SVC or nn::os call
    would never give it up.
  - *Where they stay:* every back edge.
  - *Emitted form:* `load atomic i32 monotonic` of `ctx.poll` and a cold branch to `a64_poll_cur()`.
  - *Declaration invariant:* `a64_poll_cur` is declared as possibly reading and writing guest memory, which keeps
    spin loops on plain loads correct (section 8). It is not declared as touching the context, so values stay in
    registers across the loop.
  - *Cost:* 0.05 loads per guest instruction.
- **SVC**: the generic sync point (all live registers to the context, `a64_svc(c, imm)`, reload).
- **Traps** (UDF, BRK, undecodable): the context synced, `a64_trap`, no return.
- **setjmp owners**: the present scheme (`recomp_sj_push`/`recomp_sj_setjmp` around calls of a function that calls
  `__setjmp`, return points as generic entries). Such calls use the generic convention.
- **PLT stubs**: resolved to the final target at lift time; a stub reached indirectly is a generic entry that sets
  x16/x17.

## 11. Build

- **Partitions.** By guest address, on the stable boundaries of `work/image/chunks.txt`. About 45 cold modules of
  about 400 k guest instructions, plus the hot set (`work/hot_funcs_play.txt`) as 8 modules grouped by call
  affinity (as `split_hot.py` does now).
  - Each module declares every callee it calls with the uniform prototype, plus `memory(read)` where the summary
    proves a function store-free (free, if small).
  - Cold modules build at -O1, hot modules at -O2 with IR-level PGO.
  - The lift writes the `.ll` files, the summaries and the probe and hook tables in one pass; `build.py` compiles
    them with the present job scheduler and memory gate.
- **Lift time.** Decoding and IR construction are per function and parallel (16 processes); the summary fixed
  point is cheap (one pass over SCCs; summaries are bitmasks). The estimate is 5-10 min in Python against 3 min now.
  If it is slower, the decoder and emitter move to a compiled language with the IR unchanged.
- **Build time.** About as now (11-15 min): the optimiser dominates (section 2.1), and there are about 25 % fewer
  instructions to optimise.
- **Executable size.**
  - Cold code: from 23.8 x86 bytes per guest instruction to about 16-17. That is -28 % instructions measured with
    registers as values, saves deleted and marks off. With the marks kept, about -20 %.
  - Hot vector code: about half (section 1).
  - Lifted code from 446 MB to about 310-340 MB.
- **Layout.**
  - The symbol-ordering file stays (`work/hot_order.txt`, names `f_`/`g_`).
  - Cold paths are split out with `-fsplit-machine-functions` and the profile.
  - The ordered hot region (about 90 MB today) is put on 2 MB pages at start-up: `MADV_COLLAPSE` on the text range,
    or a copy into an anonymous `MADV_HUGEPAGE` mapping and `mremap` over it.
  - `-fno-stack-protector` for lifted modules.
- **Windows.** Same IR, `x86_64-w64-windows-gnu` triple, the pinned llvm-mingw of the same LLVM major. The uniform
  prototype with `musttail` works there (tested); Win64 passes the vector arguments by reference, which is accepted
  for a build whose purpose is portability (there is no Windows renderer).

## 12. Validation and bring-up

- **Per instruction:** `nx2/test_insn.py` gains `--lifter 2`. Every form of the census, random states, Unicorn's
  ARMv8.x "max" CPU against the new lift's code for that one instruction: X0-X30, SP, NZCV, V0-V31 and memory
  compared. FPSR is compared only at the MRS sites' semantics (section 7).
- **Per function against Unicorn:** `nx2/test_leaf.py --lifter 2` (leaves, then `--calls` trees), unchanged
  otherwise.
- **Differential, old lift against new lift:** `nx2/test_lift2_diff.py`. Both lifts of the same functions are
  linked into one harness under different names (`old_f_<addr>`, the new `g_<addr>`). Each is run from the same
  random state and memory snapshot; registers, NZCV where the summary says it is live, and changed pages are
  compared. Unicorn is not needed, so SVC-free functions with calls and indirect calls can be compared too (both
  call the same old-lift callees).
- **Mixed builds.** A list `work/lift2/use.txt` names the functions taken from the new lift.
  - For those, the old `f_<addr>` is replaced by `void f_<addr>(Ctx *c) { g_<addr>(c); }`.
  - New code calling a function not in the list calls the old `f_<addr>(c)` through the generic protocol.
  - Old and new code meet only at generic entries, whose protocol is the old one, so any mix links and runs. A
    divergence is bisected by halving the list.
  - The list grows module by module until it holds every function; then the old lifter, `a64emit`, suyu's
    translator and the text-patch tools are deleted.
- **In the game:** a smoke run against the phashed golden and a timing run at each milestone that changes code in
  the game. Run by the owner or one agent at a time.

## 13. Milestones

Each is one agent task of a few hours with its own pass/fail check. Estimates are agent-hours.

| # | milestone | pass/fail | est. |
|---|---|---|---|
| 1 | Decoder + IR + LLVM emission for integer leaf functions (specified below) | Unicorn 0 mismatches on 500 leaves; old-lift differential 0 mismatches | 4 |
| 2 | All integer data-processing and integer load/store forms of the census (`census.py` coverage report) | `test_insn --lifter 2` 0 mismatches on every integer form; coverage 100 % of integer forms | 4 |
| 3 | Jump tables as indexed switches, polls, traps, PAC no-ops; leaves with loops and switches | `test_leaf --lifter 2` 0 mismatches on 2,000 leaves incl. switches | 3 |
| 4 | Calls through the generic entry only (BL, tail B, PLT), call trees | `test_leaf --calls --lifter 2` 0 mismatches on 500 trees | 3 |
| 5 | Differential harness old vs new, and the mixed-build list in `build.py` | 1,000 random functions with calls, 0 mismatches; a mixed build with one cold module from lift 2 links | 4 |
| 6 | Barriers, LDAR/STLR, LSE, CASP, exclusives (LDXR/STXR loop recognition and monitor) | `test_insn` on all atomic forms; a two-thread litmus test (STLR then LDAR, message passing) on host threads | 3 |
| 7 | SVC, setjmp owners, hooks (`ENTRY_HOOK`, `REPLACE`), `SPLICE_AT`, probe table with the 20 patch functions ported | `tests/` rewritten for the table; the differential harness passes on every hooked or probed function with hooks stubbed | 4 |
| 8 | Mixed build of all integer-only functions (no FP/NEON) from lift 2 | smoke run vs golden (owner/agent run) | 3 |
| 9 | FP scalar in host FP mode, FPCR/MXCSR handling incl. fibers, FPSR sites, fix-ups | `test_insn` 0 mismatches outside the documented classes; shadow build counts | 4 |
| 10 | Vector/FP loads and stores, NEON integer and permutes | `test_insn` 0 mismatches on those forms | 4 |
| 11 | NEON FP | as 10; f_817472e0's x86 instruction count ≤ 2,600 | 4 |
| 12 | Shadow FP run in the game; goldens re-baselined | mismatch report per class; smoke | 2 |
| 13 | Whole-program stage: call graph, local facts, SCC summaries, contract checks, the violation list | lift report; summaries stable over two lifts; unit tests on synthetic functions | 4 |
| 14 | Runtime verification build (`LIFT2_VERIFY=1`) | one game run with 0 violations | 2 |
| 15 | Fast entries and the uniform prototype for direct calls, poison args/results, callee-saved save deletion | `test_leaf --calls` and the differential 0 mismatches; cold corpus instruction count ≤ 0.85x the old lift | 4 |
| 16 | Frame-slot promotion and mark elision for own-frame stores | differential 0 mismatches; marks per guest store reported | 3 |
| 17 | Build: partitions, hot modules, IR PGO, `-fsplit-machine-functions`, 2 MB text pages, Windows triple | full build time and size reported; Windows build links | 4 |
| 18 | Whole game from lift 2 (`use.txt` = all) | smoke vs golden, timing run vs the old build | 3 |
| 19 | Delete the old lifter, `a64emit`, suyu's translator, `poll.py`, `lifted_c.py`, `split_hot.py`, `pgo_pieces.py`, text patches; docs updated | `tests/run_all.py` green; nothing references `code_*.c` | 3 |

Total about 65 agent-hours. Milestones 1-8 bring the integer game up through mixed builds; the performance work is
9-11 and 15-17.

## 14. Milestone 1: integer leaves (done)

**Scope.** Functions whose every instruction is in the integer subset and that stay inside themselves:
- data processing (immediate and register), moves, bitfield ops, extends, conditional select/compare, MUL/MADD/MSUB,
  the long multiplies, UMULH/SMULH, UDIV/SDIV, shifts by register, CLZ/CLS/RBIT/REV*, EXTR;
- integer loads and stores of every addressing mode (unsigned offset, unscaled, unprivileged, pre/post-index,
  register offset with extend/shift, literal, pairs, LDPSW), PRFM as a no-op;
- ADR/ADRP; B, B.cond, CBZ/CBNZ, TBZ/TBNZ, RET/RETAA/RETAB; hints (NOP, the PAC hints) as no-ops; UDF as a trap.

Out of scope (the function is passed over): BL/BLR/BR, B or fall-through to another entry, SVC, FP/NEON, LSE atomics,
acquire/release, exclusives, CRC32, MRS/MSR, barriers.

**Files** (`nx2/lift2/`):
- `enc.py`: the encodings as data. Each entry has a 32-bit pattern of literal bits and named fields, a semantic
  template with fixed parameters, and a validity expression for the encodings the Arm ARM leaves unallocated inside
  the pattern. The decoder dispatches on bits 28:25 and caches by word. 36 entries cover the 432 integer forms of
  the census.
- `asm.py`: a decoded instruction printed as `llvm-mc -M no-aliases` prints it. llvm-mc keeps some spellings even
  without aliases (the bitfield aliases, `lsl`/`lsr`/`asr`/`ror` for the shift-by-register forms, `mov` for
  MOVZ/MOVN/ORR immediate, `ret x30`, `[xN, xM, lsl #k]` whatever S is); the printer follows them.
- `selfcheck.py`: the decoder against llvm-mc, word by word (text equal; no word decoded that llvm-mc calls invalid;
  no word missed whose mnemonic the decoder can print). `--game` takes every distinct word of the game, `--synthetic N`
  random words per entry plus random words.
- `census.py`: the forms census into `work/lift2/forms.txt` with the decoder's coverage per form and per class.
- `ir.py`: values, blocks with parameters, the builder (`get`/`set` of guest registers, SSA by first-read block
  parameters resolved in `finish()`), constant folding including constant address chains.
- `sem_int.py`: 19 templates, one per family (addsub_imm, logic_shift, bitfield, ldst, ldst_pair, ...).
  - The Arm ARM's definitions where LLVM's operations differ: division by zero and INT_MIN / -1, shift amounts
    modulo the width, ADC/SBC flags through a one-bit-wider sum.
  - `cond()` folds a condition into one `icmp` of the last compare's operands while its flags are current.
- `build_ir.py`: blocks at branch targets and after branches; `scope()` says why a function is out;
  `group_marks()` merges the page marks of the stores of a block through one base within a 4 KB span into one
  mark after the last of them (the present lifter's `mark_groups`).
- `emit_llvm.py`: textual LLVM IR, generic entry only (`define void @g_<addr>(ptr noalias nocapture %ctx)`).
  - Each register read before written is loaded at entry.
  - **Each register write is stored to the context where it happens (write-through)**, and LLVM removes the
    stores a later one overwrites. Storing every written register at the returns, as first planned, kept every
    register live from entry to return: 1.16x the present lifter's instructions, against 0.74x write-through.
  - N, Z, C, V are stored only for tests (`flags_out`), since the AAPCS64 leaves them undefined at a return.
  - Page marks go through one `alwaysinline` helper per module, which also keeps the text smaller.
- `llvm.py`: the pinned major (`PINNED_MAJOR = 23`); `check()` refuses another major with a message naming the found
  version and `HOOKSHOT_LLVM_BIN`; llvm-mc's disassembly for the self-check.

**Tests.**
- `tests/test_lift2.py` (game-free, in `tests/run_all.py`):
  - the entries do not overlap (random words per pattern);
  - every entry's template exists;
  - known words print as llvm-mc prints them;
  - the synthetic self-check against llvm-mc (about 19 k words);
  - six synthetic functions (loop and flags, bit operations, memory with every addressing mode, multiply and
    divide edge cases, ADC/SBC/CCMP/CSEL flags, branches and ADR/ADRP), from words to IR to LLVM IR, compiled,
    run, and compared on X0-X30 and NZCV with values computed in the test from the Arm ARM's definitions;
  - the version check's refusal.
  - The llvm-mc and compile cases are skipped without LLVM 23 or off Linux.
- `nx2/test_lift2_leaf.py` (local: dump, Unicorn, Linux).
  - It draws the game's functions by `test_leaf.py`'s module shares and lifts them with both lifters.
  - Its harness is Linux-native (mmap, signals): `test_leaf.py`'s harness is Windows-only.
  - Each case runs the old and the new lift from the same state, compared with Unicorn and with each other.
  - It reports both lifts' x86 instruction counts and lift2's speed.

**Results** (seed 1, 8 states per function):

| check | result |
|---|---|
| decoder vs llvm-mc, every distinct game word | 2,032,675 words, 1,790,347 decoded, 0 differences |
| decoder vs llvm-mc, synthetic | 189 k words, 0 differences |
| census, integer classes | 432 of 542 forms fully covered; by words: integer 99.94 %, integer load/store 97.97 %, branches 100 % |
| 500 game leaves (427 with at least one case Unicorn can run, 2,390 cases) | 0 differences against Unicorn (X0-X30, SP, NZCV, changed pages), 0 against the present lifter (X0-X30, SP, pages) |
| x86 instructions at -O1, same 500 functions | present lifter 35,824, lift2 26,298 (0.73x); 3.32 vs 2.44 per guest instruction |
| compile time, the same functions, -O1 / -O2 | lift2 IR 1.0 s / 1.4 s, the present lifter's C 1.6 s / 2.0 s |
| lift speed, one core (decode, IR, LLVM IR) | 55.6 k guest instructions/s; decode alone 2.25 M words/s over the game's 18.2 M words, cached |
| LLVM IR text | 212 bytes per guest instruction (the present C: 86) |

- 73 of the 500 never ran in Unicorn: all their states fault there, on pointers into unmapped memory.
- The passed-over reasons in the draw, in order: calls or traps in the present lifter's view, size, BR,
  branches out of the body, undecoded words (the atomics and the like).

**Tables and templates per module.**
- `enc.register()` takes a table in `ENCODINGS`' format.
- `enc_*.py` modules of the package are loaded with their `ENCODINGS`, optional `ENV` (names for the validity
  expressions) and `RENDER` (printers).
- `sem.TEMPLATES` gathers `sem_int.py` and every `sem_*.py` (`TEMPLATES`; `None` means decoded, not lifted yet).
- Families can be added in parallel without touching the integer table.

## 14a. Milestones 2-4: all integer forms, control flow, calls (done)

**Milestone 2.**
- Every integer form of the census decodes: 542 of 542 forms in the integer, integer load/store, branch and system
  classes. `selfcheck.py --game`: 2,032,675 words, 0 differences from llvm-mc. Synthetic: 269 k words, 0 differences.
- Lifted:
  - CRC32/CRC32C: a bit-serial internal helper `lift2_crc32`, checked against zlib.
  - MRS/MSR of NZCV, TPIDR_EL0 (read and write), TPIDRRO_EL0, FPCR and FPSR (as the context fields, like the
    present lifter: milestone 9 maps them to MXCSR), CNTPCT/CNTVCT (`recomp_cntpct`).
  - CNTFRQ_EL0: 31,250,000, the Switch 2's counter; the present lifter returns the Switch 1's 19,200,000, and the
    game never reads it.
  - CTR_EL0 (0x8444C004) and DCZID_EL0 (4).
  - PAC*/AUT*/XPAC* as the identity and PACGA as 0 (`nx2/insn.py`'s model); STG/ST2G as their base writeback.
  - DC ZVA/GZVA as a 64-byte clear; the other DC operations and IC IVAU as nothing; ISB as nothing.
- Decoded, lifted later: LSE atomics, CAS/CASP, LDAR/STLR/LDAPR, the exclusives, DMB/DSB/CLREX (milestone 6), SVC
  (milestone 7).

**Milestone 3.**
- Jump tables are a `switch` on `rotr(x - lowest target, 2)`: dense cases that LLVM turns into a table, and a
  misaligned value falls to the default. The cases inside the function are branches; the others are tail calls; the
  default is `a64_ijump(ctx, x)`.
- A BR the discovery did not resolve is `a64_ijump`.
- BRK/HLT/UDF are `a64_trap`.
- The poll (`lift2_poll`, an inlined helper: a relaxed load of `ctx->poll`, a cold call of `a64_poll_cur()`) goes at
  the end of every block with a successor at a lower or equal address. It runs before both edges of a conditional
  branch, where `nx2/poll.py` polls on the backward edge only.

**Milestone 4.**
- BL sets X30 and calls the callee's generic entry; BLR calls `a64_icall(ctx, target)`. After either, the block's
  later register reads load from the context (`Builder.after_call`, the `regload` operation).
- Write-through means nothing needs storing before a call.
- B, or falling through, to another function's entry is `musttail call void @sym(ptr %ctx)`; PLT stubs are lifted
  as the present lifter does (x16 = the GOT slot, x17 = the target, then the target).
- Before control leaves for another function (a tail call, an unresolved BR), a function that writes flags stores
  them (`flagsout`): the code there may read them (a function split by the discovery, a shared tail).
- `build_ir.Program` carries what the lift of one function needs from the whole program: the entries, the jump
  tables, the PLT stubs, the callee resolution and the symbol of each callee's generic entry. `g_` is the symbol when
  lift2 lifts the callee, else `f_` (the present lifter's); that is how mixed builds link.

**Tests.**
- `tests/test_lift2.py` adds three synthetic programs, run end to end:
  - CRC32/CRC32C, the system registers, PAC;
  - BL then a tail call across three functions;
  - a jump table.
- `nx2/test_lift2_leaf.py --calls` draws roots whose call trees the present lifter can lift (no BLR, SVC or
  unresolved BR).
  - Each function of a tree that lift2 can take is lifted by it; the rest come from the present lifter.
  - The two meet at the generic entries.
  - X16/X17 are not compared with calls, and NZCV only for leaves (a callee from the present lifter drops dead flag
    stores).
- A root none of whose random states runs in Unicorn gets up to 16 more with X0-X7 all pointing into the scratch area.

**Results.**

| check | result |
|---|---|
| 500 leaves, seed 1, 8 states | 463 compared (37 never run in Unicorn, was 73 before the retries), 2,391 cases, 0 differences against Unicorn and against the present lifter |
| call trees, seed 2, 6 states | 1,975 roots (431 with calls), 2,939 functions in their trees, 2,850 lifted by lift2; 1,746 roots compared in 7,158 cases; 0 differences |
| call trees, seed 3, 6 states | 2,073 roots, 2,865 functions lifted by lift2; 1,836 roots compared in 7,663 cases; 0 differences; 237 roots never run in Unicorn |
| x86 instructions at -O1, functions with calls (seed 2 / 3) | present lifter 172,734 / 167,975, lift2 158,992 / 156,442 (0.92 / 0.93) |
| x86 instructions at -O1, all functions lift2 lifts (seed 2 / 3) | 296,074 / 315,526 against 268,144 / 290,078 (0.91 / 0.92) |
| leaves only (seed 1, with switches and polls now) | 36,954 against 31,992 (0.87) |
| lift speed, one core | 42-51 k guest instructions/s on call trees (decode, IR, LLVM IR) |

- The largest per-function ratios (up to 49x) are functions whose present-lifter body clang reduced to one jump into
  an identical function of the same file (function merging): an artefact of the count, not of the code.
- The gain on functions with calls is smaller than on leaves: every call stores and reloads through the context
  (the generic convention). That cost is milestone 15's.

**IR size and build time.**
- Text: 230 bytes per guest instruction (about 4 GB for the whole game). Bitcode (`llvm-as`): 57 bytes per guest
  instruction.
- A 95 k-instruction module at -O1 compiles in 9.4 s from text, 8.9 s from bitcode and 9.7 s from text piped on
  stdin. `llvm-as` alone parses it in 1.1 s.
- Parsing the text is about a tenth of a compile, so the build streams each partition's text into its clang
  (`-x ir -`) and keeps no IR on disk.
- A bitcode writer of our own, or llvmlite (which pins its own LLVM, not ours), is not worth it.
- An object cache keyed by the IR text's hash (computed while streaming) keeps incremental builds.

**What milestone 5 needs to know.**
- **Mixed game build.** For the functions in `use.txt`, `build.py` replaces the present lifter's `f_<addr>` with a
  thunk `void f_<addr>(Ctx *c) { g_<addr>(c); }`. lift2 functions name every other callee `f_<addr>` through
  `Program.symbol`.
- **Function table.** The table behind `a64_lookup` (`a64_table.c`) keeps `f_` (the thunks).
- **Hooks.** Hooked addresses (`a64_hooks.c`) and functions with an `ENTRY_HOOK` or `SPLICE_AFTER` stay with the
  present lifter until milestone 7. So do SVC users, setjmp owners and the milestone 6 forms (`scope()` reports them).
- **Runtime symbols lift2 code needs from the host.** All exist today: `a64_icall`, `a64_ijump`, `a64_trap`,
  `a64_poll_cur`, `recomp_cntpct`, `a64_dirty`. lift2 calls `a64_icall`/`a64_ijump` without the per-site cache;
  milestone 15 brings it back.
- **Flags at returns.** lift2 stores no flags at a return (AAPCS64). A present-lifter caller that reads flags after a
  BL without setting them would differ; the differential never saw one.
- **LLVM version.** `nx2/lift2/llvm.py` refuses another major. `build.py` must call `llvm.check("clang")` before
  compiling `.ll`.
- **Lift speed** for the whole program at 42-51 k instructions/s per core: about 6-7 min on one core, under a minute
  on 16 processes.

## 14b. Milestones 9-11: FP scalar, vector/FP loads and stores, NEON (done, host FP mode)

**Files** (`nx2/lift2/`):
- `enc_fp.py`: FP data processing (1-3 sources), FCMP/FCCMP/FCSEL, FMOV immediate, conversions to and from integers
  and fixed point, FMOV between register files (including `V.D[1]`), B/H/S/D/Q loads and stores of every addressing
  mode, pairs, literals, LD1-LD4/ST1-ST4 multiple and single structure, LD1R-LD4R. 16 entries.
- `enc_neon.py`: one entry per Arm ARM group (copy, three same/different, two-register misc, across lanes, scalar
  pairwise, permutes, EXT, TBL/TBX, modified immediate, shift by immediate, by element), each with its scalar twin:
  17 entries. A group's info function names the operation; printers and templates use it. Forms decoded but not
  lifted are left undecoded (`UNLIFTED`), so `scope()` passes their functions over.
- `sem_fp.py`, `sem_neon.py`: about 40 templates, lane-generic: each operation is written once for "a type" (`i32` or
  `v4i32`, `f32` or `v4f32`). 64-bit arrangements are 64-bit LLVM vectors (LLVM widens them); a write zeroes the upper
  half; scalar FP writes zero the register beyond lane 0 (LLVM drops the zeroing where only lane 0 is read again).
- `host/hos_fp.c` (runtime): FRECPE/FRSQRTE (Arm's estimate algorithm), the FPCR/FPSR helpers, MXCSR = f(FPCR).
- IR (`ir.py`, `emit_llvm.py`): V0-V31 are SSA variables of type `v128`; lane views are bitcasts; new operations:
  fadd/fsub/fmul/fdiv/fneg, fcmp, the FP casts, vector constants (FP ones as bit patterns), extract/insert/shuffle,
  `call` of intrinsics and of `a64_*` runtime routines (`memory(none)` unless they take the context).
- FPCR/FPSR: MRS/MSR are the integer side's (`t_sysreg`, context fields today). For host FP mode they must call
  `sem_fp.fp_sysreg_read/write` (runtime helpers `a64_fpcr_read`, `a64_fpcr_write`, `a64_fpsr_read`,
  `a64_fpsr_write`, each taking the context).

**What the lifted code assumes of the runtime** (the runtime milestone provides it):
- MXCSR = f(FPCR) on every guest thread whenever lifted code runs: FZ → DAZ|FTZ (0x8040), RMode RN/RP/RM/RZ → RC
  0/2/1/3, all exceptions masked. Thread creation: FPCR 0 → 0x1F80. `fiber_switch` saves and restores MXCSR with the
  guest context; HLE host code on guest threads runs under it.
- `a64_fpcr_write(ctx, x)`: `ctx->fpcr = x`, MXCSR from it. `a64_fpcr_read(ctx)`: `ctx->fpcr`.
- `a64_fpsr_read(ctx)`: `ctx->fpsr` | MXCSR sticky bits (IE→IOC, ZE→DZC, OE→OFC, UE→UFC, PE→IXC, DE→IDC only under
  FZ). `a64_fpsr_write(ctx, x)`: `ctx->fpsr = x`, MXCSR sticky bits cleared. (`nx2/test_lift2_fpleaf.py` has the
  reference implementation the harness uses.)
- `a64_frecpe_f32/f64`, `a64_frsqrte_f32/f64` from `host/hos_fp.c`.

**Results.**

| check | result |
|---|---|
| decoder vs llvm-mc, every distinct game word | 2,032,675 words, 2,032,409 decoded, 0 differences |
| census (words covered) | vector/FP load/store 100 % (333/333 forms), FP scalar 100 % (120/120), NEON 99.99 % (497/500); FP/NEON together 99.997 % of 1.59 M words |
| not covered (then) | SQRSHL V.4s (34 words), UDOT (4), FNEG V.8h (4): all lifted since (14c) |
| `tests/test_lift2_fp.py` (no game) | 356 instructions × 96 edge-value states (FPCR 0 and FZ), 0 differences outside 7.1; counted: `nan` 439 lanes, `fz-minmax` 11, `snan-minmax` 10 |
| 770 FP/NEON leaves (seed 2, 8 states; 512 ran in Unicorn, 1,748 cases) | 0 differences against Unicorn (X, SP, NZCV, V0-V31, changed pages lane-wise) and against the present lifter; `nan` 96 chunks in 15 functions, no other class |
| 148 FP/NEON call-tree roots (`--calls`, 306 functions lifted, 320 cases) | 0 differences; `nan` 92 chunks in 14 functions |
| x86 instructions at -O1, the 770 leaves | present lifter 703,386, lift2 152,009 (0.216); 12.47 vs 2.69 per guest instruction |
| x86 instructions, the call-tree functions | 228,378 vs 74,871 (0.328) |
| f_8170a4e0 (phys_vertices_aabb_b, 119 instructions) | present lifter 6,666, lift2 306 at -O1 (228 at -O2); 0 differences in 5 cases |
| f_817472e0 (186 instructions, a jump table) | lift2 440 at -O1 (target ≤ 2,600; section 1: 4,888 as lifted); the present lifter's harness cannot take it (its BR), so it is not compared |
| lift speed, FP/NEON-heavy code | 33-37 k guest instructions/s per core (integer leaves: 42-56 k) |

**What is left.** The shadow build (milestone 12). `LIFT2_DEFAULT_NAN` is implemented but not measured.

## 14c. FP runtime and the last forms (done)

- Runtime: `host/hos_fp.c` (`hos_fp_mxcsr`, `a64_fpcr_sync`, `a64_fpsr_sync`, `a64_fpcr_read/write`,
  `a64_fpsr_read/write`, `a64_frecpe_f32/f64`, `a64_frsqrte_f32/f64`); declarations in a64-kit `rt/a64.h`; MXCSR at
  thread start, fiber start and after a fiber switch (`host/hos_thread.c`); the present lifter's MSR FPCR/FPSR
  followed by the sync calls (`a64_lift.py` `fp_mode_sync`, takes effect at the next relift). `tests/test_hos_fp.py`
  checks the mapping, the helpers, DAZ/FTZ in effect, the estimates and MXCSR across fiber switches (with
  `win32_posix.c`'s fibers).
- `sem_int.t_sysreg` sends MRS/MSR FPCR and FPSR to `sem_fp.fp_sysreg_read/write`.
- The last forms: SQSHL/UQSHL/SQRSHL/UQRSHL by register (lanes in i128, saturated), PMUL/PMULL (8-bit), SUQADD/USQADD,
  FCVTXN (round to odd), FRECPX, SDOT/UDOT, FP16 FNEG/FABS. Census: every FP/NEON and vector load/store form of the
  game is covered (100 % of words). Left out, with no word in the game: PMULL 1Q (crypto), FP16 arithmetic other than
  FNEG/FABS, the by-element dot products, complex and BF16 forms.
- `tests/test_lift2_fp.py`: 394 instructions × 96 states, 0 differences outside 7.1.
- Whole population (every function of the game with an FP/NEON or vector load/store instruction that lift 2 can lift
  now; lifted with the leaf Program, compiled in modules of 1,500 functions at the build's IR flags, -O2 for the hot
  set `work/hot_funcs_play.txt`, -O1 for the rest; the present lifter's counts disassembled from the current build's
  symbols, read-only):

| | FP functions | lifted | guest instructions (FP/NEON) | present lifter x86 | lift 2 x86 | ratio |
|---|---|---|---|---|---|---|
| hot set | 588 | 322 | 241,299 (123,026) | 5,378,211 | 686,988 | 0.128 |
| rest | 31,335 | 26,687 | 3,693,830 (1,021,868) | 31,479,525 | 10,755,194 | 0.342 |
| all | 31,923 | 27,009 | 3,935,129 (1,144,894) | 36,857,736 | 11,442,182 | 0.310 |

  The 4,914 not lifted are passed over for milestone 6's forms (LSE 4,433, LDAPR/STLR 221, CAS 133, barriers 99) and
  28 words outside every table. The hot set is 852 functions, 836,310 guest instructions, 24.7 % of them FP/NEON
  (the whole game: 9.0 %). Caveat: the present lifter's hot functions are built with PGO and may have callees inlined,
  lift 2's counts are of the generic entry with callees not inlined across modules.

## 14c. Milestones 5-8: atomics, SVC, the mixed build in the game

**Milestone 6: atomics and ordering** (section 8's mapping).
- LD<op>/SWP are `atomicrmw` (LDCLR as `and` with the complement); CAS is `cmpxchg`, CASP a 128-bit `cmpxchg`
  (`cmpxchg16b`, x86-64-v3's cx16).
- LDAR/LDAPR are acquire loads (plain moves); STLR is a `seq_cst` store (`xchg`).
- The exclusives go through the runtime's monitor (`recomp_ldxr`/`recomp_stxr`/`recomp_ldxp`/`recomp_stxp`) as the
  present lifter does.
- DMB/DSB whose option orders stores before loads (`CRm & 3 == 3`: ISH, SY, OSH, NSH) are `fence seq_cst`
  (`lock or`); the load-only and store-only kinds are `fence acq_rel` (no instruction, a compiler barrier).
- CLREX clears the monitor; ISB is nothing.
- Tests in `tests/test_lift2.py`:
  - every family in one synthetic program;
  - the store-buffering litmus test on two threads (STLR then LDAR of the other location; ARM forbids both loads
    seeing 0). The lifted code shows the forbidden outcome 0 times in 1 M rounds. The same code with STLR as a
    release store (a plain mov) shows it about 25 times in 200 k rounds; when a machine shows none, the test skips
    and says it has no teeth there.

**Milestone 7, the part milestone 8 needs.**
- SVC: the flags are stored, then `a64_svc(ctx, imm)`; every register is the context's again after it (the OS layer
  may switch fibers).
- Kept on the present lifter in the mixed build, with the reasons the lift prints:
  - functions that call a setjmp owner (31);
  - hooked addresses (`a64_hooks.c`: the host's functions);
  - functions with C added after an instruction (`oot_wide`, `oot_jitter`, `oot_occlusion`: 12);
  - every function whose lifted C carries a host edit (`ENTRY_HOOK`, `SPLICE_AFTER`, the patches of
    `nx2/oot_patches.py` and `oot_native_resolution.py`: their C names an `entry_`, `splice_` or `hos_` symbol;
    105 of the functions lift 2 could take).
- Porting those edits to guest-address probes (section 9) is still open.

**Milestone 5: the mixed build.** `HOOKSHOT_LIFT2=all-safe` (or a file of addresses) at lift and build time;
`nx2/lift2/mixed.py`.
- **Lift time.** `nx2/lift.py` writes the plan: the discovery's entries, jump tables, PLT stubs, setjmp owners and
  hooks, and the instructions of every function `scope()` allows for the classes `HOOKSHOT_LIFT2_CLASSES` names
  (`int` by default).
- **Build time.** `nx2/build.py`:
  - checks the LLVM major;
  - lifts the chosen functions in 16 processes;
  - streams each partition's text (about 120 k instructions each, hot functions apart at `RX_HOT_OPT`) into
    `clang -x ir -`, with objects cached by the text's hash;
  - compiles copies of the present lifter's code files in which the chosen functions are static and unused, so that
    lift 2's `f_<addr>` thunks stand in.
- Unset, nothing new runs: the default build is unchanged.
- **Bisection.**
  - `nx2/lift2/bisect.py start <build>/lift2_functions.txt`, then `good`/`bad` after each build with
    `HOOKSHOT_LIFT2=<the .try file>`.
  - `nx2/test_lift2_leaf.py --calls --funcs <file>` runs the differential on a list first, which rules a half out in
    minutes.
- **Instruction classes.** `scope()` takes `Program.classes` (`sem.CLASS`: a template's class is its module's name),
  so the FP/NEON functions join with `HOOKSHOT_LIFT2_CLASSES=int,fp,neon` once the runtime's FP mode is in.

**Milestone 8: the game on the mixed build** (worktree build, native-vk fd4f312 plus this branch).
- **Lift 2's share.**
  - Plan: 262,632 of 294,523 functions are liftable with the `int` class.
  - Passed over: class fp 29,454; class neon 2,390; calls of a setjmp owner 31; C added after an instruction 12;
    undecoded 4.
  - The build took 262,527 of them (105 more stay for host edits): 10.69 M of 17.65 M guest instructions (60.6 %).
  - Of the hot set (`work/hot_funcs_play.txt`): 253 of 852 functions, but only 10.8 % of its instructions (the
    hot set is mostly FP/NEON code).
- **Build.**
  - Lift 2's lift of 262 k functions: 87 s on 16 processes.
  - Its 90 partitions compile in 124 s.
  - Recompiling the present lifter's files with the taken functions made static: 664 s, cold. Most of a file's
    functions are gone, so a 24 MB file takes 17-40 s instead of 100-200 s.
  - Whole build 884 s.
  - Executable 479 MB against 474 MB. The taken functions are 164.8 MB (generic entries plus their `f_` thunks, into
    which clang inlines small bodies) against 162.0 MB from the present lifter (1.017x). In bytes, lift 2's code is
    not smaller yet: every call goes through the context, and every register write is stored.
- **Smoke (kokiri).**
  - Mixed: PASS, worst picture distance 4 of 64, 0 new error-like log groups.
  - The same worktree's present-lifter build: PASS, worst distance 10.
  - No bisection was needed: no function was found broken.
- **Benchmarks** (`scripts/bench.sh <scene> 600`, three alternating rounds). The machine carried other agents'
  builds (load 10-16), so the rounds vary by more than the difference.

| scene | build | total s (3 runs) | median frame ms | p99 ms | CPU s | draws/frame |
|---|---|---|---|---|---|---|
| market | present lifter | 8.50 / 10.14 / 10.01 | 13.89 / 16.51 / 16.07 | 18.41 / 24.59 / 26.15 | 31.6 / 37.5 / 36.8 | 2865 |
| market | mixed | 11.52 / 10.14 / 16.18 | 18.18 / 16.40 / 25.99 | 30.22 / 24.92 / 43.44 | 39.4 / 35.7 / 51.6 | 2868 |
| zoo_static | present lifter | 5.07 / 4.57 / 16.83 | 8.01 / 7.35 / 26.28 | 13.87 / 11.63 / 55.66 | 15.2 / 13.9 / 28.1 | 1298 |
| zoo_static | mixed | 4.69 / 4.56 / 12.14 | 7.64 / 7.29 / 19.41 | 10.69 / 11.93 / 34.70 | 14.4 / 13.7 / 26.3 | 1298 |

  - The one round with both builds on a quiet machine (round 2) is equal within 1 %: market 16.90 against 16.90 ms,
    zoo_static 7.61 against 7.62 ms.
  - That is expected while lift 2 holds only 10.8 % of the hot instructions and calls cost what they do.

**What milestones 13-15 should know.**
- **Where the time goes in lift 2's code.** Calls and the write-through stores are what keep its code at the
  present lifter's size in bytes. The functions with calls measured 0.92-0.93x instructions; leaves 0.87x; the bytes
  of all taken functions 1.017x.
- **Facts the summaries need.** `Program` already carries everything per function. The summaries replace
  `Builder.after_call`'s "every register again" with the callee's clobber set, and the write-through stores with
  stores of what the callee reads.
- **Thunks.** Every taken function has an `f_` thunk for the present lifter's callers and the function table. With
  fast entries, `f_`/`g_` stay the generic convention, and lift 2's direct calls go to the fast entry.
- **Mixed builds already link** both lifters through the generic entry. Milestones 13-15 can be switched on per
  partition and bisected the same way (`bisect.py`, `--funcs`).
- **The hot set.** Its lift 2 functions are compiled at -O2 without a profile (the present profile is front-end
  instrumentation). An IR-level profile (`-fprofile-generate` on the `.ll` modules) is milestone 17's.
- **Taking the rest of the game.** It is mostly FP/NEON: `HOOKSHOT_LIFT2_CLASSES=int,fp,neon` once the runtime's FP
  mode (MXCSR following FPCR, fibers) is in.

## 14d. The mixed build with FP/NEON (`HOOKSHOT_LIFT2_CLASSES=int,fp,neon`, joining run)

Worktree build, native-vk 0ee8fc1, `HOOKSHOT_LIFT2=all-safe HOOKSHOT_LIFT2_CLASSES=int,fp,neon scripts/relift.sh`.

- **Lift 2's share.** Plan: 294,447 of 294,523 functions liftable (passed over: 32 with a word outside every table,
  32 calls of a setjmp owner, 12 with C added after an instruction). The build took 294,290 (157 kept for host
  edits): 17.44 M of 17.65 M guest instructions (98.8 %; int only: 60.6 %). Hot set: 826 of 852 functions, 94.9 % of
  its instructions (int only: 10.8 %); the 26 left are hooked or patched functions.
- **Build.** Lift 280 s (old lift and plan), lift 2's lift 152 s and its 145 partitions 265 s; whole relift 805 s
  (build 525 s) at 8 jobs. Executable 315 MB against 474 MB for the present lifter's (0.66x).
- **Correctness.** Smoke kokiri three times: worst picture distance 14 (one shot, Navi's position at present 1320),
  4, 8; the present lifter's build gives 2 and 12 on the same day. Title boot with the movie: 60.0 fps, title
  picture right. FPCR/MXCSR in the game as in 14c. No crash, hang or bisection needed.
- **Benchmarks** (`scripts/bench.sh <scene> 600`, each pair mixed then present lifter, load average 1 min beside):

| scene | load | build | total s | mean ms | median ms | p99 ms | CPU s | draws/frame | steps not 1/60 |
|---|---|---|---|---|---|---|---|---|---|
| market | 3.3 | mixed | 5.99 | 9.98 | 9.93 | 11.29 | 20.46 | 2869.8 | 0 |
| market | 3.4 | present | 7.20 | 12.00 | 11.89 | 14.80 | 27.53 | 2866.2 | 0 |
| market | 5.3 | mixed | 5.85 | 9.74 | 9.70 | 11.02 | 19.65 | 2869.3 | 4 |
| market | 5.6 | present | 7.47 | 12.45 | 12.17 | 17.20 | 28.70 | 2870.3 | 0 |
| zoo_static | 4.1 | mixed | 3.11 | 5.19 | 5.04 | 7.25 | 10.66 | 1298.1 | 0 |
| zoo_static | 3.6 | present | 4.13 | 6.88 | 6.76 | 8.54 | 12.95 | 1298.8 | 0 |
| zoo_anim | 3.6 | mixed | 3.61 | 6.02 | 5.93 | 7.65 | 11.46 | 382.3 | 0 |
| zoo_anim | 5.2 | present | 4.27 | 7.11 | 6.98 | 9.16 | 12.85 | 381.8 | 0 |
| zoo_shadows | 5.4 | mixed | 5.91 | 9.85 | 9.80 | 11.42 | 20.76 | 2927.6 | 2 |
| zoo_shadows | 6.0 | present | 7.64 | 12.73 | 12.51 | 16.89 | 29.35 | 2925.3 | 0 |

  - CPU by thread group, market (mixed / present): ModuleSystemWorker 9.35 / 11.15 s, BaseProcExecutor 6.12 / 10.17,
    sead::MainThread 2.87 / 3.46, NavMeshCAStepDe 0.89 / 1.34.
  - "Steps not 1/60" on the mixed build are pairs of a 2-step and a 0-step frame (bench.csv: 2.0000 then 0.0000):
    the game clock is sampled at the present while the faster game thread has already stepped the next frame. A
    sampling race, not a clock error; the same scene gives 0 on reruns (3 zoo_shadows reruns: 0, 2, 0).
- **Havok** (`HOS_DBG=havok`, market, two pairs, CPU ms a frame mixed / present): hknp tasks 1.04 / 2.92, cloth 1.78
  / 2.28, queries 0.32 / 1.25, step phase 0.16 / 0.44, all Havok 4.92 / 9.50; Havok wall per frame 7.06 / 13.6 ms.
- **PGO.** The present lifter's hot set is built with its front-end profile; lift 2's hot partitions are -O2 without
  a profile and still win. An IR-level profile needs `-fprofile-generate` on the `.ll` modules at a profiling build,
  a run, `llvm-profdata merge`, and `-fprofile-use` with the same IR (function names and CFG checksums stable across
  lifts, so the lift must be deterministic) (milestone 17).

## 14e. Milestones 13-14: whole-program summaries and their verification (done)

**Milestone 13** (`nx2/lift2/summary.py`, run by `nx2/lift.py` into the plan when `HOOKSHOT_LIFT2` is set).
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
- The other reasons keep the facts and only mean an interface outside the AAPCS64 (generic entry only).

**Distribution** (the whole game, 294,523 functions; 17.65 M instructions):
- Full summaries: 287,463 functions, 88.4 % of the instructions. Hot set: 688 of 852 functions, 64.2 % of its
  instructions.
- Noreturn: 708 functions. A generic-only boundary is reachable from 206,966 (anything that reaches a BLR, an
  unresolved BR, an SVC or a host function).
- FP: 6,996 functions write FPCR (or call something that does); 31,854 contain FP/NEON.
- Callee-saved save/restore pairs that a fast entry could delete: 612,455, in 174,749 functions.
- Call edges (1,073,423 BL, B and branch exits):

| target | edges | share |
|---|---|---|
| direct to a full summary (BL 725,080, tail 68,544) | 793,624 | 73.9 % |
| indirect (BLR 182,791, BR 23,561) | 206,352 | 19.2 % |
| host function or hook (41,704 + 2,221) | 43,925 | 4.1 % |
| direct to a demoted function (13,544 + 1,405) | 14,949 | 1.4 % |
| a noreturn seed | 14,573 | 1.4 % |

- **Cost.** 394 s in one process (decode and graph 67 s, the fixed point 288 s, results 35 s); peak memory 8.4 GB.
  It parallelises:
  - the decode and the per-function graphs are per function;
  - the fixed point's components form a DAG (294,025 components, the largest 85 functions), so each level of it can
    run side by side;
  - the results pass is per function.
  On 16 processes it should fit in about a minute; the memory (the per-instruction lists of every function at once)
  is the part to cut first.

**Against the ABI census** (`docs/lifter2-abi-census.md`). Every function on its non-benign list is demoted by
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
    not an ABI fact; lift 2 never takes it while the fiber functions are hooked.
  - Fatal callees: the census found 68 by BL-at-end-of-body. That evidence seeds noreturn here, and the fixed point
    adds what reaches them: 708 noreturn functions.
- **x9-x18 (`tmp_in` 6,659; `tmp_after_call` 5,118).** These are larger than the census' 877 and 1,382 because the
  liveness here is path-insensitive too, and counts reads through callees transitively.
  - The examples looked at (main 0x809d78c0, 0x80051080) read x18 or x12 on paths that correlated branches make
    infeasible.
  - Cross-function use of x9-x18 is not real: the verification build below never saw a non-input read.
  - The demotion stays conservative until liveness is path-sensitive (milestone 15 or later: on the SSA, conditions
    as values).

**Milestone 14: the verification build.** `HOOKSHOT_LIFT2_VERIFY=1` at lift time (the summaries are computed only then, or with `HOOKSHOT_LIFT2_SUMMARIES=1`: about 9 GB) and at build time wraps every lift 2 function with a
full summary. `host/lift2_verify.c`:
- at entry, poisons the registers the summary says are not read (`0xDEAD`, register, function);
- at exit, reports a function claimed noreturn that returned, a preserved register changed, and a register written
  that the summary says is not written;
- then restores what is preserved or not written and poisons the written registers no caller reads (`0xDEAE`);
- logs violations once per function, kind and register, to the game log and `lift2_violations.txt`.

Tail calls go past the callee's checks (to its body), so tail loops do not grow the host stack. An input holding
poison is not reported: inputs are an over-approximation (a BLR or host call counts as reading x0-x8), and a real
read of poison shows as a wrong picture or a crash naming the function.

**Verification results:**
- 262,527 functions on lift 2, of which those with full summaries are checked; the executable is 526 MB.
- **Zero violations and no crash** in the kokiri smoke scene (PASS, worst picture distance 6), the market and
  zoo_static benchmarks (600 frames each), and a 100 s title boot with the movie.
- What the first runs found, all fixed:
  - the runtime did not give back the preserved registers it had poisoned;
  - jump-table defaults counted as exits;
  - SVC dropped x30's entry value;
  - a noreturn call's fall-through into the next function counted toward clobbers.

**Plan for milestone 15: fast entries, given the measured summaries.**
- **Call edges.** 73.9 % of call edges reach a full summary directly and become fast calls. Another 19.2 % are
  indirect: see BLR below.
- **Expected gain.** From milestone 1's compile experiment (registers as values, x0-x3 + SP as arguments: 0.82 of the
  present lifter's instructions on call-heavy cold code; with the callee-saved saves deleted 0.80), and lift 2's
  generic entry at 0.91-0.93 of the present lifter on functions with calls today:
  - fast entries should bring functions with calls to about 0.80-0.83, about 10-12 % fewer instructions than now;
  - the 612,455 deletable save/restore pairs remove about 1.2 M guest instructions' worth of stores and loads.
- **Pass-through and forwarding** (counted over the 262,632 functions lift 2 takes):
  - 12,216 pure forwarders (call or tail-call with argument registers they never use themselves); 21,670 tail-call
    thunks of up to four instructions; 1,262 this-adjusting thunks (`add x0, x0, #k; b`).
  - Inputs are transitive. A wrapper needs x2 only if what it forwards to reads x2: an input the wrapper does not
    read itself and that no callee reads is not an input, and the poison would show it.
  - In the fast convention an input forwarded in x0-x3 stays in its host argument register: the wrapper passes its
    parameter to the callee's, with no context traffic; a this-adjusting thunk becomes `add rdi-equivalent; jmp`.
  - x4-x7, x8 and v4-v7 travel through the context. A forwarder that does not touch them does nothing: the writer
    stored them once (write-through), the final reader loads them. Stack arguments stay in guest memory, untouched
    by forwarders.
- **Problem to solve first: inputs are inflated by indirect and host calls.** 182,470 of the 262,632 functions have
  one of x4-x7 among their inputs without using it themselves. Almost all of it comes from the AAPCS assumption at
  BLR and host calls (x0-x8 read).
  - With fast entries this means the callers store x4-x7 before every call for nothing.
  - Remedy: at a BLR or host call, count as read only the argument registers the caller defines on some path since
    its entry or its last call, and those it received itself.
  - This is unsound only for a function that forwards an argument it never writes into an indirect call. The
    verification build checks that case (the poison of a missing input).
- **BLR through fast entries.** Indirect edges are 19.2 % of the static call edges. No call-count profile is at hand;
  the hot set's virtual calls (game objects, actors) suggest a larger dynamic share.
  - Proposal: the site cache and `a64_lookup` return fast-entry pointers. A function demoted to generic-only gets a
    shim with the fast prototype: it stores x0-x3 and v0-v3 to the context, calls `g_`, and returns x0, x1, v0, v1
    from the context.
  - The caller passes x0-x3 and v0-v3 as values. It stores x4-x7 only if it wrote them (the narrowing above), and
    takes x0, x1, v0, v1 from the result struct, treating every caller-saved register as clobbered.
  - It saves the four stores and two loads of the generic path per indirect call (more with vectors).
  - Cost: one shim per demoted function (about 7,000) and a second address per function in the lookup table. A
    callee reached both ways keeps one body behind its fast entry.
  - **Recommendation:** fast entries at BLR sites too, after the narrowing.
- **Milestone 15's own check:** the verification build above, with the fast convention compiled in, plus the
  differential harness and the smoke and benchmark runs. The fast convention changes only how values travel; the
  summaries it trusts are the ones verified here.

## 14f. Milestone 19, step 1: the present lifter writes only what it owns

In the mixed build the present lifter used to discover, translate and write C for the whole game, and the build then
made 98.8 % of it static and dropped it. Now (`nx2/lift2/owned.py`, `nx2/lift.py` after the plan) it translates and
writes only its own functions (`Lifter.only`):
- the ones lift 2 cannot take (the plan's bodies lack them: 76, from scope, hooks and C added after an instruction);
- the ones a host edit of the C reaches, predicted from the edits' sources rather than from the C: the guest
  addresses `nx2/oot_patches.py` names, comments included (heads `f_`, labels `L_`, constants and return addresses
  `0x`: the function holding the address, and for a return address the call before it; the output-size patches are
  anchored by text alone and name their functions only in comments), the `SPLICE_AFTER` addresses of `host/*.c`
  that lift 2 does not call itself (`probes.SPLICE_AT`), and the functions with C after an instruction (`Lifter.after`:
  `mixed.edited_functions` still counts them edited, as their C calls `hos_*`). Not edits: `ENTRY_HOOK`s (lift 2
  makes them, the build takes them from `host/*.c` for functions without present-lifter C) and the patches lift 2
  makes as probes. Checked against a full write of the C at 80f185c: 32 functions of lift 2's bodies edited, 0
  missed, 24 over-predicted (functions the patches' comments mention, a few constants: they stay on the present
  lifter; all 56 predicted hold 86,196 instructions, 0.5 % of the game; no other cost). A missed one would show as a required
  patch finding no anchor (the lift stops) or, for the others, as the edit missing in the game.
- with `HOOKSHOT_LIFT2=<file>`, everything not listed.

The lift writes `lift2_owned.txt` beside the plan (owned, predicted edits, the hot list); `mixed.prepare` takes
lift 2's set from it (every function not owned) and the hot partitions from the hot list (most hot functions have
no present-lifter C now). `chunks.write` chunks the share by size and keeps the saved boundaries of a full write.
`HOOKSHOT_LIFT1_ALL=1` writes every function as before. 57 of 294,523 functions are the present lifter's at
80f185c (56 predicted edits, 1 lift 2 cannot take; 326 at 451d4ac, before the entry hooks, probes and splices moved).

Measured (worktree relift, 14 GB scope, HOOKSHOT_JOBS=6, emit workers sized by memory):

| | lift | of it: write | build | relift | memory.peak |
|---|---|---|---|---|---|
| before (451d4ac) | 312 s | ~230 s (69 code files) | ~150 s (lift 2 objects cached) | 462 s | 10.5 GB |
| after | 105 s | ~26 s (2 code files) | 449 s (lift 2's 145 partitions cold: the chosen set changed their keys) | 554 s | 10.4 GB |
| after, on 80f185c | 102 s | ~25 s (2 code files) | ~300 s (lift 2's 146 partitions cold) | 401 s | 10.35 GB |

The build's cold lift 2 compile is once per change of the chosen set; a warm rebuild compiles 2 small C files instead
of 69. The peak is the build's (lift 2's partition workers), not the lift's. Smoke kokiri PASS (worst distance 4),
title boot 40 s right.

## 15. Risks not retired

- **Non-AAPCS functions** (SDK assembly, outlined helpers, veneers reading x16/x17): their number decides how much
  code keeps the generic convention. The contract checks (section 6) list them; until milestone 13 runs, the number is
  unknown. A static census (`nx2/lift2/abi_census.py`, [lifter2-abi-census.md](lifter2-abi-census.md)) lists what
  breaks the model besides setjmp and the fibers: libunwind's context pair, C++ exceptions (thrown in main and the
  SDK), about 3,500 noreturn call sites, 25 split frames, ST1/LD1 saves of d8-d15; and the rule that finds them.
- **Host FP mode divergence** in the game: NaN sign and propagation, the FZ tininess boundary, the 8 FPSR read sites.
  The shadow run (milestone 12) measures it; replays and goldens must be re-baselined.
- **Lift time** with whole-program summaries: the per-function part is measured (section 14); the summary fixed
  point is not.
- **Win64** passes vectors by reference and has 4 register arguments, so the Windows build is slower. It is a
  portability build.
- **Page marks** stay. The UFFD write-protect alternative is unmeasured.
- **One LLVM major** for Linux clang and llvm-mingw, kept in step by a version check.
