# Golden v2: the accepted changes of the lift's output

The Rust lifter started byte for byte equal to the Python lifter's output (the golden, kept untouched for
reference). Fixing the flaws it copied from the Python changes the output; each change is accepted here, per commit,
with what changed and why, and the output after it becomes the reference (golden v2, under `work/`, never in git).

    ounce/lift/target/release/ounce-lift <out>                                   (writes facts.tsv beside the IR)
    work/venv/bin/python -I ounce/lift/tools/diffout.py <golden v2> <out>        (each changed function and its reason)
    work/venv/bin/python -I ounce/lift/tools/diffout.py --store <out> <golden v2>  (accept)

A changed function's reason is a change of its own facts (facts.tsv: body, summary, call record) or of the facts of
a function its IR names; an unexplained change fails the gate.

## v2.0: the Python golden

The output equal to the Python lifter's golden (148/148 partitions, gen/, rt64/, image.bin), plus facts.tsv.

## v2.1: a tail exit to several functions is a tail call of each (flaw 4)

A conditional branch or a jump table whose targets are all other functions' entries was summarised as one indirect
tail (the AAPCS facts: ARGS read, callee-saved kept), whatever its targets are; it is now a tail call of each target
(their facts joined: inputs and clobbers ORed, mustdef ANDed, a frame exit per target, a graph edge and the results
passed on to each). 33 such exits in 30 functions (e.g. f_824c21f4, a 3-instruction dispatch through a 19-entry table
of other functions, one of them of unknown facts). Their old summaries were unsound: callers kept x19-x29 in SSA
across a call that may clobber them.

diffout: 487 functions' facts changed (316 Lifted -> Demoted, 0 the other way; inputs 339, clobbers 319, preserved
319, results 434, mustdef 39), 396 functions' IR changed, 0 unexplained, gen/ and image.bin equal.

## v2.2: the summaries read the bodies the lift lifts (flaws 1, 3)

The summaries read the discovered body of a function the lift traps (Plan::raw: the four module headers f_80020000,
f_80020008, f_84ec0000, f_84fa0000, lifted as UDF #0); they read the trap now. With that the summaries' bodies are
the ones patch::resolve checks a row's pc against, so a row's effects apply in its function only (flaw 3).

diffout: 4 functions' facts changed (body, insns: the four headers), their summaries and records unchanged; 0 IR
changed.

## v2.2 kept: discovery in rounds to the tables' fixed point (flaws 10, 12)

The Python walk resolved each BR when its walk met it, against predecessor maps cached per extent with the edges
known then (a table found later added its edges only to a cached extent, twice when met twice; the second pass over
the misses added none; the last walk of a shared BR won; the redo set's order decided). Now each round walks every
function against the previous round's tables and resolves every BR its bodies hold against them, until no table
changes (4 rounds here). For this game the result is the same: diffout 0 changed, 0 facts changed.

## v2.3: the frame pass in phases, each independent of its visiting order (flaw 5)

The frame pass collected its exits, saves, restores and reasons from every state it computed on the way, so a state
that was not final yet (one path's, larger than the join of all) added exits and reasons the final one does not have,
and which states those were depended on the worklist's order. Now the must states reach their fixed point first; then
the may states (the same transfer, joined by union) over those; then one pass over the final states collects. A store
of a callee-saved register's entry value through another pointer (save_ptr) and LR used as data (lr_data) are asked
of the may states (on some path), the rest of the must states. A save_ptr store is a whole one (8 bytes or more): a
narrower store keeps no value to restore. A FIFO worklist gives the same facts.

diffout: 1733 functions' facts changed (110 Lifted -> Demoted, 60 the other way; demote 193, results 1634, inputs 167,
preserved 172, clobbers 122, mustdef 60), 668 functions' IR changed, 0 unexplained, gen/ and image.bin equal. The new
save_ptr are stores on a path where the register still holds its entry value (e.g. f_829537f0 spills x21 through x29
on a path that skips x21's load, where x29 is not known to be a frame register on every path).

## v2.3 kept: a table on its index is the BR's targets in the summaries too (flaw 2)

The lift switches on a table's index (its entries, the rest to the BR's indirect exit) where the load is in the body
with no probe or row up to the BR; the summaries read the BR's whole resolved table. plan::parts now keeps only the
index tables the lift switches on, and the summaries take their entries as the BR's targets; the lift stops if such a
table's load is in another block than its BR. For this game every index table's entries are its whole table:
diffout 0 changed, 0 facts changed.

## v2.3 kept: a fall-through after a call is known before the fixed point (flaw 6)

An entry a call's return falls into was demoted after the fixed point, once the call was known to return, so its
summary during the fixed point (what its callers read) lacked the reason. Which calls those are is known before: a
direct call with nothing after it makes its callee a noreturn seed (the callers' evidence), so only an SVC or an
indirect call returns into the next entry. The call graph's pass marks those entries with the others that are fallen
into. Nothing reads a callee's demotion during the fixed point today: diffout 0 changed, 0 facts changed.

## v2.4: an HLE'd function writes no register on every path (flaw 7)

An HLE'd function's summary said it writes x30 on every path (host_summary's mustdef, the AAPCS one), its record
said none (must 0). The host's code is not known to write x30: the summary says none too. A function that ends in a
tail call of an HLE'd function no longer counts x30 as written on every path.

diffout: 530 functions' facts changed (mustdef: x30 out; 529 records' must), 0 functions' IR changed, gen/ and
image.bin equal.

## v2.5: functions.txt gives the lifted bodies' sizes (flaw 9)

The plan's instruction counts (gen/functions.txt) were the discovered bodies' sizes, while the partitions, the
summaries and the lift count the lifted ones. They are the lifted sizes now: the four trapped module headers count
one instruction (their trap); no body is cut to its reachable part in this game.

diffout: 0 functions changed, 0 facts changed; gen/functions.txt 4 lines (the headers).

## v2.5 kept: symbols at one address, and C names (flaws 14, 17)

Two HLE'd or stubbed symbols at one address gave the hook to the later in sorted order, silently; and two symbols of
one C identifier overwrote each other's symbol. Now an implemented symbol takes its address before a stubbed one
(the first stubbed of several), two implemented ones stop the lift, and so do two symbols of one C name. None occurs
in this game. The NSO loader's init-array list, read from the unrelocated bytes and read by nothing, is gone (the
image's init arrays are read relocated). diffout 0 changed.

## v2.5 kept: a64_stubs.c without the toolchain, and by symbol (flaws 16, 17)

The stubs' demangled names came from whichever llvm-cxxfilt was found first ($EMSDK, else PATH), so the generated
file could change with the toolchain; they come from cpp_demangle now, its version fixed by Cargo.lock. And whether
the host defines a hook was asked by mixing two spellings (imp_<symbol> against the hook's C name, the symbol made
an identifier), so a symbol whose C name differs was stubbed even when implemented; it is asked by symbol now.
diffout 0 changed: the 155 stubs' text equal to llvm-cxxfilt's.

## v2.5 kept: one build id (flaw 18)

hos_image.c's main build id was the header's 32-byte field less its trailing zero bytes, the identity check's the
field's first 20 bytes: equal unless the id ends in a zero byte. Both are nso::build_id now, the first 20 bytes.
diffout 0 changed.

## v2.5 kept: the scan reads every host file, in ASCII (flaws 20, 21, 23)

A host file without a declaration keyword was skipped before it was tokenised, so a malformed one went unreported;
every file is read now (the scan and the records check). Identifiers and numbers are ASCII, as C's (Python's tests
took any Unicode letter or digit, the Rust number test did not); the C names of hooks too. A # after a block comment
spanning lines is a directive exactly when the comment began its line: that is C's rule (a comment is one space) and
cpp's, kept and now tested. diffout 0 changed; all 62 host files tokenise.

## v2.5 kept: the unwind function starts from the image (flaw 8)

The unwind data's function starts were read from each module's unrelocated bytes, while discovery reads the
relocated image; they are read from the image now (eden::eh_starts over the modules' .eh_frame_hdr). Discovery's
rounds re-resolve only the BRs of the function extents whose tables changed (a resolution reads its own extent's
tables only). diffout 0 changed.

## v2.6: a save of a callee-saved register is no read of it

A function's inputs counted the prologue's store of x19-x29's entry values as a read, so every caller wrote those
registers to the context before the call, though the callee only saves and restores them. A save of x19-x29's entry
value into a frame slot the register is restored from, the slot read by nothing else, the register preserved and no
save_ptr, is now no use of the register: liveness runs without those uses, and the inputs (and the records' reads)
lose the registers that only such saves read, up the call graph. The callee then saves and restores a stale context
value no one reads (C++ unwinding stops, host/cxx_throw.c; backtraces are replaced, host/nn_diag.c); a caller keeps
its value in SSA (the register is in the record's keeps). The frame address being taken does not matter: only an
out-of-bounds read reaches a save slot. inconsistent asks the inputs with the saves counted: the summary's saved, the
registers it or a callee reads only to save.

diffout: 204412 functions' facts changed (inputs 204052, records 197920, results 435; 21 undemoted: inconsistent
found by the saves of callees it missed), 123075 functions' IR changed (the stores before calls gone), 0
unexplained, gen/ and image.bin equal. f_800200a0's stores of x19 and x29 before memset are gone.

## v2.7: the analyses start at the entry, not at the body's lowest address

A body is sorted by address, and 26 functions have code below their entry (shared returns, table cases): the
liveness took the inputs at the body's first instruction, and the must-definitions and the frame pass started
there, so their summaries described the code below the entry (f_824f4454: inputs none, from a `mov w0, #4; ret` at
0x824d793c). The Python lifter did the same. They start at the entry's instruction now.

diffout: 58 functions' facts changed (the 26 and their callers: inputs 34, mustdef 24, preserved 8, results 33,
unknown 7; 3 newly demoted, by sp, lr_ret or cs_clobber their own code shows), 10 functions' IR changed, 0
unexplained, gen/ and image.bin equal.

## v2.8: LR at a tail call

The frame pass checked that x30 holds its entry value at a `ret`, not at a tail call (a direct or indirect tail
call, or a Callee::Tails exit) whose callee returns: the callee's lifted `ret` returns to our caller, so a call made
with B (`adr x30, 1f; b g`) or a tail call after a missing restore went wrong silently. It is lr_ret now.

diffout: 117 functions' facts changed (demote only: lr_ret added; all were demoted already, by sp, cs_clobber,
split_frame or an unknown callee), 0 functions' IR changed, 0 unexplained, gen/ and image.bin equal.

## v2.9: v2.6's saves made the game hang; the stores they dropped, back

v2.6 (a save of x19-x29 is no read) hung the game in the sdk's static initializers (std::call_once waiting in
pthread_cond_wait on a flag left "running"); the game was not run after it. Bisected by commit, then by function at
-O0 (the rule applied to a range of entries): pthread_mutex_lock, f_85111ce0, which saves x19, x20 and x29 only to
restore them. Two holes:

- omit_stores took a call's record's must as a kill of the registers: a store before the call was dead when the callee
  writes the register on every path. A callee that preserves a register writes it back as it found it in the
  context: with the store gone, the stale value. The caller holds the right value in SSA, but writes it no more, and a
  later reader of the context (a callee that does read it) got the stale one. A preserved register (the record's
  keeps) is no kill now: the call is transparent for it.
- a register read after its restore (by a tail call after the epilogue, or anything before the return) is the entry
  value read through the slot: such a save is a read again.

diffout: 17,465 functions' IR changed, 0 unexplained: 17,089 by context stores only (stores of x19-x29 before calls
added back, ~64 k), the others by their own or callees' facts (243 functions' inputs gain callee-saved registers, 5
results; nothing else). In the game: a build with v2.6 to v2.8 never reaches nnMain; with this, 600 presents in 25 s
headless in Kokiri, as the build without v2.6.

tools/diffout.py: a change by a hook (gen/a64_hooks.c's lines naming it), a function only renumbered, and context stores
only, each counted apart from the unexplained ones.

## v2.10: SVCs called with values, without the context; the undeclared ones stops

`svc #n` was a context call of `a64_svc(ctx, n)`. Now host/hos_svc.c declares each SVC it answers,
SVC(number, name, inputs, results) (host/patch.h): the lift calls hos_svc_0x<NN>(x0..x7, pc, more) -> i128, no
context: x0 up to inputs as values (poison beyond), x0 and x1 from the result, x2 up through `more` (a local array
holding the registers' values before; GetThreadCoreMask only); every other register keeps its value, nothing is
written to or reloaded from the context. The summaries take a declared svc as reading x0..inputs-1 and writing
x0..results-1 (they took x0-x7 both). An undeclared number is a stop, hos_svc_stop(number, pc) (noreturn, reads no
register), and a trap to the summaries. The counts: switchbrew's tables and Atmosphere's svc definitions; the inputs
checked against the compiled host (tests/test_host_context_reads.py test_svc_inputs).

diffout: 261 functions' IR changed, 0 unexplained (the svc wrappers and the callers whose callees' facts changed);
3,425 functions' facts changed: mustdef shrinks in 3,126 (a declared svc writes its results, not x0-x7), 138 no longer
return (the 31 sites of 27 undeclared numbers: the wrappers and 105 functions that reach one on every path; the game
stopped at those before too, in a64_svc's default), their mustdef ALL and inputs gaining x19-x29 (a save of a function
that does not return is a read), 55 lose `boundary` (the svc was it). 33 sites call 27 declared numbers. image.bin
equal. In the game: smoke test in Kokiri passes (pictures within 2 of 64 bits of the golden runs, 1,800 presents).

## v2.13: the generated code is IR; the guest model leaves the lift

gen/ holds IR instead of C: a64_table, a64_hooks, a64_code_slots, a64_stubs, hos_image and nvn2_exports `.ll`, the
build compiling them as it compiles lift_answers. The lift no longer writes rt64/: `ounce-lift rt64` does, before
the lift, as it reads no dump. Each object equals clang's of the C it replaces in its allocated sections, symbols but
the file symbol, and relocations (`hookshot-builder objeq`, .text and .eh_frame included); the executable built so
equals the one before in every loaded byte but the build ID, with the version stamp held equal. An answered hook is a
comment in a64_hooks.ll, `; f_<address> answers <symbol>: lifted IR`.

diffout: 0 functions changed, 0 facts changed, 0 partition headers; image.bin equal; every gen/ file replaced.
