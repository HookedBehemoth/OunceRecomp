"""Build an actor-zoo overlay: a scene we describe, placed in an existing map, booted straight into (HOS_FS_OVERLAY).
Game-derived output: local only.

usage: work/venv/bin/python tools/dump/make_zoo.py <scene.toml> <out_dir> [--romfs DIR]

The scene description (zoo/*.toml) holds actor names and numbers only:
    base = "SageRoom"            an existing map (its GameBancParam name); its two bancs are rewritten in the overlay
    start = ""                   the StartPos (its Name) the player starts at; "" the map's unnamed one
    keep = ["StartPos", ...]     the base map's actors kept, by Gyaml name ("*" every one); the rest are left out
    hour = 12                    the time of day the run sets (0-23): the overlay's live.txt holds the host command
                                 ("!cheat 1<hour>", the Cheats page's Set time) for HOS_INPUT_LIVE; scripts/run-vk.sh
                                 and scripts/play.sh pass it (optional; a new game's field is at night)
    [[actor]]                    one entry per actor, or per grid of copies
    name = "Obj_Pot_A_01"        the actor (Pack/Actor/<name>.pack.zs)
    translate = [x, y, z]        metres; rotate = [x, y, z] degrees; scale = [x, y, z] (both optional)
    grid = { count = [nx, ny, nz], step = [dx, dy, dz] }   copies at translate + (i dx, j dy, k dz) (optional)
    params = { ... }             the actor's Dynamic block (the game's per-instance parameters, optional)
    fields = { ... }             other keys of the banc entry, e.g. IsPhysicsStable = true (optional)
Added actors go to the map's Dynamic banc. Their instance hashes come from the scene's name, the entry and the copy, so
the same description gives the same files. The overlay holds the chiseled boot (make_chisel_overlay.py --route newgame
--banc <base> --start <start>), the two bancs, and the resource size table with the bancs' entries raised when they grew
(the loader drops a file larger than its entry).
"""
import argparse, hashlib, math, os, shutil, struct, subprocess, sys, tomllib, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import byml_write as B
import zs
MARK = '.zoo'   # (in every overlay this tool writes: its scene file; a folder without it is never emptied)
TABLE = 'System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs'


def default_romfs():
    for line in open(os.path.join(ROOT, 'work', 'dump.txt'), encoding='utf-8'):
        line = line.strip()
        if line and not line.startswith('#'):
            return os.path.join(line, 'romfs')
    sys.exit('no dump: write its folder into work/dump.txt')


def vec3(v, what):
    if not (isinstance(v, list) and len(v) == 3 and all(isinstance(x, (int, float)) for x in v)):
        sys.exit('%s: three numbers wanted, got %r' % (what, v))
    return [float(x) for x in v]


def actors(scene, desc):
    """the description's actors as banc entries"""
    out = []
    for n, e in enumerate(desc.get('actor', [])):
        name = e['name']
        base = vec3(e.get('translate', [0, 0, 0]), name + '.translate')
        rot = [math.radians(x) for x in vec3(e['rotate'], name + '.rotate')] if 'rotate' in e else None
        scale = vec3(e['scale'], name + '.scale') if 'scale' in e else None
        grid = e.get('grid', {})
        count = [int(c) for c in grid.get('count', [1, 1, 1])]
        step = vec3(grid.get('step', [0, 0, 0]), name + '.grid.step')
        copy = 0
        for i in range(count[0]):
            for j in range(count[1]):
                for k in range(count[2]):
                    pos = [base[0] + i * step[0], base[1] + j * step[1], base[2] + k * step[2]]
                    h = B.U64(int.from_bytes(hashlib.blake2b(('%s/%d/%d' % (scene, n, copy)).encode(), digest_size=8).digest(), 'little'))
                    srt = struct.pack('<9f', *pos, *(rot or [0, 0, 0]), *(scale or [1, 1, 1]))
                    a = {'Gyaml': name, 'Hash': h, 'Phive': {'Placement': {'ID': h}}, 'SRTHash': B.U32(zlib.crc32(srt)),
                         'Translate': pos}
                    if rot: a['Rotate'] = rot
                    if scale: a['Scale'] = scale
                    if e.get('params'): a['Dynamic'] = dict(e['params'])
                    a.update(e.get('fields', {}))
                    out.append(a)
                    copy += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scene')
    ap.add_argument('out')
    ap.add_argument('--romfs', help="the romfs folder (default: work/dump.txt's path + /romfs)")
    a = ap.parse_args()
    romfs = a.romfs or default_romfs()
    desc = tomllib.load(open(a.scene, 'rb'))
    scene = os.path.splitext(os.path.basename(a.scene))[0]
    base, start, keep = desc['base'], desc.get('start', ''), desc.get('keep', ['StartPos'])
    param = f'{romfs}/Banc/GameBancParam/{base}.game__banc__GameBancParam.bgyml'
    if not os.path.exists(param):
        sys.exit(f'no map {base} (no {param})')
    missing = sorted({e['name'] for e in desc.get('actor', []) if not os.path.exists(f"{romfs}/Pack/Actor/{e['name']}.pack.zs")})
    if missing:
        sys.exit('no such actors: ' + ', '.join(missing))

    if os.path.isdir(a.out):
        if os.listdir(a.out) and not os.path.exists(os.path.join(a.out, MARK)):
            sys.exit(f'{a.out} exists and was not made by make_zoo.py: left alone')
        shutil.rmtree(a.out)
    os.makedirs(a.out)
    subprocess.run([sys.executable, '-I', os.path.join(HERE, 'make_chisel_overlay.py'), a.out, '--route', 'newgame',
                    '--banc', base, '--start', start, '--romfs', romfs], check=True)

    main_path = B.load(open(param, 'rb').read())['Banc']['Main']   # Work/Banc/Scene/<dir>/<banc>.bcett.json
    folder, banc = main_path.split('/')[-2], main_path.split('/')[-1].split('.')[0]
    added = actors(scene, desc)
    sizes = {}
    dic = zs._dicts()
    for part in ('Static', 'Dynamic'):
        rel = f'Banc/{folder}/{banc}_{part}.bcett.byml'
        raw = open(f'{romfs}/{rel}.zs', 'rb').read()
        doc = B.load(raw)
        kept = [x for x in doc.get('Actors', []) if '*' in keep or x['Gyaml'] in keep]
        if part == 'Dynamic':
            kept += added
        new = {'FilePath': doc['FilePath']}
        if kept: new['Actors'] = kept
        if 'PhiveHeader' in doc: new['PhiveHeader'] = doc['PhiveHeader']
        # (rails, AI groups and placement groups name actors that are left out: only kept with every actor)
        if '*' in keep:
            for k, v in doc.items():
                new.setdefault(k, v)
        data = B.dump(new)
        os.makedirs(f'{a.out}/Banc/{folder}', exist_ok=True)
        import zstandard
        frame = zstandard.ZstdCompressor(level=19, dict_data=dic[zstandard.get_frame_parameters(raw).dict_id]).compress(data)
        open(f'{a.out}/{rel}.zs', 'wb').write(frame)
        sizes[rel] = (len(zs.decompress(raw)), len(data))
        print(f'{rel}: {len(kept)} actors ({len(added) if part == "Dynamic" else 0} added), {len(data)} bytes')
    size_table(romfs, a.out, sizes)
    if 'hour' in desc:
        open(os.path.join(a.out, 'live.txt'), 'w').write('!cheat %d\n' % (100 + int(desc['hour']) % 24))
    open(os.path.join(a.out, MARK), 'w').write(os.path.abspath(a.scene) + '\n')


def size_table(romfs, out, sizes):
    """the resource size table with each banc's entry at least its new size plus the entry's allowance over the old"""
    import zstandard
    src = os.path.join(romfs, TABLE)
    b = bytearray(zs.decompress(open(src, 'rb').read()))
    assert b[:6] == b'RESTBL', src
    _ver, name_len, hashed, named = struct.unpack_from('<IIII', b, 6)
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
    os.makedirs(os.path.dirname(os.path.join(out, TABLE)), exist_ok=True)
    open(os.path.join(out, TABLE), 'wb').write(zstandard.ZstdCompressor(level=3).compress(bytes(b)))
    print(f'size table (from {os.path.relpath(src, ROOT)}): {raised} entries raised')


if __name__ == '__main__':
    main()
