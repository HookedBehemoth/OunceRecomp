"""The direct back end's register-pressure pass on a stage's SPIR-V (gpu/direct/direct_spv_late.cpp), without a game or a GPU.

Modules built here have the shape the translator emits (the native code's order: everything loaded first, used later):

- within the budget a module is left word for word;
- past it (a budget of 2 values) the loads follow their uses: every instruction still there once, the instructions with
  effects in their order, every value made before it is used, a phi still its block's first instruction, and fewer loaded
  values held at once (8 before);
- a module that stores by address keeps its load by address between the two stores around it;
- across blocks: the loads that only a branch uses go down into that branch; a value a loop uses from outside it does not
  go into the loop; a subgroup operation does not leave its block; and nothing leaves its block unless the function is
  past `sink_peak`;
- the same input gives the same output.

With glslangValidator on the PATH the hand-written mesh stages (gpu/direct/shaders/mesh) go through the pass too, and
with spirv-opt the synthetic spilling stage (tools/shaders/probes/spill.mesh: 320 values loaded before the
first is used), which must come out holding about the budget; every output through spirv-val when it is there.

The gate that holds RADV's compile of the game's stages against the pass needs the player's captures and a GPU, and is a
tool: tools/shaders/late_gate.py (docs/direct/spv-late.md).

    python -I -B tests/test_direct_spv_late.py [--cxx clang++]
"""
import argparse
import re
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


class Module:
    """A compute module: a uniform block of 8 words, a private word; ids given out as they come"""

    def __init__(self, physical=False, subgroup=False):
        self.out, self.ids = [], [0]
        new, op = self.new, self.op
        self.void, fn, self.u32, self.boolean, block, arr = new(), new(), new(), new(), new(), new()
        p_block, self.p_u32, self.var, p_priv, self.priv, self.main, eight = new(), new(), new(), new(), new(), new(), new()
        self.consts = [new() for _ in range(8)]
        u64, self.p_phys, self.addr = new(), new(), new()
        op(17, 1)                                            # OpCapability Shader
        if physical:
            op(17, 5347), op(17, 11)                         # PhysicalStorageBufferAddresses, Int64
        if subgroup:
            op(17, 61), op(17, 64)                           # GroupNonUniform, GroupNonUniformBallot
        op(14, 5348 if physical else 0, 1)                   # OpMemoryModel
        op(15, 5, self.main, *text("main"), self.var, self.priv)   # OpEntryPoint GLCompute
        op(16, self.main, 17, 1, 1, 1)                       # OpExecutionMode LocalSize
        op(71, arr, 6, 16)                                   # ArrayStride
        op(71, block, 2)                                     # Block
        op(72, block, 0, 35, 0)                              # Offset
        op(71, self.var, 34, 0), op(71, self.var, 33, 0)     # DescriptorSet, Binding
        op(19, self.void), op(33, fn, self.void), op(21, self.u32, 32, 0), op(20, self.boolean)
        op(43, self.u32, eight, 8)
        for i, c in enumerate(self.consts):
            op(43, self.u32, c, i)
        op(28, arr, self.u32, eight), op(30, block, arr)
        op(32, p_block, 2, block), op(32, self.p_u32, 2, self.u32), op(59, p_block, self.var, 2)
        op(32, p_priv, 6, self.u32), op(59, p_priv, self.priv, 6)
        if physical:
            op(21, u64, 64, 0), op(32, self.p_phys, 5349, self.u32), op(43, u64, self.addr, 4096, 0)
        op(54, self.void, self.main, 0, fn)

    def new(self):
        self.ids[0] += 1
        return self.ids[0]

    def op(self, code, *operands):
        self.out.append([(len(operands) + 1) << 16 | code] + list(operands))

    def load(self, index):
        """a word of the uniform block: its access chain and its load"""
        chain, value = self.new(), self.new()
        self.op(65, self.p_u32, chain, self.var, self.consts[0], self.consts[index])
        self.op(61, self.u32, value, chain)
        return value

    def add(self, a, b):
        r = self.new()
        self.op(128, self.u32, r, a, b)   # OpIAdd
        return r

    def words(self):
        self.op(56)   # OpFunctionEnd
        return [0x07230203, 0x00010600, 0, self.ids[0] + 1, 0] + [w for i in self.out for w in i]


def module(physical_store):
    """main: 8 loads of the uniform block's words, a store to the private word, a chain of adds over the loads; a branch;
    a block with a phi and a second store"""
    m = Module(physical=physical_store)
    op, new = m.op, m.new
    entry, then, merge = new(), new(), new()
    op(248, entry)
    loads = [m.load(i) for i in range(8)]
    if physical_store:
        ptr = new()
        op(120, m.p_phys, ptr, m.addr)              # OpConvertUToPtr
        op(62, ptr, m.consts[1], 2, 4)              # OpStore Aligned 4
        ptr2, through = new(), new()
        op(120, m.p_phys, ptr2, m.addr)
        op(61, m.u32, through, ptr2, 2, 4)          # a load by address: it must stay between the stores around it
        loads[0] = through
        ptr3 = new()
        op(120, m.p_phys, ptr3, m.addr)
        op(62, ptr3, m.consts[2], 2, 4)             # the second store by address
    op(62, m.priv, m.consts[0])                     # a store: nothing of the chain is needed yet
    acc = loads[0]
    for value in loads[1:]:
        acc = m.add(acc, value)
    op(62, m.priv, acc)                             # the chain's store
    op(249, then)                                   # OpBranch
    op(248, then)
    phi = new()
    op(245, m.u32, phi, acc, entry)                 # OpPhi
    twice = m.add(phi, phi)
    op(62, m.priv, twice)
    op(249, merge)
    op(248, merge)
    op(253)                                         # OpReturn
    return m.words()


def branching():
    """main: 8 loads that only a branch uses, a value that only a loop's body uses, a subgroup operation that only the
    branch uses; the branch; the loop. Returns the words and the ids to look for"""
    m = Module(subgroup=True)
    op, new = m.op, m.new
    entry, then, merge, header, body, done = new(), new(), new(), new(), new(), new()
    op(248, entry)
    loads = [m.load(i) for i in range(8)]
    for_loop = m.add(m.load(1), m.load(2))          # used by the loop's body alone
    elected = new()
    op(333, m.boolean, elected, m.consts[3])        # OpGroupNonUniformElect (scope Subgroup = 3): used by the branch alone
    cond = new()
    op(171, m.boolean, cond, m.load(0), m.consts[0])   # OpINotEqual
    op(247, merge, 0)                               # OpSelectionMerge
    op(250, cond, then, merge)                      # OpBranchConditional
    op(248, then)
    acc = loads[0]
    for value in loads[1:]:
        acc = m.add(acc, value)
    chosen = new()
    op(169, m.u32, chosen, elected, acc, m.consts[0])   # OpSelect
    op(62, m.priv, chosen)
    op(249, merge)
    op(248, merge)
    op(249, header)
    op(248, header)
    i, i2 = new(), new()
    op(245, m.u32, i, m.consts[0], merge, i2, body)
    less = new()
    op(176, m.boolean, less, i, m.consts[4])        # OpULessThan
    op(246, done, body, 0)                          # OpLoopMerge
    op(250, less, body, done)
    op(248, body)
    op(62, m.priv, m.add(for_loop, i))
    op(128, m.u32, i2, i, m.consts[1])
    op(249, header)
    op(248, done)
    op(253)
    return m.words(), {"then": then, "merge": merge, "header": header, "loads": loads, "for_loop": for_loop, "elected": elected}


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
    effects = lambda seq: [x for x in seq if (x[0] & 0xffff) in (62, 245, 246, 247, 248, 249, 250, 253, 54, 56)]
    if effects(a) != effects(b):
        return "%s: the instructions with effects changed order" % what
    made, body, prev = set(), False, None
    for x in b:
        code = x[0] & 0xffff
        if code == 54:
            body = True
        if not body:
            made.update(x[1:3])
            continue
        if code in (65, 61, 128, 120, 169, 171, 176):   # (operands after the result; a load's and a convert's first one alone)
            for operand in x[3:4] if code in (61, 120) else x[3:]:
                if operand not in made:
                    return "%s: %d used before it is made" % (what, operand)
            made.add(x[2])
        elif code == 333:
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


def block_of(words):
    """{result id: the label of the block that makes it}"""
    out, label = {}, None
    for x in instructions(words):
        code = x[0] & 0xffff
        if code == 248:
            label = x[1]
        elif label is not None and code in (61, 65, 128, 169, 171, 176, 333):
            out[x[2]] = label
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=shutil.which("clang++") or shutil.which("g++") or "c++")
    a = ap.parse_args()
    val = shutil.which("spirv-val")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        exe = tmp / "spvlate"
        r = subprocess.run([a.cxx, "-std=c++20", "-O1", "-o", str(exe), str(ROOT / "tools" / "shaders" / "spvlate.cpp")], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            print("FAIL: spvlate does not build")
            return 1

        def run(words, name, *args):
            src, dst = tmp / (name + ".spv"), tmp / (name + ".late.spv")
            src.write_bytes(struct.pack("<%dI" % len(words), *words))
            line = subprocess.run([str(exe), str(src), str(dst)] + list(args), check=True, capture_output=True, text=True).stdout
            data = dst.read_bytes()
            if val and subprocess.run([val, "--target-env", "vulkan1.3", "--scalar-block-layout", str(dst)], capture_output=True).returncode != 0:
                raise SystemExit("FAIL: %s after the pass is not valid SPIR-V" % name)
            return list(struct.unpack("<%dI" % (len(data) // 4), data)), line

        plain = module(False)
        if val:
            (tmp / "check.spv").write_bytes(struct.pack("<%dI" % len(plain), *plain))
            if subprocess.run([val, "--target-env", "vulkan1.3", str(tmp / "check.spv")], capture_output=True).returncode != 0:
                print("FAIL: the test's own module is not valid SPIR-V")
                return 1
        same, _ = run(plain, "within")
        if same != plain:
            print("FAIL: a module within the budget was changed")
            return 1
        print("PASS: a module within the budget is left word for word")
        late, _ = run(plain, "plain", "--budget", "2")
        again, _ = run(plain, "plain2", "--budget", "2")
        why = check(plain, late, "the module")
        if why:
            print("FAIL: " + why)
            return 1
        before, after = most_loads_held(plain), most_loads_held(late)
        if before != 8 or after > 4:
            print("FAIL: loaded values held at most: %d before (8 expected), %d after (4 or fewer expected)" % (before, after))
            return 1
        if again != late:
            print("FAIL: the same module came out differently twice")
            return 1
        print("PASS: past the budget the loads follow their uses (%d loaded values held at most, %d before); effects in order, the phi first; "
              "the same output twice" % (after, before))
        writer = module(True)
        late_w, _ = run(writer, "writer", "--budget", "2")
        why = check(writer, late_w, "the module that writes by address")
        if why:
            print("FAIL: " + why)
            return 1
        seq = instructions(late_w)
        through = [n for n, x in enumerate(seq) if (x[0] & 0xffff) == 61 and len(x) == 6]
        stores = [n for n, x in enumerate(seq) if (x[0] & 0xffff) == 62 and len(x) == 5]
        if late_w == writer or len(through) != 1 or len(stores) != 2 or not (stores[0] < through[0] < stores[1]):
            print("FAIL: the load by address left the two stores by address around it (or nothing moved)")
            return 1
        print("PASS: a load by address stays between the stores by address around it")
        words, ids = branching()
        local, _ = run(words, "branch_local", "--budget", "2", "--sink-peak", "0")
        why = check(words, local, "the branching module, within blocks")
        if why:
            print("FAIL: " + why)
            return 1
        if block_of(local) != block_of(words):
            print("FAIL: an instruction left its block with sink_peak 0")
            return 1
        sunk, line = run(words, "branch", "--budget", "2", "--sink-peak", "4")
        why = check(words, sunk, "the branching module")
        if why:
            print("FAIL: " + why)
            return 1
        was, now = block_of(words), block_of(sunk)
        gone = [v for v in ids["loads"][1:] if now[v] == ids["then"]]
        if len(gone) != 7:
            print("FAIL: %d of the 7 loads that only the branch uses went into it (%s)" % (len(gone), line.strip()))
            return 1
        if now[ids["for_loop"]] != was[ids["for_loop"]]:
            print("FAIL: a value the loop's body uses from outside went into the loop")
            return 1
        if now[ids["elected"]] != was[ids["elected"]]:
            print("FAIL: a subgroup operation left its block")
            return 1
        print("PASS: loads that only a branch uses go into it; nothing goes into a loop; a subgroup operation stays in its block")
        glslang = shutil.which("glslangValidator")
        if glslang:
            mesh = ROOT / "gpu" / "direct" / "shaders" / "mesh"
            n = 0
            for entry in (mesh / "codes.txt").read_text().splitlines():
                if not entry.strip() or entry.startswith("#"):
                    continue
                code, src, *defs = entry.split()
                spv = tmp / (code + ".in.spv")
                subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "-I" + str(mesh)] + defs + ["-o", str(spv), str(mesh / src)], check=True, capture_output=True)
                data = spv.read_bytes()
                src_words = list(struct.unpack("<%dI" % (len(data) // 4), data))
                for args in (("--budget", "8"), ("--budget", "8", "--sink-peak", "8")):
                    out, _ = run(src_words, code, *args)
                    if sorted(instructions(src_words)) != sorted(instructions(out)):
                        print("FAIL: %s: the instructions are not the same set" % code)
                        return 1
                n += 1
            print("PASS: %d hand-written mesh stages through the pass: the same instructions%s" % (n, ", spirv-val accepts them" if val else ""))
            opt = shutil.which("spirv-opt")
            if opt:   # (the stage's locals as values, as the translator's stages have them: glslang keeps them in variables)
                spv, ssa = tmp / "spill.in.spv", tmp / "spill.ssa.spv"
                subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "-o", str(spv), str(ROOT / "tools" / "shaders" / "probes" / "spill.mesh")], check=True, capture_output=True)
                subprocess.run([opt, "--eliminate-local-single-block", "--eliminate-local-single-store", "--ssa-rewrite", "--eliminate-dead-code-aggressive",
                                "-o", str(ssa), str(spv)], check=True, capture_output=True)
                data = ssa.read_bytes()
                src_words = list(struct.unpack("<%dI" % (len(data) // 4), data))
                out, line = run(src_words, "spill")
                m = re.search(r"values held at most (\d+) -> (\d+)", line)
                if not m or int(m.group(1)) < 320 or int(m.group(2)) > 200 or sorted(instructions(src_words)) != sorted(instructions(out)):
                    print("FAIL: the synthetic spilling stage: %s (320 or more before and 200 or fewer after expected)" % line.strip())
                    return 1
                print("PASS: the synthetic spilling stage holds %s values at most after the pass, %s before" % (m.group(2), m.group(1)))
            else:
                print("spirv-opt not found: the synthetic spilling stage not tried")
        else:
            print("glslangValidator not found: the hand-written mesh stages and the synthetic spilling stage not tried")
    return 0


if __name__ == "__main__":
    sys.exit(main())
