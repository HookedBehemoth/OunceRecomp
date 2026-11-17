"""Every GLSL.std.450 Fma of a SPIR-V module decorated NoContraction (in place).

    python3 tools/shaders/spv_exact_fma.py <module.spv>

The translator decorates each of its fused multiply-adds NoContraction, and Mesa keeps such an Fma fused; glslang leaves a
`precise` fma() undecorated, and Mesa then splits it into a multiply and an add (vtn: an Fma that is not exact). A replacement
stage that must give the translator's results bit for bit (gpu/direct/shaders/frag) is built through this step
(tools/shaders/build_shaders.sh). Decorations already present are kept.
"""
import struct
import sys


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    w = list(struct.unpack("<%dI" % (len(data) // 4), data))
    glsl = set()
    fma = []
    decorated = set()
    first_decorate = None
    i = 5
    while i < len(w):
        wc, op = w[i] >> 16, w[i] & 0xFFFF
        if op == 11:   # OpExtInstImport
            name = struct.pack("<%dI" % (wc - 2), *w[i + 2:i + wc]).split(b"\0")[0]
            if name == b"GLSL.std.450":
                glsl.add(w[i + 1])
        elif op == 71:   # OpDecorate
            if first_decorate is None:
                first_decorate = i
            if w[i + 2] == 42:   # NoContraction
                decorated.add(w[i + 1])
        elif op == 12 and w[i + 3] in glsl and w[i + 4] == 50:   # OpExtInst ... Fma
            fma.append(w[i + 2])
        i += wc
    add = [x for x in fma if x not in decorated]
    if not add:
        return 0
    if first_decorate is None:
        raise SystemExit("%s: no annotation section" % path)
    ins = []
    for x in add:
        ins += [(3 << 16) | 71, x, 42]
    w = w[:first_decorate] + ins + w[first_decorate:]
    open(path, "wb").write(struct.pack("<%dI" % len(w), *w))
    print("%s: %d of %d Fma decorated NoContraction" % (path, len(add), len(fma)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
