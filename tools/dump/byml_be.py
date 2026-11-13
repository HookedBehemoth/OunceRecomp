"""BYML reader for both endiannesses (v2-v7, incl. 0x20/0x21 hash dicts). load(bytes) -> python objects. Usage: python tools/dump/byml_be.py <file[.zs]> (JSON on stdout)."""
import struct, sys, json


def load(data):
    import zs
    data = zs.decompress(data)
    E = '<' if data[:2] == b'YB' else '>'
    assert data[:2] in (b'YB', b'BY'), data[:4]
    u32 = lambda o: struct.unpack_from(E + 'I', data, o)[0]

    def hdr(o):  # type byte + 24-bit count
        if E == '<':
            return data[o], u32(o) >> 8
        return data[o], u32(o) & 0xffffff

    def strtab(off):
        if not off: return []
        _, n = hdr(off)
        out = []
        for i in range(n):
            s = off + u32(off + 4 + 4 * i)
            out.append(data[s:data.index(b'\0', s)].decode('utf8', 'replace'))
        return out
    keys, strs, root = strtab(u32(4)), strtab(u32(8)), u32(12)

    def node(t, v):
        if t == 0xA0: return strs[v]
        if t in (0xC0, 0xC1, 0x20, 0x21): return container(v)
        if t == 0xA1: return ('<bin@%#x>' % v)
        if t == 0xD0: return bool(v)
        if t == 0xD1: return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xD2: return struct.unpack('<f', struct.pack('<I', v))[0]
        if t == 0xD3: return v
        if t in (0xD4, 0xD5, 0xD6): return struct.unpack_from(E + {0xD4: 'q', 0xD5: 'Q', 0xD6: 'd'}[t], data, v)[0]
        if t == 0xFF: return None
        return '<type %#x>' % t

    def container(off):
        t, n = hdr(off)
        if t == 0xC0:
            types = data[off + 4:off + 4 + n]
            vo = off + 4 + ((n + 3) & ~3)
            return [node(types[i], u32(vo + 4 * i)) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                e = off + 4 + 8 * i
                if E == '<':
                    k = u32(e) & 0xffffff; ty = data[e + 3]
                else:
                    k = u32(e) >> 8; ty = data[e + 3]
                out[keys[k]] = node(ty, u32(e + 4))
            return out
        if t in (0x20, 0x21):  # hash dict: entries (hash u32|u64, value u32), then types
            hs = 4 if t == 0x20 else 8
            es = hs + 4
            types = data[off + 4 + n * es: off + 4 + n * es + n]
            out = {}
            for i in range(n):
                e = off + 4 + i * es
                h = struct.unpack_from(E + ('I' if hs == 4 else 'Q'), data, e)[0]
                out[h] = node(types[i], u32(e + hs))
            return out
        return '<container %#x>' % t
    return container(root) if root else None


if __name__ == '__main__':
    print(json.dumps(load(open(sys.argv[1], 'rb').read()), indent=1, ensure_ascii=False, default=str)[:20000])
