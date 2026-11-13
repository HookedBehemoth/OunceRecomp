"""BFSHA (NintendoWare shader archive, version 9.1.1) + embedded BNSH reader.

    python tools/dump/bfsha.py <file.bfsha[.zs] | pack.sarc.zs::member.bfsha> [-v] [--program MODEL IDX]

Prints shading models with option counts, program counts, key lengths and per-stage binary statistics.
--program decodes one program's key into option choices and shows its stage binaries.

Layout (v9, checked against romfs/Shader/*.bfsha; offsets in work/assets/formats.md):
  header 0x00 'FSHA    ' +0x08 u32 version 0x00090101 ... +0x20 u64 self/archive ptr, +0x28 u64 string pool,
         +0x30 u32 pool size, +0x38 u64 name, +0x40 u64 path, +0x48 u64 shading models, +0x50 u64 model dict,
         +0x58..+0x77 user ptr / reserved, +0x78 u16 model count, +0x7A u16 flags
  shading model (0x100 bytes) see ShadingModel.__init__
  option (0x28 bytes): name, choice dict, choice values (u32[]), u16 choice count, i16 default choice, u16 pad,
         u8 uniform-block offset, u8 key offset, u32 mask, u8 key word index, u8 shift, u16 pad
  program (0x40 bytes): sampler / image / uniform-block / storage-buffer location tables (per resource: i32 vertex
         location, i32 fragment location), u64 BNSH variation, u64 parent model, u32 used-attribute bits,
         u16 flags, u16 #samplers, u16 #images, u16 #blocks, u16 #storage buffers
  key table: per program (static key words + dynamic key words) u32; option choice =
         (key[word] & mask) >> shift with word = index (static) or static_len + index - key_offset (dynamic)
"""
import struct, sys, os, hashlib
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:] = [p for p in sys.path if os.path.abspath(p or '.') != _HERE] + [_HERE]   # (tools/ has a dis.py that shadows the stdlib)

NVN2_CONTROL_MAGIC = 0x98761234
# BNSH program header stage slots (+0x08..+0x5F, u64 each). Slots 0-5 as in BfshaLibrary; on NVN2 slots 7 and 8 are
# used by the meshlet shading model and their control sections carry stage 6 / 7 (task / mesh, by the NVN2 stage
# numbering vertex 0, fragment 1, geometry 2, tess control 3, tess eval 4, compute 5, task 6, mesh 7).
STAGES = ('vertex', 'tess_control', 'tess_eval', 'geometry', 'fragment', 'compute', 'slot6', 'task', 'mesh', 'slot9', 'slot10')


class Reader:
    def __init__(self, b):
        self.b = b

    def u64(self, o): return struct.unpack_from('<Q', self.b, o)[0]

    def string(self, p):
        if not p: return None
        n, = struct.unpack_from('<H', self.b, p)
        return self.b[p + 2:p + 2 + n].decode('utf-8', 'replace')

    def str_at(self, o): return self.string(self.u64(o))

    def dict_keys(self, o):
        if not o: return []
        n, = struct.unpack_from('<I', self.b, o + 4)
        return [self.str_at(o + 8 + 16 * i + 8) for i in range(1, n + 1)]


class Option:
    def __init__(self, r, o, dynamic):
        b = r.b
        self.name = r.str_at(o)
        cdict, cvals = struct.unpack_from('<QQ', b, o + 8)
        self.n_choices, self.default, _p, self.block_offset, self.key_offset, self.mask, self.word, self.shift = \
            struct.unpack_from('<HhHBBIBB', b, o + 0x18)
        self.choices = r.dict_keys(cdict)
        self.choice_values = list(struct.unpack_from('<%dI' % self.n_choices, b, cvals)) if cvals else []
        self.dynamic = dynamic

    def key_word(self, static_len):
        return static_len + self.word - self.key_offset if self.dynamic else self.word

    def choice_of(self, key, static_len):
        return (key[self.key_word(static_len)] & self.mask) >> self.shift


class ShadingModel:
    def __init__(self, arc, o):
        r = arc.r
        b = r.b
        self.off = o
        self.name = r.str_at(o)
        q = struct.unpack_from('<26Q', b, o + 8)
        (so, sd, do_, dd, ao, ad, smo, smd, imo, imd, ubo, ubd, uarr, sbo, sbd, unk0, self.prog_off, self.key_off,
         self.archive_off, self.symbol_off, self.bnsh_off) = q[:21]
        p = o + 8 + 26 * 8          # 0xD8
        self.n_uniforms, self.n_storage, self.default_program = struct.unpack_from('<IIi', b, p)
        self.n_static, self.n_dynamic, self.n_programs = struct.unpack_from('<HHH', b, p + 12)
        (self.static_key_len, self.dynamic_key_len, self.n_attribs, self.n_samplers, self.n_images, self.n_blocks,
         self.unk2) = struct.unpack_from('<7B', b, p + 18)
        self.block_indices = tuple(b[p + 25:p + 29])
        self.static_options = [Option(r, so + 0x28 * i, False) for i in range(self.n_static)]
        self.dynamic_options = [Option(r, do_ + 0x28 * i, True) for i in range(self.n_dynamic)]
        self.attributes = r.dict_keys(ad)
        self.samplers = r.dict_keys(smd)
        self.images = r.dict_keys(imd)
        self.uniform_blocks = r.dict_keys(ubd)
        self.storage_buffers = r.dict_keys(sbd)
        kl = self.static_key_len + self.dynamic_key_len
        self.key_len = kl
        self.keys = [struct.unpack_from('<%dI' % kl, b, self.key_off + 4 * kl * i) for i in range(self.n_programs)]
        self.bnsh = Bnsh(b, self.bnsh_off)
        self.programs = [self.program(self.prog_off + 0x40 * i) for i in range(self.n_programs)]

    def program(self, o):
        b = self.bnsh.b
        smp, img, ub, sb, var, parent = struct.unpack_from('<6Q', b, o)
        attr_bits, flags, ns, ni, nb, nsb = struct.unpack_from('<IHHHHH', b, o + 0x30)
        loc = lambda p, n: [struct.unpack_from('<ii', b, p + 8 * i) for i in range(n)] if p else []
        return {'variation': var, 'variation_index': (var - self.bnsh_off - self.bnsh.var_start_rel) // 0x40,
                'attr_bits': attr_bits, 'flags': flags, 'samplers': loc(smp, ns), 'images': loc(img, ni),
                'blocks': loc(ub, nb), 'storage': loc(sb, nsb)}

    def options_of(self, i):
        """{option: choice name} for program i."""
        k = self.keys[i]
        out = {}
        for op in self.static_options + self.dynamic_options:
            c = op.choice_of(k, self.static_key_len)
            out[op.name] = op.choices[c] if c < len(op.choices) else '#%d' % c
        return out

    def find_program(self, choices):
        """Program index for {option: choice} (unlisted options: default choice); -1 if absent."""
        want = [0] * self.key_len
        for op in self.static_options + self.dynamic_options:
            c = op.choices.index(choices[op.name]) if op.name in choices else max(op.default, 0)
            w = op.key_word(self.static_key_len)
            want[w] |= (c << op.shift) & op.mask
        for i, k in enumerate(self.keys):
            if list(k) == want: return i
        return -1


class Bnsh:
    """BNSH container embedded in a shading model ('BNSH' file header + 'grsc' block)."""
    def __init__(self, b, o):
        self.b = b
        self.off = o
        assert b[o:o + 4] == b'BNSH', b[o:o + 4]
        self.version, = struct.unpack_from('<I', b, o + 8)
        self.file_size, = struct.unpack_from('<I', b, o + 0x1C)
        g = o + 0x60          # 'BNSH' file header (0x20) + 0x40 reserved, then the 'grsc' block
        assert b[g:g + 4] == b'grsc', b[g:g + 4]
        self.api_type, self.api_version, self.code_target, self.compiler_version, self.n_variations = \
            struct.unpack_from('<HHIII', b, g + 0x10)
        self.var_start, self.mem_pool = struct.unpack_from("<QQ", b, g + 0x20)
        # offsets inside the BNSH are relative to the BNSH start
        self.var_start_rel = self.var_start

    def variation(self, i):
        b, o = self.b, self.off
        v = o + self.var_start + 0x40 * i
        # variation (0x40): u64 source program, u64 intermediate program (both 0 here), u64 binary program header
        # (BNSH-relative), u64 parent grsc, reserved
        src, ir, prog, parent = struct.unpack_from('<4Q', b, v)
        return self.program(o + prog) if prog else None

    def program(self, p):
        b, o = self.b, self.off
        flags, code_type, fmt, _ = struct.unpack_from('<4B', b, p)
        bin_fmt, = struct.unpack_from('<I', b, p + 4)
        stages = struct.unpack_from('<11Q', b, p + 8)
        out = {'flags': flags, 'code_type': code_type, 'format': fmt, 'binary_format': bin_fmt, 'stages': {}}
        obj_size, = struct.unpack_from('<I', b, p + 0x60)
        obj_off, parent, refl = struct.unpack_from('<QQQ', b, p + 0x68)
        out['object_size'], out['object_off'], out['reflection'] = obj_size, obj_off, refl
        for name, s in zip(STAGES, stages):
            if not s: continue
            sc = o + s
            ctrl_off, code_off, code_size, ctrl_size = struct.unpack_from('<QQII', b, sc + 8)
            ctrl = b[o + ctrl_off:o + ctrl_off + ctrl_size] if ctrl_off else b''
            code = b[o + code_off:o + code_off + code_size] if code_off else b''
            out['stages'][name] = {'ctrl_off': o + ctrl_off, 'ctrl_size': ctrl_size, 'code_off': o + code_off,
                                   'code_size': code_size, 'ctrl': ctrl, 'code': code}
        return out


def nvn2_control(ctrl):
    """Fields of the NVN2 control section that gsys / vk_shader.cpp use."""
    if len(ctrl) < 0x60: return None
    magic, = struct.unpack_from('<I', ctrl, 0)
    code_len, = struct.unpack_from('<I', ctrl, 0x38)
    stage, = struct.unpack_from('<H', ctrl, 0x56)
    return {'magic_ok': magic == NVN2_CONTROL_MAGIC, 'code_len': code_len, 'stage': stage}


class Bfsha:
    def __init__(self, b):
        assert b[:8] == b'FSHA    ', b[:8]
        self.r = Reader(b)
        self.b = b
        self.version, = struct.unpack_from('<I', b, 8)
        self.name, self.path = self.r.str_at(0x38), self.r.str_at(0x40)
        mo, md = struct.unpack_from('<QQ', b, 0x48)
        self.n_models, self.flags = struct.unpack_from('<HH', b, 0x78)
        self.models = [ShadingModel(self, mo + 0x100 * i) for i in range(self.n_models)]


def read_bytes(path):
    """File bytes; 'archive.sarc.zs::member' reads a SARC member."""
    import zs
    if '::' in path:
        import sarc
        a, m = path.split('::', 1)
        return sarc.unpack(open(a, 'rb').read())[m]
    return zs.decompress(open(path, 'rb').read())


def load(path):
    return Bfsha(read_bytes(path))


def stage_hash(st):
    return hashlib.sha1(st['ctrl'] + st['code']).hexdigest()


def stats(arc):
    """Per model: programs, programs with a binary, unique stage binaries per stage."""
    out = []
    for m in arc.models:
        uniq = {}
        with_bin = 0
        nvar = m.bnsh.n_variations
        ctrl_ok = 0
        nst = 0
        for i in range(nvar):
            p = m.bnsh.variation(i)
            if not p: continue
            with_bin += 1
            for s, st in p['stages'].items():
                nst += 1
                c = nvn2_control(st['ctrl'])
                ctrl_ok += bool(c and c['magic_ok'])
                uniq.setdefault(s, set()).add(stage_hash(st))
        out.append({'model': m.name, 'programs': m.n_programs, 'variations': nvar, 'variations_with_binary': with_bin,
                    'stage_binaries': nst, 'ctrl_magic_ok': ctrl_ok,
                    'unique': {k: len(v) for k, v in uniq.items()}, 'unique_hashes': uniq,
                    'static_options': m.n_static, 'dynamic_options': m.n_dynamic,
                    'key_len': (m.static_key_len, m.dynamic_key_len)})
    return out


def summary(path, verbose=False, prog=None):
    a = load(path)
    v = a.version
    print('%s: BFSHA v%d.%d.%d name=%r path=%r shading models=%d flags=%#x' % (
        os.path.basename(path), v >> 16, (v >> 8) & 0xff, v & 0xff, a.name, a.path, a.n_models, a.flags))
    for m in a.models:
        bn = m.bnsh
        print(' model %-28r static opts %3d dynamic %3d programs %6d key words %d+%d attribs %d samplers %d images %d '
              'blocks %d storage %d default prog %d | BNSH v%#x api %d/%d target %#x compiler %#x variations %d' % (
                  m.name, m.n_static, m.n_dynamic, m.n_programs, m.static_key_len, m.dynamic_key_len, m.n_attribs,
                  m.n_samplers, m.n_images, m.n_blocks, m.n_storage, m.default_program, bn.version, bn.api_type,
                  bn.api_version, bn.code_target, bn.compiler_version, bn.n_variations))
        if verbose:
            for op in m.static_options + m.dynamic_options:
                print('   %s %-48s choices=%-3d default=%-3d word=%d mask=%#010x shift=%-2d keyoff=%d %s' % (
                    'D' if op.dynamic else 'S', op.name, op.n_choices, op.default, op.word, op.mask, op.shift,
                    op.key_offset, op.choices[:6]))
    if not prog:
        for s in stats(a):
            print('  %-28s programs %6d binaries %6d stages %6d ctrl-magic ok %6d unique per stage %s' % (
                s['model'], s['programs'], s['variations_with_binary'], s['stage_binaries'], s['ctrl_magic_ok'], s['unique']))
    else:
        m = next(x for x in a.models if x.name == prog[0])
        i = int(prog[1])
        p = m.programs[i]
        print('program %d key %s variation %d attr bits %#x flags %#x' % (i, ['%08x' % x for x in m.keys[i]], p['variation_index'], p['attr_bits'], p['flags']))
        print(' options:', {k: v for k, v in m.options_of(i).items()})
        bp = m.bnsh.variation(p['variation_index'])
        for s, st in bp['stages'].items():
            print(' stage %-8s ctrl %#x+%#x code %#x+%#x control=%s sha1=%s' % (s, st['ctrl_off'], st['ctrl_size'], st['code_off'],
                  st['code_size'], nvn2_control(st['ctrl']), stage_hash(st)[:16]))


if __name__ == '__main__':
    args = sys.argv[1:]
    prog = None
    if '--program' in args:
        i = args.index('--program'); prog = args[i + 1:i + 3]
    summary(args[0], '-v' in args, prog)
