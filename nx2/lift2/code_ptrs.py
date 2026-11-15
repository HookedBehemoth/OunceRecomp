"""Code pointers are host function pointers (docs/lifter-output.md, "Indirect calls and jumps"): where the lifted code forms a
function's address, it gets the host function's address instead.

The shapes, and only these (the census of agent/addr-taken: every relocated value that is code lands on a function
entry; every other code address the code forms lands on an entry, a jump-table base or one local continuation):
- ADR Xd, <entry>;
- ADRP Xn, <page> followed within 4 instructions, in one block, by ADD Xd, Xn, #imm12 (no shift) with page + imm12 an
  entry, Xn not written in between.
An entry is a function of the table (a lifted function or an HLE hook), except the modules' bases: each module's DT_INIT
passes its own base to __nnmusl_init_dso as a range bound (main's at 0x81237808), and f_80020000 (main's MOD0 header,
taken for code) is a table entry. Jump-table bases, the 9 `adr xN, .` self-anchors and zstd_decompress_sequences'
local `adr x7` / `br x7` are not entries: they stay guest addresses.

sites() is the census: build_ir translates exactly its sites and stops when a site's ADD does not see the page as a
constant, or when an ADR or a 64-bit ADD / SUB immediate forms an entry's address anywhere else (build_ir.build);
the build stops when the translated count differs from the census (game.prepare), and when audit() finds a code
address formed in a way it does not know (an ADR of a text address that is no entry and no known kind, an ADRP of a
text page whose register leaves its block or is read by something other than an ADD or a load): an equality guard
`cmp xF, known` with one side untranslated would fail silently.
"""
ADD_IMM, ADD_MASK = 0x91000000, 0xFFC00000   # ADD Xd, Xn, #imm12 (64-bit, no flags, no shift)


def targets(plan):
    """the addresses a code-formed constant is translated at: the function table less the modules' bases"""
    return (set(plan["bodies"]) | set(plan["hooks"])) - set(plan.get("code_excluded", ()))


def _imm21(w):
    v = ((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3
    return v - (1 << 21) if v >> 20 else v


def _branch_target(pc, w):
    """the target of a B, BL, B.cond, CBZ/CBNZ or TBZ/TBNZ word, else None"""
    if (w & 0x7C000000) == 0x14000000:
        o = w & 0x3FFFFFF
        return pc + 4 * (o - (1 << 26) if o >> 25 else o)
    if (w & 0xFF000010) == 0x54000000 or (w & 0x7E000000) == 0x34000000:
        o = (w >> 5) & 0x7FFFF
        return pc + 4 * (o - (1 << 19) if o >> 18 else o)
    if (w & 0x7E000000) == 0x36000000:
        o = (w >> 5) & 0x3FFF
        return pc + 4 * (o - (1 << 14) if o >> 13 else o)
    return None


def _ends_block(w):
    """a word after which the lift starts a block: a branch (B, BL, B.cond, CB(N)Z, TB(N)Z, BR, BLR, RET), an exception"""
    return (_branch_target(0, w) is not None or (w & 0xFE000000) == 0xD6000000 or (w & 0xFF000000) == 0xD4000000)


def sites(pcs, words, tg, jt=None):
    """{pc: (entry, page)} of one function's body (pcs sorted, their words): the ADRs (page None) and the ADDs that form
    an entry's address. tg: targets(); jt: the plan's jump tables (their targets start blocks)"""
    from .summary import X, def_use
    out = {}
    pos = None
    for k, (pc, w) in enumerate(zip(pcs, words)):
        if (w & 0x9F000000) == 0x10000000:   # ADR
            t = pc + _imm21(w)
            if t in tg:
                out[pc] = (t, None)
            continue
        if (w & ADD_MASK) != ADD_IMM:
            continue
        n = (w >> 5) & 31
        if n == 31:
            continue
        for d in range(1, 5):
            if k < d or pcs[k - d] != pc - 4 * d:
                break
            p = words[k - d]
            if (p & 0x9F00001F) == (0x90000000 | n):   # ADRP Xn: the pair, if no block starts in between
                t = ((pc - 4 * d) & ~0xFFF) + (_imm21(p) << 12) + ((w >> 10) & 0xFFF)
                if t in tg:
                    if pos is None:
                        pos = _leaders(pcs, words, jt)
                    if not any(q in pos for q in range(pc - 4 * d + 4, pc + 4, 4)):
                        out[pc] = (t, t - ((w >> 10) & 0xFFF))
                break
            if _ends_block(p) or def_use(p)[2] & X[n]:
                break
    return out


def _leaders(pcs, words, jt):
    """the block starts the lift makes inside the body (build_ir.build: branch targets, jump-table targets, after a
    branch)"""
    body = set(pcs)
    lead = set()
    for pc, w in zip(pcs, words):
        t = _branch_target(pc, w)
        if t is not None and t in body:
            lead.add(t)
        if _ends_block(w):
            lead.add(pc + 4)
            if jt and pc in jt:
                lead.update(x for x in jt[pc] if x in body)
    return lead


def audit(pcs, words, tg, excluded, texts, jt=None):
    """The code addresses the body forms that are not sites, each of a known kind, and those of no known kind
    (pc, why): an ADR of a text address that is not an entry must be a jump-table base (its register's next reader an
    ADD of a register), one of the game's local continuations (its description's local_continuations: `adr xN,
    <label>` ... `br xN` inside one function) or a module's base (or base + 1); an ADRP of a page of text must be
    read, in its block, by ADDs or loads with that register as base before it is written again (so no entry's address
    can be formed from it elsewhere). Returns ({kind: count}, [(pc, why)])."""
    from .summary import X, def_use
    import gamedesc
    continuations = gamedesc.current().local_continuations
    in_text = lambda a: any(lo <= a < hi for lo, hi in texts)
    kinds, odd = {}, []
    pos = None
    for k, (pc, w) in enumerate(zip(pcs, words)):
        if (w & 0x9F000000) == 0x10000000:   # ADR
            t = pc + _imm21(w)
            if t in tg or not in_text(t):
                continue
            n = w & 31
            if t in excluded or t - 1 in excluded:
                kind = "module base"
            elif continuations.get(pc) == w:
                kind = "local continuation"
            else:
                kind = None
                for j in range(k + 1, min(k + 6, len(pcs))):
                    du = def_use(words[j])
                    if du[1] & X[n]:
                        u = words[j]
                        if (u & 0xFF200000) in (0x8B000000, 0x8B200000):   # ADD (shifted / extended register)
                            kind = "jump-table base"
                        break
                    if du[2] & X[n]:
                        break
            if kind is None:
                odd.append((pc, "ADR of %#x, not an entry" % t))
            else:
                kinds[kind] = kinds.get(kind, 0) + 1
            continue
        if (w & 0x9F000000) != 0x90000000 or (w & 31) == 31:
            continue
        n = w & 31
        page = (pc & ~0xFFF) + (_imm21(w) << 12)
        if not any(lo & ~0xFFF <= page < hi for lo, hi in texts):
            continue
        if pos is None:
            pos = _leaders(pcs, words, jt)
        why = None
        j = k + 1
        while why is None:
            if j >= len(pcs) or pcs[j] != pcs[j - 1] + 4 or pcs[j] in pos:
                why = "its page is live at a block's end"
                break
            u = words[j]
            du = def_use(u)
            if du[1] & X[n] and not (((u & 0xFFC00000) == 0x91000000 or (u & 0x3B000000) == 0x39000000)
                                     and ((u >> 5) & 31) == n):
                why = "its page is read by %08x at %#x" % (u, pcs[j])
            elif du[2] & X[n]:
                break
            elif _ends_block(u):
                why = "its page is live at a branch"
            j += 1
        if why:
            odd.append((pc, "ADRP of the text page %#x: %s" % (page, why)))
        else:
            kinds["text page consumed"] = kinds.get("text page consumed", 0) + 1
    return kinds, odd


def count(plan, names, texts):
    """(ADR sites, ADD sites, {kind: count} of the other code addresses formed, [(pc, why)] of no known kind) over
    these functions"""
    tg = targets(plan)
    excluded = set(plan.get("code_excluded", ()))
    jt = plan["jt"]
    a = p = 0
    kinds, odd = {}, []
    for e in names:
        pcs, words = plan["bodies"][e]
        pcs, words = list(pcs), list(words)
        for _, page in sites(pcs, words, tg, jt).values():
            if page is None:
                a += 1
            else:
                p += 1
        k, o = audit(pcs, words, tg, excluded, texts, jt)
        for kk, v in k.items():
            kinds[kk] = kinds.get(kk, 0) + v
        odd += o
    return a, p, kinds, odd

