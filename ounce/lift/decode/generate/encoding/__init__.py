"""The AArch64 encoding tables the decoder's generated tables.rs is made from (copied from the Python lifter, which is gone).

enc.py         the instruction encodings as data
asm.py         decoded instructions printed in LLVM's syntax (enc.py imports its tables back from it)
enc_fp.py      FP scalar and vector/FP load/store encodings
enc_neon.py    Advanced SIMD encodings, vector and scalar
enc_crypto.py  the crypto extension's encodings
"""
