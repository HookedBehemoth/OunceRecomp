"""The runtime's tables of the lifted game, written by lift 2's side (milestone 19, step 3): what the present lifter's
Lifter._write_table wrote, byte for byte.

- a64_table.c: every function and hooked address, sorted, with its f_<addr>: a64_lookup's binary search (BLR, BR,
  host calls into the guest: third_party/a64-kit/rt/a64_rt.c through nx2/rtgen.py);
- a64_hooks.c: the hooked addresses (the image's hooks: HLE functions and generated stubs, nx2/image.py), each
  f_<addr> a tail call of its host function, so a call of a hooked function reaches the host under the guest name.
"""
import os


def fname(a):
    return "f_%08x" % a


def write(out_dir, functions, hooks):
    """functions: every function entry; hooks: {guest address: the host function's C name}"""
    fns = sorted(set(functions) | set(hooks))
    with open(os.path.join(out_dir, "a64_table.c"), "w", newline="\n") as f:
        f.write('#include "a64.h"\n')
        f.write("".join("void %s(Ctx *A64_NOESC);\n" % fname(a) for a in fns))
        f.write("const uint32_t a64_fn_count = %d;\n" % len(fns))
        f.write("const uint32_t a64_fn_addr[] = {\n%s};\n" % "".join("0x%08x,\n" % a for a in fns))
        f.write("const A64Fn a64_fn_ptr[] = {\n%s};\n" % "".join("%s,\n" % fname(a) for a in fns))
    with open(os.path.join(out_dir, "a64_hooks.c"), "w", newline="\n") as f:
        f.write("/* Hooked guest addresses: f_<address> is the host function (an HLE or a generated stub). */\n")
        f.write('#include "a64.h"\n')
        f.write("".join("void %s(Ctx *A64_NOESC);\n" % n for n in sorted(set(hooks.values()))))
        f.write("".join("void f_%08x(Ctx *A64_NOESC c) { TAILCALL(%s); }\n" % (a, n) for a, n in sorted(hooks.items())))
