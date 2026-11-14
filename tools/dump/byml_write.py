"""BYML v7 (little endian) writer, with a typed reader whose output it writes back unchanged. load(bytes) keeps each scalar's
node type: bool, float (f32), str and None stay plain Python; i32 is a plain int; u32, i64, u64 and f64 are wrapped
(U32, I64, U64, F64), so a document read and written again has the same nodes. dump(obj) -> bytes; a plain int is written
as i32, a float as f32. Identical containers and 64-bit values are written once and shared, as the game's files do (the
game's bancs come back at their own size). Hash dicts (0x20/0x21) and binary nodes are not written.
Usage: python tools/dump/byml_write.py <file[.zs]> (reads it, writes it, reports whether the nodes are equal)"""
import struct, sys


class _Typed:
    __slots__ = ('v',)
    T = 0
    def __init__(self, v): self.v = v
    def __eq__(self, o): return type(o) is type(self) and o.v == self.v
    def __hash__(self): return hash((self.T, self.v))
    def __repr__(self): return '%s(%r)' % (type(self).__name__, self.v)
class U32(_Typed): T = 0xD3
class I64(_Typed): T = 0xD4
class U64(_Typed): T = 0xD5
class F64(_Typed): T = 0xD6
WIDE = {0xD4: (I64, '<q'), 0xD5: (U64, '<Q'), 0xD6: (F64, '<d')}


def load(data):
    if data[:4] == b'\x28\xb5\x2f\xfd':
        import zs; data = zs.decompress(data)
    assert data[:2] == b'YB', data[:4]
    u32 = lambda o: struct.unpack_from('<I', data, o)[0]
    def strtab(off):
        if not off: return []
        n = u32(off) >> 8
        return [data[off + u32(off + 4 + 4 * i):data.index(b'\0', off + u32(off + 4 + 4 * i))].decode('utf8') for i in range(n)]
    keys, strs, root = strtab(u32(4)), strtab(u32(8)), u32(12)
    def node(t, v):
        if t == 0xA0: return strs[v]
        if t in (0xC0, 0xC1): return container(v)
        if t == 0xD0: return bool(v)
        if t == 0xD1: return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xD2: return struct.unpack('<f', struct.pack('<I', v))[0]
        if t == 0xD3: return U32(v)
        if t in WIDE: return WIDE[t][0](struct.unpack_from(WIDE[t][1], data, v)[0])
        if t == 0xFF: return None
        raise ValueError('BYML node type %#x is not read here' % t)
    def container(off):
        t, n = data[off], u32(off) >> 8
        if t == 0xC0:
            vo = off + 4 + ((n + 3) & ~3)
            return [node(data[off + 4 + i], u32(vo + 4 * i)) for i in range(n)]
        if t == 0xC1:
            return {keys[u32(off + 4 + 8 * i) & 0xFFFFFF]: node(data[off + 7 + 8 * i], u32(off + 8 + 8 * i)) for i in range(n)}
        raise ValueError('BYML container %#x is not read here' % t)
    return container(root) if root else None


def _type(o):
    if isinstance(o, bool): return 0xD0
    if isinstance(o, _Typed): return o.T
    if isinstance(o, int):
        if not -0x80000000 <= o <= 0x7FFFFFFF: raise ValueError('%d does not fit an i32: wrap it (U32, I64, U64)' % o)
        return 0xD1
    if isinstance(o, float): return 0xD2
    if isinstance(o, str): return 0xA0
    if isinstance(o, list): return 0xC0
    if isinstance(o, dict): return 0xC1
    if o is None: return 0xFF
    raise TypeError('no BYML node for %s' % type(o).__name__)


def _canon(o):   # (a key that tells subtrees apart exactly: f32 by its bits, -0.0 from 0.0)
    if isinstance(o, dict): return ('d', tuple((k, _canon(v)) for k, v in o.items()))
    if isinstance(o, list): return ('l', tuple(_canon(v) for v in o))
    if isinstance(o, float): return ('f', struct.pack('<f', o))
    if isinstance(o, F64): return ('F', struct.pack('<d', o.v))
    return (_type(o), o)


def dump(root, version=7):
    """root (a dict or list) -> the BYML file's bytes. Dict keys and strings go to the two sorted tables; containers are
    written depth first after the tables, each container's children after it"""
    keys, strs = set(), set()
    def scan(o):
        if isinstance(o, dict):
            for k, v in o.items(): keys.add(k); scan(v)
        elif isinstance(o, list):
            for v in o: scan(v)
        elif isinstance(o, str): strs.add(o)
    scan(root)
    keys = sorted(keys, key=lambda s: s.encode()); strs = sorted(strs, key=lambda s: s.encode())
    ki = {k: i for i, k in enumerate(keys)}; si = {s: i for i, s in enumerate(strs)}
    out = bytearray(16)
    def align():
        out.extend(b'\0' * (-len(out) % 4))
    def table(lst):
        if not lst: return 0
        align(); off = len(out)
        enc = [s.encode() + b'\0' for s in lst]
        out.extend(struct.pack('<I', 0xC2 | len(lst) << 8))
        pos = 4 + 4 * (len(lst) + 1)
        for e in enc: out.extend(struct.pack('<I', pos)); pos += len(e)
        out.extend(struct.pack('<I', pos))
        for e in enc: out.extend(e)
        return off
    kt, st = table(keys), table(strs)
    shared = {}
    def write(o):
        key = _canon(o)
        if key in shared: return shared[key]
        align(); off = shared[key] = len(out)
        items = sorted(o.items(), key=lambda kv: ki[kv[0]]) if isinstance(o, dict) else list(enumerate(o))
        if isinstance(o, dict):
            out.extend(struct.pack('<I', 0xC1 | len(items) << 8))
            slots = []
            for k, v in items:
                out.extend(struct.pack('<I', ki[k] | _type(v) << 24)); slots.append(len(out)); out.extend(b'\0' * 4)
        else:
            out.extend(struct.pack('<I', 0xC0 | len(items) << 8))
            out.extend(bytes(_type(v) for _, v in items)); align()
            slots = []
            for _ in items: slots.append(len(out)); out.extend(b'\0' * 4)
        later = []
        for slot, (_, v) in zip(slots, items):
            t = _type(v)
            if t in (0xC0, 0xC1) or t in WIDE: later.append((slot, v)); continue
            val = (si[v] if t == 0xA0 else int(v) if t == 0xD0 else struct.unpack('<I', struct.pack('<f', v))[0] if t == 0xD2
                   else v & 0xFFFFFFFF if t == 0xD1 else v.v if t == 0xD3 else 0)
            struct.pack_into('<I', out, slot, val)
        for slot, v in later:
            if isinstance(v, _Typed):
                key = _canon(v)
                if key not in shared:
                    align(); shared[key] = len(out); out.extend(struct.pack(WIDE[v.T][1], v.v))
                struct.pack_into('<I', out, slot, shared[key])
            else:
                struct.pack_into('<I', out, slot, write(v))
        return off
    rt = write(root)
    struct.pack_into('<2sHIII', out, 0, b'YB', version, kt, st, rt)
    return bytes(out)


if __name__ == '__main__':
    import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import zs
    raw = zs.decompress(open(sys.argv[1], 'rb').read())
    doc = load(raw); again = dump(doc)
    print('%s: %d bytes, written again %d bytes, nodes %s, bytes %s' % (sys.argv[1], len(raw), len(again),
          'equal' if load(again) == doc else 'DIFFER', 'equal' if again == raw else 'differ'))
