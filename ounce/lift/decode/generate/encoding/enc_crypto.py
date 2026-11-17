"""The cryptographic extension (FEAT_AES, FEAT_SHA1, FEAT_SHA256): the table (enc.py's format) and its printers. The
game's SDK uses them in its AES/SHA library (76 functions); PMULL/PMULL2 of 64-bit lanes (FEAT_PMULL) is in enc_neon's
three-different group.
"""

ENCODINGS = [
    ("CRYPTO_AES", "01001110 00 10100 001 op:2 10 Rn:5 Rd:5", "crypto_aes", {}, "True"),
    ("CRYPTO_SHA3", "01011110 00 0 Rm:5 0 opcode:3 00 Rn:5 Rd:5", "crypto_sha3", {}, "opcode != 7"),
    ("CRYPTO_SHA2", "01011110 00 10100 opcode:5 10 Rn:5 Rd:5", "crypto_sha2", {}, "opcode < 3"),
]

AES = ["aese", "aesd", "aesmc", "aesimc"]
SHA3 = ["sha1c", "sha1p", "sha1m", "sha1su0", "sha256h", "sha256h2", "sha256su1"]
SHA2 = ["sha1h", "sha1su1", "sha256su0"]


def _CRYPTO_AES(f, pc):
    return "%s v%d.16b, v%d.16b" % (AES[f["op"]], f["Rd"], f["Rn"])


def _CRYPTO_SHA3(f, pc):
    o = f["opcode"]
    if o < 3:
        return "%s q%d, s%d, v%d.4s" % (SHA3[o], f["Rd"], f["Rn"], f["Rm"])
    if o in (4, 5):
        return "%s q%d, q%d, v%d.4s" % (SHA3[o], f["Rd"], f["Rn"], f["Rm"])
    return "%s v%d.4s, v%d.4s, v%d.4s" % (SHA3[o], f["Rd"], f["Rn"], f["Rm"])


def _CRYPTO_SHA2(f, pc):
    if f["opcode"] == 0:
        return "sha1h s%d, s%d" % (f["Rd"], f["Rn"])
    return "%s v%d.4s, v%d.4s" % (SHA2[f["opcode"]], f["Rd"], f["Rn"])


RENDER = {"CRYPTO_AES": _CRYPTO_AES, "CRYPTO_SHA3": _CRYPTO_SHA3, "CRYPTO_SHA2": _CRYPTO_SHA2}
