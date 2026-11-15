"""Which instruction forms the game's code uses, and which of them the lifter's decoder covers.

    python -I nx2/lift2/census.py [--out work/lift2/forms.txt]

A form is llvm-mc's mnemonic plus the operand shape: register classes (X, W, SP, V.<arrangement>, Q/D/S/H/B),
immediates as #i, addressing modes kept. Every word of each module's .text is counted (the little data in .text
included). The classes are klass()'s. Writes one line per form (count, class, covered, form, an example) and prints the coverage per class.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lift2 import enc, llvm  # noqa: E402

INT_CLASSES = ("int", "ldst-int", "branch", "system")


def shape(t):
    mn, _, ops = t.partition(" ")
    if mn.startswith("b."):
        mn = "b.cc"
    s = ops
    s = re.sub(r"\bv\d+\.(\d*[bhsd])\[\d+\]", r"V.\1[i]", s)
    s = re.sub(r"\bv\d+\.(\d*[bhsd])\b", r"V.\1", s)
    s = re.sub(r"\{[^}]*\}", lambda m: "{%dregs:%s}" % (m.group(0).count(",") + 1,
                                                         (re.findall(r"\.\d*[bhsd]", m.group(0)) or [""])[0]), s)
    s = re.sub(r"\b(x\d+|xzr|fp|lr)\b", "X", s)
    s = re.sub(r"\bsp\b", "SP", s)
    s = re.sub(r"\b(w\d+|wzr|wsp)\b", "W", s)
    s = re.sub(r"\b([qdshb])\d+\b", lambda m: m.group(1).upper(), s)
    s = re.sub(r"#-?0x[0-9a-f]+|#-?\d+(\.\d+)?(e[+-]?\d+)?", "#i", s)
    s = re.sub(r"\b(eq|ne|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al|nv)\b", "cc", s)
    return (mn + " " + s).strip()


def klass(form):
    mn, _, ops = form.partition(" ")
    if mn in ("udf", "hint", "nop"):
        return "system"
    if re.match(r"(b|bl|br|blr|ret|reta[ab]|cbn?z|tbn?z|b\.cc|bra|blra)$", mn) or mn.startswith("b."):
        return "branch"
    if mn in ("svc", "mrs", "msr", "dmb", "dsb", "isb", "clrex", "brk", "hlt", "sys", "dc", "ic", "yield", "wfe",
              "wfi", "sev", "sevl", "prfm", "prfum") or re.match(r"(pac|aut|xpac|bti)", mn):
        return "system" if not mn.startswith("prf") else "ldst-int"
    vec = re.search(r"\bV\.|\{|\b[QDSHB]\b", ops)
    if re.match(r"(ld|st|cas|swp|prf)", mn):
        return "ldst-vec" if vec else "ldst-int"
    if "V." in ops or "{" in ops:
        return "neon"
    if vec or mn.startswith("f") or mn in ("scvtf", "ucvtf"):
        return "fp-scalar"
    return "int"


def game_counts():
    import kit
    import nso2  # noqa: F401  (DT_RELR)
    import nso
    import struct
    c = collections.Counter()
    for m in nso.load_exefs(kit.need_exefs()):
        lo, hi = m.code_range()
        c.update(struct.unpack_from("<%dI" % ((hi - lo) // 4), m.image, lo))
    return c


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=None)
    a = p.parse_args()
    import kit
    out = a.out or os.path.join(kit.WORK, "lift2", "forms.txt")
    counts = game_counts()
    words = sorted(counts)
    texts = []
    for k in range(0, len(words), 200000):
        texts += llvm.mc_disassemble(words[k:k + 200000])
    forms = collections.Counter()
    covered = collections.Counter()
    example = {}
    for w, t in zip(words, texts):
        if t is None:
            continue
        f = shape(t)
        forms[f] += counts[w]
        example.setdefault(f, "%08x %s" % (w, t))
        if enc.decode(w) is not None:
            covered[f] += counts[w]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    per = collections.defaultdict(lambda: [0, 0, 0, 0])
    with open(out, "w") as fo:
        for f, n in forms.most_common():
            k = klass(f)
            full = covered[f] == n
            per[k][0] += 1
            per[k][1] += full
            per[k][2] += n
            per[k][3] += covered[f]
            fo.write("%10d  %-9s %-4s %-50s %s\n" % (n, k, "yes" if full else ("part" if covered[f] else "no"), f,
                                                    example[f]))
    print("%-10s %7s %8s %12s %8s" % ("class", "forms", "covered", "words", "covered"))
    tf = tc = 0
    for k in ("int", "ldst-int", "branch", "system", "ldst-vec", "fp-scalar", "neon"):
        nf, cf, nw, cw = per[k]
        print("%-10s %7d %8d %12d %7.2f%%" % (k, nf, cf, nw, 100.0 * cw / max(nw, 1)))
        if k in INT_CLASSES:
            tf += nf
            tc += cf
    print("integer classes: %d of %d forms fully covered; list in %s" % (tc, tf, out))


if __name__ == "__main__":
    main()
