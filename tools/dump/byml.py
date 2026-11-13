"""Minimal BYML (v2-v7, little endian) reader -> Python objects. Usage: python tools/dump/byml.py <file[.zs]> (JSON on stdout); import byml; byml.load(bytes)"""
import struct, sys, json, os
def load(data):
    import zs; data = zs.decompress(data)
    assert data[:2] == b'YB', data[:4]
    u16 = lambda o: struct.unpack_from('<H', data, o)[0]
    u32 = lambda o: struct.unpack_from('<I', data, o)[0]
    def strtab(off):
        if not off: return []
        n = u32(off) >> 8
        return [data[off + u32(off + 4 + 4 * i):data.index(b'\0', off + u32(off + 4 + 4 * i))].decode('utf8', 'replace') for i in range(n)]
    keys, strs, root = strtab(u32(4)), strtab(u32(8)), u32(12)
    def node(t, v):
        if t == 0xA0: return strs[v]
        if t in (0xC0, 0xC1, 0x20, 0x21): return container(v)
        if t == 0xD0: return bool(v)
        if t == 0xD1: return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xD2: return struct.unpack('<f', struct.pack('<I', v))[0]
        if t == 0xD3: return v
        if t in (0xD4, 0xD5, 0xD6): return struct.unpack_from({0xD4: '<q', 0xD5: '<Q', 0xD6: '<d'}[t], data, v)[0]
        if t == 0xFF: return None
        return '<type %#x>' % t
    def container(off):
        t, n = data[off], u32(off) >> 8
        if t == 0xC0:
            types = data[off + 4:off + 4 + n]
            vo = off + 4 + ((n + 3) & ~3)
            return [node(types[i], u32(vo + 4 * i)) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                e = off + 4 + 8 * i
                out[keys[u32(e) & 0xFFFFFF]] = node(data[e + 3], u32(e + 4))
            return out
        return '<container %#x>' % t
    return container(root) if root else None
DICTS = {1: 'zs.zsdic', 2: 'bcett.byml.zsdic', 3: 'pack.zsdic'}
if __name__ == '__main__':
    print(json.dumps(load(open(sys.argv[1], 'rb').read()), indent=1, ensure_ascii=False, default=str)[:20000])
