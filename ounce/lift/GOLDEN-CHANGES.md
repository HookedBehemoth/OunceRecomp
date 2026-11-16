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
