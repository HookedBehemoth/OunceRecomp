#!/usr/bin/env python3
"""Readers of the game's frame-step time sources in the lifted C, each classified by how it uses the step.

    work/venv/bin/python -I nx2/step_readers.py [work/sa/gen] [--all] [--func f_8xxxxxxx]

Sources (the guest addresses are this lift's, HOS_GAME_VERSION 100):
- the frame-time object: **0x84c9e890 (f_83b6e300 fills it each frame from f_80706be0's step; +0 the step, +16 the ratio,
  six 48-byte entries of time groups from +0);
- the global step, the float at 0x84d61fc8 (f_83b6ea90: 60 / the nominal rate);
- the frame-time object's +28, the integer the engine makes from the step (f_83b6e300): a whole-frame count;
- the game clock, the double at +0xd58 of f_80706be0's object (by offset only: any 64-bit float load from +0xd58);
- getters: small functions that return one of the above in s0 (or the object in x0), found in a first pass.

Each function's statements are followed in text order (no control flow) and the registers holding a step, the object or
the step cut to an integer are tracked. A use is classed:
  cut       the step converted to an integer (recomp_fp_to_int), or the engine's own integer of it (+28) read: 0 for
            every step under 1, i.e. above 60 fps
  compare   the step, or the step cut to an integer, compared or used as a count (flags, branch, multiply into a count)
  scaled    float arithmetic or a store of the step (scales with the frame time: fine)
  unknown   the step read but only passed on to a call, spilled, or lost to the tracking
A function takes its worst class (cut > compare > scaled > unknown). cut and compare are the suspects above 60 fps.
Seconds to run; it reads the lifted C only."""
import glob
import os
import re
import sys

FRAME_PTR = 0x84c9e890
GLOBAL_STEP = 0x84d61fc8
CLOCK_OFF = 0xd58
WHOLE_FIELD = 28   # (f_83b6e300 writes it: the step times the ratio, cut toward zero and rounded at 0.5, as an integer)

FN = re.compile(r'^void (f_[0-9a-f]+)\(Ctx \*__restrict')
PAGE = re.compile(r'c->x\[(\d+)\]=g_module_base \+ 0x([0-9a-f]+)ULL \+ \(int64_t\)(-?\d+);')
ADDI = re.compile(r'_a=c->x\[(\d+)\], _b=(\d+)ULL; uint64_t _r=_a\+_b; c->x\[(\d+)\]=_r;')
LOAD = re.compile(r'recomp_load(8|16|32|64)\(c,\(?c->x\[(\d+)\] ?\+ ?(\d+)(?:ULL)?\)')
WX = re.compile(r'c->x\[(\d+)\]\s*=(?!=)')
WV = re.compile(r'(?:a64_vset64|a64_vst16)\(c->vreg\[(\d+)\]|memcpy\(&c->vreg\[(\d+)\]\[0\],')
RX = re.compile(r'c->x\[(\d+)\]')
RV = re.compile(r'c->vreg\[(\d+)\]')
LABEL = re.compile(r'^L_([0-9a-f]+):')
LR = re.compile(r'c->x\[30\] = 0x([0-9a-f]+)ULL')
CALL = re.compile(r'\bf_[0-9a-f]+\(c\);|A64_ICALL|TAILCALL')
FPARITH = re.compile(r'a64_fpx_(?:mul|add|sub|div|fma|fms|fnm)|recomp_f(?:mul|add|sub|div)')
RANK = {'cut': 5, 'compare': 4, 'patched': 3, 'scaled': 2, 'unknown': 1}
IMM = re.compile(r'^\s*c->x\[(\d+)\] = 0x([0-9a-f]+)ULL;')
VIMM = re.compile(r'a64_vset64\(c->vreg\[(\d+)\],0x([0-9a-f]+)ULL\)')
VFROMX = re.compile(r'memcpy\(&c->vreg\[(\d+)\]\[0\],&c->x\[(\d+)\],4\)')
FCMP = re.compile(r'a64_fcmp32\(c,(c->vreg\[\d+\]|a64_vzero),(c->vreg\[\d+\]|a64_vzero)\)')


def f32(bits):
    import struct
    return struct.unpack('<f', struct.pack('<I', bits & 0xffffffff))[0]


def statements(lines, first):
    """(line number, text) per statement: a line, or the lines of a brace block until it closes"""
    buf, depth, at = [], 0, first
    for n, line in enumerate(lines, first):
        if not buf:
            at = n
        buf.append(line)
        depth += line.count('{') - line.count('}')
        if depth <= 0:
            yield at, ''.join(buf)
            buf, depth = [], 0
    if buf:
        yield at, ''.join(buf)


def functions(gen):
    for path in sorted(glob.glob(os.path.join(gen, 'code_*.c'))):
        name, start, body = None, 0, []
        with open(path, errors='replace') as f:
            for n, line in enumerate(f, 1):
                m = FN.match(line)
                if m:
                    if name:
                        yield path, name, start, body
                    name, start, body = m.group(1), n + 1, []
                elif name:
                    if line.startswith('}') and not line.startswith('};'):
                        yield path, name, start, body
                        name = None
                    else:
                        body.append(line)
        if name:
            yield path, name, start, body


def analyse(name, start, body, getters):
    """{'uses': [(class, line, near, what)], 'fields': set, 'returns': kind or None}"""
    const = {}           # x reg -> known guest address
    imm, vimm = {}, {}   # x reg -> immediate, vreg -> float constant bits
    taint = {}           # ('x'|'v', n) -> 'ptr' | 'obj' | 'step' | 'cut'
    fields, uses, near = set(), [], name[2:]
    returns = None
    for line, st in statements(body, start):
        m = LABEL.match(st.strip()) or LR.search(st)
        if m:
            near = m.group(1)
        if st.strip().startswith('return;'):
            if taint.get(('v', 0)) == 'step':
                returns = 'step'
            elif taint.get(('x', 0)) in ('obj', 'ptr'):
                returns = taint[('x', 0)]
            continue
        m = IMM.match(st)
        if m:
            imm[int(m.group(1))] = int(m.group(2), 16)
            taint.pop(('x', int(m.group(1))), None)
            continue
        if re.match(r'^\s*c->x\[(\d+)\] &= 0xFFFFFFFFULL;', st):
            continue
        m = VFROMX.search(st)
        if m and int(m.group(2)) in imm:
            vimm[int(m.group(1))] = imm[int(m.group(2))]
            taint.pop(('v', int(m.group(1))), None)
            continue
        m = VIMM.search(st)
        if m and st.count(';') <= 2:
            vimm[int(m.group(1))] = int(m.group(2), 16)
            taint.pop(('v', int(m.group(1))), None)
            continue
        m = PAGE.search(st)
        if m:
            r = int(m.group(1))
            const[r] = int(m.group(2), 16) + int(m.group(3))
            taint.pop(('x', r), None)
            continue
        m = ADDI.search(st)
        if m and int(m.group(1)) in const:
            const[int(m.group(3))] = const[int(m.group(1))] + int(m.group(2))
            if int(m.group(3)) != int(m.group(1)):
                pass
            taint.pop(('x', int(m.group(3))), None)
            continue
        wx = [int(a) for a in WX.findall(st)]
        wv = [int(a or b) for a, b in WV.findall(st)]
        text_r = WX.sub('', st)
        text_r = re.sub(r'a64_vset64\(c->vreg\[\d+\]|a64_vst16\(c->vreg\[\d+\]|memcpy\(&c->vreg\[\d+\]\[0\],', '', text_r)
        rx = {int(a) for a in RX.findall(text_r)}
        rv = {int(a) for a in RV.findall(text_r)}
        src = None
        lm = LOAD.search(st)
        if lm:
            base, off = int(lm.group(2)), int(lm.group(3))
            addr = const.get(base, None)
            if addr is not None and addr + off == FRAME_PTR:
                src = ('ptr', None)
            elif addr is not None and addr + off == GLOBAL_STEP and wv:
                src = ('step', 'global')
            elif taint.get(('x', base)) == 'ptr':
                src = ('obj', None)
            elif taint.get(('x', base)) == 'obj':
                if wv:
                    src = ('step', '+%d' % off)
                elif lm.group(1) == '64':
                    src = ('obj', None)
                else:
                    src = ('int', '+%d' % off)
            elif off == CLOCK_OFF and lm.group(1) == '64' and wv:
                src = ('step', 'clock+0xd58')
        tainted_r = {('x', r) for r in rx if ('x', r) in taint} | {('v', r) for r in rv if ('v', r) in taint}
        kinds = {taint[k] for k in tainted_r}
        if src:
            kind, field = src
            if field:
                fields.add(field)
            if kind == 'int':
                if field == '+%d' % WHOLE_FIELD:
                    uses.append(('cut', line, near, 'the engine\'s whole-frame count +%d (0 under a step of 1)' % WHOLE_FIELD))
                    kind = 'cut'
                else:
                    uses.append(('unknown', line, near, 'integer field %s of the frame-time object' % field))
                    kind = None
            for r in wx:
                taint.pop(('x', r), None)
                if kind:
                    taint[('x', r)] = kind
            for r in wv:
                taint.pop(('v', r), None)
                if kind:
                    taint[('v', r)] = kind
            continue
        if CALL.search(st):
            if any(taint.get(('v', r)) == 'step' for r in range(8)):
                uses.append(('unknown', line, near, 'step passed to a call'))
            for k in list(taint):
                if (k[0] == 'x' and k[1] <= 18) or (k[0] == 'v' and k[1] <= 7):
                    del taint[k]
            for g, kind in getters.items():
                if g + '(c)' in st or 'TAILCALL(%s)' % g in st:
                    if kind == 'step':
                        taint[('v', 0)] = 'step'
                        fields.add('getter %s' % g)
                    else:
                        taint[('x', 0)] = kind
            continue
        new = None
        if 'step' in kinds or 'cut' in kinds:
            if 'recomp_fp_to_int' in st and 'step' in kinds:
                uses.append(('cut', line, near, 'step cut to an integer'))
                new = 'cut'
            elif 'hos_frame_whole' in st:
                uses.append(('patched', line, near, 'whole frames carried (WHOLE_SITES)'))
                new = 'cut'
            elif 'fcmp' in st:
                fm = FCMP.search(st)
                ops = [o for o in (fm.groups() if fm else ()) if not (o.startswith('c->vreg') and taint.get(('v', int(o[8:-1]))) == 'step')]
                other = ops[0] if ops else '?'
                if other == 'a64_vzero':
                    uses.append(('scaled', line, near, 'step compared with 0'))
                elif other.startswith('c->vreg') and int(other[8:-1]) in vimm:
                    uses.append(('compare', line, near, 'step compared with %g' % f32(vimm[int(other[8:-1])])))
                else:
                    uses.append(('compare', line, near, 'step compared with %s (not a constant here)' % other.replace('c->', '')))
            elif 'cut' in kinds and ('c->z' in st or 'c->n' in st or 'c->c' in st or st.lstrip().startswith('if (')):
                uses.append(('compare', line, near, 'cut step compared or counted'))
                new = 'cut'
            elif 'cut' in kinds:
                new = 'cut'
            elif FPARITH.search(st):
                uses.append(('scaled', line, near, 'float arithmetic'))
                new = 'step'
            elif 'recomp_store' in st:
                uses.append(('scaled', line, near, 'stored'))
            else:
                new = 'step'
        elif 'obj' in kinds and wx and not lm:
            new = 'obj'
        for r in wx:
            imm.pop(r, None)
        for r in wv:
            vimm.pop(r, None)
        for r in wx:
            taint.pop(('x', r), None)
            if new:
                taint[('x', r)] = new
        for r in wv:
            taint.pop(('v', r), None)
            if new:
                taint[('v', r)] = new
    return {'uses': uses, 'fields': fields, 'returns': returns}


def worst(uses):
    return max((u[0] for u in uses), key=lambda k: RANK[k], default='unknown')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    gen = args[0] if args else os.path.join(root, 'work', 'sa', 'gen')
    only = sys.argv[sys.argv.index('--func') + 1] if '--func' in sys.argv else None
    show_all = '--all' in sys.argv
    hint = re.compile(r'\+ 2192\)|\+ 4040|\+3416ULL|\+ 3416\)')
    funcs = [(p, n, s, b) for p, n, s, b in functions(gen)]
    getters, results = {}, {}
    for p, n, s, b in funcs:   # (first pass: the sources read directly, and the small getters)
        if not hint.search(''.join(b)):
            continue
        r = analyse(n, s, b, {})
        results[n] = (p, r)
        if r['returns'] and len(b) < 60:
            getters[n] = r['returns']
    if getters:   # (second pass: the getters' callers too)
        names = re.compile('|'.join(sorted(getters)))
        for p, n, s, b in funcs:
            text = ''.join(b)
            if names.search(text) or hint.search(text):
                r = analyse(n, s, b, getters)
                if r['uses'] or r['fields'] or n in results:
                    results[n] = (p, r)
    counts = {k: 0 for k in RANK}
    rows = []
    for n, (p, r) in sorted(results.items()):
        if not (r['uses'] or r['fields']):
            continue
        cls = worst(r['uses'])
        counts[cls] += 1
        rows.append((n, p, r, cls))
    print('getters: %s' % (', '.join('%s (%s)' % kv for kv in sorted(getters.items())) or 'none'))
    print('readers: %d  (cut %d, compare %d, scaled %d, unknown %d)' % (
        len(rows), counts['cut'], counts['compare'], counts['scaled'], counts['unknown']))
    for n, p, r, cls in sorted(rows, key=lambda t: (-RANK[t[3]], t[0])):
        if only and n != only:
            continue
        if not show_all and not only and RANK[cls] < RANK['compare']:
            continue
        print('%-8s %s  %s  fields %s' % (cls, n, os.path.basename(p), ' '.join(sorted(r['fields'])) or '-'))
        for u in r['uses']:
            if show_all or only or RANK[u[0]] >= RANK['compare']:
                print('    %-8s %s:%d  near 0x%s  %s' % (u[0], os.path.basename(p), u[1], u[2], u[3]))


if __name__ == '__main__':
    main()
