# The lifter: which guest functions are not ordinary AAPCS64 functions

The question: any change that keeps guest registers in SSA values across calls trusts callee-saved registers and the
stack discipline (the summaries and the verification build, [lifter.md](lifter.md) "Whole-program summaries and the
verification build"; the queue's item C, [lifter-optimisations.md](lifter-optimisations.md)). setjmp/longjmp and the
fiber switch are handled by host code. Are those all the functions that break that model?

**Short answer: no, but almost.** Every function in the game that switches stacks or restores callee-saved registers
from memory it did not save them to is one of: musl's `__setjmp`/`longjmp` (handled), the nn::os fiber switch
(handled: its public API is HLE'd, so the assembly underneath is never reached), and libunwind's context save/restore
pair (reached only by C++ exceptions, which are a fatal stop: `host/cxx_throw.c`). What the special-cased set misses is not more context switches
but four classes a list does not cover: C++ exceptions (thrown in main and the SDK, 623 functions with landing
pads), noreturn callees (about 3,500 call sites that the compiler ended with nothing after the BL), functions the
discovery cuts so that a frame spans two entries (25), and hand-written assembly that saves d8-d15 with ST1/LD1 or
uses x29 as a general register. The summary stage can find all of them with one rule (below) instead of a list.

The census is `nx2/lift2/abi_census.py` (the lifter's decoder, the discovery with its HLE set, an abstract
interpretation of SP, x29, x30, x19-x28, d8-d15 and the own frame per function, 294,523 functions in 2.5 minutes on
4 processes). Its output is `work/lift2/abi_census.txt` (local).

## 1. What is special-cased today

| what | functions | how | where |
|---|---|---|---|
| setjmp owners | `__setjmp` 0x8504ec40 (`libc_setjmp`), `longjmp` 0x8504ed50 (`libc_longjmp`); 14 callers of `__setjmp` (main 4, sdk 10), 11 of `longjmp` (incl. `pthread_exit` 0x85112e10) | setjmp and longjmp run lifted; each setjmp return address becomes an entry (13, all of the census' "entry inside another function's FDE"), each call of an owner is wrapped in a host setjmp, a BR to a pushed return address is a host longjmp | `nx2/lift2/discover.py` (`find_setjmp_owners`), `nx2/lift2/game.py` (`sjcall_<owner>`); `host/hos_setjmp.c:95` (`hos_sj_call`), `:119` (`hos_setjmp_return`) |
| fibers | `nn::os::InitializeFiber`, `SwitchToFiber`, `FinalizeFiber`, `GetCurrentFiber` | HLE at the definition: a guest fiber is a host fiber with its own guest context; the SDK's `DispatchFiberContext` 0x8510d214 and `CallFiberFunction` 0x8510d430 are only reached from inside the HLE'd functions | `host/hos_thread.c:1003-1050` |
| HLE / stubs | 529 host `imp_` functions + 218 stubbed OS-facing imports, hooked at their definition; among them `nn::diag::detail::AbortImpl` (noreturn), `nn::diag::GetBacktrace` (returns 0 frames), `nn::os::CreateThread`/`StartThread` (thread first frames: x30 = 0, SP set, `a64_icall`) | the lifter emits a call of the host function; the body is not lifted | `nx2/standalone.py:97` (`hle_set`), `nx2/lift.py:54`; `host/hos_thread.c:677-692`, `host/nn_diag.c:22` |
| `ENTRY_HOOK` | 42: assets/textures 14, direct back end passes/layers/mesh 10, libc by symbol 6 (`host/nn_libc.c`: memcpy, memmove, memset, memcmp, strlen, strcmp), C++ throws 4 (`host/cxx_throw.c`), render slots 3, zstd 2, the occlusion grid reader 1 (`host/nvn_native.c`), diagnostics 1 (`host/direct_diag_env.c`), bench 1 | `if (entry_<addr>(c)) return;` at the top (the hook sees x0-x7 and x30 only, host/hos.h); engine and performance hooks, and the throws' fatal stop. None exists because of SP/LR/callee-saved behaviour | `nx2/entry_hooks.py`, `host/*.c` |
| PAC | PACIBSP/AUTIBSP/RETAB (182,316 functions), PAC/AUT register forms on x30 (3: setjmp, longjmp, DispatchFiberContext) | the identity | `nx2/lift2/sem_int.py` (`t_pac`, `t_hint`) |
| noreturn | none listed | a UDF after a call traps as on the hardware; a BL followed by another function's entry is a call, then a tail call into that entry (never reached) | the summaries' noreturn seeds (`summary.NORETURN_NAMES`, `NORETURN_PARTS`) |

## 2. Census classes and counts

All modules the lift covers (main, subsdk0, sdk, rtld), HLE'd functions excluded.

| class | functions | benign? | what they are |
|---|---|---|---|
| SP from a loaded or argument value (`sp_from_value`) | 3 | no | `longjmp`, `DispatchFiberContext`, libunwind's jump (0x857f1880) |
| SP not entry SP at an exit, unknown (`sp_exit`) | 8 | no (3), discovery (5) | the 3 above; GetContextSize 0x8507fcd0, 0x81dc6f78, 0x81dcff78, 0x824e2ea8, 0x857da520: code the discovery merged across two functions (a block reached with two SP values) |
| frame spans two entries (`split_frame`) | 25 | no | 12 pairs in main 0x824c43bc-0x824f8adc (a function the discovery cut at an inner entry: one half tails into the other with its frame allocated, the other returns with SP+N), rtld's lazy-bind trampoline 0x80001bd0, `nn::ro::BindEntry` 0x851187fc |
| RET xN (N != 30) or BR x30 (`lr_ret_reg`) | 1 | handled | `longjmp` (BR x30) |
| x30 at RET not the entry x30 (`lr_exit`) | 23 | 3 no, rest tool | setjmp, longjmp, DispatchFiberContext, libunwind; the others are split/merged functions above or slots the tool lost to overlapping or register-offset stores |
| entry x30 used as data (`lr_data`) | 8 | no (needs x30 in the context) | setjmp, longjmp area, `DispatchFiberContext`, `unw_getcontext` (stores it), `VAbortImpl` (stores it: the abort report), `nn::ro::LookupNextSymbol`, `nn::ro::BindEntry`, rtld 0x80001bd0/0x80001c64 |
| x30 loaded from non-frame memory (`lr_load`) | 171 | yes | x30 as a scratch register after its save (LLVM allocates LR); reloaded from the frame before the return |
| ADR into x30 (`adr_x30`) | 15 | yes | the same: x30 as a scratch register |
| callee-saved entry values stored outside the frame, 4+ (`cs_save_arg`) | 4 | no | `__setjmp`, `unw_getcontext` 0x857f49c0, `UserExceptionHandler` 0x851109b8 and 0x85110e58 (d8-d15 only) |
| x29 or x30 plus 3 more callee-saved loaded from outside the frame (`cs_restore_arg`) | 244 | 3 no | longjmp, DispatchFiberContext, libunwind's jump; the other 241 load x30 as a scratch register (as `lr_load`) |
| callee-saved not restored at an exit (`cs_exit`) | 50 | see text | longjmp, DispatchFiberContext, libunwind (real); 18 libjpeg-turbo NEON functions (`jsimd_*`, sdk 0x84ffae90-0x84fff040) and 4 audio tap filters (0x852196f0-0x85219fdc) that save d8-d15 with ST1/LD1 multi-register forms (restored; the tool does not track those); 10 sdk audio/AAC functions (`VoiceInfo::UpdateForCommandGeneration`, four `CommandListProcessor::ProcessCommand`, three `MasteringForRtcScreenSharing`, two `FDKaacEnc_*`) where x29 or x25-x28 are not their entry values at a return; not resolved by the tool; the split/merged functions |
| entry value of x19-x28 used as data (`cs_entry_data`) | 34 | 2 no, rest probably tool | `__setjmp` and `DispatchFiberContext` (they mangle the values before storing them, so their save also shows here); main 0x821beea0-0x824190ec: reads on paths the tool cannot prove infeasible |
| x9-x18 read before written (`livein_tmp`) | 877 | mostly tool | x16 in rtld's trampoline (real, a PLT resolver); the rest are reads on infeasible paths (correlated branches) |
| x9-x18 or flags read after a BL (`after_call`) | 1,382 (629 callees blamed) | tool | checked by hand on the most blamed callees (0x80686590, `std_atomic_wait_backoff`): they write those registers, so the callers' reads are on infeasible paths. No sign of interprocedural register allocation |
| flags live at entry (`livein_nzcv`) | 1 | tool | `agl_primitive_texture` |
| MRS/MSR NZCV | 12 functions | yes | flag save/restore inside a function (main 0x81fe0610-0x83a23cb0, crypto bignum add/sub) |
| MSR FPCR / FPSR | 9 functions | lifter.md "FP and NEON" | `fesetround`, `fesetenv`, `feclearexcept`, `feraiseexcept`, UserExceptionHandler, 0x857f71f0, main 0x80ec0ea0 and 0x8169d350 (save, set, restore FPCR) |
| MSR TPIDR_EL0 | 3 | HLE'd path | `ThreadManagerImplByHorizon` ctor 0x8510e010, 0x85056b60, 0x8510eb00: thread setup inside nn::os |
| MRS TPIDR_EL0 / TPIDRRO_EL0 | 1,196 / 324 instructions | yes | TLS |
| x18 used | 3,829 | yes | an ordinary temporary in this toolchain |
| SVC | 26 functions | yes | the sync point |
| SP set by a register (alloca, VLA, `ld1 [sp], xN`) (`sp_dyn`) | 239 | yes | SP back at entry SP at every exit |
| entry inside another function's FDE (`mid_fde`) | 13 | handled | all 13 are setjmp return addresses |
| BL with nothing after it (`noret_call`) | 68 callees, ~3,500 sites | no (noreturn) | below |
| fixed point not reached in 200 visits | 2 | tool | 0x811cc790, 0x812f5340 (very large) |

### Noreturn callees (BL at the end of a body)

`nn::diag::detail::AbortImpl` and its overloads (0x85059010, 0x85059040: 1,146 sites), `UnexpectedDefaultImpl`
0x8505a1d0 (84), `abort` 0x85717f20 (119), `__assert_fail` 0x85717f30 (16), `exit`, `_Exit`, `nn::os::ExitProcess`,
`nn::os::QuickExit`, `std::terminate` (11), `__cxa_throw`, `_Unwind_Resume` (136), libc++'s `__throw_*`
(`__throw_system_error`, `__throw_bad_alloc`), libpng's `png_error`/`png_chunk_error`/`png_fixed_error` (longjmp
out), `TryEnterInternalCriticalSectionAbort`, and in main `libcxx_bad_variant_access_abort` 0x8284c6f0 (603 sites),
`libcxx_bad_optional_access_abort` 0x82becaf0 (139), `cxx_call_terminate` 0x82118640 (44) and 20 more with 1-4 sites.
Without a noreturn mark the summary would count the tail call after such a BL as an exit (wrong SP/x30 at that
"exit"); the noreturn seeds prevent it.

## 3. C++ exceptions

- The SDK carries libc++abi and libunwind (`__cxa_throw` 0x857ef650, `_Unwind_RaiseException` 0x857f4160,
  `_Unwind_Resume` 0x857f4380, `unw_getcontext` 0x857f49c0).
- main imports `__cxa_throw`, `__cxa_rethrow`, `__cxa_begin_catch`, `_Unwind_Resume`, `__gxx_personality_v0`.
  2,183 of main's 247,520 FDEs have an LSDA (CIE augmentation `zPLRB`); rtld and subsdk0 have none.
- Throw sites: `__cxa_throw` has 24 callers (main 6: 0x821195b0, 0x821197b0, 0x8211c230, 0x82395d00, 0x823d7c00,
  0x824421f0; sdk 18, libc++'s `__throw_*`); `__cxa_rethrow` 4. `_Unwind_Resume` has 984 call sites in 623 functions
  (main 704): those are the landing pads' cleanup paths. Most of main's libc++ is built with `-fno-exceptions` (the two
  aborts above say so); one component around 0x8211xxxx-0x824xxxxx is built with exceptions (its throwers are length
  and allocation errors of `vector` and `basic_string`).
- Can it happen? Statically: the throw sites exist and are reachable from normal code, but they are error paths
  (container length overflow, allocation failure, `system_error`). Nothing shows a throw used for control flow. If one
  ever ran, `_Unwind_RaiseException` would walk guest frames by `.eh_frame` from the guest SP and x30: lifted frames
  keep neither in memory, so unwinding through lifted code cannot work.
- Treatment: `__cxa_throw`, `__cxa_rethrow`, `_Unwind_RaiseException` and `_Unwind_ForcedUnwind` are entry hooks
  that stop the game with the type and the caller (`host/cxx_throw.c`, `hos_fatal`); the summaries seed them and
  `_Unwind_Resume` as noreturn. Landing pads are unreachable code. The kokiri smoke and a 600-frame market bench throw
  nothing.

## 4. The non-benign list and treatment

| function | what | treatment |
|---|---|---|
| `__setjmp` 0x8504ec40, `longjmp` 0x8504ed50 | jmp_buf save/restore, PAC-hashed | calls of setjmp owners go through `hos_sj_call`, the return points are entries; longjmp's BR is handled by the runtime (`hos_setjmp_return`) |
| `DispatchFiberContext` 0x8510d214, `CallFiberFunction` 0x8510d430, `os_fiber_switch_impl` 0x851092f0, `os_fiber_start_impl` 0x85109240 | the SDK's fiber switch | unreachable while `SwitchToFiber`/`InitializeFiber` are HLE'd; lifted, not listed as never-lift |
| `unw_getcontext` 0x857f49c0, libunwind's jump 0x857f1880 | libunwind context save and resume | unreachable: every throw stops at the fatal hooks (section 3) |
| `__cxa_throw`, `__cxa_rethrow`, `_Unwind_RaiseException`, `_Unwind_ForcedUnwind` | exception raise | fatal stop (`host/cxx_throw.c`); noreturn seeds of the summaries |
| `UserExceptionHandler` 0x851109b8, 0x85110e58 | the SDK's user exception handler (CPU exception context, FPCR/FPSR) | never entered on the host (no guest CPU exceptions); generic entry only, or never-lift |
| rtld 0x80001bd0, `nn::ro::BindEntry` 0x851187fc, `LookupNextSymbol` | lazy-binding trampolines (x16/x17 in, x30 as data, SP+16 out) | unused (every import bound by the image builder); never-lift or generic only |
| `VAbortImpl` 0x85058df0 | stores x30 for the abort report | its callers store x30 (summary fact "x30 as data"), or the HLE of `AbortImpl` covers it |
| noreturn callees (68, list above) | | noreturn from the summaries (rule below: 708 functions); the lifted code after such a BL is kept, never reached |
| 12 split-frame pairs, main 0x824c43bc-0x824f8adc; 5 merged bodies (0x8507fcd0, 0x81dc6f78, 0x81dcff78, 0x824e2ea8, 0x857da520) | the discovery cut a function at an inner entry, or merged two | demoted by the summaries (`sp`, `split_frame`); the halves are not merged |
| jsimd (18) and audio tap filters (4) | hand-written NEON; d8-d15 saved with ST1/LD1 | fine as lifted; the summaries' frame pass accepts ST1/LD1 saves of d8-d15 (full summaries; one audio tap filter demoted for `tmp_in`) |
| 10 sdk audio/AAC functions (above) | x29/x25-x28 at a return not proven by the tool | the lifter's summary decides; demoted if not proven |

Nothing else in the game manipulates SP, x30 or callee-saved registers outside the AAPCS64. The remaining census hits
are benign (x30 as scratch, alloca, TLS, flag save/restore within a function) or the tool's path-insensitivity.

## 5. Detecting them in the summary stage instead of by list

A function has a **full summary** only if all of these are proven on its own IR (with callees' summaries); any
failure demotes it and lists it with the reason (`summary.py`'s `DEMOTE` reasons):

1. **SP**: SP at every RET and tail call equals entry SP (an SSA value equal to `%sp`); SP is only ever entry SP plus
   constants, or a value whose every use before the exit is replaced by a reload of entry SP + constant (alloca
   frames). SP from a load or from an argument demotes.
2. **x30**: every RET returns through the entry x30 (the value is the entry argument or a reload of the slot that
   holds it); no BR x30, no RET xN with N != 30; entry x30 not stored outside the frame (else it is "x30 as data":
   callers store it).
3. **Callee-saved**: at every exit, x19-x29 and d8-d15 are their entry values, by the save pattern (stored to a frame
   slot with STR/STP/ST1, reloaded from the same slot) or by not being written. A store of an entry callee-saved value
   or of SP to memory not provably in the own frame demotes (the jmp_buf signature, `cs_save_arg`).
4. **Exits**: a tail call with SP != entry SP demotes the caller (`split_frame`). A fall-through into another entry
   not after a call demotes.
5. **noreturn**: a callee is noreturn when none of its paths reaches RET or a tail call to a returning function (fixed
   point over SCCs, seeded with UDF/BRK and the fatal functions' names). A BL whose return address is not in the
   body, or is another function's entry, must reach a noreturn callee; otherwise demote.
6. **Inputs**: x9-x18 or NZCV live at entry, and x9-x18 read after a BL that the callee's summary clobbers, demote
   (the summaries' liveness is path-insensitive like the census', so their counts, `tmp_in` 6,659 and
   `tmp_after_call` 5,118, are upper bounds).

The verification build ([lifter.md](lifter.md), "Whole-program summaries and the verification build") is the check
that the rule is conservative enough.

## 6. What the census could not settle statically

- Whether any C++ exception is thrown in a real run beyond the scenes run so far: a throw stops the game with its
  type (`host/cxx_throw.c`).
- The 23 `lr_exit` and 34 `cs_entry_data` hits in main that are not split/merged functions: the tool loses frame
  slots to overlapping and register-offset stores; the lifter's IR will decide.
- The identity of main's exception-enabled component (0x8211xxxx-0x824xxxxx) and of the merged bodies 0x81dc6f78,
  0x81dcff78, 0x857da520.
- Whether the SDK's own threads ever run `UserExceptionHandler` (only on a guest CPU fault, which the host turns into a
  host signal).
