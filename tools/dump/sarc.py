"""Minimal SARC reader: python tools/dump/sarc.py <file[.zs]> <out dir>"""
import struct, sys, os, zstandard
def unpack(data):
    import zs; data = zs.decompress(data)
    assert data[:4] == b'SARC'
    hdr, bom, size, doff = struct.unpack_from('<HHII', data, 4)
    sfat = 0x14
    n = struct.unpack_from('<H', data, sfat + 6)[0]
    nodes = [struct.unpack_from('<IIII', data, sfat + 12 + 16 * i) for i in range(n)]
    sfnt = sfat + 12 + 16 * n
    out = {}
    for h, attr, s, e in nodes:
        name = data[sfnt + 8 + (attr & 0xFFFF) * 4:].split(b'\0', 1)[0].decode() if attr >> 24 else '%08x' % h
        out[name] = data[doff + s:doff + e]
    return out
if __name__ == '__main__':
    os.makedirs(sys.argv[2], exist_ok=True)
    for k, v in unpack(open(sys.argv[1], 'rb').read()).items():
        p = os.path.join(sys.argv[2], k); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, 'wb').write(v); print(k, len(v))
