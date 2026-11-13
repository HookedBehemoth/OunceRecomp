"""A SPIR-V module with every side-effect-free instruction moved down to its first use within its block: a prototype of a
late scheduling the translator could do, specified in docs/direct/hf-fix-report.md and NOT used by the game (an earlier
version of it, applied in the game, made two mesh stages that write by address spill and lost the device in the market).

    python3 -I tools/meshlab/spv_sink.py <in.spv> <out.spv> [--stats]

The translator emits a stage in the native code's order, and the game's compiler loaded early: a mesh stage that reads 112
rows of a probe table holds all of them in registers before the first is used, more than the 256 vector registers RADV
has, so ACO spills to scratch memory. This pass keeps each block's instructions with effects (stores, barriers, atomics,
loads of workgroup and private memory, calls of functions that write) in their order and emits every other instruction
just before the first instruction that needs it, operands first (the larger operand tree first); once an instruction is
emitted, every instruction that is ready and frees at least as many values as it makes follows at once (a row is consumed
as it is loaded). A read by address stays between the writes by address around it. The result is kept only when it holds
fewer values at once than the input, by the pass's own count (--stats prints both for the large functions); otherwise the
module is written unchanged.
"""
import struct
import sys

PURE = set([12, 65, 66, 67, 77, 78, 79, 80, 81, 82, 83]) | set(range(109, 125)) | set(range(126, 153)) | set(range(154, 192)) | set(range(194, 206))
WRITES = {62, 63, 64}   # (OpStore, OpCopyMemory, OpCopyMemorySized)
ATOMIC = set(range(227, 243))
READ_ONLY_CLASSES = {0, 1, 2, 9}   # (UniformConstant, Input, Uniform, PushConstant)
PHYSICAL = 5349
EAGER = True


def parse(words):
    out = []
    i = 5
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        out.append((op, words[i:i + wc]))
        i += wc
    return out


# (the instructions of a function body that make no value; every other one has its type at word 1 and its result at word 2)
NO_RESULT = {0, 8, 317, 62, 63, 64, 99, 218, 219, 220, 221, 224, 225, 228, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 318,
             4416, 5380, 5294, 5295, 54, 56}


def has_result(op):
    return op not in NO_RESULT


def main():
    src, dst = sys.argv[1], sys.argv[2]
    stats = "--stats" in sys.argv
    global EAGER
    EAGER = "--lazy" not in sys.argv
    data = open(src, "rb").read()
    words = list(struct.unpack("<%dI" % (len(data) // 4), data))
    insts = parse(words)
    pointer_class = {}   # (pointer type id -> storage class)
    type_of = {}         # (result id -> type id)
    width = {}           # (type id -> 32-bit values, for --stats)
    inside = False
    for op, w in insts:
        if op == 32:
            pointer_class[w[1]] = w[2]
        elif op in (21, 22):
            width[w[1]] = max(1, w[2] // 32)
        elif op == 20:
            width[w[1]] = 1
        elif op == 23:
            width[w[1]] = width.get(w[2], 1) * w[3]
        if op == 54:
            inside = True
        elif op == 56:
            inside = False
        elif (inside and has_result(op) or op == 59) and len(w) >= 3:   # (a function's values, and the module's variables)
            type_of[w[2]] = w[1]
    # (functions: whether they write memory that is not their own, read memory by address, read other memory that can
    # change (workgroup, private, output), themselves or through a call)
    funcs, cur = {}, None
    for idx, (op, w) in enumerate(insts):
        if op == 54:
            cur = w[2]
            funcs[cur] = {"first": idx, "writes": False, "reads_physical": False, "reads_other": False, "calls": set()}
        elif op == 56:
            funcs[cur]["last"] = idx
            cur = None
        elif cur is not None:
            if op in WRITES and pointer_class.get(type_of.get(w[1]), 7) != 7:
                funcs[cur]["writes"] = True
            if op in ATOMIC or op in (224, 225, 4428, 4429, 5294, 5295):   # (atomics, barriers, mesh outputs)
                funcs[cur]["writes"] = True
            if op == 61:
                cls = pointer_class.get(type_of.get(w[3]))
                if cls == PHYSICAL:
                    funcs[cur]["reads_physical"] = True
                elif cls not in READ_ONLY_CLASSES and cls != 7:
                    funcs[cur]["reads_other"] = True
            if op == 57:
                funcs[cur]["calls"].add(w[3])
    changed = True
    while changed:
        changed = False
        for f in funcs.values():
            for c in f["calls"]:
                g = funcs.get(c, {"writes": True, "reads_physical": True, "reads_other": True})
                for k in ("writes", "reads_physical", "reads_other"):
                    if g[k] and not f[k]:
                        f[k] = changed = True

    def physical_writer(op, w):
        """an instruction after which memory read by address may hold other values: a store or an atomic by address, a
        barrier (other invocations' writes), a call of a function that writes"""
        if op in WRITES:
            return pointer_class.get(type_of.get(w[1])) == PHYSICAL
        if op in ATOMIC or op in (224, 225):
            return True
        if op == 57:
            f = funcs.get(w[3])
            return f is None or f["writes"]
        return False

    def physical_reader(op, w):
        """a movable instruction that reads memory by address: it stays between the writers around it"""
        if op == 61:
            return pointer_class.get(type_of.get(w[3])) == PHYSICAL
        if op == 57:
            f = funcs.get(w[3])
            return f is not None and f["reads_physical"]
        return False

    def movable(op, w):
        if op in PURE or 333 <= op <= 366:   # (a subgroup operation's result is the same anywhere in its block)
            return True
        if op == 61:   # (OpLoad: of memory nothing writes, or by address)
            cls = pointer_class.get(type_of.get(w[3]))
            return cls in READ_ONLY_CLASSES or cls == PHYSICAL
        if op == 57:   # (OpFunctionCall: of a function that only reads, and nothing that can change but by address)
            f = funcs.get(w[3])
            return f is not None and not f["writes"] and not f["reads_other"]
        return False

    def live_peak(seq):
        last = {}
        for n, (op, w) in enumerate(seq):
            for x in w[1:]:
                if x in defined:
                    last[x] = n
        peak = live = 0
        ends = {}
        for x, n in last.items():
            ends.setdefault(n, []).append(x)
        born = set()
        for n, (op, w) in enumerate(seq):
            if has_result(op) and len(w) >= 3 and w[2] in last and last[w[2]] > n:
                live += width.get(w[1], 1)
                born.add(w[2])
            for x in ends.get(n, ()):
                if x in born:
                    live -= width.get(type_of.get(x), 1)
            peak = max(peak, live)
        return peak

    out = []
    moved = 0
    worst_before = worst_after = 0
    for f in funcs.values():
        body = insts[f["first"]:f["last"] + 1]
        defined = {w[2] for op, w in body if has_result(op) and len(w) >= 3}
        uses_total = {}
        for op, w in body:
            own = w[2] if has_result(op) and len(w) >= 3 else None
            for x in dict.fromkeys(w[1:]):
                if x in defined and x != own:
                    uses_total[x] = uses_total.get(x, 0) + 1
        new_body = []
        block = []

        def flush():
            nonlocal moved
            if not block:
                return
            n_block = len(block)
            pos = {}
            for n, (op, w) in enumerate(block):
                if movable(op, w):
                    pos[w[2]] = n
            where = {w[2]: n for n, (op, w) in enumerate(block) if has_result(op) and len(w) >= 3}
            # (each instruction's operands defined earlier in this block, movable or not; who uses each value here)
            deps = [[] for _ in block]       # (movable operands: what must be emitted first)
            operands = [[] for _ in block]   # (every operand of the block: what may die at this instruction)
            users = [[] for _ in block]
            remaining = [0] * n_block
            for n, (op, w) in enumerate(block):
                for x in dict.fromkeys(w[1:]):
                    k = where.get(x)
                    if k is None or k >= n:
                        continue
                    operands[n].append(k)
                    users[k].append(n)
                    remaining[k] += 1
                    if x in pos:
                        deps[n].append(k)
            for n, (op, w) in enumerate(block):
                if has_result(op) and len(w) >= 3 and uses_total.get(w[2], 0) > remaining[n]:
                    remaining[n] += 1 << 20   # (used in another block: it does not die here)
            emitted = [False] * n_block
            order = []
            # (a read by address stays between the writes by address around it: `after` is the writer it follows, and each
            # writer is preceded by the readers that came before it)
            after = [-1] * n_block
            readers = []
            writer = -1
            for n, (op, w) in enumerate(block):
                if physical_writer(op, w):
                    writer = n
                elif movable(op, w) and physical_reader(op, w):
                    after[n] = writer
                    readers.append(n)
            # (an instruction's tree: how many movable instructions of the block it needs, itself counted; the larger
            # operand is emitted first, so a chain is evaluated before the leaf that joins it: Sethi and Ullman's order)
            size = [1] * n_block
            for n in range(n_block):
                size[n] = min(1 + sum(size[d] for d in deps[n]), 1 << 30)

            def values(n):
                op, w = block[n]
                return width.get(w[1], 1) if has_result(op) and len(w) >= 3 else 0

            def place(k, head=False):
                """instruction k goes here; then every movable instruction that is ready and frees at least as many values
                as it makes (its operands' last use) goes right after it, and so on. head: k is one of the block's first
                instructions (its label, phis and variables, placed already: nothing may go between them): only what
                follows from it is looked for"""
                work = [k]
                while work:
                    k = work.pop()
                    if emitted[k] and not head:
                        continue
                    near = list(users[k])
                    if not head:
                        emitted[k] = True
                        order.append(k)
                        for d in operands[k]:
                            remaining[d] -= 1
                            if remaining[d] == 1:   # (its last user may now free it)
                                near.extend(users[d])
                    head = False
                    for c in dict.fromkeys(near):
                        if emitted[c] or not EAGER:
                            continue
                        op, w = block[c]
                        if not movable(op, w) or any(not emitted[d] for d in deps[c]) or any(not emitted[d] for d in operands[c]):
                            continue
                        if after[c] >= 0 and not emitted[after[c]]:
                            continue
                        freed = sum(values(d) for d in operands[c] if remaining[d] == 1)
                        if op == 81 or values(c) <= freed:   # (a component taken out of a vector is the vector's own register)
                            work.append(c)

            def emit(n):
                stack = [n]
                while stack:
                    k = stack[-1]
                    if emitted[k]:
                        stack.pop()
                        continue
                    todo = [d for d in deps[k] if not emitted[d]]
                    if todo:
                        todo.sort(key=lambda d: (size[d], -d))   # (the stack pops the last: the largest tree first)
                        stack.extend(todo)
                        continue
                    stack.pop()
                    place(k)

            tail = n_block
            while tail > 0 and block[tail - 1][0] in (246, 247, 249, 250, 251, 252, 253, 254, 255):   # (merges and terminators)
                tail -= 1
            first = 0   # (the label, the phis, the variables and their line marks stay first, together)
            while first < tail and block[first][0] in (248, 245, 59, 8, 317):
                emitted[first] = True
                order.append(first)
                for d in operands[first]:
                    remaining[d] -= 1
                first += 1
            for n in range(first):
                place(n, True)
            next_reader = 0
            for n in range(first, tail):
                op, w = block[n]
                if not movable(op, w):
                    if physical_writer(op, w):
                        while next_reader < len(readers) and readers[next_reader] < n:
                            emit(readers[next_reader])
                            next_reader += 1
                    emit(n)
            for n in range(tail):
                if not emitted[n]:
                    emit(n)
            for n in range(tail, n_block):
                emit(n)
            assert len(order) == n_block and order[tail:] == list(range(tail, n_block)) if tail < n_block else True
            moved += sum(1 for x, y in zip(order, range(n_block)) if x != y)
            new_body.extend(block[k] for k in order)
            block.clear()

        for op, w in body:
            if op == 248:   # (OpLabel)
                flush()
            block.append((op, w))
        flush()
        before, after = live_peak(body), live_peak(new_body)
        worst_before, worst_after = max(worst_before, before), max(worst_after, after)
        if stats and len(body) > 2000:
            print("function of %d instructions: live 32-bit values at most %d before, %d after" % (len(body), before, after))
        insts[f["first"]:f["last"] + 1] = new_body
    if worst_after >= worst_before:   # (the order is kept only when it holds fewer values at once, by this pass's own count)
        open(dst, "wb").write(data)
        print("%s: left as it was (%d values held at most, %d scheduled late)" % (dst, worst_before, worst_after))
        return 0
    res = words[:5]
    for op, w in insts:
        res += w
    open(dst, "wb").write(struct.pack("<%dI" % len(res), *res))
    print("%s: %d instructions moved" % (dst, moved))
    return 0


if __name__ == "__main__":
    sys.exit(main())
