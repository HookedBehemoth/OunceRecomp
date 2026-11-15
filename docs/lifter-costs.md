# Pessimizations and constraints

This page lists where the lifted code is knowingly slower than it could be, and the rules the lifter keeps for
correctness. Read it before you change what the lifter emits, so that you know which costs are deliberate and which
experiment would decide whether a rule can go. The queue of planned changes is [Open lifter work](lifter-open-work.md).

Each pessimization says what is emitted, what it costs, and what would remove it. Each constraint says why it is kept,
or says *unproven* and names the experiment that decides it.

## Pessimizations

1. **Context traffic.** Registers are stored to the guest context before calls and exits, and loaded again after
   every BLR and SVC and after calls to callees without a full summary. Stores no reader reads are already omitted.
   What remains are stores kept only for an indirect call, an SVC, an unknown callee or an exit, and callee-saved
   registers that the callee only saves and restores. Context accesses were about a third of the code bytes before
   the latest omissions; not measured since. Removed by: the call-boundary work in
   [Open lifter work](lifter-open-work.md#call-boundaries).
2. **Callee-saved saves and guest frames in guest memory.** Every function keeps its save and restore pairs and its
   frame in guest memory (about 600,000 pairs). Not measured alone. Removed by: pair deletion and frame-slot
   promotion.
3. **Host-stack spills.** About 6 % of code bytes in a sample of functions. The cause is not analyzed. Next step: read
   the spills of the largest functions with `nx2/lift2/dump.py`.
4. **Sparse jump-table switches.** About a fifth of the jump tables cannot be proven and switch on the target's
   offset, which LLVM lowers to compare chains. Kept because an index switch would rest on an unproven fact.
5. **STLR as `xchg`.** Every STLR is a sequentially consistent store, although a plain x86 store is already a release
   store. Few sites are hot.
6. **A generic entry for every function, and a guest-address path behind every BLR.** The function table serves every
   function, although about a third are reached by no known origin of a code pointer.
7. **Opaque indirect calls.** About one call edge in five is a BLR or BR, mostly through vtables. Each calls a host
   pointer through a register with no promotion, so LLVM cannot inline or keep registers across it. Removed by
   profile-guided promotion.
8. **The cycle barrier.** A guest load in a loop is not hoisted or merged across iterations. It emits no instruction;
   the speed cost is not measured.
9. **Flags stored at exits.** A function that writes flags stores all four before every tail call and indirect jump.
   With summaries, the store could be limited to targets that read flags at entry.
10. **Saturating shifts by register, lane by lane** (a few dozen sites). AVX2 has variable shifts for 32-bit and
    64-bit lanes only.
11. **SHA-256 in scalar code** (a few hundred sites). The `x86-64-v3` target has no SHA-NI; a per-function target
    feature with a CPUID check is possible, as for AES.
12. **CRC32 as a bit loop.** The CRC32C variants equal SSE4.2's `crc32` instruction.
13. **`-O1` for cold partitions.** `-O2` is about 3 % smaller and takes over twice the compile time, for code no
    profile shows hot.

## Constraints the lifter keeps

| Constraint | Why | Status and deciding experiment |
|---|---|---|
| Stores before calls to code that reads the context | The callee's inputs, `a64_svc`, the exclusive monitor, the indirect-call path, setjmp stubs, unknown callees and replaced functions read registers from the context | Needed at those sites; elsewhere unproven |
| Frame pointer chains and guest frames | The host hashes a pool's creation call chain through `x29` (`NV(nvnMemoryPoolInitialize, reads(x29, ...))`); guest calls from host code run below the SP they are given | Needed in those functions; for every other function unproven. Experiment: delete the pairs of full-summary functions, then a verification run |
| The cycle barrier | The SDK has count-bounded spin-waits on a plain byte load; a hoisted load would wait out the count instead of seeing the flag | Evidence; keep |
| STLR stays sequentially consistent | An STLR followed by an LDAR of another address keeps ARM's order only with `xchg` | Unproven: no store-then-load-acquire pattern was looked for. Experiment: emit a release store behind a lift-time switch, then the smoke test and the market benchmark |
| The guest-address path behind every BLR, and table entries no origin names | A code pointer from an origin the translation missed would otherwise crash | Evidence: every relocated code slot lands on an entry, every code-formed address is of a known kind, and runs log 0 guest-address calls. Experiment: the `code pointers:` log line over a long play session; at 0, the path and the unreached entries go |
| Unprovable jump tables switch on the offset | An index switch needs the table's facts | Keep until discovery proves more tables |
| C++ throws stop the game | Lifted frames cannot be unwound | Keep; normal play throws nothing |
| `-O1` for cold partitions | Build time | Keep unless a profile shows cold code hot |
| `-ffp-contract=off` and native FP | Exact results; contraction costs nothing without `contract` flags | Keep |

## Functions outside the AAPCS64

Keeping registers across calls trusts the stack and callee-saved discipline. The census
(`nx2/lift2/abi_census.py`, output in `work/lift2/abi_census.txt`) found these exceptions in the game, and each is
handled:

| Functions | What they do | Treatment |
|---|---|---|
| `__setjmp`, `longjmp` | Save and restore registers through a buffer | Callers of `__setjmp` are called through a host setjmp stub; a `longjmp` returns through the host |
| The SDK's fiber switch | Switches stacks | Never reached: the public fiber API is replaced by the host |
| libunwind's context save and resume, `__cxa_throw` and relatives | Exception unwinding | Never reached: a throw stops the game |
| Noreturn callees (abort, assert, terminate, and others) | The compiler leaves nothing after the call | Proven noreturn by the summaries, seeded with the platform's fatal function names |
| Functions discovery splits or merges so a frame spans two entries | A tail call with the frame still allocated | Demoted by the summaries |
| Hand-written NEON saving `d8`-`d15` with ST1 and LD1 | Ordinary saves in an unusual form | Accepted by the summaries' frame pass |
| The SDK's user exception handler and lazy-binding trampolines | Read a CPU exception context, or bind imports lazily | Never entered: there are no guest CPU exceptions, and every import is bound when the image is built |

Nothing else in the game manipulates SP, `x30` or callee-saved registers outside the AAPCS64. The remaining census hits
are benign (`x30` as a scratch register, `alloca`, TLS, flag save and restore inside one function) or come from the
census tool's path-insensitive analysis. The summaries detect every one of these classes by rule, so a new exception
is demoted rather than miscompiled.
