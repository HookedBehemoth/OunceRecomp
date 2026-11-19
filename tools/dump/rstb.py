"""The resource size table (RESTBL) of an overlay. The game gives a file's resource a heap of its table entry's size, the
file plus an allowance for what the loader builds from it, so a file an overlay makes larger needs its entry raised:
otherwise the loader drops it or the resource's own allocations fail. Game-derived output: local only."""
import os, struct, sys, zlib
import zs

TABLE = 'System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs'


def raise_sizes(romfs, out, sizes):
    """Each file's entry (sizes: romfs path -> (old size, new size)) raised to its new size plus the entry's allowance
    over the old, in the overlay's table: the one already in `out` when an earlier step wrote it, else the romfs's."""
    import zstandard
    have = os.path.join(out, TABLE)
    src = have if os.path.exists(have) else os.path.join(romfs, TABLE)
    b = bytearray(zs.decompress(open(src, 'rb').read()))
    assert b[:6] == b'RESTBL', src
    _ver, _name_len, hashed, _named = struct.unpack_from('<IIII', b, 6)
    want = {zlib.crc32(rel.encode()): (rel, old, new) for rel, (old, new) in sizes.items()}
    raised = 0
    for i in range(hashed):
        at = 22 + 8 * i
        h, entry = struct.unpack_from('<II', b, at)
        if h in want:
            rel, old, new = want.pop(h)
            if new + (entry - old) > entry:
                struct.pack_into('<I', b, at + 4, new + (entry - old)); raised += 1
    if want:
        sys.exit('no size table entry for ' + ', '.join(r for r, _, _ in want.values()))
    os.makedirs(os.path.dirname(have), exist_ok=True)
    open(have, 'wb').write(zstandard.ZstdCompressor(level=3).compress(bytes(b)))
    print(f'size table: {raised} entries raised')
