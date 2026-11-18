//! The store the renderer maps (renderer/vk_assets.cpp), all little-endian, format version 1:
//! - `pack_NNN.bin`: a 64-byte header ("HKTXPAK1", u32 version, u32 pack id), then standard zstd frames back to back,
//!   up to the pack limit.
//! - `index.bin`: a 128-byte header, then the file, texture, mip and frame records and the string table.
//!   Header: magic "HKTXIDX1", u32 version, u32 header size, u64 config hash, u32 counts of files, textures, mips,
//!   frames, packs and string bytes, u64 offsets of the five tables, u16 sizes of the four records.
//!   File (64 B, one per BNTX): path, source size, BLAKE2b-64 of the romfs file, mtime (ns), BNTX size, first texture,
//!   textures, metadata frame (the BNTX up to BRTD+0x10 and from _RLT on), header length, _RLT offset, flags (1: inside
//!   a container).
//!   Texture (128 B): see `tex_record`; mip (16 B): offset in the raw data, bytes of one layer; frame (32 B): pack,
//!   compressed size, offset in the pack, offset and size in the texture's raw data.

use crate::convert::{Rec, Status};
use crate::plan::Kind;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

pub const FORMAT_VERSION: u32 = 1;
const FILE_REC: usize = 64;
const TEX_REC: usize = 128;
const MIP_REC: usize = 16;
const FRAME_REC: usize = 32;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct FrameRef {
    pub pack: u16,
    pub off: u64,
    pub csize: u32,
    pub raw_off: u64,
    pub raw_size: u32,
}

#[derive(Debug, Clone)]
pub struct TexEntry {
    pub rec: Rec,
    pub mips: Vec<(u64, u32)>,
    pub frames: Vec<FrameRef>,
}

#[derive(Debug, Clone)]
pub struct FileEntry {
    pub path: String,
    pub src_size: u64,
    pub src_hash: u64,
    pub mtime: u64,
    pub bntx_size: u64,
    pub head_len: u32,
    pub rlt_off: u32,
    pub flags: u32,
    pub meta: FrameRef,
    pub textures: Vec<TexEntry>,
}

pub fn pack_name(i: u32) -> String {
    format!("pack_{i:03}.bin")
}

/// Append-only packs of zstd frames, rolled over at `limit` bytes.
pub struct Packs {
    dir: PathBuf,
    limit: u64,
    f: Option<BufWriter<File>>,
    pos: u64,
    pub count: u32,
}

impl Packs {
    pub fn new(dir: &Path, limit: u64) -> Packs {
        Packs { dir: dir.to_path_buf(), limit, f: None, pos: 0, count: 0 }
    }

    fn open_next(&mut self) -> std::io::Result<()> {
        self.close()?;
        let id = self.count;
        let mut f = BufWriter::with_capacity(4 << 20, File::create(self.dir.join(pack_name(id)))?);
        f.write_all(b"HKTXPAK1")?;
        f.write_all(&FORMAT_VERSION.to_le_bytes())?;
        f.write_all(&id.to_le_bytes())?;
        f.write_all(&[0u8; 48])?;
        self.f = Some(f);
        self.pos = 64;
        self.count += 1;
        Ok(())
    }

    /// (pack, offset) of the frame written
    pub fn put(&mut self, data: &[u8]) -> std::io::Result<(u16, u64)> {
        if self.f.is_none() || (self.pos + data.len() as u64 > self.limit && self.pos > 64) {
            self.open_next()?;
        }
        let off = self.pos;
        self.f.as_mut().unwrap().write_all(data)?;
        self.pos += data.len() as u64;
        Ok(((self.count - 1) as u16, off))
    }

    pub fn close(&mut self) -> std::io::Result<()> {
        if let Some(f) = self.f.take() {
            let f = f.into_inner().map_err(|e| e.into_error())?;
            f.sync_all()?;
        }
        Ok(())
    }
}

fn put_u16(v: &mut Vec<u8>, x: u16) {
    v.extend_from_slice(&x.to_le_bytes());
}
fn put_u32(v: &mut Vec<u8>, x: u32) {
    v.extend_from_slice(&x.to_le_bytes());
}
fn put_u64(v: &mut Vec<u8>, x: u64) {
    v.extend_from_slice(&x.to_le_bytes());
}

fn frame_record(v: &mut Vec<u8>, f: &FrameRef) {
    put_u16(v, f.pack);
    put_u16(v, 0);
    put_u32(v, f.csize);
    put_u64(v, f.off);
    put_u64(v, f.raw_off);
    put_u32(v, f.raw_size);
    put_u32(v, 0);
}

/// The 128-byte texture record.
fn tex_record(v: &mut Vec<u8>, file: u32, r: &Rec, name: (u32, u32), mip_first: u32, fr_first: u32, fr_n: u32) {
    for x in [file, r.index, name.0, name.1, r.nvn_format, r.vk_format] {
        put_u32(v, x);
    }
    v.extend_from_slice(&[r.kind as u8, r.status as u8, r.view, r.target]);
    v.extend_from_slice(&r.map);
    v.extend_from_slice(&r.comp_sel);
    v.extend_from_slice(&[r.dropped, r.bh_log2]);
    put_u16(v, r.flags);
    for x in [r.width, r.height, r.depth, r.layers, r.mips, mip_first, fr_first, fr_n] {
        put_u32(v, x);
    }
    put_u64(v, r.raw_size);
    put_u64(v, r.tex_hash);
    for x in [r.brti_off, r.image_size, r.data_off] {
        put_u32(v, x);
    }
    v.extend_from_slice(&r.psnr.to_le_bytes());
    v.extend_from_slice(&r.psnr_min.to_le_bytes());
    put_u32(v, r.alignment);
    put_u32(v, r.hdr_blocks);
    v.extend_from_slice(&[r.rule, 0, 0, 0]);
    v.extend_from_slice(&r.psnr_first.to_le_bytes());
    for d in [r.nrm_mean, r.nrm_max] {
        put_u16(v, (d * 100.0).round().clamp(0.0, 65535.0) as u16);
    }
}

/// The index of `entries` (in their order), written to a temporary and renamed.
pub fn write_index(path: &Path, cfg_hash: u64, entries: &[FileEntry], n_packs: u32) -> std::io::Result<()> {
    let mut strings = Vec::new();
    let mut strtab = std::collections::HashMap::<String, u32>::new();
    let mut s = |x: &str| -> (u32, u32) {
        let o = *strtab.entry(x.to_string()).or_insert_with(|| {
            let o = strings.len() as u32;
            strings.extend_from_slice(x.as_bytes());
            o
        });
        (o, x.len() as u32)
    };
    let (mut files, mut texs, mut mips, mut frames) = (Vec::new(), Vec::new(), Vec::new(), Vec::new());
    let (mut n_tex, mut n_mip, mut n_frame) = (0u32, 0u32, 0u32);
    for (fi, e) in entries.iter().enumerate() {
        let (po, pl) = s(&e.path);
        let mf = n_frame;
        frame_record(&mut frames, &e.meta);
        n_frame += 1;
        put_u32(&mut files, po);
        put_u32(&mut files, pl);
        for x in [e.src_size, e.src_hash, e.mtime, e.bntx_size] {
            put_u64(&mut files, x);
        }
        for x in [n_tex, e.textures.len() as u32, mf, e.head_len, e.rlt_off, e.flags] {
            put_u32(&mut files, x);
        }
        for t in &e.textures {
            let name = s(&t.rec.name);
            let mip_first = n_mip;
            for &(mo, lb) in &t.mips {
                put_u64(&mut mips, mo);
                put_u32(&mut mips, lb);
                put_u32(&mut mips, 0);
                n_mip += 1;
            }
            let fr_first = n_frame;
            for f in &t.frames {
                frame_record(&mut frames, f);
                n_frame += 1;
            }
            tex_record(&mut texs, fi as u32, &t.rec, name, mip_first, fr_first, t.frames.len() as u32);
            n_tex += 1;
        }
    }
    let o_files = 128u64;
    let o_tex = o_files + files.len() as u64;
    let o_mips = o_tex + texs.len() as u64;
    let o_frames = o_mips + mips.len() as u64;
    let o_str = o_frames + frames.len() as u64;
    let mut hdr = b"HKTXIDX1".to_vec();
    put_u32(&mut hdr, FORMAT_VERSION);
    put_u32(&mut hdr, 128);
    put_u64(&mut hdr, cfg_hash);
    for x in [entries.len() as u32, n_tex, n_mip, n_frame, n_packs, strings.len() as u32] {
        put_u32(&mut hdr, x);
    }
    for x in [o_files, o_tex, o_mips, o_frames, o_str] {
        put_u64(&mut hdr, x);
    }
    for x in [FILE_REC, TEX_REC, MIP_REC, FRAME_REC] {
        put_u16(&mut hdr, x as u16);
    }
    hdr.resize(128, 0);
    let tmp = path.with_extension("bin.tmp");
    {
        let mut f = BufWriter::new(File::create(&tmp)?);
        for part in [&hdr, &files, &texs, &mips, &frames, &strings] {
            f.write_all(part)?;
        }
        let f = f.into_inner().map_err(|e| e.into_error())?;
        f.sync_all()?;
    }
    std::fs::rename(tmp, path)
}

fn u16_(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes(b[o..o + 2].try_into().unwrap())
}
fn u32_(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64_(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
fn f32_(b: &[u8], o: usize) -> f32 {
    f32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

fn frame_at(b: &[u8], o: usize) -> FrameRef {
    FrameRef { pack: u16_(b, o), csize: u32_(b, o + 4), off: u64_(b, o + 8), raw_off: u64_(b, o + 16), raw_size: u32_(b, o + 24) }
}

pub struct Index {
    pub cfg_hash: u64,
    pub n_packs: u32,
    pub entries: Vec<FileEntry>,
}

/// An index read back.
pub fn read_index(path: &Path) -> Result<Index, String> {
    let b = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if b.len() < 128 || &b[..8] != b"HKTXIDX1" {
        return Err(format!("{}: not a texture index", path.display()));
    }
    if u32_(&b, 8) != FORMAT_VERSION {
        return Err(format!("{}: index version {}", path.display(), u32_(&b, 8)));
    }
    let cfg_hash = u64_(&b, 16);
    let (n_files, n_tex, _n_mip, _n_frame, n_packs, n_str) =
        (u32_(&b, 24), u32_(&b, 28), u32_(&b, 32), u32_(&b, 36), u32_(&b, 40), u32_(&b, 44) as usize);
    let o: Vec<usize> = (0..5).map(|i| u64_(&b, 48 + 8 * i) as usize).collect();
    let st = &b[o[4]..o[4] + n_str];
    let text = |off: u32, n: u32| String::from_utf8_lossy(&st[off as usize..(off + n) as usize]).into_owned();
    let mut texs = Vec::with_capacity(n_tex as usize);
    for i in 0..n_tex as usize {
        let p = o[1] + i * TEX_REC;
        let t = &b[p..p + TEX_REC];
        let kind = [Kind::None, Kind::CopyBc, Kind::Copy, Kind::Bc7, Kind::Bc5, Kind::Bc4, Kind::Bc1, Kind::Raw][t[24] as usize];
        let status = [Status::Ok, Status::Refused, Status::Unsupported, Status::Error][t[25] as usize];
        let rec = Rec {
            index: u32_(t, 4),
            name: text(u32_(t, 8), u32_(t, 12)),
            nvn_format: u32_(t, 16),
            vk_format: u32_(t, 20),
            kind,
            status,
            view: t[26],
            target: t[27],
            map: t[28..32].try_into().unwrap(),
            comp_sel: t[32..36].try_into().unwrap(),
            dropped: t[36],
            bh_log2: t[37],
            flags: u16_(t, 38),
            width: u32_(t, 40),
            height: u32_(t, 44),
            depth: u32_(t, 48),
            layers: u32_(t, 52),
            mips: u32_(t, 56),
            raw_size: u64_(t, 72),
            tex_hash: u64_(t, 80),
            brti_off: u32_(t, 88),
            image_size: u32_(t, 92),
            data_off: u32_(t, 96),
            psnr: f32_(t, 100),
            psnr_min: f32_(t, 104),
            alignment: u32_(t, 108),
            hdr_blocks: u32_(t, 112),
            rule: t[116],
            psnr_first: f32_(t, 120),
            nrm_mean: u16_(t, 124) as f32 / 100.0,
            nrm_max: u16_(t, 126) as f32 / 100.0,
        };
        let (mip_first, fr_first, fr_n) = (u32_(t, 60) as usize, u32_(t, 64) as usize, u32_(t, 68) as usize);
        let nm = if status == Status::Ok { rec.mips as usize } else { 0 };
        let mips = (0..nm).map(|m| (u64_(&b, o[2] + (mip_first + m) * MIP_REC), u32_(&b, o[2] + (mip_first + m) * MIP_REC + 8))).collect();
        let frames = (0..fr_n).map(|k| frame_at(&b, o[3] + (fr_first + k) * FRAME_REC)).collect();
        texs.push(Some(TexEntry { rec, mips, frames }));
    }
    let mut entries = Vec::with_capacity(n_files as usize);
    for i in 0..n_files as usize {
        let f = &b[o[0] + i * FILE_REC..][..FILE_REC];
        let (tf, tn) = (u32_(f, 40) as usize, u32_(f, 44) as usize);
        entries.push(FileEntry {
            path: text(u32_(f, 0), u32_(f, 4)),
            src_size: u64_(f, 8),
            src_hash: u64_(f, 16),
            mtime: u64_(f, 24),
            bntx_size: u64_(f, 32),
            head_len: u32_(f, 52),
            rlt_off: u32_(f, 56),
            flags: u32_(f, 60),
            meta: frame_at(&b, o[3] + u32_(f, 48) as usize * FRAME_REC),
            textures: texs[tf..tf + tn].iter_mut().map(|t| t.take().unwrap()).collect(),
        });
    }
    Ok(Index { cfg_hash, n_packs, entries })
}

/// The bytes of frames as they are in the packs (compressed).
pub struct PackReader {
    dir: PathBuf,
    files: Vec<Option<File>>,
}

impl PackReader {
    pub fn new(dir: &Path) -> PackReader {
        PackReader { dir: dir.to_path_buf(), files: Vec::new() }
    }

    pub fn read(&mut self, f: &FrameRef) -> std::io::Result<Vec<u8>> {
        use std::os::unix::fs::FileExt;
        let i = f.pack as usize;
        if self.files.len() <= i {
            self.files.resize_with(i + 1, || None);
        }
        if self.files[i].is_none() {
            self.files[i] = Some(File::open(self.dir.join(pack_name(i as u32)))?);
        }
        let mut buf = vec![0u8; f.csize as usize];
        self.files[i].as_ref().unwrap().read_exact_at(&mut buf, f.off)?;
        Ok(buf)
    }
}
