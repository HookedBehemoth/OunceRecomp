"""Minimal read-only AINB (AI node graph) reader, versions 0x404-0x408.

Layout follows the public description of the format by the TOTK modding community
(dt-12345/ainb). Expressions are not disassembled; only their indices are reported.

    import ainb; g = ainb.load(open(path,'rb').read()); print(ainb.to_text(g))
"""
import struct, sys, json

NODE_TYPES = {0: 'UserDefined', 1: 'S32Selector', 2: 'Sequential', 3: 'Simultaneous', 4: 'F32Selector',
              5: 'StringSelector', 6: 'RandomSelector', 7: 'BoolSelector', 8: 'Fork', 9: 'Join', 10: 'Alert',
              20: 'Expression', 100: 'ModuleIF_Input_S32', 101: 'ModuleIF_Input_F32', 102: 'ModuleIF_Input_Vec3f',
              103: 'ModuleIF_Input_String', 104: 'ModuleIF_Input_Bool', 105: 'ModuleIF_Input_Ptr',
              200: 'ModuleIF_Output_S32', 201: 'ModuleIF_Output_F32', 202: 'ModuleIF_Output_Vec3f',
              203: 'ModuleIF_Output_String', 204: 'ModuleIF_Output_Bool', 205: 'ModuleIF_Output_Ptr',
              300: 'ModuleIF_Child', 400: 'StateEnd', 500: 'SplitTiming'}
PTYPES = ['Int', 'Bool', 'Float', 'String', 'Vec3f', 'Ptr']
PLUGS = ['Generic', '_01', 'Child', 'Transition', 'String', 'Int', '_06', '_07', '_08', '_09']
PROP_SIZE = [0xc, 0xc, 0xc, 0xc, 0x14, 0xc]
IN_SIZE = [0x10, 0x10, 0x10, 0x10, 0x18, 0x14]


class R:
    def __init__(self, d):
        self.d = d
        self.p = 0
        sp = struct.unpack_from('<I', d, 0x24)[0]
        self.strs = {}
        off = sp
        for s in d[sp:].split(b'\0'):
            self.strs[off - sp] = s.decode('utf-8', 'replace')
            off += len(s) + 1

    def u(self, fmt, at=None):
        if at is not None:
            self.p = at
        v = struct.unpack_from('<' + fmt, self.d, self.p)
        self.p += struct.calcsize('<' + fmt)
        return v if len(v) > 1 else v[0]

    def s(self, off=None):
        if off is None:
            off = self.u('I')
        return self.strs.get(off, f'<str@{off:#x}>')

    def guid(self):
        b = self.d[self.p:self.p + 16]
        self.p += 16
        a, b1, c = struct.unpack_from('<IHH', b)
        return f'{a:08x}-{b1:04x}-{c:04x}-{b[8:10].hex()}-{b[10:].hex()}'


def _flag(f):
    o = {}
    if f & 0x800000:
        o['default'] = True
    if f & 0xc2000000 == 0xc2000000:
        o['expr'] = f & 0xffff
    elif f & 0xc2000000:
        o['bb'] = f & 0xffff
        c = f >> 0x1a & 3
        if c:
            o['comp'] = 'xyz'[c - 1]
    return o


def _val(r, t, prop):
    if t == 0: return r.u('i')
    if t == 1: return r.u('I') != 0
    if t == 2: return round(r.u('f'), 6)
    if t == 3: return r.s()
    if t == 4: return [round(x, 6) for x in r.u('3f')]
    if t == 5:
        if not prop:
            r.u('I')
        return None


def load(data):
    r = R(data)
    assert data[:4] == b'AIB ', 'not AINB'
    ver = r.u('I', 4)
    (fn, ncmd, nnode, nquery, natt, nout, bb_off, sp_off) = r.u('8I')
    (enum_off, prop_off, trans_off, io_off, multi_off, att_off, atti_off, expr_off, repl_off, query_off,
     x50, x54, x58, mod_off, cat_name, cat, action_off, x6c, bbid_off) = r.u('19I')
    g = {'file': r.s(fn), 'version': hex(ver), 'category': r.s(cat_name)}
    cmds = []
    for _ in range(ncmd):
        name = r.s(); gd = r.guid(); root = r.u('H'); sec = r.u('H') - 1
        cmds.append({'name': name, 'guid': gd, 'root': root, **({'root2': sec} if sec >= 0 else {})})
    node_off = r.p

    # blackboard
    r.p = bb_off
    bbtypes = ['String', 'S32', 'U32', 'F32', 'Bool', 'Vec3f', 'VoidPtr']
    hdrs = []
    for i, t in enumerate(bbtypes):
        if t == 'U32' and ver < 0x408:
            hdrs.append((0, 0, 0)); continue
        c, bi, o, _ = r.u('4H'); hdrs.append((c, bi, o))
    infos = []
    for c, _, _ in hdrs:
        lst = []
        for _ in range(c):
            f = r.u('I'); notes = r.s()
            lst.append((r.s(f & 0x3fffff), notes, (f >> 0x18 & 0x7f) if f >> 31 else -1))
        infos.append(lst)
    base = r.p
    fref = base + hdrs[5][2] + hdrs[5][0] * 0xc
    bb = []
    for ti, t in enumerate(bbtypes):
        r.p = base + hdrs[ti][2]
        for name, notes, fr in infos[ti]:
            v = {'String': lambda: r.s(), 'S32': lambda: r.u('i'), 'U32': lambda: r.u('I'),
                 'F32': lambda: round(r.u('f'), 6), 'Bool': lambda: r.u('I') != 0,
                 'Vec3f': lambda: list(r.u('3f')), 'VoidPtr': lambda: None}[t]()
            e = {'type': t, 'idx': len([b for b in bb if b['type'] == t]), 'name': name, 'default': v}
            if notes: e['notes'] = notes
            if fr >= 0:
                e['file'] = r.s(struct.unpack_from('<I', data, fref + 0x10 * fr)[0])
            bb.append(e)
    g['blackboard'] = bb

    # properties (immediate params)
    offs = r.u('6I', prop_off)
    ends = list(offs[1:]) + [io_off]
    props = []
    for t in range(6):
        r.p = offs[t]; lst = []
        for _ in range((ends[t] - offs[t]) // PROP_SIZE[t]):
            name = r.s(); cls = r.s() if t == 5 else None
            f = r.u('I'); vo = r.p; v = _val(r, t, True)
            e = {'name': name, 'value': v, **_flag(f), '_off': vo}
            if cls: e['class'] = cls
            lst.append(e)
        props.append(lst)

    # multi sources
    multi = []
    r.p = multi_off
    for _ in range((trans_off - multi_off) // 8):
        n, o, f = r.u('hhI'); multi.append({'node': n, 'out': o, **_flag(f)})

    # io params
    hd = [r.u('2I', io_off + 8 * t) for t in range(6)]
    ins, outs = [], []
    for t in range(6):
        ie, oe = hd[t]
        oend = hd[t + 1][0] if t < 5 else multi_off
        r.p = ie; lst = []
        for _ in range((oe - ie) // IN_SIZE[t]):
            name = r.s(); cls = r.s() if t == 5 else None
            n, o, f = r.u('hhI'); vo = r.p; v = _val(r, t, False)
            e = {'name': name, 'default': v, '_off': vo}
            if cls: e['class'] = cls
            if n <= -100:
                e['sources'] = multi[-100 - n: -100 - n + o]
            elif n >= 0 or o < 0 or f:
                src = _flag(f)
                if o < 0 and n >= 0:
                    src['bb_input'] = True; o &= 0x7fff
                if n >= 0:
                    src['node'] = n; src['out'] = o
                src.pop('default', None)
                if src: e['src'] = src
            lst.append(e)
        ins.append(lst)
        r.p = oe; lst = []
        for _ in range((oend - oe) // (8 if t == 5 else 4)):
            f = r.u('I'); e = {'name': r.s(f & 0x3fffffff)}
            if t == 5: e['class'] = r.s()
            if f >> 31: e['linked'] = True
            lst.append(e)
        outs.append(lst)

    # transitions
    trans = []
    if trans_off < query_off:
        r.p = trans_off
        tof = [r.u('I')]
        while r.p < tof[0]:
            tof.append(r.u('I'))
        for o in tof:
            f = r.u('I', o)
            trans.append({'type': f & 0xff, **({'cmd': r.s()} if f & 0xff == 0 else {})})

    # attachments
    atts = []
    for i in range(natt):
        r.p = att_off + i * (0x10 if ver > 0x404 else 0xc)
        name = r.s(); po = r.u('I'); r.u('HH')
        r.p = po; r.u('I'); ap = {}
        for t in range(6):
            bi, c = r.u('2I')
            if c: ap[PTYPES[t]] = props[t][bi:bi + c]
        atts.append({'name': name, 'props': ap})
    atti = [struct.unpack_from('<I', data, atti_off + 4 * i)[0] for i in range((att_off - atti_off) // 4)]

    queries = []
    end = expr_off if expr_off else mod_off
    r.p = query_off
    while r.p + 4 <= end and r.p < len(data):
        queries.append(r.u('HH')[0])

    actions = {}
    n = r.u('I', action_off)
    for _ in range(n):
        idx = r.u('i'); a, b = r.s(), r.s()
        actions.setdefault(idx, []).append({'slot': a, 'action': b})

    n = r.u('I', mod_off)
    g['modules'] = [{'path': r.s(), 'category': r.s(), 'count': r.u('I')} for _ in range(n)]
    g['commands'] = cmds

    nodes = []
    nsize = 0x3c if ver > 0x404 else 0x38
    for i in range(nnode):
        r.p = node_off + i * nsize
        typ, idx, natt_n, flags, _ = r.u('HhHBB')
        name = r.s()
        if ver >= 0x407: r.u('I')
        r.u('I'); po = r.u('I'); nexpr, exio, nmulti, _ = r.u('4H')
        batt = r.u('I'); bq, nq = r.u('HH'); r.u('I'); gd = r.guid()
        nd = {'idx': idx, 'type': NODE_TYPES.get(typ, typ), 'name': name}
        fl = [n for b, n in ((1, 'query'), (2, 'module'), (4, 'root'), (8, 'multi2')) if flags & b]
        if fl: nd['flags'] = fl
        nd['guid'] = gd
        r.p = po
        np = {}
        for t in range(6):
            bi, c = r.u('2I')
            if c: np[PTYPES[t]] = props[t][bi:bi + c]
        if np: nd['props'] = np
        ni, no = {}, {}
        for t in range(6):
            bi, c, bo, co = r.u('4I')
            if c: ni[PTYPES[t]] = ins[t][bi:bi + c]
            if co: no[PTYPES[t]] = outs[t][bo:bo + co]
        if ni: nd['inputs'] = ni
        if no: nd['outputs'] = no
        pinfo = [r.u('BB') for _ in range(10)]
        pbase = r.p
        plugs = {}
        for pt, (cnt, bi) in enumerate(pinfo):
            if not cnt: continue
            ol = [struct.unpack_from('<I', data, pbase + (bi + k) * 4)[0] for k in range(cnt)]
            lst = []
            for k, o in enumerate(ol):
                last = k == cnt - 1
                r.p = o
                nidx = r.u('i')
                if pt == 3:
                    ti = r.u('I'); lst.append({'node': nidx, **(trans[ti] if ti < len(trans) else {'t': ti}), '_off': o}); continue
                e = {'node': nidx, 'name': r.s(), '_off': o}
                tn = nd['type']
                if pt == 2:
                    if tn == 'S32Selector':
                        bi2, fl2 = r.u('hH')
                        if fl2 >> 15: e['bb'] = bi2
                        if last: e['default'] = True
                        else: e['cond'] = r.u('i')
                    elif tn == 'StringSelector':
                        bi2, fl2 = r.u('hH')
                        if fl2 >> 15: e['bb'] = bi2
                        if last: e['default'] = True
                        else: e['cond'] = r.s()
                    elif tn == 'F32Selector':
                        if last: e['default'] = True
                        else:
                            a1, f1, v1, a2, f2, v2 = r.u('hHfhHf')
                            e['min'] = ('bb%d' % a1) if f1 >> 15 else round(v1, 4)
                            e['max'] = ('bb%d' % a2) if f2 >> 15 else round(v2, 4)
                    elif tn == 'RandomSelector':
                        r.u('hH'); e['weight'] = round(r.u('f'), 4)
                elif pt in (4, 5) and tn in ('StringSelector', 'S32Selector', 'Expression') and ver >= 0x407:
                    r.u('I'); e['default'] = r.s() if pt == 4 else r.u('i')
                lst.append(e)
            plugs[PLUGS[pt]] = lst
        if plugs: nd['plugs'] = plugs
        if natt_n: nd['attachments'] = [atts[atti[k]] for k in range(batt, batt + natt_n)]
        if nq: nd['queries'] = queries[bq:bq + nq]
        if nexpr: nd['n_expr'] = nexpr
        if idx in actions: nd['actions'] = actions[idx]
        nodes.append(nd)
    qidx = [n['idx'] for n in nodes if 'query' in n.get('flags', [])]
    for n in nodes:
        if 'queries' in n:
            n['queries'] = [qidx[q] if q < len(qidx) else q for q in n['queries']]
    g['nodes'] = nodes
    g['has_expressions'] = bool(expr_off)
    return g


def _fmtsrc(e, nodes):
    s = e.get('src')
    if 'sources' in e:
        return ' <- multi[' + ', '.join(f"#{x['node']}.{x['out']}" for x in e['sources']) + ']'
    if not s: return ''
    if 'node' in s:
        return f" <- #{s['node']}.out{s['out']}" + (' (bb)' if s.get('bb_input') else '')
    if 'bb' in s: return f" <- bb[{s['bb']}]" + (s.get('comp', ''))
    if 'expr' in s: return f" <- expr{s['expr']}"
    return ''


def to_text(g):
    L = [f"# {g['file']}  ({g['category']}, v{g['version']})"]
    for c in g['commands']:
        L.append(f"COMMAND {c['name']} -> #{c['root']}" + (f" / #{c['root2']}" if 'root2' in c else ''))
    if g['blackboard']:
        L.append('BLACKBOARD:')
        for b in g['blackboard']:
            L.append(f"  {b['type']}[{b['idx']}] {b['name']} = {b['default']!r}" + (f"  file={b['file']}" if 'file' in b else ''))
    if g['modules']:
        L.append('MODULES: ' + ', '.join(m['path'] for m in g['modules']))
    for n in g['nodes']:
        L.append(f"\n#{n['idx']} [{n['type']}] {n['name']}" + (f"  {{{','.join(n['flags'])}}}" if 'flags' in n else ''))
        for t, ps in n.get('props', {}).items():
            for p in ps:
                src = f" <- bb[{p['bb']}]" if 'bb' in p else (f" <- expr{p['expr']}" if 'expr' in p else '')
                L.append(f"    prop {t} {p['name']} = {p['value']!r}{src}")
        for t, ps in n.get('inputs', {}).items():
            for p in ps:
                L.append(f"    in   {t} {p['name']} = {p['default']!r}{_fmtsrc(p, g['nodes'])}")
        for t, ps in n.get('outputs', {}).items():
            for p in ps:
                L.append(f"    out  {t} {p['name']}" + (' (linked)' if p.get('linked') else ''))
        for pt, ps in n.get('plugs', {}).items():
            for p in ps:
                extra = {k: v for k, v in p.items() if k not in ('node', 'name', '_off')}
                L.append(f"    -> {pt} {p.get('name', '')!s} #{p['node']}" + (f" {extra}" if extra else ''))
        for a in n.get('attachments', []):
            L.append(f"    attach {a['name']} {json.dumps(a['props'], ensure_ascii=False)}")
        for a in n.get('actions', []):
            L.append(f"    action {a['slot']}: {a['action']}")
        if 'queries' in n: L.append(f"    queries {n['queries']}")
        if 'n_expr' in n: L.append(f"    ({n['n_expr']} expressions)")
    return '\n'.join(L)


if __name__ == '__main__':
    g = load(open(sys.argv[1], 'rb').read())
    print(to_text(g))
