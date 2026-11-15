# Lifter optimisations: plan and sources

This plan takes techniques from emulators, binary translators and recompilers, checks each against the code that
implements it, and says what transfers to lift 2 as of ba509aec. We take ideas and cite them; we copy no code, so a
source's licence only names where an idea came from. "Not read" next to a claim means the code was not opened.

Measured facts used throughout (play binary 860eb332, 254.8 MB of lifted code, a size-weighted sample of 120
functions recompiled from edited IR; the savings were the same at -O1 and -O2):

| fact | number |
|---|---|
| back-edge polls | 13.3 % of code bytes, about 50 B a site; 58 % of polled backward branches are on no CFG cycle |
| polls only on cycles | -6.6 % |
| poll slow call as `preserve_allcc` | -6.3 % |
| page marks | 12.5 % of bytes; one `store i16 0x0101` a group -5.2 %; plain instead of release -0.4 to -0.8 % |
| context traffic (`disp(%reg)`) | about 31-33 % of bytes; `nocapture` on callees 0.0 % (-O1) / 0.7 % (-O2) |
| jump tables | 1,041 of 1,468 under 10 % dense: compare chains, not tables |
| BLR sites | 182,791; 166,256 load the target from a computed address (vtables), 1,335 from writable globals, 0 from read-only memory |
| exclusives | 3 LDXR/LDAXR, 2 STXR in the whole game; LSE 10,721 CAS and 25,802 LDADD-family |
| whole-program summaries | 117 s and 1.7 GB cold, cached per image |

Our hazards, which every item is checked against:

- **unwind**: C++ exceptions through guest frames. The guest unwinder reads saved registers from the guest stack
  through CFI.
- **setjmp/fibers**: setjmp owners and fibers.
- **hooks**: entry hooks, probes and At rows that read or write registers at a guest address.
- **indirect entry**: entry through the function table.
- **layer-ahead**: the layer-ahead splice that reads SP.

Bench step: one on/off run of `scripts/bench.sh market 600` per item.

## What the plan preserves, and why

This host came from emulator-style code. A constraint stays only if this tree shows it is needed. Verdicts:
- **NEEDED**: something breaks without it.
- **UNPROVEN**: inherited, nothing here shows a need; one experiment decides it.
- **NOT NEEDED**: delete it.

### How the counts were made

- **Static counts**: from `work/sa/plan.pickle` (the plan of HOS_GAME_VERSION 100: 294,523 bodies, PLT stubs resolved
  through its `plt` map), by `xlat_survey/why/count.py` and `spin.py` in the session scratchpad. Each was a light read
  under a 3 GB memory cap; no lift and no build.
- **sdk symbols**: from the NSO symbol table. sdk base 0x84fa0000, e.g. `__cxa_throw` 0x857ef650.
- **Run results**: quoted from `work/assets/native/cores.md` (2026-11-11) and the scratchpad's `spin_status.txt`. Not
  rerun here.

| # | constraint | verdict |
|---|---|---|
| 1 | virtual cores | NEEDED: same-(core, priority) exclusion and core masks. UNPROVEN: the 2 ms equal-priority slice, kicks, escapes |
| 2 | the poll (`ctx.poll`, `a64_poll_cur`) | UNPROVEN (serves only 1's kicks) |
| 3 | a memory barrier on every loop cycle | NEEDED (cheap) |
| 4 | C++ unwinding through lifted frames | NOT NEEDED, once a throw is a fatal stop |
| 5 | setjmp owners and fibers kept generic | NEEDED (cheap) |
| 6 | any function may be entered by BLR through the table | UNPROVEN |
| 7 | write-through of every register before calls | NEEDED only at the sites listed; elsewhere decided by C and the summaries |
| 8 | libc hooks: guest-range check, memmove semantics | range check NEEDED; memmove for memcpy UNPROVEN (free to keep) |
| 9 | FP: MXCSR follows FPCR, no contraction | FZ/RMode NEEDED; contraction off costs nothing |
| 10 | STLR as `xchg` | UNPROVEN |
| 11 | host code that reads a lifted function's frame | NEEDED: four sites, all named |

### 1. Virtual cores — NEEDED in part

- **What the game relies on.**
  - sead's thread entry 0x834a2e50 stores `GetCurrentCoreNumber() + 1` in a TLS slot when the thread's mask has one bit.
  - 324 call sites in main read that slot. 166 of them index a six-entry per-core array without a lock (`cmp w0, #6`;
    `host/hos_thread.c:107-112`; cores.md "How the game uses the core number", counted by
    `work/proto/cores/coreuse.py`).
  - 68 of 72 threads ask once, at their start.
- **What broke without exclusion** (cores.md table):
  - `HOS_VCORE_ANY` crashed ModuleSystemWorker1-4 (a read of 0x24900000004), at 24 GB and 27 fps.
  - Showing a thread a core outside its mask aborted the movie library at 7 s (`HOS_VCORE_LEND_TLS` prototype).
  - `HOS_VCORES=0` survived its runs and was the fastest (Hyrule Field +2.5 fps walking), but it runs the ~20 core-1
    threads at once with the same number. A market A/B in the scratchpad's `spin_status.txt` found no difference
    (13.3-13.6 ms either way, 4.1 k kicks/s at the default).
- **The needed guarantee.** No two threads with the same cached core number and the same priority run guest code at
  once, and no thread is shown a core outside its mask.
  - Horizon preempts across priorities at any instruction, so per-core data shared across priorities must already be
    locked by the game.
  - `HOS_VCORE_BY_PRIO` (commit 67a7e492, on no branch) keeps exactly this guarantee. Measured: waits ~27,000 → ~2,100
    per 5 s, kicks ~13,000 → ~1, zero escapes, gameplay 2-3 s sooner, fps unchanged; 6 soak runs and ~20 Hyrule Field
    checks without a hang.
- **Inherited and UNPROVEN.**
  - The equal-priority time slice (`HOS_VCORE_SLICE_US`, 2 ms). cores.md: "Horizon does not time-slice these".
  - Kicks across priorities. They are only needed for latency; no recorded bug.
  - The escapes. They break the guarantee themselves (6-10 per run). The comment at `host/hos_thread.c:1083` gives
    defaults of 1000/20000 µs; the code reads 250000.
  - The vcores arrived with the initial import (04407384), citing other recompilers (MacNativeGames, totk-pc-recomp),
    not this game.
- **Residual risk** (cores.md): data the game shares one-way between a better and a worse thread. Nothing found
  relies on it.
- **One host thread per guest thread, no vcores**: correct only if the 166 per-core array sites get a per-thread or
  locked replacement. That is a patch of the game (TLS slot per thread, arrays sized per thread), in the spirit of
  patching the game rather than imitating the console. Not attempted.
- **Experiment.** Land BY_PRIO with the slice, the kicks and the escapes off; run the cores soak
  (`work/proto/cores/soak.sh`) and the market bench.
- **Plan items that change.** A: the poll's only consumer shrinks to near zero kicks (entry 2).

### 2. The poll — UNPROVEN

- **Its only consumer is the virtual-core kick.**
  - The only writer of `ctx.poll` is `vc_acquire` (`host/hos_thread.c:1209`). A fiber switch carries a pending request
    over (`:1010`).
  - `a64_poll_cur` (`host/mkw_glue.c:216`) calls only `hos_vcore_checkpoint` (`:1311`).
  - Nothing else reads the flag: no suspension, shutdown, profiler, bench step or stop-the-world (grep of `host`,
    `gpu/direct`, the runtime).
- **Every nn::os call and SVC checkpoints anyway.** The poll serves only a lifted loop that makes no OS call while a
  better thread of its core waits. Under BY_PRIO that is about 1 kick per 5 s. For equal priorities the console would
  not preempt at all.
- **The switch.** `nx2/build.py --no-polls` only drops `-DA64_POLLING` for C. Lift 2 polls unconditionally
  (`nx2/lift2/build_ir.py` near line 247), so the experiment needs a lift-time switch.
- **Experiment.** Lift without polls (an empty memory barrier on cycles instead, entry 3), under BY_PRIO. Watch the
  `vcores:` log lines (waits, max wait, escapes) in the soak.
- **Plan items that change.** A: if the poll goes, A is the whole 13.3 % of bytes (measured share) instead of
  6.6 % + 6.3 %, and step 4 (signals) is not needed.

### 3. A memory barrier on every loop cycle — NEEDED (zero bytes)

- **Spin-waits on plain loads exist.** Scan for loops of at most 10 instructions with no call and no store, ending in a
  backward branch, that load:
  - 52,281 plain: almost all searches and walks;
  - 242 with an acquire load or CAS retry (e.g. `0x800db740`);
  - 8 with YIELD on a plain byte load. In the sdk at 0x850f5640-0x850fac30:
    `subs w20, w20, #1; b.eq out; yield; ldrb w8, [x19, #0x40]; tbnz w8, #0, loop`.
  - WFE 1 site, SEV none, YIELD 27 sites.
- **What the barrier protects.** Those 8 are count-bounded. Without a barrier LLVM may hoist the load, and the loop
  then waits out the count instead of seeing the flag. That changes timing, not a hang. An unbounded plain-load spin
  was not found by this scan (loops over 10 instructions not looked at).
- **What the game waits with.** The sdk's waits are OS calls (`hos_cv_sleep`, light events, `hos_sleep_ns`,
  `hos_thread.c:619-850`), which release the core.
- **Verdict.** Keep a zero-byte `asm sideeffect "" ~{memory}` (or the poll) on cycles; no reason to spend effort
  removing it.

### 4. C++ unwinding through lifted frames — NOT NEEDED, with a throw made a fatal stop

Static counts (`count.py`):

| what | count |
|---|---|
| `__cxa_throw` call sites | 24 (main 6, sdk 18); `__cxa_allocate_exception` 24, the same functions |
| `__cxa_rethrow` | 4 (main 1, in 0x823b01c0) |
| `_Unwind_RaiseException` callers | 4, all sdk |
| functions calling `__cxa_begin_catch` | 10, of which 0x82118640 is `__clang_call_terminate` |
| real catchers | 9: one in main (0x823b01c0, which rethrows), 8 in the sdk |
| `_Unwind_Resume` (cleanup-only pads) | 984 sites in 623 functions |
| `std::terminate` callers | 106 |
| libc++ `__throw_bad_alloc` callers | 33 sites |
| `__throw_system_error` callers | 25 sites |
| main FDEs with an LSDA | 2,183 of 247,520; rtld and subsdk0 have none (`docs/lifter-abi-census.md` section 3) |

- **What the throws are.** The census reads main's throwers as `vector`/`basic_string` length and allocation errors.
  Most of main's libc++ is built `-fno-exceptions`.
- **Measured.** The throw/unwind counters of `host/cxx_throw.c` read zero in a kokiri smoke and a 600-frame market
  bench.
- **Today.** Unwinding through lifted frames cannot work anyway (census section 3: no guest SP/x30 chain the unwinder
  could resume into).
- **Verdict.** Hook `__cxa_throw`, `__cxa_rethrow`, `_Unwind_RaiseException` and `_Unwind_ForcedUnwind` to a fatal stop
  that names the type and the caller, and mark them and `_Unwind_Resume` noreturn. That is the census's own proposed
  treatment. Landing pads become dead code.
  - Not yet looked at: what 0x823b01c0 catches (the one main-side catch).
- **Experiment.** None needed for correctness. The fatal stop is the assertion: a run that throws stops loudly.
- **Plan items that change.**
  - C may delete the callee-saved save/restore pairs (612,455; docs/lifter.md section 7). That is the fast-entry gain,
    without the prototype.
  - F loses its unwind limit: slots across calls may be promoted (entries 5 and 11 remain).
  - The paused fast entries lose their soundness objection; the boot crash and the doubled size remain.

### 5. setjmp owners and fibers — NEEDED (cheap)

- **setjmp.** 14 call sites in 13 owner functions (the plan's `no_call`/`sj_rets`):
  - main: 0x8167a900, 0x81681670 (`sys_setjmp_wrapper`), 0x82385230;
  - sdk: 10.
  - `longjmp` has 11 call sites.
  - They go through `hos_sj_call` (`host/hos_setjmp.c`) because a longjmp re-enters the owner mid-function.
- **Fibers.** `nn::os::SwitchToFiber` has 20 call sites in 9 functions (main 3: 0x83adb550, 0x83b53060 with 10 sites,
  0x83bdef30). `InitializeFiber` has 2, sdk. The fiber functions are HLE'd host code.
- **Cost of keeping them generic.** 13 functions and their calls. Nothing measurable.
- **Not checked.** Whether they run. `HOS_DBG=hooks` logs each hook's first call; setjmp owners have no such counter.
- **Plan items that change.** None. C, D and F treat these as unknown callees, already written.

### 6. Entry through the function table — UNPROVEN

- **What the table holds.** `a64_lookup` serves every lifted entry (295,270 in the plan's `entries`).
- **What could replace it.** The address-taken set: relocation targets, ADR/ADRP+ADD materialisations
  (`nx2/adr_entries.py`), vtables and pointer tables in data, thread and fiber entry points, exports, hooks. It is not
  computed as a set today; its size against 294,523 is unknown.
- **What trusting it would allow.** Functions outside it could drop their generic entry, and D's site caches would only
  ever see members.
- **What is unsound.** Guest code that computes a code address arithmetically (a jump into the middle of a table of
  stubs). The jump tables are resolved separately.
- **Experiment.** Emit the set into `a64_table.c`. In `a64_lookup`'s miss path (`third_party/a64-kit/rt/a64_rt.c:31`),
  trap with the address when a target is not in the set. Then run the smoke and the market bench. No switch exists.
- **Plan items that change.** D (smaller target domain) and the fast entries.

### 7. Who reads the context of a running lifted function from outside — NEEDED at these sites only

- **Entry hooks** (`entry_<addr>(ctx)` at entry, `emit_llvm.emit_function`): read and write registers at the entry.
- **C-after and SPLICE sites** (`lift2_after_<pc>(ctx)`: `oot_wide`, `oot_jitter`, `host/direct_*.c`,
  `host/rt_slots.c`): read and write at their pc.
- **At rows**: take SSA values, not the context.
- **SVC and runtime calls with ctx**: `a64_svc`, `recomp_ldxr`/`stxr`, `a64_icall`/`ijump`, `hos_sj_call`.
- **The stall dump** (`host/hos_thread.c:276-301`): reads the saved `ctx.x[29]` and walks the frame chain. It is a
  diagnostic and accepts stale values (the comment at `:1305` says so).
- **Not readers.** The vcore checkpoint (`:1305-1312`: "must never read or write the thread's guest context"). The
  unwinder reads guest memory, not the context.
- **So.** Write-through is needed before calls to unknown callees, hooks, splices and runtime calls. Elsewhere C's
  summaries decide.
- **Plan items that change.** C (its checklist already names hooks and splices).

### 8. libc hooks' extra semantics

- **Guest-range check: NEEDED.** Commit 6f8f2432, "host copies, file reads and writes, and the NV12 reader stay in
  guest memory", with `tests/test_guest_copies.py`. It keeps a host `memmove` from writing outside the guest
  reservation. This is host memory safety, not imitation.
- **memmove for memcpy: UNPROVEN** (from the initial import). It costs nothing. For G's intrinsic keep `llvm.memmove`;
  no experiment needed.

### 9. Floating point

- **FZ: NEEDED.** The game sets FPCR.FZ on its main thread (docs/lifter.md section 8: `sys_fpcr_flush_to_zero`,
  observed in the game). Results differ without it.
- **RMode following: NEEDED in principle.** Only reachable through `fesetround`/`fesetenv` by function pointer.
  Kept because it is free outside `strictfp` functions.
- **No contraction: costs nothing.** The emitter writes plain `fmul`/`fadd` without `contract` flags, and LLVM does
  not fuse those.
- **Default NaN and the other 8.1 deviations.** Already not imitated.
- **No evidence in the tree for frame-rate determinism tied to these.** No doc mentions determinism. Nothing to
  decide.

### 10. STLR as `xchg` — UNPROVEN

- **Counts.** 1,546 STLR sites, 52 in hot functions; LDAR 1,478.
- **What it protects.** A plain x86 store is already a release store. The `xchg` matters only for STLR followed by
  LDAR to another address, where the game relies on store-load order (a Dekker-style flag pair). None was looked for or
  found.
- **Experiment.** A lift-time switch emitting STLR as `store atomic release` (a `mov`), then the cores soak and the
  market bench. No switch exists.
- **Gain.** Small: 52 hot sites.

### 11. Host code that reads a lifted function's frame — NEEDED (four sites, named)

- `host/direct_layer_ahead.c:213`: `c->x[31]`.
- `host/direct_job_sync.c:117`: `G64(c->x[31] + 0x10)`, a frame slot of its caller.
- `host/direct_mesh_system.c:52-56`: builds a call frame below SP and calls guest code.
- `host/rt_slots.c:718`: `alloc_sp = c->x[31]`.
- Plus the stall dump's frame-chain walk (entry 7).
- **Plan items that change.** F must exclude the functions of these splice sites. A frame slot those splices read
  must stay in guest memory.

## A. Poll placement

### Our side (read)

- **Where the polls are.** `nx2/lift2/build_ir.py` `build`, the loop over blocks after the exits: any block with a
  successor at an address at or below its own gets a `poll`, so every backward branch polls whether or not it is on a
  cycle. The emitted form is `emit_llvm.HELPERS["lift2_poll"]`: a `load atomic i32 monotonic` of `ctx+856`, then a
  cold path that stores 0 and calls `a64_poll_cur()`.
- **What the poll calls.** `a64_poll_cur` (`host/mkw_glue.c`) calls `hos_vcore_checkpoint` (`host/hos_thread.c`).
  That yields the virtual core when `t->vc_kick` is set. The comment there requires that it, and everything it calls,
  never reads or writes the guest context or runs guest code.
- **Who sets the flag.** The kicker is `vc_acquire`. A waiter picks the worst holder on an allowed core and sets
  `vc_kick` and `ctx.poll` (or `run_ctx->poll` for a fiber), then sleeps on its condition variable for 1 ms per round.
- **Timing it needs.** After `HOS_VCORE_SLICE_US` (2 ms) an equal-priority holder is also asked. A waiter "escapes"
  without a core after 250 ms (`HOS_VCORE_ESCAPE_US`). The latency the poll needs is therefore well under a
  millisecond, not per instruction.
- **Other places the request is seen.** Every nn::os call and every SVC also checkpoints. `HOS_VCORES` is on by
  default; `docs/development.md` names `0` as the switch off.
- **A second job the poll does.** docs/lifter.md section 9: a spin loop on a plain guest load stays a loop only
  because the poll is a call that may write guest memory. Any change must keep a compiler barrier on every cycle.

### How others decide where to check

- **QEMU TCG** (`accel/tcg/translator.c`, `gen_tb_start`/`gen_tb_end`, v9.0.0,
  https://github.com/qemu/qemu/blob/v9.0.0/accel/tcg/translator.c; GPL-2.0+).
  - Every translation block starts with `tcg_gen_ld_i32(count, env, offsetof(ArchCPU, parent_obj.neg.icount_decr.u32))`
    and `brcondi_i32(LT, count, 0, exitreq_label)`. `gen_tb_end` places `exit_tb(TB_EXIT_REQUESTED)` at that label.
  - The check is left out only for TBs built with `CF_NOIRQ`. Under `CF_USE_ICOUNT` the same word doubles as the
    instruction budget: the `sub` immediate is patched in at the end of the block.
  - So: one check per block entry, on a word in the CPU state, merged with the cycle budget.
  - Lesson: they put the request flag and the budget in one signed word, so one compare serves both.
- **Dynarmic** (`src/dynarmic/backend/x64/a64_emit_x64.cpp`, `EmitTerminalImpl` overloads, master,
  https://github.com/lioncash/dynarmic; 0BSD).
  - The check lives in the block-linking terminals. `LinkBlock` emits `cmp [r15+cycles_remaining],0; jg` when cycle
    counting is on, else `cmp [r15+halt_reason],0; jz`, before the patched jump to the next block. A taken halt
    writes `pc` and returns to the dispatcher.
  - `LinkBlockFast`, `PopRSBHint` and `FastDispatchHint` have no halt check. `CheckHalt` is an explicit terminal.
  - Lesson: they check per block link, not per loop, and leave the check out on the fast link. Their dispatcher bounds
    the latency instead.
- **Go** (proposal 24543, https://go.googlesource.com/proposal/+/master/design/24543-non-cooperative-preemption.md;
  `src/runtime/preempt.go`, `src/runtime/signal_amd64.go` `pushCall`, go1.22.0; BSD-3).
  - Loop back-edge polls were rejected. The proposal quotes a fault-based variant at a 7.8 % geomean slowdown
    (their number), and says explicit polls cost more in tight loops.
  - Go instead sends a signal (SIGURG) to the thread. The handler checks `isAsyncSafePoint` (compiler-marked unsafe
    points, runtime code, enough stack) and rewrites the signal context with `pushCall`: `sp -= 8; *sp = resumePC;
    rip = asyncPreempt`. `asyncPreempt` saves every user register and calls `asyncPreempt2`.
  - At an unsafe point it gives up and retries later.
  - Lesson: `pushCall` writes below SP with no red-zone gap, which is correct for Go's ABI, which has no red zone.
    Our lifted code is built with the SysV red zone: `nx2/build.py` has no `-mno-red-zone`. A copy would have to step
    over 128 bytes first.
- **HotSpot C2** (JDK-8186027 "C2: loop strip mining", review thread
  https://mail.openjdk.org/pipermail/hotspot-compiler-dev/2017-October/027223.html; GPL-2.0 with CPE; bug page not
  opened, 403).
  - C2 used to leave the safepoint poll out of counted loops. Time-to-safepoint grew with the trip count, so the fix
    nests the loop and polls every `LoopStripMiningIter` (1000) iterations.
  - Later reports (JDK-8220374, JDK-8241492, from the search results; not read) still found polls hoisted out of hot
    loops and seconds-long time-to-safepoint.
  - Lesson: exempting "short" loops was a recurring source of latency bugs. Do not exempt loops by guessed trip count.
- **Box64/FEX**: no source for loop polls or signal preemption found. Both run guest threads 1:1 on host threads, so
  the kernel preempts and they have no reason to poll (not read; from memory).

### What transfers

1. **Poll only on CFG cycles.** In `build_ir.build`, replace "successor at a lower address" with "edge into a block
   that dominates its source" (a natural loop back edge), or, for irreducible CFGs, retreating edges of a DFS from
   the entry. The other 58 % of backward branches (shared epilogues, cold blocks laid out above) stop polling.
   - Measured here: -6.6 %.
   - Latency: none lost. A path with no cycle runs a bounded number of instructions before it reaches a call, an exit
     or a cycle.
2. **Cheap slow path.** Declare `a64_poll_cur` `preserve_allcc` in `emit_llvm` and define it with
   `__attribute__((preserve_all))` in `host/mkw_glue.c`. The fast path then has no caller-saved spills around the cold
   call.
   - Measured here: -6.3 %.
   - Condition: `hos_vcore_checkpoint` and what it calls (SRW lock, condition variable) are ordinary C, and the
     `preserve_all` wrapper saves around them.
   - Check: whether clang 23's `preserve_all` on x86-64 keeps the YMM upper halves. We keep `<2 x i64>` only, so XMM
     is enough. Not verified.
3. **QEMU's merged word.** Our flag is already one `i32` load and compare. Nothing to gain unless a cycle budget joins
   it.
4. **Signal-based preemption, Go's design (no polls at all).**
   - How it would work: the kicker in `vc_acquire` sends a signal to the holder's host thread as well as setting
     `vc_kick`. The handler leaves at once unless the interrupted RIP is inside lifted text. Lifted code holds no host
     locks, and `hos_vcore_checkpoint` reads no guest state, so every instruction boundary of lifted code is a safe
     point.
   - The handler rewrites the context: `rsp -= 128` (red zone), then 8-aligned push of the resume RIP, then `rip =`
     a trampoline that saves every GPR, XMM and RFLAGS, calls `hos_vcore_checkpoint`, restores them, undoes the
     128-byte gap and returns.
   - When the RIP is in host code (HLE, the runtime), the handler does nothing. The next nn::os/SVC checkpoint, or
     the kicker's next 1 ms round (which would re-signal), covers it.
   - What remains in the code: a zero-byte compiler barrier on each cycle (`call void asm sideeffect "",
     "~{memory}"()`) for the spin-loop rule.
   - Expected: removes the remaining polls; that is the 13.3 % minus the 6.6 % and 6.3 % above, if those two land
     first. None measured.
   - Risks: signals interrupting host syscalls (EINTR in HLE code; needs `SA_RESTART`); the Superluminal eBPF profiler
     and debuggers seeing an extra signal; Windows-era host code (`host/posix`) if it waits in non-restartable calls.
5. **Counted-loop exemption.** Not transferred (HotSpot's lesson above, and our IR does not know trip counts).

### Soundness checklist

- **Whether the poll is needed at all** is UNPROVEN ("What the plan preserves", entries 1-3). Its only consumer is the
  virtual-core kick. The cycle barrier stays either way.

- **unwind**: polls touch no guest state; unaffected.
- **setjmp/fibers**: the flag is the running context's (`run_ctx`); unchanged.
- **hooks**: none read the poll.
- **indirect entry**: unaffected.
- **layer-ahead**: unaffected.
- **Spin loops**: every cycle must keep a memory barrier. In step 1 it keeps the poll; in step 4 it keeps an empty
  asm barrier.
- **Step 4 only**: signal safety of the trampoline (it must not run inside the handler); the red zone; `SA_RESTART`
  for host calls.

### First step to bench

Step 1 (cycle-only polls) alone, on/off: `scripts/bench.sh market 600`. Then step 2 on top. Step 4 is a separate
experiment after both, with `HOS_VCORES` statistics (`vc_st.kicks`, the wait times) compared to show the latency is
kept.

## C. Summary-driven call boundaries, with no new prototype

### Our side (read)

- **What a BL costs today.** `build_ir.build` at a BL sets x30 to a constant, emits `gcall`, then calls
  `ir.Builder.after_call`, which clears `blk.defs`: every later read of any register is a `regload` from the context.
- **Write-through.** Every register write before the call has already been stored to the context (`writeback`).
  LLVM keeps those stores because the callee gets `ctx`.
- **What we know about callees.** `summary.py` computes per function: inputs, clobbers and must-defs, the preserved
  set (frame pass: x19-x30 and d8-d15 saved and restored), noreturn and the demotion reasons. Unknown-facts functions
  (sp, save_ptr, lr_data, lr_ret, split_frame) are treated as clobbering everything.
- **Measured here.** 31-33 % of bytes are context accesses, and `nocapture` gains nothing. The cost is the
  convention, which is exactly this item.

### How others do it

- **XenonRecomp** (`XenonRecomp/recompiler.cpp`, main, https://github.com/hedge-dev/XenonRecomp; MIT).
  - The register accessor lambdas decide per register whether it is a C local or `ctx.rN`:
    - `nonVolatileRegistersAsLocalVariables`: r14 and up, f14 and up, v14-v31 and v64-v127;
    - `nonArgumentRegistersAsLocalVariables`: r0, r2, r11, r12, f0, v32-v63;
    - the CR, CTR, XER and reserved flags.
  - Locals are declared value-initialised (`PPCRegister r14{};`), never loaded from ctx. Calls pass only `(ctx, base)`.
    Calls to the compiler's `__savegpr*/__restgpr*` helpers are printed as nothing when non-volatile locals are on.
  - So their callee sees zeros, not the caller's r14, and its saves go away. That is our parked fast-entry design,
    with the same exposure.
  - Their README says it "assumes the game follows the ABI" and that exceptions are not supported (`skip_lr`).
    `setjmp` copies the whole `ctx` into a local `env` and restores it after a non-zero return. Locals are not part
    of `ctx`, so a non-volatile local is not restored by longjmp; the code does not handle this (our reading of
    the code).
  - Their number (README): about 20 MB smaller and "several milliseconds" less frame time in UnleashedRecomp.
- **rev.ng** (`enforce-abi`, `promote-csvs`, https://docs.rev.ng/references/pipeline/; GPL-2.0).
  - From the documented pipeline and artifacts only (not read): after `enforce-abi`, "each register promoted to a
    local variable, an argument and/or a return value". The step order is `enforce-abi`, then `promote-csvs`, then
    `promote-init-csv-to-undef`.
  - It is a decompiler's view and needs the full prototype.
- **N64Recomp**: not read. Its README states only that calls and lookups go through a context and a runtime table.
- **Dynarmic** `a64_get_set_elimination_pass.cpp` (0BSD): block-local forwarding of get/set pairs. We already get this
  from SSA; it does nothing across calls.

### The variant for us, and why it is not exposed to unwinding

Keep the generic `f_<addr>(ptr ctx)` and every callee-saved save in the guest frame. Change only what the caller does
around a direct BL whose callee has full facts:

1. **Keep every write-through store of x19-x29, SP and d8-d15 before the call.** The callee loads them at entry and
   saves them into the guest frame, exactly as today. That is what makes this variant unwind-safe:
   - the guest frames hold the same saved values as now;
   - the context holds the live values at any throw point;
   - so a guest unwinder (libunwind via CFI, `unw_getcontext`) sees what it sees today.
   - Measured: the throw and unwind counters of `host/cxx_throw.c` (`__cxa_throw`, `__cxa_rethrow`,
     `_Unwind_RaiseException`, `_Unwind_ForcedUnwind`, `_Unwind_Resume`) read zero in a kokiri smoke run and a
     600-frame market bench.
   - Zero in two scenes is not a proof that no exception unwinds through lifted code, so the condition stays: the
     guest frames and the context must hold what a guest unwinder reads. The variant makes nothing worse.
2. **After the call, do not reload the registers the callee's summary preserves.** The caller keeps its SSA values
   for them. This removes the reloads.
   - It is sound when the callee really restores them. A landing pad is not a successor of the call in our IR, so no
     path from an unwind reaches these SSA values.
   - setjmp: calls of setjmp owners already go through `hos_sj_call`, which re-enters from the context. Those calls
     must stay "reload everything".
3. **Do not store dead temporaries.** A write of x9-x18 that is neither a callee input nor read after the call (the
   AAPCS says x9-x18 are clobbered) needs no write-back. Its value is never read again; LLVM keeps the store only
   because of the call.
4. **x30 and the flags.**
   - x30: drop the store only when the callee neither saves x30 nor reads it as data (`lr_data`), i.e. leaf callees.
     A non-leaf callee saves x30 into its frame, and a guest frame walk reads it there.
   - Flags: drop the store when the target has no `flags_in` (the `flagsout` on tail exits, `build_ir` exits loop).
5. **Reloads of everything else stay**: callee inputs it may have written, results x0/x1/v0-v3, and every register
   after an unknown callee, a BLR, an SVC or a hooked function.

What this changes in code:

- `ir.Builder.after_call(preserved)` keeps `defs` entries for the preserved set.
- `build_ir.build` at a BL looks up the callee's summary through `Program`, and marks the writes it may drop.
  `writeback` would need a "dead after this point" analysis: a backward pass per function over the blocks, killed
  by callee inputs.
- No change to `emit_llvm`.

### Needs

- Summaries on every build: measured at 117 s and 1.7 GB cold, cached per image.
- Per-function liveness for item 3.

### Soundness checklist

- **Constraints this checklist relies on**: "What the plan preserves", entries 4, 5, 7 and 11.
- **If a throw becomes a fatal stop** (entry 4, NOT NEEDED), the unwind condition below no longer holds C back: the
  save/restore pairs may go too.

- **unwind**: saves unchanged in guest frames; ctx live values unchanged at every call. Safe as argued in 1 and 2.
- **setjmp/fibers**: setjmp-owner calls and the HLE'd fiber functions (host hooks) count as unknown callees, so
  everything is reloaded.
- **hooks**:
  - An ENTRY_HOOK that returns 1 replaces the body. Its function's summary must not be trusted for preservation.
    Treat every hooked function as an unknown callee (the hook list is known at lift time, `nx2/entry_hooks.py`).
  - At rows already use SSA values; unaffected.
  - The SPLICE/C-after sites read the context after an instruction. If such a site is reached after a call and
    reads a preserved register, the context still holds it (stores kept), so it is fine. A splice that writes a
    preserved register into ctx (`rt_slots`, the mesh system) must end the "kept SSA" region: treat a splice as a call
    that clobbers everything.
- **indirect entry**: a function entered from the table is entered through its generic entry; unchanged.
- **layer-ahead**: reads the guest stack through SP; the stack is unchanged.
- **Verification**: `HOOKSHOT_LIFT_VERIFY=1` already checks "preserved register changed" at exit. That is the
  assumption step 2 relies on, so a verify run covers it.

### Gain

- None measured for this variant alone.
- Our own reference point: registers as values with saves deleted measured 29,442 → 24,189 instructions
  (section 2.1). This variant gets only the reload and dead-store part of that.
- Their number (XenonRecomp, with saves deleted): 20 MB and several ms.

### First step to bench

Step 2 only (no reloads of the preserved set after BLs to full-summary, unhooked callees), with summaries on. Bench
`market 600` on/off, and one verify run for violations. Steps 3 and 4 follow.

## D. Indirect calls (166,256 vtable-style BLR sites)

### Our side (read)

- `build_ir` BLR: emits `icall`, then `after_call`. `emit_llvm` emits `call void @a64_icall(ptr %ctx, i64 %t)`.
- `third_party/a64-kit/rt/a64_rt.c`:
  - `a64_icall` calls `a64_lookup`: one global direct-mapped cache of 2^18 slots, keyed `(a>>2 ^ a>>20)`, each
    slot `addr<<32 | offset` read whole, then a binary search of the function table.
  - The same file still holds the deleted lifter's per-site two-slot inline cache: `a64.h` `A64_SITE_CALL`, a static
    `uint64_t[2]` per site with the first slot compared inline, and `a64_rt.c` `site_call`/`a64_icall_site`, which fill
    a free slot and keep only the first two targets so threads do not keep rewriting a line.
  - It also holds `A64_SITE_PROFILE`, which writes "site target calls" lines every 5 s for a `scripts/devirt.py` that
    is no longer in the tree.
- Lift 2 uses none of these.

### How others do it

- **Dynarmic** (`a64_emit_x64.cpp`, 0BSD): fast dispatch table of `{location_descriptor, code_ptr}` entries, indexed by
  `crc32(descriptor) & mask` (SSE4.2), filled on a miss. The RSB pairs a pushed descriptor with a code pointer. The
  same idea as our `a64_lookup` cache, with a better hash.
- **QEMU**: `lookup_tb_ptr` with the per-CPU jump cache; not read this pass.
- **Rosetta 2** (dougallj's article, https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/; no code):
  lookup tables for indirect branches, falling back to the JIT on a miss.
- **LLVM indirect-call promotion** (`llvm/lib/Transforms/Instrumentation/IndirectCallPromotion.cpp`, llvmorg-19.1.0;
  Apache-2.0 with LLVM exception).
  - Reads value-profile candidates of an indirect `call` (`ICallPromotionAnalysis`), takes them in count order, and
    resolves each with `Symtab->getFunction(Target)`.
  - **It stops at the first target that is missing or only a declaration** ("UnableToFindTarget"), or that fails
    `isLegalToPromote`.
  - With a profile summary, only hot sites are promoted.
  - Vtable mode (`EnableVTableProfileUse`) compares the loaded vptr against a vtable address point. It needs
    `llvm.type.test` type metadata and the vtable definitions in the module, which we have neither of.
- **Whole-program/CHA devirtualisation** (LLVM WholeProgramDevirt): needs type metadata; not read.

### What transfers

1. **Per-site monomorphic or two-target inline cache.** Rebuild `A64_SITE_CALL` in `emit_llvm` for lift 2:
   - a private global `[2 x i64]` per BLR site; load slot 0 and compare the high half with the target;
   - on a hit, call through the relative offset, else `a64_icall_site(ctx, t, site)`.
   - Per site, needs nothing. It removes the hash, the call into `a64_icall` and the global-cache line sharing, but the
     call is still opaque: no summary, no inlining.
   - Expected: none measured. The old lifter used it; no saved number.
2. **Profile-guided promotion at lift time, our own form.**
   - Revive `A64_SITE_PROFILE` (runtime exists) and a small `scripts/devirt.py` that writes `site → dominant target`.
   - `build_ir` at a BLR with a profiled target K emits `t == K ? gcall f_K : icall t`.
   - On the direct path, item C and inlining apply, because the callee is known at lift time.
   - This avoids both LLVM ICP conditions we cannot meet: the target need not be defined in the partition (we
     compare guest addresses, not host function pointers), and no type metadata is needed.
   - Needs a profile per scene (the plan's cache key must include it).
3. **LLVM ICP itself.** It would apply only if a BLR became `%f = call ptr @a64_lookup(i64 %t)` plus `call void %f(ptr
   %ctx)` under IR PGO (`-fprofile-generate`/`-fprofile-use` on the partitions' IR; whether clang's `-x ir` path
   instruments the same way is not verified). Even then it refuses every target defined in another partition (above),
   which is most of them, unless ThinLTO imports them. Not recommended: 2 does the same with less machinery.
4. **Vtable contents as constant data** (the coordinator's point: the vtables live in relocated read-only data even
   though the object's vptr does not).
   - The slot load `ldr x8, [vptr, #off]` cannot be folded without knowing vptr.
   - A static candidate set for a site, "every function at offset `off` of every vtable in RO data", is too large to
     use for common offsets.
   - It can guard 2: a profiled target must be in that set, or the site is not promoted.
   - Low value on its own.

### Soundness checklist

- **The table-entry assumption** is entry 6 (UNPROVEN). An asserted address-taken set would shrink D's target domain.

- **unwind**: unchanged; the call is a call.
- **setjmp/fibers**: a promoted target that is a setjmp owner or hooked must go through the same path a BL to it
  takes (`hos_sj_call`, the hook symbol). Use `prog.symbol(prog.callee(K))` as for BL.
- **hooks**: an ENTRY_HOOK on K applies because `f_K` is the hooked entry.
- **indirect entry**: unchanged; the fallback is `a64_icall`.
- **layer-ahead**: unaffected.
- **Site cache**: the slots are written racily but whole (64-bit), as `site_call` already argues.

### First step to bench

Transfer 1: per-site cache on all BLRs, on/off with `market 600`. Then 2 on the hot partition's sites with one
profile from the same bench.

## E. Jump tables as index switches

### Our side (read)

- `build_ir.build`, BR with `prog.jt[pc]`: `switch` on `fshr(t - lo, t - lo, 2)`, cases `(target - lo) >> 2`.
- The cases are code offsets, so they are as sparse as the targets are spread: 1,041 of 1,468 tables are under 10 %
  dense and become compare chains (measured here).
- `discover.py` already finds the table base, entry size, shift and bound (CMP + B.HI/B.HS, CCMP, AND mask).

### How others do it

- **Ghidra** (`Decompiler/src/decompile/cpp/jumptable.hh`, Ghidra_11.1_build; Apache-2.0), class comments read; the
  .cc search was not read.
  - `JumpBasic::recoverModel` finds the normalized switch variable, "the Varnode with the most restricted set of
    values used by the model to produce the destination addresses", and its guards, with a `maxtablesize` cap.
  - `findUnnormalized` walks back through bounded add/sub, shift and extension steps.
  - `JumpBasic2` handles a default path; `JumpModelTrivial` is the fallback.
  - Addresses are checked against the function and dropped if they fall outside it.
- **rev.ng** `simplify-switch` (pipeline step) and remill/anvill: not read.

### What transfers

- Switch on the **table index**, with cases `0..n-1` mapping to targets. That is dense by construction, and LLVM emits
  one table load.
- The discovery knows the load instruction and its index register. `build_ir` takes the index's SSA value at the
  load pc (the index register may be reused before the BR) and carries it to the BR.
- Default: an index outside `0..n-1` goes to `a64_ijump(t)` as today, so the result is exact.
- Ghidra's "addresses checked against the function" corresponds to our `inside()`.

### Soundness checklist

Exact; no hazard. A probe or At row on the load instruction must see the same value; the switch reads the SSA index
after any probe.

### Gain

Speed (no size change, measured as "speed, not size"). None measured.

### First step to bench

Index switches for bounded tables only (1,385 of 1,475 per docs/lifter.md section 4), on/off.

## F. Frame-slot promotion

### Sources (read in pass 1)

- WYTIWYG/BinRec (https://escholarship.org/content/qt8d3363v4/qt8d3363v4.pdf; paper CC-BY-4.0, code not read).
  - Recovers variables dynamically from traces and replaces the emulated stack with "native stack allocations".
  - Their numbers: 1.22x for GCC 4.4 -O3 binaries, 2.10x for -O0, about 1.0-1.1x for modern -O3.
  - They note hmmer got slower with symbolisation (2.28x → 3.04x slowdown against native).
- SecondWrite "associates all local variables of functions beyond a certain complexity with a single symbol" (same
  paper, section 1).
- rev.ng `detect-stack-size`, `promote-stack-pointer`, `segregate-stack-accesses`: names and order only; not read.

### Our hazards decide the subset

- **Re-judged in "What the plan preserves".** With a throw made fatal (entry 4), the unwind limit below goes. setjmp owners (entry 5) and the four frame-reading splice sites (entry 11) remain.

- **Unwind**: a promoted save slot breaks the guest unwinder. So never promote the callee-saved save slots, and never
  promote a slot that is live across a call that may unwind (most calls).
- **Sound subset 1: leaf functions** (no BL/BLR/SVC) whose frame address is never taken (`frame_taken` false in
  `summary.frame_pass`). Nothing can observe their frame while they run, unwinding included: a leaf cannot throw. Spill
  slots and locals become an `alloca`.
- **Sound subset 2: non-leaf slots that are not saves and are dead at every call.** Their values cross no call, so no
  unwinder or callee sees them.
- **Layer-ahead**: it reads the guest stack through SP at one site. Its function must be excluded by name. It is a
  splice, so `build_ir` knows it.
- **setjmp owners**: excluded (their frames are restored by longjmp).
- **Hooks**: At rows take register values, not frame slots. Any C-after site gets the context, not the frame. A
  function with a C-after site is excluded.

### Gain

None measured. Sections 2.1 and 7 numbers (saves) do not apply to this subset. Value is lower than A-D because the
cross-call slots, the bulk of stack traffic, are excluded by the unwind hazard.

### First step to bench

Subset 1 (leaves), on/off.

## G. Short entries

- **Libc calls as intrinsics** (FEX lists memcpy/memset with a compile-time direction among its optimisations,
  https://fex-emu.com/FEX-2312/; code not read).
  - A BL to the ENTRY_HOOK_SYM'd `memcpy`/`memset`/`memmove` (`host/nn_libc.c`) is emitted as
    `llvm.memcpy(gaddr(x0), gaddr(x1), x2)` plus x0 := x0. x9-x18 become poison and x30 is not stored.
  - Small constant sizes then become moves, and the call costs nothing in context traffic.
  - Hooks: `host/nn_libc.c` (read) does more than the copy. For it to be equivalent, the intrinsic site must reproduce:
    - the `hos_guest_range` check on both ranges (outside the window the hook returns 0 and the guest's own copy runs);
    - `memmove` semantics even for `memcpy` (the hook tolerates overlap), so emit `llvm.memmove`;
    - the same for `__memcpy_aarch64` and `__memcpy_aarch64_simd`, which the hooks cover too.
  - The range check can stay as an inline compare with the hook call as the fallback.
  - Gain none.
- **CRC32C**: `sem_int.t_crc32` + `emit_llvm.HELPERS["lift2_crc32"]` is a bit loop.
  - The C variants equal SSE4.2 `crc32` (same Castagnoli polynomial, reflected), one instruction.
  - The ISO variant needs a table or PCLMUL.
  - Exact. Gain depends on use; the site count is not measured here.
- **Noreturn**: with summaries on every build, emit `unreachable` after BLs to the 708 noreturn functions. Code size
  only. Verify checks "noreturn returned".
- **CCMP chains**: `sem_int.t_ccmp` clears `b.cmp` and builds four selects. Keep the compare as
  `(ok, x, y, kind, nzcv-imm)` and fold the consumer to `select(ok, icmp(pred, x, y), imm-cond)`. Whether LLVM already
  recovers it is not checked (look with `nx2/lift2/dump.py`).
- **NEON gaps** (coordinator's counts):
  - SQRSHL lane expansion, 34 sites: per-lane variable shift with rounding and saturation. AVX2 has `vpsllv`/`vpsrlv`
    for 32/64-bit lanes only, so 8/16-bit lanes need widening.
  - SHA-256 without SHA-NI, 392 sites: x86-64-v3 has no SHA. Zen has SHA-NI, but the build targets v3 and SHA-NI is
    not part of it. A function-level `target-features="+sha"` like the AES path (`fn.target_features`) with a CPUID
    check, or the scalar form.
  - Value depends on whether these are hot.
- **Build level**:
  - IR PGO on the hot partition (also enables D3);
  - `-fsplit-machine-functions` with that profile;
  - hot/cold wrappers (a cold generic entry wrapping a hot body);
  - 2 MB text pages for the 255 MB of lifted code (`MADV_HUGEPAGE` on the text after start, or a linker-aligned
    segment).
  - All "none" measured. They depend on a profile and do not change correctness.

## B. Page marks (decided: deleted)

Nothing reads page marks, so the lifter emits none.

- **Finding.** The only reader was `collect_in` (`host/rx_backend.c`, lines 47-80 at dd1db1de). It was reachable only
  through the `MkwVkHost` fields `collect_written`/`collect_written_in` (`gpu/api/vk_api.h`), which nothing in
  `gpu/direct` or `gpu/api` reads.
- **Not a reader.** `hos_watch_guest_writes` (`RX_NVN_WATCH_TEX`) is an `mprotect` watch and does not use `a64_dirty`.
- **Decision.** The user decided to delete the marks (12.5 % of code bytes measured here), with `emit_llvm.MARK_FN`,
  `build_ir.group_marks`, `sem_int.mark_atomic` and `A64_WRITE_TRACK`. The deletion is on its own branch.

If a reader is ever needed, these are the options in order of cost:

- **SP-provenance exemption** (Lasagne PLDI 2022, slides https://systems-rg.github.io/slides/2022-06-24-lasagne.pdf,
  "for every ld and st, explore the use-def chain of their pointer operand; if the access is performed on a stack
  address, no fence is inserted"; their code not read).
  - For us: `build_ir.base_offset` would root an address at the entry SP or a register `summary.frame_pass` tracks as
    SP plus a constant (x29, `mov x9, sp`). Today `sem_int.store` only skips Rn == 31.
  - Per function, safe by construction.
  - Gain none measured.
- **One mark per group.** `group_marks` already gives a group one mark of its first and last byte. Emitting it as a
  single `store i16 0x0101` when both bytes fall on adjacent pages, and as a byte store when on one page, is measured
  here at -5.2 %. Plain stores instead of release: -0.4 to -0.8 %. Release only matters if the reader may run on
  another thread while the store is in flight; the reader exchanges with acquire and the GPU upload happens after,
  so a plain store plus the scan's fence suffices (to confirm with the reader).
- **Write-protect and fault notification, no per-store code.**
  - QEMU (`accel/tcg/cputlb.c`, v9.0.0; LGPL-2.1+): a clean page gets `TLB_NOTDIRTY` in its soft-TLB write entry, so
    the fast-path compare fails. `notdirty_write` sets the dirty bitmap (and invalidates translated code), then
    `tlb_set_dirty` restores the fast path. `tlb_reset_dirty` sets the flag again when a client clears the range.
    That is one slow store per page per clear, which is what a host page fault gives us without a soft TLB.
  - Dynarmic/yuzu fastmem has no write notification of its own: GPU-visible memory is tracked by the GPU's rasterizer
    cache with page-granular `mprotect` (not read).
  - On Linux, three host forms:
    - `mprotect` + SIGSEGV: one signal per first write per page after each re-protect, plus an `mprotect` call per
      range.
    - userfaultfd write-protect with `UFFD_FEATURE_WP_ASYNC` and `PAGEMAP_SCAN` (Linux 6.7+): the kernel resolves the
      fault itself; one minor fault per first write per page, and one ioctl reads and re-protects a range. This is
      what docs/lifter.md section 9 already names.
    - Soft-dirty bits (`/proc/self/clear_refs` `4`, bit 55 of `/proc/self/pagemap`): clearing is process-wide, so
      every page of the 512 GB window is write-protected again, and reading needs a pagemap read. Too coarse for
      a per-frame scan.
  - Cost against marks: a fault is about a microsecond or more (none measured), against about 8-12 % of all code bytes
    for marks. It wins for pages written once per scan and loses only for many pages rewritten every frame. That is
    the measurement to make before choosing.

## Summary

The constraints behind each risk are judged in "What the plan preserves, and why". Order by value, given the
measurements: A (bytes measured), C (the 31-33 % context traffic is the convention), D
(166k opaque calls), E, then F and G. B is decided (marks deleted, nothing reads them) and is listed last.

| item | value | depends on | first step | risk |
|---|---|---|---|---|
| A poll placement | HIGH: -6.6 % and -6.3 % measured; the whole 13.3 % if the poll goes (preserves 2) | nothing; BY_PRIO for removal (preserves 1) | polls on natural-loop back edges only | spin loops need a barrier on every cycle |
| C call boundaries | HIGH; larger with pair deletion if a throw is fatal (preserves 4) | summaries every build (117 s, 1.7 GB cached) | no reload of preserved registers after full-summary, unhooked BLs | wrong summary (verify build), hooks returning 1, splices writing ctx |
| D indirect calls | MEDIUM-HIGH | nothing (1); a profile (2) | per-site 2-slot cache (existing runtime code) | none (exact compare) |
| E index switches | MEDIUM (speed) | discovery's load pc and index register | bounded tables only | none |
| F frame slots | LOW-MEDIUM; MEDIUM if a throw is fatal (preserves 4) | `frame_pass` per function | leaf functions, frame not taken | unwind if extended beyond the subset |
| G libc intrinsics, CRC32C, noreturn, CCMP, NEON, build | LOW-MEDIUM each | various | one each | small |
| B page marks | done by deletion: 12.5 % of bytes | nothing (no reader) | the deletion branch | none; a future reader takes the fallbacks in B |

## Ruled out, with the count

- LDXR/STXR loop recognition: 3 LDXR/LDAXR and 2 STXR in the whole game, against 10,721 CAS and 25,802
  LDADD-family LSE operations. docs/lifter.md section 9 claims the recognition exists; lift 2 has none, and it is not
  worth adding. The doc line should be corrected.
- Constant-folding BLR targets from read-only memory: 0 of 182,791 BLR sites load their target from read-only memory
  (166,256 from computed addresses, 1,335 from writable globals).
- Fast entries (parked by the user): crash at boot, the executable doubled through inlining, unsound under unwinding
  because the saves leave the guest frame. C is the subset that keeps the saves.
