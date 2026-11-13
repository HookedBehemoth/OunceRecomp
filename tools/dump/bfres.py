"""BFRES (NintendoWare G3d, version 10.4.0, Switch-family 64-bit layout) reader: models, vertex buffers, shapes/meshes,
materials and their shader assigns.

    python tools/dump/bfres.py <file.bfres[.zs]> [-v]       summary (-v: every material's options / samplers / render infos)

Strings: a string field is a u64 that is either a file offset (-> u16 length + chars) or, when the file's external
flag (header +0xEE) has bit 1 (HasExternalString), a 64-bit ID resolved through romfs/Shader/ExternalBinaryString.bfres
(external flag bit 2, "HoldsExternalStrings": a table of u64 IDs at header +0xB8 whose names are the keys of the
dict at +0xC0). Layout notes in work/assets/formats.md; offsets were checked against the romfs files.
"""
import struct, sys, os
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:] = [p for p in sys.path if os.path.abspath(p or '.') != _HERE] + [_HERE]   # (tools/ has a dis.py that shadows the stdlib)

ROOT = os.path.dirname(os.path.dirname(_HERE))


def default_romfs():
    """The romfs of the dump named in work/dump.txt (its first line that is not a comment)."""
    try:
        for line in open(os.path.join(ROOT, 'work', 'dump.txt'), encoding='utf-8'):
            line = line.strip()
            if line and not line.startswith('#'):
                return os.path.join(line, 'romfs')
    except OSError:
        pass
    sys.exit('no dump: write its folder into work/dump.txt')


# vertex attribute format: stored as 2 bytes [variant, type] (BfresLibrary reads it as a big-endian u16)
ATTR_TYPE = {0x01: ('4_4', 1), 0x02: ('8', 1), 0x03: ('16(i2f)', 2), 0x04: ('8_8(i2f)', 2), 0x07: ('16_16(i2f)', 4),
             0x09: ('8_8', 2), 0x0a: ('16', 2), 0x0b: ('8_8_8_8', 4), 0x0e: ('10_10_10_2', 4), 0x12: ('16_16', 4),
             0x14: ('32', 4), 0x15: ('16_16_16_16', 8), 0x16: ('32', 4), 0x17: ('32_32', 8), 0x18: ('32_32_32', 12),
             0x19: ('32_32_32_32', 16)}
ATTR_VAR = {0: 'UNorm', 1: 'UNorm', 2: 'SNorm', 3: 'UInt', 4: 'SInt', 5: 'Float', 8: 'UIntToFloat', 0xa: 'SIntToFloat'}
PRIM = {0: 'points', 1: 'lines', 2: 'line_strip', 3: 'triangles', 4: 'triangle_strip', 5: 'lines_adj',
        6: 'line_strip_adj', 7: 'triangles_adj', 8: 'triangle_strip_adj', 9: 'patches'}
INDEX = {0: 'u8', 1: 'u16', 2: 'u32'}
RENDER_INFO_TYPE = {0: 'int', 1: 'float', 2: 'string'}
PARAM_TYPE = ['bool', 'bool2', 'bool3', 'bool4', 'int', 'int2', 'int3', 'int4', 'uint', 'uint2', 'uint3', 'uint4',
              'float', 'float2', 'float3', 'float4', 'reserved2', 'float2x2', 'float2x3', 'float2x4', 'reserved3',
              'float3x2', 'float3x3', 'float3x4', 'reserved4', 'float4x2', 'float4x3', 'float4x4', 'srt2d', 'srt3d',
              'texsrt', 'texsrtex']

_ext = None


def external_strings():
    """{id: name} from romfs/Shader/ExternalBinaryString.bfres.zs."""
    global _ext
    if _ext is None:
        import zs
        b = zs.decompress(open(os.path.join(default_romfs(), 'Shader/ExternalBinaryString.bfres.zs'), 'rb').read())
        r = Bfres(b, resolve_external=False)
        assert r.ext_flag & 4, 'not a string holder'
        ids_off, dict_off = struct.unpack_from('<QQ', b, 0xB8)
        keys = r.dict_keys(dict_off)
        _ext = {struct.unpack_from('<Q', b, ids_off + 8 * i)[0]: k for i, k in enumerate(keys)}
    return _ext


class Bfres:
    def __init__(self, b, resolve_external=True):
        assert b[:8] == b'FRES    ', b[:8]
        self.b = b
        self.version, = struct.unpack_from('<I', b, 8)
        self.align, self.addr = b[0xE], b[0xF]
        (self.name_off, self.flags, self.block_off, self.rlt_off, self.file_size) = struct.unpack_from('<IHHII', b, 0x10)
        self.ext_flag = b[0xEE]
        self.ext = external_strings() if (resolve_external and self.ext_flag & 2) else {}
        self.name = self.str_at(0x20)
        # header +0x28.. (v10): models, 32 reserved, 5 anim kinds (array+dict), memory pool, buffer info,
        # external files (array+dict), pad, string table, pool size, counts
        self.models_off, self.models_dict = struct.unpack_from('<QQ', b, 0x28)
        self.anim = {k: struct.unpack_from('<QQ', b, o) for k, o in
                     (('skeletal', 0x58), ('material', 0x68), ('bone_vis', 0x78), ('shape', 0x88), ('scene', 0x98))}
        self.mem_pool, self.buf_info, self.extfiles_off, self.extfiles_dict, _, self.str_tbl = struct.unpack_from('<6Q', b, 0xA8)
        self.str_pool_size, = struct.unpack_from('<I', b, 0xD8)
        (self.n_models, self.n_unk1, self.n_unk2, self.n_skel_anim, self.n_mat_anim, self.n_bone_vis, self.n_shape_anim,
         self.n_scene_anim, self.n_ext_files) = struct.unpack_from('<9H', b, 0xDC)
        # BufferInfo: u32 unk, u32 size, u64 GPU data offset (vertex/index data base)
        if self.buf_info:
            self.gpu_unk, self.gpu_size, self.gpu_off = struct.unpack_from('<IIQ', b, self.buf_info)
        else:
            self.gpu_unk = self.gpu_size = self.gpu_off = 0
        self.models = [self.model(self.models_off + 0x78 * i) for i in range(self.n_models)] if self.ext_flag & 4 == 0 else []

    # -- primitives
    def u64(self, o): return struct.unpack_from('<Q', self.b, o)[0]

    def string(self, p):
        if p == 0: return None
        if p >= len(self.b):
            return self.ext.get(p, '<ext:%016x>' % p)
        n, = struct.unpack_from('<H', self.b, p)
        return self.b[p + 2:p + 2 + n].decode('utf-8', 'replace')

    def str_at(self, o): return self.string(self.u64(o))

    def strings(self, o, n): return [self.str_at(o + 8 * i) for i in range(n)] if o else []

    def dict_keys(self, o):
        """ResDict: u32 0, u32 count, (count+1) nodes {u32 ref bit, u16 left, u16 right, u64 key} (node 0 = root)."""
        if not o: return []
        n, = struct.unpack_from('<I', self.b, o + 4)
        return [self.str_at(o + 8 + 16 * i + 8) for i in range(1, n + 1)]

    # -- structures
    def model(self, o):
        b = self.b
        assert b[o:o + 4] == b'FMDL', hex(o)
        m = {'off': o}
        m['flags'], = struct.unpack_from('<I', b, o + 4)
        m['name'], m['path'] = self.str_at(o + 8), self.str_at(o + 0x10)
        skel, vbs, shp, shp_d, mat, mat_d, sa, ud, ud_d, up = struct.unpack_from('<10Q', b, o + 0x18)
        nvb, nshp, nmat, nsa, nud = struct.unpack_from('<5H', b, o + 0x68)
        m['skeleton'] = self.skeleton(skel) if skel else None
        m['vertex_buffers'] = [self.vertex_buffer(vbs + 0x58 * i) for i in range(nvb)]
        m['shapes'] = [self.shape(shp + 0x60 * i) for i in range(nshp)]
        m['shader_assigns'] = {sa + 0x58 * i: self.shader_assign(sa + 0x58 * i) for i in range(nsa)}
        m['materials'] = [self.material(mat + 0xB0 * i, m['shader_assigns']) for i in range(nmat)]
        m['shape_keys'] = self.dict_keys(shp_d)
        m['material_keys'] = self.dict_keys(mat_d)
        m['n_user_data'] = nud
        return m

    def skeleton(self, o):
        b = self.b
        assert b[o:o + 4] == b'FSKL', hex(o)
        # FSKL: magic, u32 flags, u64 bone dict, u64 bones, u64 matrix->bone, u64 inverse matrices, u64 user ptr,
        # (v10: + ...) then u16 bone count, smooth count, rigid count
        flags, = struct.unpack_from('<I', b, o + 4)
        bone_dict, bones = struct.unpack_from('<QQ', b, o + 8)
        keys = self.dict_keys(bone_dict)
        return {'flags': flags, 'n_bones': len(keys), 'bones': keys}

    def vertex_buffer(self, o):
        b = self.b
        if b[o:o + 4] == bytes(4):
            # all-zero FVTX: the shapes using it have flag 0x10 (geometry in a packed/meshlet encoding inside the GPU
            # section, decoded at run time -- format not identified yet)
            return {'off': o, 'zeroed': True, 'attributes': [], 'buffers': [], 'vertex_count': 0, 'skin_count': 0}
        assert b[o:o + 4] == b'FVTX', hex(o)
        v = {'off': o, 'zeroed': False}
        v['flags'], = struct.unpack_from('<I', b, o + 4)
        attrs, attr_d, pool, unk1, unk2, sizes, strides, pad = struct.unpack_from('<8Q', b, o + 8)
        boff, nattr, nbuf, idx, vcount, skin, align = struct.unpack_from('<IBBHIHH', b, o + 0x48)
        v.update(buffer_offset=boff, vertex_count=vcount, skin_count=skin, gpu_align=align, index=idx)
        v['attributes'] = []
        for i in range(nattr):
            a = attrs + 0x10 * i
            var, typ = b[a + 8], b[a + 9]
            aoff, bidx = struct.unpack_from('<HH', b, a + 0xC)
            v['attributes'].append({'name': self.str_at(a), 'format': (var << 8) | typ,
                                    'format_name': '%s_%s' % (ATTR_TYPE.get(typ, ('t%02x' % typ, 0))[0], ATTR_VAR.get(var, 'v%x' % var)),
                                    'offset': aoff, 'buffer': bidx})
        v['buffers'] = []
        p = self.gpu_off + boff
        for i in range(nbuf):
            sz, sflag = struct.unpack_from('<II', b, sizes + 0x10 * i)
            st, = struct.unpack_from('<I', b, strides + 0x10 * i)
            if align: p = (p + align - 1) // align * align
            v['buffers'].append({'size': sz, 'size_flag': sflag, 'stride': st, 'data_off': p})
            p += sz
        return v

    def shape(self, o):
        b = self.b
        assert b[o:o + 4] == b'FSHP', hex(o)
        s = {'off': o}
        s['flags'], = struct.unpack_from('<I', b, o + 4)
        s['name'] = self.str_at(o + 8)
        vb, meshes, skin_idx, ks, ks_d, bbox, radius, up = struct.unpack_from('<8Q', b, o + 0x10)
        idx, s['material'], s['bone'], s['vertex_buffer'], nskin, s['skin_count'], nmesh, nkeys, s['target_attribs'] = \
            struct.unpack_from('<5HBBBB', b, o + 0x50)
        s['skin_bones'] = list(struct.unpack_from('<%dH' % nskin, b, skin_idx)) if nskin else []
        s['key_shapes'] = self.dict_keys(ks_d)
        s['meshes'] = []
        s['encoded'] = bool(s['flags'] & 0x10)
        if s['encoded']:
            # flag 0x10: index/count fields are zero, the FVTX is zeroed and the mesh record differs; geometry lives
            # in an encoded blob in the GPU section (see formats.md). Not decoded.
            return s
        for i in range(nmesh):
            m = meshes + 0x38 * i
            sub, pool, buf, bsize = struct.unpack_from('<4Q', b, m)
            face_off, prim, ifmt, icount, first, nsub = struct.unpack_from('<5IH', b, m + 0x20)
            size, sflag = struct.unpack_from('<II', b, bsize)
            s['meshes'].append({'primitive': PRIM.get(prim, prim), 'index_format': INDEX.get(ifmt, ifmt),
                                'index_count': icount, 'first_vertex': first, 'index_size': size,
                                'index_data_off': self.gpu_off + face_off,
                                'submeshes': [struct.unpack_from('<II', b, sub + 8 * j) for j in range(nsub)]})
        return s

    def shader_assign(self, o):
        """ShaderAssign (v10: shared per model, 0x58 bytes): archive, shading model, render-info and shader-param
        name tables + dicts, attribute / sampler / option dicts (keys = the shading model's names)."""
        b = self.b
        arc, sm, ri_list, ri_d, sp_list, sp_d, aa_d, sa_d, op_d = struct.unpack_from('<9Q', b, o)
        nri, nsp, sp_size = struct.unpack_from('<3H', b, o + 0x48)
        a = {'archive': self.string(arc), 'shading_model': self.string(sm), 'param_size': sp_size}
        a['render_infos'] = [(self.str_at(ri_list + 16 * i), RENDER_INFO_TYPE.get(b[ri_list + 16 * i + 8])) for i in range(nri)]
        a['params'] = []
        for i in range(nsp):
            p = sp_list + 24 * i
            doff, typ = struct.unpack_from('<HH', b, p + 16)
            a['params'].append((self.str_at(p + 8), doff, PARAM_TYPE[typ] if typ < len(PARAM_TYPE) else typ))
        a['attrib_keys'] = self.dict_keys(aa_d)
        a['sampler_keys'] = self.dict_keys(sa_d)
        a['option_keys'] = self.dict_keys(op_d)
        return a

    def material(self, o, assigns):
        b = self.b
        assert b[o:o + 4] == b'FMAT', hex(o)
        m = {'off': o}
        m['flags'], = struct.unpack_from('<I', b, o + 4)
        m['name'] = self.str_at(o + 8)
        (info, tex_arr, tex_names, smp_arr, smp, smp_d, ri_data, ri_count, ri_offs, param_src, param_idx, _r, ud, ud_d,
         volatile, up, smp_slot, tex_slot) = struct.unpack_from('<18Q', b, o + 0x10)
        idx, nsmp, ntex, _r2, nud, ri_size, ubo = struct.unpack_from('<HBBHHHH', b, o + 0xA0)
        m['textures'] = self.strings(tex_names, ntex)
        m['samplers'] = self.dict_keys(smp_d)
        m['render_info_size'] = ri_size
        if not info:
            m['assign'] = None
            return m
        # ShaderInfo (0x50): assign ptr, attrib assign names, attrib idx, sampler assign names, sampler idx,
        # bool option bits (u64[]), choice option strings, option choice indices, pad, counts
        sa, aa, aai, sma, smai, otog, ostr, oidx = struct.unpack_from('<8Q', b, info)
        naa, nsma, nbool, nchoice = struct.unpack_from('<BBHH', b, info + 0x44)
        a = assigns.get(sa) or self.shader_assign(sa)
        m['assign'] = a
        attrib_vals = self.strings(aa, naa)
        smp_vals = self.strings(sma, nsma)
        def idx8(p, used, total):
            return list(struct.unpack_from('<%db' % total, b, p + used)) if p else list(range(total))
        def idx16(p, used, total):
            return list(struct.unpack_from('<%dh' % total, b, p + 2 * used)) if p else list(range(total))
        ai = idx8(aai, naa, len(a['attrib_keys']))
        si = idx8(smai, nsma, len(a['sampler_keys']))
        m['attrib_assign'] = {k: (attrib_vals[i] if i >= 0 else None) for k, i in zip(a['attrib_keys'], ai)}
        m['sampler_assign'] = {k: (smp_vals[i] if i >= 0 else None) for k, i in zip(a['sampler_keys'], si)}
        bits = [self.u64(otog + 8 * i) for i in range((nbool + 63) // 64)] if otog else []
        choices = ['1' if (bits[i // 64] >> (i % 64)) & 1 else '0' for i in range(nbool)] + self.strings(ostr, nchoice - nbool)
        oi = idx16(oidx, nchoice, len(a['option_keys']))
        m['options'] = {k: (choices[i] if 0 <= i < len(choices) else None) for k, i in zip(a['option_keys'], oi)}
        m['n_bool_options'], m['n_choice_options'] = nbool, nchoice
        # render infos: per info a u16 count and u16 data offset, values in the data table
        m['render_infos'] = {}
        for i, (name, typ) in enumerate(a['render_infos']):
            cnt, = struct.unpack_from('<H', b, ri_count + 2 * i)
            doff, = struct.unpack_from('<H', b, ri_offs + 2 * i)
            p = ri_data + doff
            if typ == 'string': val = self.strings(p, cnt)
            elif typ == 'float': val = list(struct.unpack_from('<%df' % cnt, b, p))
            else: val = list(struct.unpack_from('<%di' % cnt, b, p))
            m['render_infos'][name] = val
        m['param_data_off'] = param_src
        return m


def load(path):
    import zs
    return Bfres(zs.decompress(open(path, 'rb').read()))


def summary(path, verbose=False):
    r = load(path)
    v = r.version
    print('%s: BFRES v%d.%d.%d name=%r models=%d anims(skel=%d mat=%d bonevis=%d shape=%d scene=%d) ext_files=%d '
          'ext_flag=%#x gpu data @%#x size %#x' % (os.path.basename(path), v >> 16, (v >> 8) & 0xff, v & 0xff, r.name,
          r.n_models, r.n_skel_anim, r.n_mat_anim, r.n_bone_vis, r.n_shape_anim, r.n_scene_anim, r.n_ext_files, r.ext_flag, r.gpu_off, r.gpu_size))
    for m in r.models:
        sk = m['skeleton']
        print(' model %r: %d bones, %d vertex buffers, %d shapes, %d materials, %d shader assigns' % (
            m['name'], sk['n_bones'] if sk else 0, len(m['vertex_buffers']), len(m['shapes']), len(m['materials']), len(m['shader_assigns'])))
        for i, vb in enumerate(m['vertex_buffers']):
            print('  vb%d: %d verts, skin %d, buffers %s; attrs %s' % (i, vb['vertex_count'], vb['skin_count'],
                  [(x['stride'], x['size']) for x in vb['buffers']],
                  ' '.join('%s:%s@%d/b%d' % (a['name'], a['format_name'], a['offset'], a['buffer']) for a in vb['attributes'])))
        for s in m['shapes']:
            ms = s['meshes']
            print('  shape %r mat=%d vb=%d skin=%d lods=%d: %s' % (s['name'], s['material'], s['vertex_buffer'], s['skin_count'], len(ms),
                  ', '.join('%s %s x%d' % (x['primitive'], x['index_format'], x['index_count']) for x in ms[:3])))
        for a in m['shader_assigns'].values():
            print('  assign: archive=%r model=%r options=%d samplers=%d attribs=%d params=%d render_infos=%d' % (
                a['archive'], a['shading_model'], len(a['option_keys']), len(a['sampler_keys']), len(a['attrib_keys']), len(a['params']), len(a['render_infos'])))
        for mt in m['materials']:
            a = mt['assign']
            print('  material %r -> %s/%s textures=%s' % (mt['name'], a and a['archive'], a and a['shading_model'], mt['textures']))
            if verbose and a:
                print('    options:', {k: v for k, v in mt['options'].items() if v is not None})
                print('    samplers:', {k: v for k, v in mt['sampler_assign'].items() if v is not None})
                print('    attribs:', {k: v for k, v in mt['attrib_assign'].items() if v is not None})
                print('    render infos:', mt['render_infos'])


if __name__ == '__main__':
    summary(sys.argv[1], '-v' in sys.argv)
