# Open lifter work

This page is the queue of planned improvements to the lifter's output. Each item says what changes, the source the
idea comes from, when the change is sound, and the first step that one on/off run of `scripts/bench.sh market 600` can
measure. Read it before you start lifter work, so you pick up an item rather than repeat a ruled-out one. The current
costs these items address are in [Pessimizations and constraints](lifter-costs.md).

Ideas from other translators are cited; no code is copied. "Not read" means the source's code was not opened.

Every item must stay correct for these cases:

- **setjmp callers.** A `longjmp` re-enters the caller mid-function through the host. Calls of setjmp callers keep
  reloading every register.
- **Host functions.** Replaced functions and SDK replacements are counted by their declarations' effects.
- **Frame readers.** The host sites that read a lifted function's frame (see the constraints table).
- **Indirect entry.** Any function can still be entered through the function table.

## Call boundaries

1. **Callee-saved registers a callee only saves.** Skip the stores before a BL of `x19`-`x29` and `x30` that the
   callee reads only to save and restore them. Needs summaries whose inputs leave out save-only registers, and the
   frame readers checked, since a save slot would then hold an older value. The same applies to flags at exits:
   skip the store when the target does not read flags at entry.
2. **Pair deletion.** Delete the save and restore pairs of full-summary functions outside the frame readers'
   functions. Throws are fatal, so no unwinder reads the saves. The guest stack then no longer holds what a debugger,
   the stall dump or a `longjmp` buffer expects. Needs a verification run.
3. **The fiber functions read every register** because they declare `clobbers(...)`. That widens the inputs of their
   callers and demotes some. A declaration of what the fiber switch really reads removes it.
4. **Path-sensitive liveness** in the summaries would remove most demotions for temporary registers live at entry or
   after a call; the examples read are on paths that correlated branches make infeasible.
5. **Typed prototypes for host-implemented functions.** Every call of an `HLE` or `NV` function counts as reading
   `x0`-`x8`; a typed prototype, as `PATCH_FUNC` has, would say what each reads.
6. **A record for NVN procs.** A row with `guest(computed)` counts as reading every register because a proc's facts are
   unknown; a record per NVN proc would remove that.
7. **Register-passing prototypes.** Passing arguments and results in registers instead of the context is the step
   expected to change speed. An earlier attempt is paused (below).

Sources:

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT): non-volatile registers as C locals, never loaded from
  the context. It assumes the game follows the ABI and does not support exceptions; its `setjmp` does not restore those
  locals. Lesson: the gain is real, and setjmp is where it breaks.
- [rev.ng](https://docs.rev.ng/references/pipeline/) (`enforce-abi`, `promote-csvs`; documentation only): every
  register promoted once the full prototype is known.

Soundness: a wrong summary breaks these changes. The verification build (parked: [parked patches](parked-patches.md#the-summary-verification-build)) checked preserved registers and noreturn
functions at every exit of a full-summary function; it has to come back before these land.

## Indirect calls

**Profile-guided promotion.** The runtime's site profiler (`A64_SITE_PROFILE`) writes each BLR site's targets and
counts. A script turns that into a site-to-dominant-target map, and the lifter emits `target == K ? call f_K : call
target`. The direct path then gets the call-boundary work and inlining.

Sources:

- [Dynarmic](https://github.com/lioncash/dynarmic) (0BSD): a fast dispatch table and a return stack buffer.
- [Rosetta 2](https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/) (no code): lookup tables for indirect
  branches.
- LLVM's indirect-call promotion stops at targets defined in another module and needs type metadata. Most targets live
  in other partitions, so comparing guest addresses at lift time is the way here.

Needs: a profile per scene, in the partitions' cache key. First step: the hot partitions' sites with one profile from
the market benchmark.

## Frame-slot promotion

Guest frame slots become `alloca`s, in two subsets: leaf functions whose frame address is never taken, then non-leaf
slots that are not saves and are dead at every call. Excluded: setjmp callers, frame readers' functions.

Source: [WYTIWYG/BinRec](https://escholarship.org/content/qt8d3363v4/qt8d3363v4.pdf) (code not read): replaces the
emulated stack with native stack allocations. Modern optimized code gained little there, and the gain is not
guaranteed. First step: the leaf subset, on/off.

## Short items

- **libc copies as intrinsics.** A BL to the replaced `memcpy`, `memset` or `memmove` becomes `llvm.memmove` or
  `llvm.memset` on guest addresses, with an inline range check and the replacement's call as the fallback
  (the check `nvn::guest_copies` guards host memory safety). Source: FEX lists compile-time memcpy and memset among its
  optimizations (code not read).
- **CRC32C as `crc32`.** Exact. First step: count the sites.
- **CCMP chains.** Fold the four selects a CCMP builds into one compare and select. First step: read whether LLVM
  already recovers it.
- **NEON gaps.** The saturating shifts by register on 8-bit and 16-bit lanes need widening; SHA-256 needs a
  per-function `+sha` feature with a CPUID check. First step: whether the sites are hot.
- **STLR as a release store**: see the constraints table for its experiment.
- **Remove the old code-pointer resolver path** once a long play session logs zero guest-address calls (see the
  constraints table).
- **The null-call skip in the runtime's call-miss handler** (`a64_call_miss`, `runtime/hos_hostcall.c`) exists for a game
  site nobody has identified. Find the site and patch it there, or remove the skip.
- **The host reads the caller's frame chain** in `pool_creation_key` (`runtime/nvn_native.c`), and the render-target slot
  family key hashes the caller's return address (`games/oot/patches/rt_slots.c`). Both tie the host to the guest's frames and return
  addresses; a row at the calling site would say it explicitly.
- **Split `runtime/`** into the OS layer, the renderer's host side and a per-game folder.
- **Build.** `-fsplit-machine-functions` with a profile, hot and cold wrappers, and 2 MB text pages for the lifted code.
  None measured.

A per-function register prototype ("fast entries") is paused: the whole-game build crashed at boot and the executable
doubled in size because LLVM inlined across the new entries.

## Ruled out

- **Recognizing LDXR and STXR loops.** The game has almost no exclusives against tens of thousands of LSE atomics.
- **Folding BLR targets loaded from read-only memory.** No BLR site loads its target from read-only memory.
- **LLVM's own indirect-call promotion.** It refuses targets defined in another partition, which is most of them.
