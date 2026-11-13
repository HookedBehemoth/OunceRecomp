"""Patch an AINB in place-size (except appended strings) and write a copy. Local use only: outputs are game-derived.

usage: python tools/dump/ainb_patch.py <in.ainb> <out.ainb> op [op ...]
  in:<node>:<Type>:<Name>=<value>      set an input parameter's default (Type: Int Bool Float String)
  prop:<node>:<Type>:<Name>=<value>    set an immediate property
  plug:<node>:<PlugType>:<k>=<newnode> retarget the k-th plug of that type (Child, Transition, ...)
  unlink:<node>:<Type>:<Name>=<value>  cut an input from its source node/blackboard and give it a constant
Strings not already in the pool are appended to it (the pool is the file's last section).
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ainb


def patch(data, ops):
    data = bytearray(data)
    g = ainb.load(bytes(data))
    sp = struct.unpack_from('<I', data, 0x24)[0]
    pool = {}
    off = 0
    for s in bytes(data[sp:]).split(b'\0'):
        pool.setdefault(s.decode('utf-8', 'replace'), off); off += len(s) + 1

    def stroff(s):
        if s in pool: return pool[s]
        # append at the end; the pool's trailing NUL run may be padding, so write after the last byte
        o = len(data) - sp
        data.extend(s.encode() + b'\0')
        pool[s] = o
        return o

    def pack(t, v):
        if t == 'Int': return struct.pack('<i', int(v, 0))
        if t == 'Bool': return struct.pack('<I', 1 if v.lower() in ('1', 'true') else 0)
        if t == 'Float': return struct.pack('<f', float(v))
        if t == 'String': return struct.pack('<I', stroff(v))
        raise ValueError(t)

    for op in ops:
        kind, rest = op.split(':', 1)
        lhs, val = rest.split('=', 1)
        n, t, name = lhs.split(':', 2)
        node = g['nodes'][int(n)]
        if kind in ('in', 'prop', 'unlink'):
            lst = node.get('props' if kind == 'prop' else 'inputs', {}).get(t, [])
            hit = [p for p in lst if p['name'] == name]
            assert hit, f'{op}: no such parameter on #{n} ({node["name"]})'
            data[hit[0]['_off']:hit[0]['_off'] + 4] = pack(t, val)
            if kind == 'unlink':  # source: s16 node, s16 output, u32 flags, just before the value
                struct.pack_into('<hhI', data, hit[0]['_off'] - 8, -1, 0, 0)
        elif kind == 'plug':
            pl = node.get('plugs', {}).get(t, [])
            k = int(name)
            assert k < len(pl), f'{op}: #{n} has {len(pl)} {t} plugs'
            struct.pack_into('<i', data, pl[k]['_off'], int(val))
        else:
            raise ValueError(op)
    ainb.load(bytes(data))  # must still parse
    return bytes(data)


if __name__ == '__main__':
    src, dst, *ops = sys.argv[1:]
    out = patch(open(src, 'rb').read(), ops)
    open(dst, 'wb').write(out)
    print(f'wrote {dst} ({len(out)} bytes)')
