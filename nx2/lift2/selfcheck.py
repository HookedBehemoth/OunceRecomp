"""The decoder against llvm-mc's disassembler, word by word.

    python -I nx2/lift2/selfcheck.py [--game] [--synthetic N] [--show 20]

--game takes every distinct word of the game's code (the dump, through nx2's NSO loader); --synthetic N makes N
random words per encoding entry (its literal bits, random fields) and as many fully random words. For each word:
- both decode: the decoder's text (asm.py) must equal llvm-mc's;
- the decoder decodes a word llvm-mc calls invalid: an error (an unallocated encoding taken);
- llvm-mc decodes a word in the decoder's scope (a mnemonic the decoder can print, integer registers only) that the
  decoder does not: an error (an encoding missed).
The game check needs the dump and stays a local tool; tests/test_lift2.py runs the synthetic one.
"""
import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lift2 import enc, asm, llvm  # noqa: E402

NOT_INT = re.compile(r"\b([vqdshb]\d+|z\d+|p\d+|za)\b|\{")


def synthetic_words(n, rng):
    words = []
    for e in enc.ENTRIES:
        for _ in range(n):
            words.append((rng.getrandbits(32) & ~e.mask) | e.value)
    words += [rng.getrandbits(32) for _ in range(n * len(enc.ENTRIES))]
    return words


def game_words():
    import kit
    import nso2  # noqa: F401  (DT_RELR)
    import nso
    import struct
    words = set()
    for m in nso.load_exefs(kit.need_exefs()):
        lo, hi = m.code_range()
        words.update(struct.unpack_from("<%dI" % ((hi - lo) // 4), m.image, lo))
    return sorted(words)


def check(words, show=20, out=sys.stdout):
    """(errors, summary dict)"""
    t0 = time.time()
    theirs = []
    for k in range(0, len(words), 200000):
        theirs += llvm.mc_disassemble(words[k:k + 200000])
    t_mc = time.time() - t0
    ours = [enc.decode(w) for w in words]
    scope = set()
    for o in ours:
        if o is not None:
            scope.add(asm.render(o).split()[0])
    errs = {"text": [], "wide": [], "missed": []}
    decoded = 0
    for w, o, t in zip(words, ours, theirs):
        if o is not None:
            decoded += 1
            s = asm.render(o)
            if t is None:
                errs["wide"].append((w, s, None))
            elif s != t:
                errs["text"].append((w, s, t))
        elif t is not None and t.split()[0] in scope and not NOT_INT.search(t.partition(" ")[2]):
            errs["missed"].append((w, None, t))
    n_err = sum(len(v) for v in errs.values())
    for k, v in errs.items():
        if v:
            print("%s: %d" % (k, len(v)), file=out)
            for w, s, t in v[:show]:
                print("  %08x %-13s ours: %-38s llvm: %s" % (w, (enc.decode(w).name if enc.decode(w) else "-"), s, t), file=out)
    summary = {"words": len(words), "decoded": decoded, "errors": n_err, "llvm_mc_s": t_mc}
    return n_err, summary


def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--game", action="store_true")
    p.add_argument("--synthetic", type=int, default=0)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--show", type=int, default=20)
    a = p.parse_args()
    words = []
    if a.game:
        words += game_words()
    if a.synthetic:
        words += synthetic_words(a.synthetic, random.Random(a.seed))
    if not words:
        p.error("--game and/or --synthetic N")
    n, s = check(sorted(set(words)), a.show)
    print("%(words)d words, %(decoded)d decoded, %(errors)d differences (llvm-mc %(llvm_mc_s).0f s)" % s)
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
