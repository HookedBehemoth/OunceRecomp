"""meshlab cpu_draw: a captured task+mesh draw run on the CPU, no device. The task stage (which meshlets it keeps, compared with
the map the GPU left in the capture), then the mesh stage of every kept group (clip positions, triangles, how much of the
target they cover), from the stages' IR prints (meshtool's <prefix>.pre.c, run by irpy.py) and the capture's memory, with the
uniform buffers and the model's buffer optionally overwritten first: what a stale or garbage input would make the draw do.
For a draw whose task stage is of the meshlet family (2c56dc1006942f74: the model's buffer in the task stage's slot 0).

    cpu_draw.py <frame.bin> <seq> --task-ir <task.pre.c> --mesh-ir <mesh.pre.c> [--groups N] [--no-mesh] [--model-slot N]
                [--block B] [--against <other mesh.pre.c> --against-slot N --against-block B --lines N] [name=value ...]
      c3.<off>=<u32>   f.c4.<off>=<float>   fill.c4.<lo>.<hi>=<float>   scale.c4.<lo>.<hi>=<float>     both stages' copies
      grid=<float>                          every cell of the task stage's depth grid (c3 + 3120)
      model=zero | random[:seed] | blob:<id> | shift:<bytes>     the model buffer's bytes replaced
      model.<off>=<u32>                     one word of it
    --block: the bytes of the stage buffer a block of 32 vertices takes: 128 times the stage's output components (meshtool's
    vertex_lines; default 3584 = 28 components).
    --against: a second mesh stage (reading the model from --against-slot) run on the same groups, its first --lines output
    components, indices and primitive count compared with the first stage's.

Nothing of the game is in this file; its inputs (captures, IR prints) are local and stay under work/.
"""
import math
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import irpy
from capture import Cap


def main():
    c = Cap(sys.argv[1])
    seq = int(sys.argv[2])
    d = [x for x in c.draws if x['seq'] == seq and x['mesh']][0]
    args = sys.argv[3:]
    ngroups = None
    do_mesh = True
    ov = []
    task_ir = mesh_ir = against = None
    model_slot, against_slot, lines = 2, 0, 16
    block = against_block = 3584
    while args:
        a = args.pop(0)
        if a == '--groups': ngroups = int(args.pop(0))
        elif a == '--no-mesh': do_mesh = False
        elif a == '--task-ir': task_ir = args.pop(0)
        elif a == '--mesh-ir': mesh_ir = args.pop(0)
        elif a == '--model-slot': model_slot = int(args.pop(0))
        elif a == '--against': against = args.pop(0)
        elif a == '--against-slot': against_slot = int(args.pop(0))
        elif a == '--lines': lines = int(args.pop(0))
        elif a == '--block': block = int(args.pop(0))
        elif a == '--against-block': against_block = int(args.pop(0))
        else: ov.append(a)

    def ubo(stage, idx):
        u = [u for u in d['ubos'] if u['stage'] == stage and u['index'] == idx]
        return bytearray(c.blob(u[0]['blob'])) if u and u[0]['blob'] != 0xffffffff else bytearray()
    cbs = {st: {i: ubo(st, i) for i in range(8)} for st in (6, 7)}
    model_g = struct.unpack_from('<Q', cbs[6][0], 0x670)[0]
    model_i = [s for s in d['ssbos'] if s['stage'] == 6 and s['slot'] == 0][0]['blob']
    overlay = {}   # (guest base -> bytes: memory overwritten for this run)
    for a in ov:
        k, v = a.split('=')
        if k == 'model':
            cur = bytearray(c.blob(model_i))
            if v == 'zero': cur = bytearray(len(cur))
            elif v.startswith('random'): cur = bytearray(random.Random(int(v[7:] or 1)).randbytes(len(cur)))
            elif v.startswith('blob:'):
                src = c.blob(int(v[5:]))
                cur = bytearray((src * (len(cur) // len(src) + 1))[:len(cur)])
            elif v.startswith('shift:'):
                n = int(v[6:])
                cur = cur[n:] + cur[:n]
            overlay[model_g] = cur
        elif k.startswith('model.'):
            overlay.setdefault(model_g, bytearray(c.blob(model_i)))
            struct.pack_into('<I', overlay[model_g], int(k[6:]), int(v, 0))
        elif k == 'grid':
            gw, gh = struct.unpack_from('<2I', cbs[6][3], 3088)
            for i in range(gw * gh): struct.pack_into('<f', cbs[6][3], 3120 + 4 * i, float(v))
        else:
            p = k.split('.')
            kind = p[0] if p[0] in ('f', 'fill', 'scale') else 'u'
            p = p[1:] if kind != 'u' else p
            for buf in (cbs[6][int(p[0][1:])], cbs[7][int(p[0][1:])]):
                lo = int(p[1])
                hi = int(p[2]) if len(p) > 2 else lo + 4
                for o in range(lo, min(hi, len(buf) - 3), 4):
                    if kind == 'u': struct.pack_into('<I', buf, o, int(v, 0))
                    elif kind == 'scale': struct.pack_into('<f', buf, o, struct.unpack_from('<f', buf, o)[0] * float(v))
                    else: struct.pack_into('<f', buf, o, float(v))

    class Env:
        ballot_value = 0
        missing = 0

        def __init__(s, stage, table):
            s.cb = cbs[stage]
            s.table = table

        def cbuf(s, b, o): return struct.unpack_from('<I', s.cb[b], o)[0] if o + 4 <= len(s.cb[b]) else 0

        def glob(s, a):
            for g, b in overlay.items():
                if g <= a and a + 4 <= g + len(b): return struct.unpack_from('<I', b, a - g)[0]
            v = c.u32(a)
            if v is None:
                s.missing += 1
                return 0
            return v

        def storage(s, i, o): return s.glob((struct.unpack_from('<Q', s.cb[0], s.table + 16 * i)[0] & ~3) + o)

    # the task stage: one lane a meshlet; the lane's ballot argument is "kept", its payload write the meshlet's number
    trun, _ = irpy.compile_ir(task_ir)
    e = Env(6, 0x670)
    e.isbe_read = lambda o: e.wg
    ops = irpy.Ops(e)
    kept, per = [], []
    for wg in range(d['y']):
        n = 0
        for lane in range(32):
            e.wg, e.lane, e.ballot, e.writes = wg, lane, [], []
            trun(ops)
            if e.ballot[0]:
                n += 1
                kept.append([x for o, x in e.writes if o != 4][0])
        per.append(n)
    print('task: kept', len(kept), 'of', 32 * d['y'], 'lanes; per task', per, '; loads of memory not captured', e.missing)
    if not ov:
        pw = d['header'][13]
        gpu = sorted(d['payload'][(m >> 8) * pw + 4 + (m & 255)] for m in d['map'])
        print('  the capture\'s map (the GPU\'s): %d groups; the same meshlets: %s' % (len(gpu), sorted(kept) == gpu))
    if not do_mesh: return

    W, H = d['w'], d['h']

    def clip_area(tri):
        """the triangle clipped to NVN's clip volume (-w <= x, y, z <= w): its area on the target in pixels, and whether all
        of it is at depth 1 after the back end's depth conversion"""
        poly = tri
        for ax, sg in ((0, 1), (0, -1), (1, 1), (1, -1), (2, 1), (2, -1)):
            out = []
            for i in range(len(poly)):
                a, b = poly[i], poly[(i + 1) % len(poly)]
                da, db = a[3] + sg * a[ax], b[3] + sg * b[ax]
                if da >= 0: out.append(a)
                if (da >= 0) != (db >= 0):
                    t = da / (da - db)
                    out.append([a[k] + t * (b[k] - a[k]) for k in range(4)])
            poly = out
            if len(poly) < 3: return 0.0, False
        poly = [q for q in poly if q[3] > 0]
        if len(poly) < 3: return 0.0, False
        pts = [((q[0] / q[3] * 0.5 + 0.5) * W, (0.5 - q[1] / q[3] * 0.5) * H) for q in poly]
        a2 = sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts)))
        far = all(irpy.r32(irpy.r32((q[2] + q[3]) * 0.5) / q[3]) >= 1.0 for q in poly)
        return abs(a2) / 2, far

    def run_mesh(run, env, o, meshlet, blk):
        env.isbe_read = lambda off: 0 if off == 0 else meshlet   # (a payload of one word: this group's meshlet)
        attr, idx, prims = {}, {}, None
        for lane in range(32):
            env.lane, env.ballot, env.writes, env.map8, env.map32 = lane, [], [], [], []
            run(o)
            # (the stage buffer: component k of vertex v at (v / 32) * block + k * 128 + (v % 32) * 4; index bytes from 4)
            for off, x in env.writes: attr[(off // blk * 32 + (off % 128) // 4, (off % blk) // 128)] = x
            for off, x in env.map8: idx[off - 4] = x
            for off, x in env.map32: prims = x
        return attr, idx, prims

    mrun, _ = irpy.compile_ir(mesh_ir)
    e = Env(7, 0x770)
    e.ballot_value = 1
    ops = irpy.Ops(e)
    if against:
        arun, _ = irpy.compile_ir(against)
        cbs[8] = {k: bytearray(v) for k, v in cbs[7].items()}
        cbs[8][0][0x770 + 16 * against_slot:0x780 + 16 * against_slot] = cbs[7][0][0x770 + 16 * model_slot:0x780 + 16 * model_slot]
        e2 = Env(8, 0x770)
        e2.ballot_value = 1
        ops2 = irpy.Ops(e2)
        same = different = 0
    st = dict(groups=0, triangles=0, not_finite=0, outside=0, drawn=0, area=0.0, at_depth_1=0, area_at_depth_1=0.0, over_quarter_target=0, largest_coordinate=0.0)
    for meshlet in kept[:ngroups]:
        attr, idx, prims0 = run_mesh(mrun, e, ops, meshlet, block)
        st['groups'] += 1
        for p in range(min(prims0 or 0, 126)):
            tri = [[irpy.u2f(attr.get((min(idx.get(3 * p + j, 0), 125), k), 0)) for k in range(4)] for j in range(3)]
            st['triangles'] += 1
            if any(x != x or math.isinf(x) for vtx in tri for x in vtx):
                st['not_finite'] += 1
                continue
            st['largest_coordinate'] = max(st['largest_coordinate'], max(abs(x) for vtx in tri for x in vtx))
            area, far = clip_area(tri)
            if area <= 0.0:
                st['outside'] += 1
                continue
            st['drawn'] += 1
            st['area'] += area
            if far:
                st['at_depth_1'] += 1
                st['area_at_depth_1'] += area
            if area > 0.25 * W * H: st['over_quarter_target'] += 1
        if against:
            attr2, idx2, prims2 = run_mesh(arun, e2, ops2, meshlet, against_block)
            ok = prims2 == prims0 and idx == idx2 and {k: v for k, v in attr.items() if k[1] < lines} == {k: v for k, v in attr2.items() if k[1] < lines}
            same += ok
            if not ok and not different:
                bad = sorted({k[1] for k in set(attr) | set(attr2) if k[1] < lines and attr.get(k) != attr2.get(k)})
                print('  first difference: meshlet', meshlet, 'count', prims0, prims2, 'indices equal', idx == idx2, 'components', bad)
            different += not ok
    print('mesh:', st, '; loads of memory not captured', e.missing)
    if against: print('  against %s: %d groups equal in the first %d components, the indices and the count; %d different' % (against, same, lines, different))
    print('  coverage (the clipped triangles\' areas summed, both windings, no depth test): %.3f targets; at depth 1.0: %.3f targets'
          % (st['area'] / (W * H), st['area_at_depth_1'] / (W * H)))


if __name__ == '__main__':
    main()
