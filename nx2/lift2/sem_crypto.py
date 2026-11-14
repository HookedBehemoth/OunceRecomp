"""The cryptographic extension's semantics (enc_crypto.py's table).

AES: ARM and x86 keep the AES state in the same byte order, so each ARM step is AES-NI: AESE = SubBytes(ShiftRows(
d ^ k)) is aesenclast(d ^ k, 0), AESD aesdeclast(d ^ k, 0), AESIMC aesimc, and AESMC (MixColumns alone, which x86 has no
instruction for) aesenc(aesdeclast(x, 0), 0): the inverse steps of aesdeclast undo the forward ones of aesenc but its
MixColumns. A function using them is compiled with the AES-NI target feature (every x86-64-v3 machine Hookshot runs on
has it; the build's -march does not imply it).
SHA1/SHA256: the Arm ARM's pseudocode, lane by lane on 32-bit values (the SHA-NI instructions split the rounds
differently).
"""
from .sem_fp import vread, vwrite


def features(b, *fs):
    tf = getattr(b.fn, "target_features", None) or set()
    b.fn.target_features = tf | set(fs)


def x86(b, name, *args):
    return b.call("llvm.x86.aesni.%s" % name, "v2i64", *args)


def t_crypto_aes(b, f, pc, p):
    features(b, "+aes")
    op = f["op"]
    x = vread(b, f["Rd"] if op < 2 else f["Rn"], "v2i64")
    zero = b.zero("v2i64")
    if op < 2:
        x = b.bin("xor", x, vread(b, f["Rn"], "v2i64"))
        r = x86(b, "aesenclast" if op == 0 else "aesdeclast", x, zero)
    elif op == 2:
        r = x86(b, "aesenc", x86(b, "aesdeclast", x, zero), zero)
    else:
        r = x86(b, "aesimc", x)
    vwrite(b, f["Rd"], r)


# ---- SHA, on lists of i32 values (lane 0 first)
def lanes32(b, n):
    v = vread(b, n, "v4i32")
    return [b.extract(v, k) for k in range(4)]


def pack32(b, xs):
    v = b.zero("v4i32")
    for k, x in enumerate(xs):
        v = b.insert(v, x, k)
    return v


def rol(b, x, n):
    return b.intr("fshl", "i32", x, x, b.const(n, "i32"))


def ror(b, x, n):
    return b.intr("fshr", "i32", x, x, b.const(n, "i32"))


def add(b, *xs):
    r = xs[0]
    for x in xs[1:]:
        r = b.bin("add", r, x)
    return r


def xor(b, *xs):
    r = xs[0]
    for x in xs[1:]:
        r = b.bin("xor", r, x)
    return r


def choose(b, x, y, z):
    return b.bin("xor", b.bin("and", b.bin("xor", y, z), x), z)


def parity(b, x, y, z):
    return xor(b, x, y, z)


def majority(b, x, y, z):
    return b.bin("or", b.bin("and", x, y), b.bin("and", b.bin("or", x, y), z))


def sha1_rounds(b, f, fn):
    X = lanes32(b, f["Rd"])
    y = vread(b, f["Rn"], "i32")
    W = lanes32(b, f["Rm"])
    for e in range(4):
        t = fn(b, X[1], X[2], X[3])
        y = add(b, y, rol(b, X[0], 5), t, W[e])
        X[1] = rol(b, X[1], 30)
        # <Y, X> = ROL(Y : X, 32): the 160 bits shift up one lane, X's top lane becomes Y
        X, y = [y] + X[:3], X[3]
    vwrite(b, f["Rd"], pack32(b, X))


def sigma0(b, x):
    return xor(b, ror(b, x, 2), ror(b, x, 13), ror(b, x, 22))


def sigma1(b, x):
    return xor(b, ror(b, x, 6), ror(b, x, 11), ror(b, x, 25))


def sha256_hash(b, X, Y, W, part1):
    X, Y = list(X), list(Y)
    for e in range(4):
        chs = choose(b, Y[0], Y[1], Y[2])
        maj = majority(b, X[0], X[1], X[2])
        t = add(b, Y[3], sigma1(b, Y[0]), chs, W[e])
        X[3] = add(b, t, X[3])
        Y[3] = add(b, t, sigma0(b, X[0]), maj)
        # <Y, X> = ROL(Y : X, 32): 256 bits, Y above X
        X, Y = [Y[3]] + X[:3], [X[3]] + Y[:3]
    return X if part1 else Y


def t_crypto_sha3(b, f, pc, p):
    o = f["opcode"]
    if o < 3:
        sha1_rounds(b, f, (choose, parity, majority)[o])
    elif o == 3:   # SHA1SU0: (Vn's low half : Vd's high half) ^ Vd ^ Vm
        d, n, m = (vread(b, r, "v2i64") for r in (f["Rd"], f["Rn"], f["Rm"]))
        t = b.shuffle(d, n, [1, 2])
        vwrite(b, f["Rd"], b.bin("xor", b.bin("xor", t, d), m))
    elif o in (4, 5):
        d, n, W = lanes32(b, f["Rd"]), lanes32(b, f["Rn"]), lanes32(b, f["Rm"])
        r = sha256_hash(b, d, n, W, True) if o == 4 else sha256_hash(b, n, d, W, False)
        vwrite(b, f["Rd"], pack32(b, r))
    else:   # SHA256SU1
        d, n, m = lanes32(b, f["Rd"]), lanes32(b, f["Rn"]), lanes32(b, f["Rm"])
        T0 = n[1:] + m[:1]
        T1 = m[2:]
        r = []
        for e in range(4):
            elt = T1[e] if e < 2 else r[e - 2]
            elt = xor(b, ror(b, elt, 17), ror(b, elt, 19), b.bin("lshr", elt, b.const(10, "i32")))
            r.append(add(b, elt, d[e], T0[e]))
        vwrite(b, f["Rd"], pack32(b, r))


def t_crypto_sha2(b, f, pc, p):
    o = f["opcode"]
    if o == 0:   # SHA1H: ROL(Sn, 30)
        vwrite(b, f["Rd"], rol(b, vread(b, f["Rn"], "i32"), 30))
    elif o == 1:   # SHA1SU1
        d, n = lanes32(b, f["Rd"]), lanes32(b, f["Rn"])
        t = [b.bin("xor", d[k], n[k + 1]) if k < 3 else d[3] for k in range(4)]   # (T = Vd ^ (Vn >> 32))
        r = [rol(b, t[k], 1) for k in range(4)]
        r[3] = b.bin("xor", r[3], rol(b, t[0], 2))
        vwrite(b, f["Rd"], pack32(b, r))
    else:   # SHA256SU0
        d, n = lanes32(b, f["Rd"]), lanes32(b, f["Rn"])
        T = d[1:] + n[:1]
        r = [add(b, xor(b, ror(b, T[e], 7), ror(b, T[e], 18), b.bin("lshr", T[e], b.const(3, "i32"))), d[e])
             for e in range(4)]
        vwrite(b, f["Rd"], pack32(b, r))


TEMPLATES = {k[2:]: v for k, v in globals().items() if k.startswith("t_")}
