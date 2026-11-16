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
