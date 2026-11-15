# Lifter optimisations: the queue

The open changes to the lifter's output, each with the source it is taken from, what it needs, when it is sound and
the first step one on/off run of `scripts/bench.sh market 600` can measure. What the lifter emits now, its
pessimizations and why they are kept are in [lifter.md](lifter.md). We take ideas from other translators and cite
them; we copy no code. "Not read" next to a claim means the source code was not opened.

Measured facts the items use (play binary 860eb332, the size-weighted sample of 120 functions recompiled from edited
IR; savings the same at -O1 and -O2):

| fact | number |
|---|---|
| context traffic (`disp(%reg)`) | about 31-33 % of code bytes (with page marks and polls still in that binary; not rescaled); `nocapture` on callees 0.0 % (-O1) / 0.7 % (-O2) |
| host-stack spills | 5.8 % of code bytes |
| jump tables | 1,041 of 1,468 under 10 % dense: compare chains |
| BLR sites | 182,791; 166,256 load the target from a computed address (vtables), 1,335 from writable globals, 0 from read-only memory |
| callee-saved save/restore pairs | 612,455 in 174,749 functions |
| summaries | 394 s and 8.4 GB in the lift's process (this tree); 117 s and 1.7 GB as a job of its own, cached (`agent/lift-fast`) |

Static counts come from `work/sa/plan.pickle` (HOS_GAME_VERSION 100: 294,523 bodies, PLT stubs resolved through its
`plt` map), read under a 3 GB memory cap; sdk symbols from the NSO symbol table (sdk base 0x84fa0000).

What every item is checked against:

- **setjmp owners**: a longjmp re-enters the owner mid-function through `hos_sj_call` (`host/hos_setjmp.c`); 14 call
  sites of `__setjmp` in 13 owners, 11 of `longjmp`. Calls of owners stay "reload everything".
- **hooks**: entry hooks (`entry_<addr>(ctx)` reads and writes registers at the entry), C-after and SPLICE sites
  (`lift2_after_<pc>(ctx)` at their pc). At rows take SSA values and are unaffected.
- **frame readers**: the host sites that read a lifted function's frame (lifter.md, pessimization 2).
- **indirect entry**: any function may be entered through `a64_lookup`'s table.

## Unproven constraints

Each is kept until its experiment runs.

- **STLR as `xchg`** (lifter.md, pessimization 5). The `xchg` matters only for an STLR followed by an LDAR of another
  address where the game relies on store-load order (a Dekker-style flag pair). None was looked for. Experiment: a
  lift-time switch emitting STLR as `store atomic release`, then the smoke and the market bench. Gain small: 52 hot
  sites.
- **Entry through the function table** (pessimization 6). Unsound if guest code computes a code address
  arithmetically. Experiment: emit the address-taken set (relocation targets, `nx2/adr_entries.py`'s
  materialisations, vtables and pointer tables in data, thread and fiber entries, exports, hooks) into `a64_table.c`
  and trap in `a64_lookup`'s miss path (`third_party/a64-kit/rt/a64_rt.c:31`) on a target outside it; smoke and market
  bench. Shrinks D's target domain.
- **Write-through and reloads beyond the sites that need them** (pessimization 1): decided by C's first step.
- **Saves and frames beyond the five frame readers** (pessimization 2): decided by C's pair deletion and F.
- **memmove semantics for `memcpy`** in `host/nn_libc.c`'s hooks: from the initial import, costs nothing; G keeps
  `llvm.memmove`. No experiment needed.

## C. Summary-driven call boundaries

**What.** Keep the generic `f_<addr>(ptr ctx)`. Around a direct BL whose callee has full facts:

1. After the call, do not reload the registers the callee's summary preserves; the caller keeps its SSA values
   (`ir.Builder.after_call(preserved)` keeps their `defs`; `build_ir.build` looks the callee's summary up through
   `Program`).
2. Do not store dead temporaries: a write of x9-x18 that is neither a callee input nor read after the call needs no
   write-back (a backward liveness pass per function, killed by callee inputs).
3. x30: drop the store only for leaf callees that do not read x30 as data (`lr_data`). Flags: drop the store when the
   target has no `flags_in`.
4. Delete the callee-saved save/restore pairs of full-summary functions outside the frame readers' functions (612,455
   pairs). With throws fatal (`host/cxx_throw.c`) no unwinder reads the saves.
5. Reloads of everything else stay: callee inputs it may write, results x0/x1/v0-v3, everything after an unknown
   callee, a BLR, an SVC, a hooked function.

No change to `emit_llvm`.

**Sources.**
- XenonRecomp (`XenonRecomp/recompiler.cpp`, https://github.com/hedge-dev/XenonRecomp; MIT). Non-volatile and
  non-argument registers are C locals, value-initialised, never loaded from ctx; calls pass only `(ctx, base)`; calls of
  `__savegpr*/__restgpr*` print as nothing. Their README says it "assumes the game follows the ABI" and that exceptions
  are not supported. Their `setjmp` copies `ctx` and restores it after a non-zero return, but the locals are not in
  `ctx`, so a non-volatile local is not restored by longjmp (our reading). Their number: about 20 MB smaller and
  "several milliseconds" less frame time in UnleashedRecomp. Lesson: the gain is real, and setjmp is where it breaks.
- rev.ng (`enforce-abi`, `promote-csvs`, https://docs.rev.ng/references/pipeline/; GPL-2.0; documentation only): every
  register promoted to a local, argument or return value after `enforce-abi`. It needs the full prototype.
- Dynarmic `a64_get_set_elimination_pass.cpp` (0BSD): block-local get/set forwarding; SSA gives us that already.

**Needs.** Summaries on every build (394 s and 8.4 GB in the lift today; a job of its own on `agent/lift-fast`: 117 s,
1.7 GB, cached). Per-function liveness for step 2.

**Soundness.**
- A wrong summary breaks step 1: `HOOKSHOT_LIFT_VERIFY=1` checks "preserved register changed" at every exit.
- setjmp owners and the HLE'd fiber functions count as unknown callees.
- A hooked function (entry hook returning 1 replaces the body) counts as an unknown callee; the hook list is known at
  lift time (`nx2/entry_hooks.py`).
- A C-after or SPLICE site that writes a register into ctx (`rt_slots`, the mesh system) ends the kept-SSA region: it
  counts as a call that clobbers everything.
- Step 4 excludes the functions of the frame readers.

**First step.** Step 1 only, with summaries on: `market 600` on/off and one verification run for violations. Steps 2-4
follow. Gain not measured; the bound is the 31-33 % of context traffic.

## D. Indirect calls

**What.**
1. A per-site cache: a private `[2 x i64]` per BLR site, slot 0's high half compared inline with the target, a hit
   called through the stored offset, else `a64_icall_site(ctx, t, site)`. The runtime already has it
   (`A64_SITE_CALL`, `site_call` in `a64_rt.c`); `emit_llvm` does not emit it. Removes the hash, the call into
   `a64_icall` and the shared cache lines; the call stays opaque.
2. Profile-guided promotion at lift time: `A64_SITE_PROFILE` (in the runtime) writes site, target and count; a script
   writes `site → dominant target`; `build_ir` emits `t == K ? gcall f_K : icall t` at a BLR with target K. The direct
   path then gets C and inlining.

**Sources.**
- Dynarmic (`src/dynarmic/backend/x64/a64_emit_x64.cpp`, https://github.com/lioncash/dynarmic; 0BSD): a fast
  dispatch table of `{location_descriptor, code_ptr}` indexed by `crc32(descriptor) & mask`, filled on a miss; the RSB
  pairs a pushed descriptor with a code pointer. The same idea as `a64_lookup`'s cache with a better hash.
- Rosetta 2 (https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/; no code): lookup tables for indirect
  branches, the JIT on a miss.
- LLVM indirect-call promotion (`llvm/lib/Transforms/Instrumentation/IndirectCallPromotion.cpp`, llvmorg-19.1.0;
  Apache-2.0 with LLVM exception): it stops at the first profiled target that is missing or only a declaration
  ("UnableToFindTarget"), and its vtable mode needs `llvm.type.test` metadata and the vtable definitions. We have
  neither: most targets live in other partitions. Lesson: compare guest addresses at lift time instead (2), which needs
  no definition in the module and no type metadata.

**Needs.** Nothing for 1. A profile per scene for 2, in the partitions' input key.

**Soundness.** The cache's slots are written racily but whole (64-bit). A promoted target that is a setjmp owner or
hooked goes the way a BL to it goes (`prog.symbol(prog.callee(K))`). The fallback is `a64_icall`, so both are exact.

**First step.** 1 on every BLR, `market 600` on/off. Then 2 on the hot partitions' sites with one profile from the same
bench.

## F. Frame-slot promotion

**What.** Guest frame slots as `alloca`s, in two subsets:
1. Leaf functions (no BL/BLR/SVC) whose frame address is never taken (`summary.frame_pass`'s `frame_taken`).
2. Non-leaf slots that are not saves and are dead at every call.

**Sources.**
- WYTIWYG/BinRec (https://escholarship.org/content/qt8d3363v4/qt8d3363v4.pdf; CC-BY-4.0; code not read): recovers
  variables from traces and replaces the emulated stack with native stack allocations. Their numbers: 1.22x for GCC
  4.4 -O3 binaries, 2.10x for -O0, about 1.0-1.1x for modern -O3; hmmer got slower with symbolisation. Lesson: modern
  optimised code gains little, and the gain is not guaranteed.
- rev.ng `detect-stack-size`, `promote-stack-pointer`, `segregate-stack-accesses`: names and order only.

**Needs.** `frame_pass` per function, so summaries on the build.

**Soundness.** Excluded: setjmp owners (longjmp restores their frames), the frame readers' functions, any function with
a C-after or SPLICE site. At rows take register values, not slots.

**First step.** Subset 1, `market 600` on/off. Gain not measured.

## G. Short entries

- **libc calls as intrinsics.** A BL to the hooked `memcpy`/`memset`/`memmove` (`host/nn_libc.c`, also
  `__memcpy_aarch64` and `__memcpy_aarch64_simd`) becomes `llvm.memmove`/`llvm.memset` on `gaddr` plus x0 := x0, x9-x18
  poison, x30 not stored. Source: FEX lists memcpy/memset with a compile-time direction among its optimisations
  (https://fex-emu.com/FEX-2312/; code not read). Soundness: the site keeps the hooks' `hos_guest_range` check on both
  ranges as an inline compare with the hook call as the fallback (commit 6f8f2432, `tests/test_guest_copies.py`: host
  memory safety), and memmove semantics. First step: the three hooks' BLs, on/off.
- **CRC32C as `crc32`.** `sem_int.t_crc32` with `emit_llvm.HELPERS["lift2_crc32"]` is a bit loop; the C variants equal
  SSE4.2's `crc32`. The ISO variant needs a table or PCLMUL. Exact. First step: count the sites, then the C variants
  on/off.
- **Noreturn as `unreachable`** after BLs to the 708 noreturn functions. Code size only. Needs summaries on the build;
  the verification build checks "noreturn returned".
- **CCMP chains.** `sem_int.t_ccmp` builds four selects. Keep the compare as `(ok, x, y, kind, nzcv-imm)` and fold the
  consumer to `select(ok, icmp(pred, x, y), imm-cond)`. First step: read whether LLVM already recovers it
  (`nx2/lift2/dump.py`).
- **NEON gaps.** SQRSHL family lane by lane (34 sites): AVX2 has `vpsllv`/`vpsrlv` for 32/64-bit lanes only, so 8/16-bit
  lanes need widening. SHA-256 (392 sites): a function-level `target-features="+sha"` as for AES, with a CPUID check,
  since x86-64-v3 has no SHA-NI. First step for both: whether the sites are in hot functions.
- **Build.** `-fsplit-machine-functions` with a profile; hot/cold wrappers (a cold generic entry around a hot body);
  2 MB text pages for the lifted code (`MADV_HUGEPAGE` after start, or a linker-aligned segment). None measured.

## Fast entries (paused)

`F_<addr>` with one register prototype for every function (`{i64, i64, <2 x i64>, <2 x i64>} (ptr ctx, i64 sp, i64
x0-x3, <2 x i64> v0-v3)`), callee-saved pairs deleted, `f_<addr>` a wrapper. On branch `agent/lift-fast`, not in this
tree. Paused: the whole-game build crashes 1.7 s into boot with a stale stack pointer (the context's SP all ones on
entry to `f_80bb0f60`), and the executable doubles (612 MB against 301 MB) because LLVM inlines F_ bodies into F_ and
into the wrappers. C takes the pair deletion and the kept SSA values without the new prototype.

## Done

- Polls: none emitted (338ce2c0); every cycle carries a zero-byte compiler barrier (lifter.md, "Loops").
- Page marks: none emitted; a guest store is one plain store.
- Virtual cores: removed; every guest thread runs freely, the per-core rings asserted single-writer.
- C++ throws: a fatal stop (`host/cxx_throw.c`).
- E, jump tables as index switches: 1,197 of 1,475 tables switch on the index, cases `0..n-1` (lifter.md, "Jump tables").

## Ruled out, with the count

- LDXR/STXR loop recognition: 3 LDXR/LDAXR and 2 STXR in the whole game, against 10,721 CAS and 25,802 LDADD-family
  LSE operations.
- Constant-folding BLR targets from read-only memory: 0 of 182,791 BLR sites load their target from read-only memory.
- LLVM's own indirect-call promotion: refuses targets defined in another partition, which is most of them (D).
