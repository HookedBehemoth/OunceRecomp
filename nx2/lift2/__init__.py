"""Lifter 2: AArch64 guest code -> typed SSA IR -> LLVM IR (docs/lifter2-design.md).

enc.py         the instruction encodings as data, and the decoder built from them
asm.py         decoded instructions printed in LLVM's syntax (the decoder's check against llvm-mc)
ir.py          the IR: values, blocks, functions, and the builder that keeps guest registers in SSA form
sem_int.py     the integer instruction semantics, one template per family of encodings
build_ir.py    a guest function body -> IR
emit_llvm.py   IR -> textual LLVM IR
llvm.py        the pinned LLVM major and the compiler calls
census.py      which instruction forms the game uses, and which of them the decoder covers
selfcheck.py   the decoder against llvm-mc's disassembler, word by word
"""
