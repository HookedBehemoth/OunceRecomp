"""The shader map: which shader programs each model of the game's romfs draws with, before it draws.

    work/venv/bin/python -I tools/shadercensus/shadermap.py map <romfs> --out work/shadermap/<game> [--jobs 8]
    work/venv/bin/python -I tools/shadercensus/shadermap.py stats work/shadermap/<game>
    work/venv/bin/python -I tools/shadercensus/shadermap.py check work/shadermap/<game> <log> [--models <list>]
    work/venv/bin/python -I tools/shadercensus/shadermap.py list work/shadermap/<game> --scope all|shapes|material|none
                                                         [--other <romfs>] [--models <models>] --to <list>
    work/venv/bin/python -I tools/shadercensus/shadermap.py scene <romfs> <banc files>... --to <models>

map: every model (bfres) is read for its materials, every material resolved to the programs of its shader archive (bfsha)
that its options select, with the vertex layouts and topology of the shapes that use it, its fixed-function state (render
infos) and the texture types of its samplers. Writes <out>/materials.jsonl (one line a material), <out>/programs.jsonl
(one line a program: archive file, shading model, index, its dynamic options, and for a program a material reached its
stages' code keys as the renderer computes them) and <out>/textures.json. All of it is the game's data and stays under
work/.

stats: how many materials resolve to one program, to a set, to none; the programs no model reaches; per program the distinct
vertex-format and texture-type sets; the shader objects a load would compile (distinct code keys per stage) and the
pipelines the files describe (program, state, pass, vertex layout, topology; the pass stands for its render targets, which
no file gives).

list: the programs of a scope (all: every program a material reaches; shapes: those of its shapes' skinning; material: of
those, the material pass), a line each, its stages' code keys; --other adds the programs of the archives that are not a
bfsha (seed.py's walk). censustool precompile takes it.

scene: the models a scene's own files name (its placement files' actors, each actor's pack, its model components), a bfres
basename a line, for list --models and check --models.

check: the programs one frame of a run drew (HOS_DBG=drawlog:<present>: each draw's stages' code keys and its first colour
target's format) against the programs the map predicts; --models limits the prediction to the bfres files named in a list
(one basename a line).

Scene loads (where a load could wait for its programs; none is hooked yet). Both games load through the same gsys
resource system: res_factories_register (OoT 0x80b13af0, DKB 0x805e81bc) makes a factory per file extension, the bfres one
by res_bfres_factory_init (OoT 0x80b5e6a0), so a bfres becoming a resource is the point that knows a model's materials.
OoT's map changes are driven by sequence and AI graphs whose nodes name the waits: QueryBancChangeIsLoaded,
QueryBancLoadMgrIsStable, ExecuteWaitLoadScreen, SeqExecuteBancBasedResourceMgrRequestLoadCurrentScene (labels seq_*_name
give their classes' vtables); a query that answers "not loaded" while programs compile would hold the load screen.

How a material selects its program (nn::g3d2, bfres 10 and bfsha 9; the offsets are from each structure's start):

- bfres FMDL +0x48 the shader reflection array (one per shader a model uses), +0x6e its count; FMAT +0x10 the static shader
  variation (ResStaticShaderVariation); FSHP +0x52 the material index, +0x56 the vertex buffer index, +0x5a the skin weight
  count; FVTX +0x08 the attributes (0x10 each: name, u32 nn::gfx::AttributeFormat, u16 offset, u8 buffer), +0x4c their count.
- the reflection (0x50 bytes, shared by the model's materials): +0x00 the archive's name, +0x08 the shading model's, +0x10 /
  +0x18 the render infos (0x10 each: name, u8 type 0 int, 1 float, 2 string), +0x20 / +0x28 the shader params, +0x30 the
  attribute dict, +0x38 the sampler dict, +0x40 the static option dict, +0x48 u16 the render info count.
- the variation (0x50 bytes, per material): +0x00 the reflection, +0x08 / +0x10 the vertex attribute names and their u8
  indices into the reflection's attribute dict (none: in order), +0x18 / +0x20 the same for samplers, +0x28 the bool options
  (a bit each, u32 words), +0x30 the other options' strings, +0x38 u16 each option's index in the reflection's option dict
  (bools first), +0x44 u8 attributes, +0x45 u8 samplers, +0x46 u16 bool options, +0x48 u16 all options.
- FMAT +0x40 / +0x48 / +0x50 the render info values, their counts (u16) and their offsets into the values (u16), +0x20 the
  texture names, +0x38 the sampler dict (a texture per sampler, in order).
- Names: a string offset (a u16 length, then the bytes); OoT's reflections give a 64-bit hash instead (render infos, params,
  option dict keys), which Shader/ExternalBinaryString.bfres resolves: +0xb8 its sorted hashes, +0xc0 a dict of the names in
  the same order.
- bfsha: +0x48 the shading models (0x100 each), +0x50 their dict, +0x78 u16 their count. A shading model: +0x00 name, +0x08 /
  +0x10 the static options and dict, +0x18 / +0x20 the dynamic ones, +0x88 the programs (0x40 each), +0x90 the key table,
  +0xa8 its BNSH, +0xe4 u16 static options, +0xe6 u16 dynamic ones, +0xe8 u16 programs, +0xea u8 the static key's words,
  +0xeb u8 the dynamic key's. An option (0x28): +0x00 name, +0x08 the choice dict, +0x18 u16 choices, +0x1a u16 the default
  choice, +0x20 u32 its mask in the key word, +0x24 u8 the key word (static and dynamic counted together), +0x25 u8 its shift.
  The key table holds each program's key (static words, then dynamic); a program's key word holds each option's choice
  index. A program (0x40): +0x20 its BNSH variation (absolute), +0x28 its shading model.
- A material's static key is each static option's choice: the material's value, else the option's default. gsys sets seven
  static options (gsys_*) itself from the material's render infos (RULES below); the dynamic options (gsys_assign_type: the
  pass; gsys_weight: the skinning) are chosen per draw, so a material reaches the set of programs whose static key is its own.

A stage's code key is the renderer's: FNV-1a 64 over the 0xd8 control bytes, then the code area they size (control +0x44).
"""
import argparse
import collections
import concurrent.futures
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import seed  # noqa: E402  (the zstd dictionaries, the BNSH layout)

# gsys's static options, from the material's render infos: option -> (render info, its value -> choice name)
COMPARE = {k: str(i) for i, k in enumerate(("never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"))}
RULES = {
    "gsys_alpha_test_enable": ("gsys_alpha_test_enable", {"true": "1", "false": "0"}),
    "gsys_alpha_test_func": ("gsys_alpha_test_func", COMPARE),
    "gsys_display_face_type": ("gsys_render_state_display_face", {"both": "0", "front": "1", "back": "2", "none": "3"}),
    "gsys_renderstate": ("gsys_render_state_mode", {"opaque": "0", "mask": "1", "translucent": "2", "custom": "3"}),
}


class Bin:
    def __init__(self, data, names=None):
        self.d, self.names = data, names or {}

    def u8(self, o):
        return self.d[o]

    def u16(self, o):
        return struct.unpack_from("<H", self.d, o)[0]

    def u32(self, o):
        return struct.unpack_from("<I", self.d, o)[0]

    def u64(self, o):
        return struct.unpack_from("<Q", self.d, o)[0]

    def str(self, o):
        if not o:
            return None
        if o >= len(self.d) - 2:   # (a name hash: OoT)
            return self.names.get(o, "#%016x" % o)
        n = self.u16(o)
        return self.d[o + 2:o + 2 + n].decode("latin1")

    def dict_keys(self, o):
        if not o:
            return []
        return [self.str(self.u64(o + 8 + 16 * (i + 1) + 8)) for i in range(self.u32(o + 4))]

    def strs(self, o, n):
        return [self.str(self.u64(o + 8 * i)) for i in range(n)] if o else [None] * n


def fnv(data, h=0xcbf29ce484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


class ShadingModel:
    def __init__(self, b, m, archive):
        self.archive, self.name = archive, b.str(b.u64(m))
        self.options = {}
        for kind, arr, count in (("static", 0x08, b.u16(m + 0xe4)), ("dynamic", 0x18, b.u16(m + 0xe6))):
            for i in range(count):
                o = b.u64(m + arr) + 0x28 * i
                self.options[b.str(b.u64(o))] = (kind, b.dict_keys(b.u64(o + 8)), b.u16(o + 0x1a), b.u8(o + 0x24),
                                                b.u8(o + 0x25), b.u32(o + 0x20))
        self.count, self.sk, dk = b.u16(m + 0xe8), b.u8(m + 0xea), b.u8(m + 0xeb)
        kt, w = b.u64(m + 0x90), self.sk + dk
        self.keys = [struct.unpack_from("<%dI" % w, b.d, kt + 4 * w * i) for i in range(self.count)]
        self.by_static = collections.defaultdict(list)
        for i, k in enumerate(self.keys):
            self.by_static[k[:self.sk]].append(i)
        self.b, self.m = b, m
        self.wild = {}

    def default_key(self):
        key = [0] * self.sk
        for kind, ch, default, word, shift, mask in self.options.values():
            if kind == "static":
                key[word] |= (default << shift) & mask
        return key

    def match(self, values, wild):
        """The programs whose static key is the values' (option -> choice name); options in wild match any choice"""
        key, bad = self.default_key(), []
        wmask = [0] * self.sk
        for name, value in values.items():
            o = self.options.get(name)
            if not o or o[0] != "static":
                continue
            kind, ch, default, word, shift, mask = o
            if value not in ch:
                bad.append(name)
                wmask[word] |= mask
                continue
            key[word] = (key[word] & ~mask) | ((ch.index(value) << shift) & mask)
        for name in wild:
            o = self.options.get(name)
            if o and o[0] == "static":
                wmask[o[3]] |= o[5]
        if not any(wmask):
            return list(self.by_static.get(tuple(key), ())), bad
        wm = tuple(wmask)
        if wm not in self.wild:
            idx = collections.defaultdict(list)
            for i, k in enumerate(self.keys):
                idx[tuple(a & ~m for a, m in zip(k[:self.sk], wm))].append(i)
            self.wild[wm] = idx
        return list(self.wild[wm].get(tuple(a & ~m for a, m in zip(key, wm)), ())), bad

    def dynamic(self, program):
        """The program's dynamic option choices"""
        k, out = self.keys[program], {}
        for name, (kind, ch, default, word, shift, mask) in self.options.items():
            if kind == "dynamic":
                c = (k[word] & mask) >> shift
                out[name] = ch[c] if c < len(ch) else c
        return out

    def stages(self, program):
        """The program's stages as (NVN stage, code key)"""
        b = self.b
        var = b.u64(b.u64(self.m + 0x88) + 0x40 * program + 0x20)
        bnsh = b.u64(self.m + 0xa8)
        prog = bnsh + b.u64(var + 16)   # (the BNSH's own offsets are from its start)
        out = []
        for slot in seed.SLOTS:
            st = b.u64(prog + slot)
            if not st:
                continue
            st += bnsh
            control_off, code_off, code_size, control_size = struct.unpack_from("<QQII", b.d, st + 8)
            if not 0x60 <= control_size <= 0x1000:
                continue
            c = bnsh + control_off
            control = b.d[c:c + 0xd8]
            if control[:4] != seed.CONTROL_MAGIC:
                continue
            end = struct.unpack_from("<I", control, 0x44)[0]
            code = b.d[bnsh + code_off:bnsh + code_off + end]
            out.append((struct.unpack_from("<H", control, 0x56)[0], fnv(code, fnv(control))))
        return out


def external_names(romfs, dicts, zs):
    path = os.path.join(romfs, "Shader", "ExternalBinaryString.bfres.zs")
    if not os.path.exists(path):
        return {}
    b = Bin(seed.unpack(open(path, "rb").read(), dicts, zs))
    hashes, d = b.u64(0xb8), b.u64(0xc0)
    names = b.dict_keys(d)
    return {b.u64(hashes + 8 * i): n for i, n in enumerate(names)}


def archives(romfs, dicts, zs):
    """Shader/*.bfsha: archive name -> {shading model name: [ShadingModel...]} (a model's programs can be split over files)"""
    out = {}
    folder = os.path.join(romfs, "Shader")
    for name in sorted(os.listdir(folder)):
        if not name.endswith((".bfsha", ".bfsha.zs")):   # (MKW keeps a .changelist.json beside each)
            continue
        b = Bin(seed.unpack(open(os.path.join(folder, name), "rb").read(), dicts, zs))
        arch = b.str(b.u64(0x38))
        source = name.split(".Nin_")[0]   # (programs are named by their file: MKW splits one archive's shading models,
        models = []                       # each with a part of the programs, over six files)
        for i in range(b.u16(0x78)):
            models.append(ShadingModel(b, b.u64(0x48) + 0x100 * i, source))
        # (materials name the archive, or the file's first part: "Model" for Model.Product.110...)
        for key in {arch, name.split(".")[0]}:
            for sm in models:
                out.setdefault(key, {}).setdefault(sm.name, [])
                if sm not in out[key][sm.name]:
                    out[key][sm.name].append(sm)
    return out


def bfres_paths(romfs):
    for top, dirs, names in os.walk(romfs):
        dirs.sort()
        for n in sorted(names):
            if ".bfres" in n and ".anim." not in n and not n.startswith("ExternalBinaryString"):
                yield os.path.join(top, n)


def render_infos(b, f, refl):
    count, arr = b.u16(refl + 0x48), b.u64(refl + 0x10)
    vals, counts, offs = b.u64(f + 0x40), b.u64(f + 0x48), b.u64(f + 0x50)
    out = {}
    if not vals:
        return out
    for r in range(count):
        name, t = b.str(b.u64(arr + 16 * r)), b.u8(arr + 16 * r + 8)
        c, off = b.u16(counts + 2 * r), b.u16(offs + 2 * r)
        if t == 2:
            v = [b.str(b.u64(vals + off + 8 * k)) for k in range(c)]
        else:
            v = list(struct.unpack_from("<%d%s" % (c, "i" if t == 0 else "f"), b.d, vals + off))
        out[name] = v
    return out


def read_bfres(path, romfs, names):
    """Each material of the file: its shader, its option values, its render infos, its shapes' vertex attributes"""
    import zstandard as zs
    data = seed.unpack(open(path, "rb").read(), READ_DICTS, zs)
    b = Bin(data, names)
    out = []
    if data[:4] != b"FRES":
        return out
    rel = os.path.relpath(path, romfs)
    models, count = b.u64(0x28), b.u16(0xdc)
    for mi in range(count):
        fm = models + 0x78 * mi
        if data[fm:fm + 4] != b"FMDL":
            break
        nvtx, nshp, nmat = b.u16(fm + 0x68), b.u16(fm + 0x6a), b.u16(fm + 0x6c)
        vtx, shp, mat = b.u64(fm + 0x20), b.u64(fm + 0x28), b.u64(fm + 0x38)
        verts = []
        for v in range(nvtx):
            fv = vtx + 0x58 * v
            attrs = {}   # (name -> format, offset, buffer, the buffer's stride (FVTX +0x38, 0x10 each))
            strides = [b.u32(b.u64(fv + 0x38) + 0x10 * k) for k in range(b.u8(fv + 0x4d))] if b.u64(fv + 0x38) else []
            for a in range(b.u8(fv + 0x4c)):
                o = b.u64(fv + 8) + 0x10 * a
                buf = b.u8(o + 0x0e)
                attrs[b.str(b.u64(o))] = (b.u32(o + 8), b.u16(o + 0x0c), buf, strides[buf] if buf < len(strides) else None)
            verts.append(attrs)
        shapes = collections.defaultdict(list)
        for s in range(nshp):
            fs = shp + 0x60 * s
            flags = b.u32(fs + 4)   # (bit 4: a meshlet shape, +0x18 then points at its meshlets, else at ResMesh: +0x24 topology)
            topo = b.u32(b.u64(fs + 0x18) + 0x24) if b.u64(fs + 0x18) and not flags & 16 else None
            shapes[b.u16(fs + 0x52)].append((b.u16(fs + 0x56), b.u8(fs + 0x5a), flags, topo))
        for m in range(nmat):
            f = mat + 0xb0 * m
            var = b.u64(f + 0x10)
            rec = {"file": rel, "model": b.str(b.u64(fm + 8)), "material": b.str(b.u64(f + 8))}
            if not var:
                rec["error"] = "no shader"
                out.append(rec)
                continue
            refl = b.u64(var)
            rec["archive"], rec["shading_model"] = b.str(b.u64(refl)), b.str(b.u64(refl + 8))
            onames = b.dict_keys(b.u64(refl + 0x40))
            nb, nt = b.u16(var + 0x46), b.u16(var + 0x48)
            bits, strs, idx = b.u64(var + 0x28), b.u64(var + 0x30), b.u64(var + 0x38)
            opts = {}
            for j in range(nt):
                name = onames[b.u16(idx + 2 * j) if idx else j]   # (no index array: in the dict's order)
                opts[name] = str((b.u32(bits + 4 * (j // 32)) >> (j % 32)) & 1) if j < nb else b.str(b.u64(strs + 8 * (j - nb)))
            rec["options"] = opts
            ri = render_infos(b, f, refl)
            rec["render"] = {k: v for k, v in ri.items() if k.startswith("gsys_") and (k in {r for r, _ in RULES.values()}
                                                                                    or k in ("gsys_pass", "gsys_meshlet_override"))}
            # (the fixed-function state a pipeline needs: blend, depth, cull, alpha test, polygon offset)
            rec["state"] = {k: v for k, v in ri.items() if any(t in k for t in ("render_state", "depth_test", "color_blend",
                                                                                "alpha_test", "polygon_offset"))}
            adict, sdict = b.dict_keys(b.u64(refl + 0x30)), b.dict_keys(b.u64(refl + 0x38))
            na, ns = b.u8(var + 0x44), b.u8(var + 0x45)
            anames, aidx = b.strs(b.u64(var + 8), na), b.u64(var + 0x10)
            snames, sidx = b.strs(b.u64(var + 0x18), ns), b.u64(var + 0x20)
            # (attribute assign: the shader's attribute -> the vertex attribute; sampler assign: the shader's sampler -> the
            # material's sampler, whose texture is the one at its index)
            amap = {adict[b.u8(aidx + i)] if aidx else adict[i]: anames[i] for i in range(na)}
            msamplers, textures = b.dict_keys(b.u64(f + 0x38)), b.strs(b.u64(f + 0x20), b.u8(f + 0xa3))
            smap = {}
            for i in range(ns):
                shader = sdict[b.u8(sidx + i)] if sidx else sdict[i]
                k = msamplers.index(snames[i]) if snames[i] in msamplers else -1
                smap[shader] = textures[k] if 0 <= k < len(textures) else None
            rec["samplers"] = smap
            rec["shapes"] = []
            for vi, skin, flags, topo in shapes.get(m, ()):
                attrs = verts[vi] if vi < len(verts) else {}
                rec["shapes"].append({"skin": skin, "meshlet": bool(flags & 16), "topology": topo,
                                      "vertex": {sa: attrs[va][0] if va in attrs else None for sa, va in amap.items()},
                                      "layout": sorted([sa] + list(attrs[va]) for sa, va in amap.items() if va in attrs)})
            out.append(rec)
    return out


READ_DICTS = None


def init_reader(romfs):
    global READ_DICTS
    import zstandard as zs
    READ_DICTS = seed.dictionaries(romfs, zs)


def texture_types(romfs, dicts, zs):
    """Tex/<name>.bntx.zs: name -> (nn::gfx::ImageDimension (BRTI +0x5c), format (+0x1c))"""
    folder, out = os.path.join(romfs, "Tex"), {}
    if not os.path.isdir(folder):
        return out
    for n in sorted(os.listdir(folder)):
        if ".bntx" not in n:
            continue
        with open(os.path.join(folder, n), "rb") as f:
            raw = f.read(1 << 18)   # (the header alone: the frame's first block, up to 128 KiB compressed)
        for d in [None] + dicts:   # (with the dictionary that fits)
            try:
                z = zs.ZstdDecompressor(dict_data=zs.ZstdCompressionDict(d, dict_type=zs.DICT_TYPE_FULLDICT)) if d else zs.ZstdDecompressor()
                head = z.decompressobj().decompress(raw) if raw[:4] == seed.ZSTD else raw
            except zs.ZstdError:
                continue
            i = head.find(b"BRTI")
            if 0 <= i and i + 0x60 <= len(head):
                break
        else:
            continue
        out[n.split(".bntx")[0]] = (struct.unpack_from("<I", head, i + 0x5c)[0], struct.unpack_from("<I", head, i + 0x1c)[0])
    return out


def cmd_map(a):
    import zstandard as zs
    dicts = seed.dictionaries(a.romfs, zs)
    names = external_names(a.romfs, dicts, zs)
    shaders = archives(a.romfs, dicts, zs)
    print("archives:", sorted({sm.archive + "/" + n for ms in shaders.values() for n, l in ms.items() for sm in l}), flush=True)
    os.makedirs(a.out, exist_ok=True)
    try:
        dicts_tex = texture_types(a.romfs, dicts, zs)
    except Exception as e:   # (a folder this doesn't read: no texture types)
        print("textures skipped:", e)
        dicts_tex = {}
    with open(os.path.join(a.out, "textures.json"), "w") as f:
        json.dump(dicts_tex, f)
    print("textures:", len(dicts_tex), flush=True)
    paths = list(bfres_paths(a.romfs))
    reached = collections.defaultdict(set)
    with open(os.path.join(a.out, "materials.jsonl"), "w") as out, \
            concurrent.futures.ProcessPoolExecutor(a.jobs, initializer=init_reader, initargs=(a.romfs,)) as ex:
        futs = {ex.submit(read_bfres, p, a.romfs, names): p for p in paths}
        done = 0
        for fu in concurrent.futures.as_completed(futs):
            done += 1
            try:
                recs = fu.result()
            except Exception as e:
                out.write(json.dumps({"file": os.path.relpath(futs[fu], a.romfs), "error": "unread: %s" % e}) + "\n")
                continue
            for rec in recs:
                if "options" in rec:
                    resolve(rec, shaders, dicts_tex)
                    for key in ("exact", "loose"):
                        for arch, sm, p in rec.get(key + "_programs", ()):
                            reached[(arch, sm)].add(p)
                out.write(json.dumps(rec) + "\n")
            if done % 500 == 0:
                print("%d / %d files" % (done, len(paths)), flush=True)
    with open(os.path.join(a.out, "programs.jsonl"), "w") as f:
        for sm in {id(sm): sm for ms in shaders.values() for l in ms.values() for sm in l}.values():
            got = reached.get((sm.archive, sm.name), set())
            for p in range(sm.count):
                row = {"archive": sm.archive, "shading_model": sm.name, "program": p, "reached": p in got,
                       "dynamic": sm.dynamic(p)}
                if p in got or a.all_keys:
                    row["stages"] = ["%d:%016x" % s for s in sm.stages(p)]
                f.write(json.dumps(row) + "\n")
            print("%s/%s: %d programs, %d reached" % (sm.archive, sm.name, sm.count, len(got)), flush=True)
    print("wrote", a.out)


def resolve(rec, shaders, textures):
    """The programs the material reaches: exact (gsys's options from its render infos) and loose (gsys's options free)"""
    sms = (shaders.get(rec["archive"]) or {}).get(rec["shading_model"])
    if not sms:
        rec["error"] = "no shading model %s/%s" % (rec["archive"], rec["shading_model"])
        return
    rec["exact_programs"], rec["loose_programs"] = [], []
    bad, unknown = set(), set(rec["options"])
    for sm in sms:
        gsys = [n for n, o in sm.options.items() if o[0] == "static" and n.startswith("gsys_")]
        values, ruled = dict(rec["options"]), []
        for opt, (info, table) in RULES.items():
            v = rec["render"].get(info)
            if opt in sm.options and v and v[0] in table:
                values[opt] = table[v[0]]
                ruled.append(opt)
        exact, b = sm.match(values, [g for g in gsys if g not in ruled])
        loose, _ = sm.match(rec["options"], gsys)
        bad |= set(b)
        unknown -= set(sm.options)
        rec["exact_programs"] += [(sm.archive, sm.name, p) for p in exact]
        rec["loose_programs"] += [(sm.archive, sm.name, p) for p in loose]
    # (a meshlet shape draws with the archive's meshlet override model when the material asks for it)
    if (rec["render"].get("gsys_meshlet_override") or [""])[0] not in ("", "0", "false", None):
        rec["meshlet_override"] = rec["render"]["gsys_meshlet_override"]
    if bad:
        rec["bad_values"] = sorted(bad)
    if unknown:
        rec["unknown_options"] = sorted(unknown)
    rec["texture_types"] = {s: textures.get(t, [None])[0] if t else None for s, t in rec["samplers"].items()}


STAGE_NAMES = {"0": "vertex", "1": "fragment", "2": "tess control", "3": "tess eval", "4": "geometry", "5": "compute",
               "6": "task", "7": "mesh"}


SCOPES = {"all": "every pass, every skinning (the exact sets)", "shapes": "every pass, the shapes' skinning",
          "material": "the material pass alone, the shapes' skinning"}


def scope_sets(ok, dyn):
    """The programs a load would compile, per scope (SCOPES), and the pipelines the files describe"""
    sets = {k: set() for k in SCOPES}
    pipes, pipes_no_layout = set(), set()
    for m in ok:
        skins = {str(s["skin"]) for s in m["shapes"]}
        state = tuple(sorted((k, tuple(v)) for k, v in m.get("state", {}).items()))
        for p in m["exact_programs"]:
            p = tuple(p)
            d = dyn[p]
            sets["all"].add(p)
            if d.get("gsys_weight") in skins:
                sets["shapes"].add(p)
                if d.get("gsys_assign_type") in (None, "gsys_assign_material"):
                    sets["material"].add(p)
            # (a pipeline: the program, a shape's vertex layout and topology, the material's fixed-function state, and the
            # pass, which stands for its render targets: the files give no target formats)
            for s in m["shapes"]:
                if d.get("gsys_weight") != str(s["skin"]):
                    continue
                key = (p, state, d.get("gsys_assign_type"))
                pipes_no_layout.add(key)
                pipes.add(key + (tuple(tuple(x) for x in s.get("layout", ())), s.get("topology")))
    return sets, pipes, pipes_no_layout


def compile_counts(ok, progs, dyn):
    """What a scene load would compile: distinct shader objects per stage (by code key) and distinct pipelines"""
    stages = {(p["archive"], p["shading_model"], p["program"]): p.get("stages") for p in progs}
    sets, pipes, pipes_no_layout = scope_sets(ok, dyn)
    for what, ps in sets.items():
        by = collections.defaultdict(set)
        for p in ps:
            for st in stages.get(p) or ():
                k, key = st.split(":")
                by[STAGE_NAMES.get(k, k)].add(key)
        print("objects to compile, %s: %d programs; %s" % (
            SCOPES[what], len(ps), ", ".join("%s %d" % (k, len(v)) for k, v in sorted(by.items()))))
    print("pipelines (program, state, pass; + vertex layout and topology): %d without the layout, %d with" % (
        len({tuple(stages.get(k[0]) or (k[0],)) + k[1:] for k in pipes_no_layout}),
        len({tuple(stages.get(k[0]) or (k[0],)) + k[1:] for k in pipes})))


def other_programs(romfs):
    """The programs of the romfs's shader archives that are not a bfsha (seed.py's walk without them: agl, gsys, terrain,
    Layout2Shader, effects, networks), each as its stages' "<stage>:<code key>" """
    import zstandard as zs
    dicts = seed.dictionaries(romfs, zs)
    out, stats = set(), collections.Counter()
    for path in seed.archives(romfs):
        if ".bfsha" in os.path.basename(path):
            continue
        try:
            for data in seed.blobs(seed.unpack(open(path, "rb").read(), dicts, zs), dicts, zs):
                try:
                    found = list(seed.programs_in(data, stats))
                except Exception:
                    continue
                for stages in found:
                    keys = set()
                    for st, control, code in stages:
                        end = struct.unpack_from("<I", control, 0x44)[0]
                        keys.add("%d:%016x" % (st, fnv(code[:end] if end <= len(code) else code, fnv(control[:0xd8]))))
                    out.add(tuple(sorted(keys)))
        except Exception as e:
            print("%s skipped: %s" % (os.path.relpath(path, romfs), e), file=sys.stderr)
    return out


def sarc_named(data):
    """A SARC's files by name"""
    start, count = struct.unpack_from("<I", data, 0x0c)[0], struct.unpack_from("<H", data, 0x1a)[0]
    names = 0x20 + count * 16 + 8
    for k in range(count):
        _, attr, st, end = struct.unpack_from("<IIII", data, 0x20 + k * 16)
        at = names + (attr & 0xffffff) * 4
        yield data[at:data.index(b"\0", at)].decode(), data[start + st:start + end]


def placed(doc):
    """The actor names a placement file gives: OoT's banc (Actors[].Gyaml), DKB's stage (each list's objects'
    UnitConfigName, and ModelName where it is a model's name)"""
    if isinstance(doc, dict) and "Actors" in doc:
        for act in doc["Actors"] or ():
            yield act["Gyaml"], None
        return
    for top in doc if isinstance(doc, list) else [doc]:
        for lst in top.values():
            for o in lst if isinstance(lst, list) else ():
                if isinstance(o, dict) and o.get("UnitConfigName"):
                    m = o.get("ModelName")
                    yield o["UnitConfigName"], m if m and "/" not in m else None


def cmd_scene(a):
    """The models a scene's own files name: each actor its placement files place, the actor's pack
    (Pack/Actor/<name>.pack.zs) and its model components: OoT's ModelInfo (Model/<ModelProjectName>.<FmdbName>.bfres.zs),
    DKB's ModelParam (ModelArcName: Model/<arc>.*.bfres.zs); DKB's objects also name their model (ModelName)"""
    import zstandard as zs
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), "dump"))
    import byml
    dicts = seed.dictionaries(a.romfs, zs)
    model_dir = os.path.join(a.romfs, "Model")
    by_arc = collections.defaultdict(set)
    for name in os.listdir(model_dir):
        if name.endswith(".bfres.zs") and ".anim." not in name:
            by_arc[name.split(".")[0]].add(name)
    actors, direct = collections.Counter(), set()
    for path in a.banc:
        data = seed.unpack(open(path, "rb").read(), dicts, zs)
        docs = [byml.load(b) for _, b in sarc_named(data)] if data[:4] == b"SARC" else [byml.load(data)]
        for doc in docs:
            for actor, model in placed(doc):
                actors[actor] += 1
                if model:
                    direct |= by_arc.get(model, set())
    models, no_pack, no_model = set(direct), [], []
    for g in sorted(actors):
        pack = os.path.join(a.romfs, "Pack", "Actor", g + ".pack.zs")
        if not os.path.exists(pack):
            no_pack.append(g)
            continue
        found = False
        for name, data in sarc_named(seed.unpack(open(pack, "rb").read(), dicts, zs)):
            if "/ModelInfo/" in name:
                info = byml.load(data)
                if info.get("ModelProjectName") and info.get("FmdbName"):
                    bfres = "%s.%s.bfres.zs" % (info["ModelProjectName"], info["FmdbName"])
                    if os.path.exists(os.path.join(model_dir, bfres)):
                        models.add(bfres)
                        found = True
            elif "ModelParam" in name:
                arc = byml.load(data).get("ModelArcName")
                if arc in by_arc:
                    models |= by_arc[arc]
                    found = True
        if not found:
            no_model.append(g)
    with open(a.to, "w") as f:
        f.writelines(m + "\n" for m in sorted(models))
    print("%d actor kinds placed (%d placements); %d without a pack, %d whose pack names no model; %d models (%d named by the "
          "objects themselves) -> %s" % (len(actors), sum(actors.values()), len(no_pack), len(no_model), len(models), len(direct), a.to))


def cmd_list(a):
    """A scope's programs, a line each (its stages' "<stage>:<code key>"), for censustool precompile"""
    progs = [json.loads(l) for l in open(os.path.join(a.out, "programs.jsonl"))]
    dyn = {(p["archive"], p["shading_model"], p["program"]): p["dynamic"] for p in progs}
    stages = {(p["archive"], p["shading_model"], p["program"]): p.get("stages") for p in progs}
    lines = set()
    if a.scope != "none":
        files = {l.strip() for l in open(a.models) if l.strip()} if a.models else None
        ok = (m for m in map(json.loads, open(os.path.join(a.out, "materials.jsonl")))
              if "exact_programs" in m and (files is None or os.path.basename(m["file"]) in files))
        sets, _, _ = scope_sets(ok, dyn)
        lines = {tuple(sorted(stages[p])) for p in sets[a.scope] if stages.get(p)}
    bfres = len(lines)
    if a.other:
        lines |= other_programs(a.other)
    with open(a.to, "w") as f:
        for ln in sorted(lines):
            f.write(" ".join(ln) + "\n")
    print("%s: %d programs (%d from bfres%s)" % (a.to, len(lines), bfres, ", the rest the other archives'" if a.other else ""))


def cmd_stats(a):
    mats = [json.loads(l) for l in open(os.path.join(a.out, "materials.jsonl"))]
    progs = [json.loads(l) for l in open(os.path.join(a.out, "programs.jsonl"))]
    ok = [m for m in mats if "exact_programs" in m]
    errors = collections.Counter(m.get("error", "?").split(":")[0] for m in mats if "exact_programs" not in m)
    print("materials: %d (%d files), resolved against a shading model: %d; not: %s" % (
        len(mats), len({m["file"] for m in mats}), len(ok), dict(errors)))
    for key in ("exact", "loose"):
        sizes = collections.Counter(len(m[key + "_programs"]) for m in ok)
        print("%s: none %d, one %d, a set %d (sizes: %s)" % (
            key, sizes[0], sizes[1], sum(v for k, v in sizes.items() if k > 1),
            ", ".join("%d x%d" % (k, v) for k, v in sorted(sizes.items()) if k > 1)[:600]))
    # (the dynamic options: gsys_assign_type is the pass, gsys_weight the skinning a shape draws with; a set narrowed to the
    # shapes' skin weight counts, and how many passes remain)
    dyn = {(p["archive"], p["shading_model"], p["program"]): p["dynamic"] for p in progs}
    narrowed, passes, weightless = collections.Counter(), collections.Counter(), 0
    for m in ok:
        skins = {str(s["skin"]) for s in m["shapes"]}
        got = [p for p in m["exact_programs"] if dyn[tuple(p)].get("gsys_weight") in skins]
        if m["exact_programs"] and not got:
            weightless += 1
        narrowed[len(got)] += 1
        passes[len({dyn[tuple(p)].get("gsys_assign_type") for p in got})] += 1
    print("exact, gsys_weight = a shape's skin weight count: %s; no program for its skin counts: %d" % (
        ", ".join("%d x%d" % kv for kv in sorted(narrowed.items())), weightless))
    print("  passes (gsys_assign_type) left per material:", sorted(passes.items()))
    compile_counts(ok, progs, dyn)
    bad = sum(1 for m in ok if m.get("bad_values"))
    unk = sum(1 for m in ok if m.get("unknown_options"))
    print("materials with a value no choice has: %d; with an option the model lacks: %d" % (bad, unk))
    by_model = collections.Counter((p["archive"], p["shading_model"]) for p in progs)
    reached = collections.Counter((p["archive"], p["shading_model"]) for p in progs if p["reached"])
    for k, n in sorted(by_model.items()):
        print("%s/%s: %d programs, %d reached by a model, %d never" % (k[0], k[1], n, reached[k], n - reached[k]))
    # per program: the distinct vertex-format sets and texture-type sets of the materials reaching it (exact, else loose)
    fmts, texs = collections.defaultdict(set), collections.defaultdict(set)
    for m in ok:
        for arch, sm, p in m["exact_programs"] or m["loose_programs"]:
            for s in m["shapes"]:
                fmts[(arch, sm, p)].add(tuple(sorted((k, v) for k, v in s["vertex"].items())))
            texs[(arch, sm, p)].add(tuple(sorted((k, v) for k, v in m["texture_types"].items())))
    if fmts or texs:
        fc = collections.Counter(len(v) for v in fmts.values())
        tc = collections.Counter(len(v) for v in texs.values())
        print("programs by distinct vertex-format sets:", sorted(fc.items()))
        print("programs by distinct texture-type sets:", sorted(tc.items()))
        dims = collections.Counter(v for m in ok for v in m["texture_types"].values())
        print("sampler texture types (nn::gfx::ImageDimension, None: no texture file):", dims.most_common())
        attrs = collections.Counter("%s=%#x" % (k, v) if v is not None else "%s=missing" % k
                                    for m in ok for s in m["shapes"] for k, v in s["vertex"].items())
        print("assigned attribute formats:", attrs.most_common(40))


def cmd_check(a):
    """The programs a run drew (HOS_DBG=drawlog:<present>: one frame's draws, each with its stages' code keys and its first
    colour target's format) against the map's prediction"""
    import re
    drawn = collections.defaultdict(lambda: {"formats": collections.Counter(), "lr": collections.Counter(), "draws": 0})
    for line in open(a.log, errors="replace"):
        m = re.search(r"direct: drawlog lr (0x[0-9a-f]+) kind (\d+) rt \d+ (\d+x\d+) c0 (-?\d+): .*? prog [0-9a-f]{16} "
                      r"codes ([0-9a-f]{16})/([0-9a-f]{16})", line)
        if m:
            d = drawn[(m[5], m[6])]
            d["formats"][int(m[4])] += 1
            d["lr"][m[1]] += 1
            d["draws"] += 1
    progs = [json.loads(l) for l in open(os.path.join(a.out, "programs.jsonl"))]
    by_pair = collections.defaultdict(list)
    for p in progs:
        st = dict(s.split(":") for s in p.get("stages", ()))
        vs = st.get("0") or st.get("7")
        if vs:
            by_pair[(vs, st.get("1", "0" * 16))].append(p)
    files = {l.strip() for l in open(a.models) if l.strip()} if a.models else None
    predicted = set()
    for l in open(os.path.join(a.out, "materials.jsonl")):
        m = json.loads(l)
        if files is None or os.path.basename(m["file"]) in files:
            predicted.update(tuple(p) for p in m.get("loose_programs", ()))
    known = {k for k in drawn if k in by_pair}
    hit = {k for k in known if any((p["archive"], p["shading_model"], p["program"]) in predicted for p in by_pair[k])}
    print("drawn graphics programs: %d (%d draws); a model archive's program: %d (%d draws); predicted%s: %d" % (
        len(drawn), sum(d["draws"] for d in drawn.values()), len(known), sum(drawn[k]["draws"] for k in known),
        " for the listed models" if files is not None else "", len(hit)))
    for k in sorted(known - hit):
        print("  not predicted:", k, [(p["archive"], p["shading_model"], p["program"], p["dynamic"]) for p in by_pair[k]][:2])
    # (the passes' first colour target formats, by gsys_assign_type: the render targets the files do not give)
    passes = collections.defaultdict(collections.Counter)
    for k in known:
        for p in by_pair[k][:1]:
            passes[p["dynamic"].get("gsys_assign_type")].update(drawn[k]["formats"])
    for t, c in sorted(passes.items(), key=lambda kv: str(kv[0])):
        print("  pass %s: c0 formats (draws) %s" % (t, dict(c)))
    lrs = collections.Counter()
    for k in set(drawn) - known:
        for lr, n in drawn[k]["lr"].items():
            lrs[lr] += 1
    print("not a model archive's program: %d (%d draws), by the drawing call's return address (programs): %s" % (
        len(drawn) - len(known), sum(d["draws"] for k, d in drawn.items() if k not in known), lrs.most_common(12)))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("map")
    m.add_argument("romfs")
    m.add_argument("--out", required=True)
    m.add_argument("--jobs", type=int, default=8)
    m.add_argument("--all-keys", action="store_true", help="code keys of every program, not only the reached ones (slow)")
    s = sub.add_parser("stats")
    s.add_argument("out")
    c = sub.add_parser("check")
    c.add_argument("out")
    c.add_argument("log")
    c.add_argument("--models")
    li = sub.add_parser("list")
    li.add_argument("out")
    li.add_argument("--scope", choices=list(SCOPES) + ["none"], default="all")
    li.add_argument("--other", metavar="ROMFS", help="also the programs of the romfs's other shader archives")
    li.add_argument("--models", help="only the materials of these bfres files (a basename a line: scene's list)")
    li.add_argument("--to", required=True)
    sc = sub.add_parser("scene")
    sc.add_argument("romfs")
    sc.add_argument("banc", nargs="+", help="the scene's placement files (OoT: Banc/<area>/*.bcett.byml.zs; DKB: StageData/<stage>.alsarc.zs)")
    sc.add_argument("--to", required=True)
    a = p.parse_args()
    return {"map": cmd_map, "stats": cmd_stats, "check": cmd_check, "list": cmd_list, "scene": cmd_scene}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
