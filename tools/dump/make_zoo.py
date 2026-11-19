"""Build an actor-zoo overlay: a scene we describe, placed in an existing map, booted straight into (fs.overlay).
Game-derived output: local only.

usage: work/venv/bin/python tools/dump/make_zoo.py <scene.toml> <out_dir> [--romfs DIR]
       work/venv/bin/python tools/dump/make_zoo.py <name>
The second form is the scene games/oot/zoo/<name>.toml into work/overlay/<name>, made only when that is missing or older
than its description or this tool; it prints the overlay's folder (for `ounce run oot --set fs.overlay=<folder>`).

The scene description (games/oot/zoo/*.toml) holds actor names and numbers only:
    base = "SageRoom"            an existing map (its GameBancParam name); its two bancs are rewritten in the overlay
    start = ""                   the StartPos (its Name) the player starts at; "" the map's unnamed one
    keep = ["StartPos", ...]     the base map's actors kept, by Gyaml name ("*" every one); the rest are left out
    hour = 12                    the time of day the run sets (0-23): the overlay's live.txt holds the host command
                                 ("!cheat 1<hour>", the Cheats page's Set time), passed with
                                 --set input.live=<overlay>/live.txt (optional; a new game's field is at night)
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
import rstb
import zs
MARK = '.zoo'   # (in every overlay this tool writes: its scene file; a folder without it is never emptied)


def default_romfs():
    """The romfs of OoT's dump in the user's config (ounce config)."""
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import ounce_config
    return os.path.join(ounce_config.dump('oot'), 'romfs')


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
    ap.add_argument('scene', help='a scene description, or the name of one in games/oot/zoo')
    ap.add_argument('out', nargs='?', help='the overlay folder (default with a name: work/overlay/<name>)')
    ap.add_argument('--romfs', help="the romfs folder (default: the config's oot dump + /romfs)")
    a = ap.parse_args()
    if a.out is None:
        if a.scene.endswith('.toml') or os.sep in a.scene:
            sys.exit('make_zoo.py <scene.toml> <out_dir>, or make_zoo.py <name> (games/oot/zoo/<name>.toml)')
        name = a.scene
        a.scene = os.path.join(ROOT, 'games', 'oot', 'zoo', name + '.toml')
        a.out = os.path.join(ROOT, 'work', 'overlay', name)
        if not os.path.exists(a.scene):
            sys.exit(f'no scene {a.scene}')
        mark = os.path.join(a.out, MARK)
        if os.path.exists(mark) and os.path.getmtime(mark) >= max(os.path.getmtime(a.scene), os.path.getmtime(__file__)):
            print(a.out)
            return
        sys.stdout = sys.stderr   # (the tool's report; the folder alone goes to stdout)
        build(a)
        sys.stdout = sys.__stdout__
        print(a.out)
        return
    build(a)


def build(a):
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
                    '--banc', base, '--start', start, '--romfs', romfs], check=True, stdout=sys.stdout)

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
    rstb.raise_sizes(romfs, a.out, sizes)
    if 'hour' in desc:
        open(os.path.join(a.out, 'live.txt'), 'w').write('!cheat %d\n' % (100 + int(desc['hour']) % 24))
    open(os.path.join(a.out, MARK), 'w').write(os.path.abspath(a.scene) + '\n')


if __name__ == '__main__':
    main()
