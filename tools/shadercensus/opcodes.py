"""The shader seed's Ampere instructions counted: per opcode and stage kind, how often, in how many stages, and how many
modifier encodings (the instruction with its register, immediate, constant-buffer, guard and scheduling fields cleared).

    work/venv/bin/python -I tools/shadercensus/opcodes.py <seed>... [--names <file>]... [--json <file>] [--dump <key>]

Several seeds (one per game) give a table each, and a list of the opcodes only one of them uses. --names takes opcode names
from Mesa's NAK encoder (src/nouveau/compiler/nak/sm70_encode.rs) or from a text file of "<hex opcode> <name>" lines; the
first file that names an opcode wins; an opcode no file names is "?". Instructions are counted up to the program's end as
program_capture.h finds it.
--dump prints the instructions of the stage of that code key (as the back end logs it), one a line with its fields split.
The output is counts and opcode numbers only; the seed itself is the player's game and stays under work/.
"""
import argparse
import collections
import hashlib
import json
import re
import struct
import sys

KIND = {0: "vertex", 1: "fragment", 2: "geometry", 3: "tess_control", 4: "tess_eval", 5: "compute", 6: "task", 7: "mesh"}
HEADER = 128   # (a graphics stage's code area starts with its program header)
FORMS = {1: "", 2: ".imm2", 3: ".cb2", 4: ".imm", 5: ".cb", 6: ".ur", 7: ".ur2"}
# (cleared for the modifier encoding: guard 12-15, Rd 16-23, Ra 24-31, operand 32-63, Rc 64-71, scheduling 105-127)
OPERANDS = (0xf << 12) | (0xff << 16) | (0xff << 24) | (0xffffffff << 32) | (0xff << 64) | (((1 << 23) - 1) << 105)


def read_seed(path):
    data = open(path, "rb").read()
    if data[:8] != b"HKSEED01":
        raise ValueError("%s: not a seed" % path)
    _, count = struct.unpack_from("<II", data, 8)
    at = 16
    for _ in range(count):
        _, stage, cn, bn = struct.unpack_from("<QIII", data, at)
        at += 20
        yield stage, data[at:at + cn], data[at + cn:at + cn + bn]
        at += cn + bn


def fnv(b, h=0xcbf29ce484222325):
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


def instructions(stage, control, code):
    """The stage's instructions up to the program's end, as program_capture.h finds it"""
    length = struct.unpack_from("<I", control, 0x38)[0]
    start = 0 if stage == 5 else HEADER
    if stage != 5 and length <= HEADER + 16:
        return
    last_call = last_forward = 0
    main_exited = computed = False
    for i in range((min(length, len(code)) - start) // 16):
        x = int.from_bytes(code[start + 16 * i:start + 16 * i + 16], "little")
        yield x
        op, guard = x & 0xfff, (x >> 12) & 0xf
        if op in (0x944, 0x947, 0x945):
            if op == 0x945:
                rel = struct.unpack("<i", struct.pack("<I", (x >> 32) & 0xffffffff))[0]
            else:
                raw = (x >> 32) & ((1 << 50) - 1)
                rel = raw - (1 << 50) if raw >> 49 else raw
            target = i * 16 + 16 + rel
            if op == 0x944 and not (x >> 86) & 1:
                last_call = max(last_call, target // 16)
            else:
                last_forward = max(last_forward, target // 16)
            if op == 0x947 and computed and guard == 7 and target == i * 16:
                return
        if op in (0x949, 0x958):
            computed = True
        if computed:
            continue
        if op == 0x94d and guard == 7 and i >= last_forward:
            if not last_call:
                return
            main_exited = True
        if op == 0x950 and guard == 7 and main_exited and i >= last_call and i >= last_forward:
            return


def nak_names(path):
    """Opcode -> NAK op name, from the encoder's set_opcode / encode_alu calls of each `impl SM70Op for Op...`"""
    text = open(path).read()
    parts = re.split(r"\nimpl SM70Op for (Op\w+)", text)
    names = collections.defaultdict(set)
    for name, body in zip(parts[1::2], parts[2::2]):
        body = body[body.find("fn encode("):]
        for h in re.findall(r"(?:set_opcode|encode_\w*alu\w*)\(\s*(0x[0-9a-f]+)", body):
            names[int(h, 16)].add(name[2:].upper())
    return {k: "/".join(sorted(v)) for k, v in names.items()}


def text_names(path):
    names = {}
    for line in open(path):
        f = line.split()
        if len(f) >= 2 and not line.startswith("#"):
            names[int(f[0], 16)] = f[1]
    return names


def name_of(op, names):
    if op in names:
        return names[op]
    base = op & 0x1ff
    if base in names and base < 0x200 and op >> 9 in FORMS:   # (ALU: the form in bits 9-11)
        return names[base] + FORMS[op >> 9]
    return "?"


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("seeds", nargs="+")
    p.add_argument("--names", action="append", default=[])
    p.add_argument("--json")
    p.add_argument("--dump")
    a = p.parse_args()
    names = {}
    for path in a.names:
        for k, v in (nak_names(path) if path.endswith(".rs") else text_names(path)).items():
            names.setdefault(k, v)
    if a.dump:
        for path in a.seeds:
            for stage, control, code in read_seed(path):
                if "%016x" % fnv(code, fnv(control[:0xd8])) != a.dump:
                    continue
                for i, x in enumerate(instructions(stage, control, code)):
                    op = x & 0xfff
                    print("%05x %03x %-14s P%d%s Rd%-3d Ra%-3d Rb/imm %08x Rc%-3d mods %08x %03x sched %06x" % (
                        i * 16, op, name_of(op, names), (x >> 12) & 7, "!" if (x >> 15) & 1 else " ", (x >> 16) & 0xff,
                        (x >> 24) & 0xff, (x >> 32) & 0xffffffff, (x >> 64) & 0xff, (x >> 72) & 0xffffffff, (x >> 104) & 1,
                        x >> 105))
                return 0
        return 1
    games = []
    # (per game: (kind, op) -> [instructions, stages, set of modifier encodings])
    for path in a.seeds:
        table = collections.defaultdict(lambda: [0, 0, set()])
        stages = collections.Counter()
        for stage, control, code in read_seed(path):
            stages[stage] += 1
            seen = set()
            for x in instructions(stage, control, code):
                op = x & 0xfff
                row = table[(stage, op)]
                row[0] += 1
                if op not in seen:
                    seen.add(op)
                    row[1] += 1
                row[2].add(hashlib.blake2b((x & ~OPERANDS).to_bytes(16, "little"), digest_size=8).digest())
        games.append((path, table, stages))
    out = {}
    for path, table, stages in games:
        print("## %s: %s" % (path, ", ".join("%d %s" % (n, KIND.get(k, k)) for k, n in sorted(stages.items()))))
        by_op = collections.defaultdict(lambda: [0, 0, set(), set()])
        for (kind, op), (n, s, mods) in table.items():
            r = by_op[op]
            r[0] += n
            r[1] += s
            r[2] |= mods
            r[3].add(KIND.get(kind, str(kind)))
        print("| opcode | NAK | instructions | stages | encodings | kinds |\n|---|---|---|---|---|---|")
        for op, (n, s, mods, kinds) in sorted(by_op.items(), key=lambda t: -t[1][0]):
            print("| %03x | %s | %d | %d | %d | %s |" % (op, name_of(op, names), n, s, len(mods), ",".join(sorted(kinds))))
        out[path] = {"%03x/%s" % (op, KIND.get(kind, kind)): [n, s, len(mods)] for (kind, op), (n, s, mods) in table.items()}
    if len(games) > 1:
        ops = [{op for _, op in t} for _, t, _ in games]
        for (path, table, _), mine in zip(games, ops):
            only = mine - set().union(*(o for o in ops if o is not mine))
            print("## only in %s: %d opcodes" % (path, len(only)))
            for op in sorted(only, key=lambda o: -sum(n for (k, q), (n, _, _) in table.items() if q == o)):
                rows = [(KIND.get(k, k), n, s) for (k, q), (n, s, _) in table.items() if q == op]
                print("  %03x %-14s %s" % (op, name_of(op, names), ", ".join("%s %d in %d stages" % r for r in rows)))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
