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
