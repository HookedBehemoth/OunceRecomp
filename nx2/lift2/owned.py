"""The functions the present lifter still owns in the mixed build (milestone 19, step 1).

The present lifter translates and writes C only for these: every other function comes from lift 2 (its f_<addr> thunk
and its code), so translating it was work thrown away. Owned:
- functions lift 2 cannot take (the plan's bodies lack them: scope, hooks, C added after an instruction);
- functions a host edit of the C reaches, found from the edits' own sources, not from the C: the guest addresses in
  nx2/oot_patches.py (function heads f_<addr>, labels L_<addr>, return addresses and constants 0x<addr>: the
  function holding the address, and for a return address the call before it) but the functions whose patch lift 2
  makes as a probe (nx2/lift2/probes.py), and the SPLICE_AFTER addresses of host/*.c (nx2/entry_hooks.py) but the
  ones lift 2 calls at their instructions (probes.SPLICE_AT). C after an instruction is not an edit either. An
  ENTRY_HOOK is not an edit: lift 2 makes it (mixed.lift_part, from entry_hooks.wanted). Over-approximate on purpose: an owned function lift 2 could take
  costs a little C, a missed one would lose its edit (the build's edited set is checked against this one:
  check()).
- with HOOKSHOT_LIFT2=<file>, everything not listed.
HOOKSHOT_LIFT1_ALL=1: the present lifter writes every function as before.
"""
import os
import re

PATCH_SOURCES = ("oot_patches.py",)
ADDR = re.compile(r"\b(?:f_|L_|0x)(8[0-9a-f]{7})\b", re.I)


def edit_addresses(gen):
    """guest addresses the host edits of the C name (see the module's description)"""
    import entry_hooks
    nx2 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = set()
    for name in PATCH_SOURCES:
        out.update(int(m.group(1), 16) for m in ADDR.finditer(open(os.path.join(nx2, name)).read()))
    from . import probes
    out -= set(probes.BY_FUNC)
    out.update(set(entry_hooks.wanted_splices()) - set(probes.SPLICE_AT))   # (SPLICE_AT: lift 2 calls those)
    return out


def compute(funcs, bodies, gen, listed=None):
    """the owned entries: funcs, every function (entry -> its pcs); bodies, the ones lift 2 can take; listed, the
    HOOKSHOT_LIFT2 file's functions or None (all-safe)"""
    addrs = edit_addresses(gen)
    near = addrs | {a - 4 for a in addrs}
    edited = set()
    for e, pcs in funcs.items():
        if e in addrs or not near.isdisjoint(pcs):
            edited.add(e)
    taken = set(bodies) - edited
    if listed is not None:
        taken &= set(listed)
    return set(funcs) - taken, edited


FILE = "lift2_owned.txt"


def save(out_dir, owned, predicted, hot):
    """the lift's decision for the build (mixed.prepare): owned functions, the ones with predicted edits ("e"), and
    the hot list's functions ("h": the hot partitions, now that most hot functions have no present-lifter C)"""
    with open(os.path.join(out_dir, FILE + ".tmp"), "w") as f:
        f.write("# the present lifter's functions in the mixed build (nx2/lift2/owned.py): o owned, e edited, h hot\n")
        f.write("".join("o %08x\n" % e for e in sorted(owned)))
        f.write("".join("e %08x\n" % e for e in sorted(predicted)))
        f.write("".join("h %s\n" % h[2:] for h in sorted(hot)))
    os.replace(os.path.join(out_dir, FILE + ".tmp"), os.path.join(out_dir, FILE))


def load(out_dir):
    """(owned, predicted edits, hot) as save() wrote them, or None (a lift that wrote every function)"""
    path = os.path.join(out_dir, FILE)
    if not os.path.exists(path):
        return None
    got = {"o": set(), "e": set(), "h": set()}
    for line in open(path):
        if line[:1] in got:
            got[line[0]].add(int(line.split()[1], 16))
    return got["o"], got["e"], got["h"]


def check(gen_edited, predicted):
    """the functions the build found edited in the C that the prediction missed (each one lost its edit)"""
    return sorted(set(gen_edited) - set(predicted))
