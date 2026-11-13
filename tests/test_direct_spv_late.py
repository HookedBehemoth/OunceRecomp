"""The direct back end's late scheduling of a stage's SPIR-V (gpu/direct/direct_spv_late.cpp), without a game or a GPU.

A module built here loads eight uniform words first and uses them one by one afterwards, with a store and a second block
between (the shape the translator emits: the native code's order). After the pass every instruction is still there once,
the instructions with effects are in their order, every value is made before it is used, a phi is still its block's first
instruction, and no more than three loaded values are held at a time (eight before). A second module stores through a
physical pointer: its loads by address stay where they are. The Python reference of the pass (tools/meshlab/spv_sink.py)
gives the same words. With glslangValidator on the PATH the hand-written mesh stages (gpu/direct/shaders/mesh) go through
both too, and through spirv-val when it is there.

    python -I -B tests/test_direct_spv_late.py [--cxx clang++]
"""
import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def text(s):
    b = s.encode() + b"\0"
    b += b"\0" * (-len(b) % 4)
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


def module(physical_store):
    """main: 8 loads of a uniform block's words, a store to a private variable, a chain of adds over the loads; a branch;
    a block with a phi and a second store. ids are given out as they come"""
    out = []
    ids = [0]

    def new():
        ids[0] += 1
        return ids[0]

    def op(code, *operands):
        out.append([(len(operands) + 1) << 16 | code] + list(operands))

    void, fn, u32, f32, block, arr = new(), new(), new(), new(), new(), new()
    p_block, p_u32, var, p_priv, priv, main, eight = new(), new(), new(), new(), new(), new(), new()
    consts = [new() for _ in range(8)]
    u64, p_phys, addr = new(), new(), new()
    op(17, 1)                                   # OpCapability Shader
    if physical_store:
        op(17, 5347), op(17, 11)                # PhysicalStorageBufferAddresses, Int64
    op(14, 5348 if physical_store else 0, 1)    # OpMemoryModel
    op(15, 5, main, *text("main"), var, priv)   # OpEntryPoint GLCompute
    op(16, main, 17, 1, 1, 1)                   # OpExecutionMode LocalSize
    op(71, arr, 6, 4)                           # ArrayStride
    op(71, block, 2)                            # Block
    op(72, block, 0, 35, 0)                     # Offset
    op(71, var, 34, 0), op(71, var, 33, 0)      # DescriptorSet, Binding
    op(19, void), op(33, fn, void), op(21, u32, 32, 0), op(22, f32, 32)
    op(43, u32, eight, 8)
    for i, c in enumerate(consts):
        op(43, u32, c, i)
    op(28, arr, u32, eight), op(30, block, arr)
    op(32, p_block, 2, block), op(32, p_u32, 2, u32), op(59, p_block, var, 2)
    op(32, p_priv, 6, u32), op(59, p_priv, priv, 6)
    if physical_store:
        op(21, u64, 64, 0), op(32, p_phys, 5349, u32), op(43, u64, addr, 4096, 0)
    op(54, void, main, 0, fn)
    entry, then, merge = new(), new(), new()
    op(248, entry)
    loads = []
    for c in consts:
        chain, value = new(), new()
        op(65, p_u32, chain, var, consts[0], c)
        op(61, u32, value, chain)
        loads.append(value)
    if physical_store:
        ptr = new()
        op(120, p_phys, ptr, addr)              # OpConvertUToPtr
        op(62, ptr, consts[1], 2, 4)            # OpStore Aligned 4
        ptr2, through = new(), new()
        op(120, p_phys, ptr2, addr)
        op(61, u32, through, ptr2, 2, 4)        # a load by address: it must not pass the stores below
        loads[0] = through
    op(62, priv, consts[0])                     # the first store: nothing of the chain is needed yet
    acc = loads[0]
    for value in loads[1:]:
        nxt = new()
        op(128, u32, nxt, acc, value)           # OpIAdd
        acc = nxt
    op(62, priv, acc)                           # the second store: the chain
    op(249, then)                               # OpBranch
    op(248, then)
    phi, twice = new(), new()
    op(245, u32, phi, acc, entry)               # OpPhi
    op(128, u32, twice, phi, phi)
    op(62, priv, twice)
    op(249, merge)
    op(248, merge)
    op(253)                                     # OpReturn
    op(56)                                      # OpFunctionEnd
    return [0x07230203, 0x00010600, 0, ids[0] + 1, 0] + [w for i in out for w in i]


def instructions(words):
    out, i = [], 5
    while i < len(words):
        wc = words[i] >> 16
        out.append(tuple(words[i:i + wc]))
        i += wc
    return out


def check(before, after, what):
    a, b = instructions(before), instructions(after)
    if sorted(a) != sorted(b):
        return "%s: the instructions are not the same set" % what
    effects = lambda seq: [x for x in seq if (x[0] & 0xffff) in (62, 245, 248, 249, 253, 54, 56)]
    if effects(a) != effects(b):
        return "%s: the instructions with effects changed order" % what
    made, body = set(), False
    for x in b:
        code = x[0] & 0xffff
        if code == 54:
            body = True
        if not body:
            made.update(x[1:3])
            continue
        if code in (65, 61, 128, 120):   # (access chain, load, add, convert: operands after the result)
            for operand in x[3:4] if code in (61, 120) else x[3:]:
                if operand not in made:
                    return "%s: %d used before it is made" % (what, operand)
            made.add(x[2])
        elif code == 245:
            made.add(x[2])
            if (prev[0] & 0xffff) != 248:
                return "%s: a phi is not its block's first instruction" % what
        prev = x
    return None


def most_loads_held(words):
    held, most, last_use = set(), 0, {}
    seq = instructions(words)
    for n, x in enumerate(seq):
        for operand in x[3:]:
            last_use[operand] = n
    for n, x in enumerate(seq):
        if (x[0] & 0xffff) == 61:
            held.add(x[2])
        most = max(most, len(held))
        held = {v for v in held if last_use.get(v, -1) > n}
    return most


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=shutil.which("clang++") or shutil.which("g++") or "c++")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        exe = tmp / "spvlate"
        r = subprocess.run([a.cxx, "-std=c++20", "-O1", "-o", str(exe), str(ROOT / "tools" / "meshlab" / "spvlate.cpp")], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            print("FAIL: spvlate does not build")
            return 1

        def both(words, name):
            src, cpp, py = tmp / (name + ".spv"), tmp / (name + ".cpp.spv"), tmp / (name + ".py.spv")
            src.write_bytes(struct.pack("<%dI" % len(words), *words))
            subprocess.run([str(exe), str(src), str(cpp)], check=True, capture_output=True)
            subprocess.run([sys.executable, "-I", "-B", str(ROOT / "tools" / "meshlab" / "spv_sink.py"), str(src), str(py)], check=True, capture_output=True)
            if cpp.read_bytes() != py.read_bytes():
                raise SystemExit("FAIL: %s: the pass and its Python reference give different modules" % name)
            data = cpp.read_bytes()
            return list(struct.unpack("<%dI" % (len(data) // 4), data)), cpp

        plain = module(False)
        late, _ = both(plain, "plain")
        why = check(plain, late, "the module")
        if why:
            print("FAIL: " + why)
            return 1
        before, after = most_loads_held(plain), most_loads_held(late)
        if before != 8 or after > 3:
            print("FAIL: loaded values held at most: %d before (8 expected), %d after (3 or fewer expected)" % (before, after))
            return 1
        print("PASS: the loads follow their uses (%d loaded values held at most, %d before); effects in order, the phi first" % (after, before))
        writer = module(True)
        late_w, _ = both(writer, "writer")
        why = check(writer, late_w, "the module that writes by address")
        if why:
            print("FAIL: " + why)
            return 1
        codes = [x[0] & 0xffff for x in instructions(late_w)]
        through = [n for n, x in enumerate(instructions(late_w)) if (x[0] & 0xffff) == 61 and len(x) == 6]
        stores = [n for n, c in enumerate(codes) if c == 62]
        if len(through) != 1 or not (stores[0] < through[0] < stores[1]):
            print("FAIL: the load by address moved past a store of a module that writes by address")
            return 1
        print("PASS: a module that writes by address keeps its loads by address in place")
        glslang, val = shutil.which("glslangValidator"), shutil.which("spirv-val")
        if glslang:
            mesh = ROOT / "gpu" / "direct" / "shaders" / "mesh"
            n = 0
            for line in (mesh / "codes.txt").read_text().splitlines():
                if not line.strip() or line.startswith("#"):
                    continue
                code, src, *defs = line.split()
                spv = tmp / (code + ".in.spv")
                subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "-I" + str(mesh)] + defs + ["-o", str(spv), str(mesh / src)], check=True, capture_output=True)
                data = spv.read_bytes()
                words = list(struct.unpack("<%dI" % (len(data) // 4), data))
                late_words, path = both(words, code)
                if sorted(instructions(words)) != sorted(instructions(late_words)):
                    print("FAIL: %s: the instructions are not the same set" % code)
                    return 1
                if val and subprocess.run([val, "--target-env", "vulkan1.3", "--scalar-block-layout", str(path)], capture_output=True).returncode != 0:
                    print("FAIL: %s scheduled late is not valid SPIR-V" % code)
                    return 1
                n += 1
            print("PASS: %d hand-written mesh stages scheduled late: the pass and its reference agree%s" % (n, ", spirv-val accepts them" if val else ""))
        else:
            print("glslangValidator not found: the hand-written mesh stages not tried")
    return 0


if __name__ == "__main__":
    sys.exit(main())
