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
