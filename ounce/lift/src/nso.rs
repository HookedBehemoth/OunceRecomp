//! NSO (Switch executable) loading (third_party/a64-kit/tools/nso.py with nx2/nso2.py's DT_RELR): the three
//! LZ4-block segments (lz4_flex), MOD0, the dynamic section, symbols, relocations (RELA, JMPREL, and RELR as RELATIVE), the
//! init array and the .eh_frame_hdr function starts. Offsets are the module's own (from its .text).

use std::collections::HashMap;
use std::path::Path;

/// nso.MODULE_ORDER: rtld's load order
pub const MODULE_ORDER: [&str; 13] =
    ["rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"];

pub const STT_FUNC: u8 = 2;
pub const STB_WEAK: u8 = 2;

#[derive(Clone, Debug)]
pub struct Symbol {
    pub name: String,
    pub typ: u8,
    pub bind: u8,
    pub defined: bool,
    pub value: u64,
    pub size: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RelType {
    Abs64,
    GlobDat,
    JumpSlot,
    Relative,
    Other(u32),
}

#[derive(Clone, Debug)]
pub struct Reloc {
    pub offset: u64,
    pub typ: RelType,
    /// the symbol's index (0: none)
    pub sym: u32,
    pub addend: i64,
}

pub struct Nso {
    pub name: String,
    pub build_id: String,
    /// (memory offset, size, name)
    pub segments: Vec<(u64, u64, &'static str)>,
    pub bss_size: u64,
    pub image: Vec<u8>,
    pub mod0: u64,
    pub bss: (u64, u64),
    pub eh_frame_hdr: (u64, u64),
    pub dynamic: HashMap<i64, Vec<u64>>,
    pub symbols: Vec<Symbol>,
    pub relocs: Vec<Reloc>,
    pub init_array: Vec<u64>,
    pub eh_funcs: Vec<u64>,
}

/// an LZ4 block (no frame) of `size` bytes (lz4_flex)
pub fn lz4_block(src: &[u8], size: usize) -> Result<Vec<u8>, String> {
    let out = lz4_flex::block::decompress(src, size).map_err(|e| format!("lz4: {e}"))?;
    if out.len() != size {
        return Err(format!("lz4: {} bytes, expected {size}", out.len()));
    }
    Ok(out)
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

/// the C string at off (UTF-8, invalid sequences replaced, as Python's decode(errors="replace"))
fn cstr(b: &[u8], off: usize) -> String {
    let end = b[off..].iter().position(|&c| c == 0).map_or(b.len(), |p| off + p);
    String::from_utf8_lossy(&b[off..end]).into_owned()
}

const DT_STRTAB: i64 = 5;
const DT_SYMTAB: i64 = 6;
const DT_RELA: i64 = 7;
const DT_RELASZ: i64 = 8;
const DT_SYMENT: i64 = 11;
pub const DT_INIT: i64 = 12;
pub const DT_FINI: i64 = 13;
const DT_PLTRELSZ: i64 = 2;
const DT_JMPREL: i64 = 23;
pub const DT_INIT_ARRAY: i64 = 25;
pub const DT_INIT_ARRAYSZ: i64 = 27;
const DT_RELRSZ: i64 = 35;
const DT_RELR: i64 = 36;
const DT_RELRENT: i64 = 37;

impl Nso {
    pub fn load(path: &Path) -> Result<Nso, String> {
        let raw = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        if raw.len() < 0x100 || &raw[..4] != b"NSO0" {
            return Err(format!("{}: not an NSO", path.display()));
        }
        let name = path.file_name().unwrap().to_string_lossy().into_owned();
        let flags = u32le(&raw, 0xC);
        let bss_size = u32le(&raw, 0x3C) as u64;
        let mut id = &raw[0x40..0x60];
        while let [rest @ .., 0] = id {
            id = rest;
        }
        let build_id = id.iter().map(|b| format!("{b:02x}")).collect();
        let mut segments = Vec::new();
        let mut datas = Vec::new();
        let mut end = 0;
        for i in 0..3 {
            let (foff, moff, size) = (u32le(&raw, 0x10 + 0x10 * i) as usize, u32le(&raw, 0x14 + 0x10 * i) as u64, u32le(&raw, 0x18 + 0x10 * i) as usize);
            let cs = u32le(&raw, 0x60 + 4 * i) as usize;
            let data = &raw[foff..foff + cs];
            let data = if flags & (1 << i) != 0 { lz4_block(data, size)? } else { data.to_vec() };
            if data.len() != size {
                return Err(format!("{name} segment {i}: {} != {size}", data.len()));
            }
            segments.push((moff, size as u64, [".text", ".rodata", ".data"][i]));
            end = end.max(moff + size as u64);
            datas.push(data);
        }
        let mut image = vec![0u8; end as usize];
        for ((moff, size, _), data) in segments.iter().zip(&datas) {
            image[*moff as usize..(*moff + *size) as usize].copy_from_slice(data);
        }
        let mut n = Nso {
            name,
            build_id,
            segments,
            bss_size,
            image,
            mod0: 0,
            bss: (0, 0),
            eh_frame_hdr: (0, 0),
            dynamic: HashMap::new(),
            symbols: Vec::new(),
            relocs: Vec::new(),
            init_array: Vec::new(),
            eh_funcs: Vec::new(),
        };
        n.read_mod0()?;
        Ok(n)
    }

    fn d(&self, k: i64) -> u64 {
        self.dynamic.get(&k).map_or(0, |v| v[0])
    }

    fn read_mod0(&mut self) -> Result<(), String> {
        let m = u32le(&self.image, 4) as usize;
        if &self.image[m..m + 4] != b"MOD0" {
            return Err(format!("{}: no MOD0", self.name));
        }
        let rel = |k: usize| i32::from_le_bytes(self.image[m + 4 + 4 * k..m + 8 + 4 * k].try_into().unwrap()) as i64;
        let (dyn_, bss_s, bss_e, eh_s, eh_e) = (rel(0), rel(1), rel(2), rel(3), rel(4));
        let m64 = m as i64;
        self.mod0 = m as u64;
        self.bss = ((m64 + bss_s) as u64, (m64 + bss_e) as u64);
        self.eh_frame_hdr = ((m64 + eh_s) as u64, (m64 + eh_e) as u64);
        if (self.image.len() as u64) < self.bss.1 {
            self.image.resize(self.bss.1 as usize, 0);
        }
        let mut off = (m64 + dyn_) as usize;
        loop {
            let tag = u64le(&self.image, off) as i64;
            let val = u64le(&self.image, off + 8);
            off += 16;
            if tag == 0 {
                break;
            }
            self.dynamic.entry(tag).or_default().push(val);
        }
        let (strtab, symtab) = (self.d(DT_STRTAB), self.d(DT_SYMTAB));
        let syment = match self.d(DT_SYMENT) {
            0 => 24,
            x => x,
        };
        let nsyms = if strtab > symtab { (strtab - symtab) / syment } else { 0 };
        for i in 0..nsyms {
            let o = (symtab + i * syment) as usize;
            let b = &self.image;
            let name_off = u32le(b, o) as u64;
            let info = b[o + 4];
            let shndx = u16::from_le_bytes([b[o + 6], b[o + 7]]);
            self.symbols.push(Symbol {
                name: cstr(b, (strtab + name_off) as usize),
                typ: info & 0xF,
                bind: info >> 4,
                defined: shndx != 0,
                value: u64le(b, o + 8),
                size: u64le(b, o + 16),
            });
        }
        for (sk, zk) in [(DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)] {
            let (start, size) = (self.d(sk), self.d(zk));
            let mut off = start;
            while off < start + size {
                let o = off as usize;
                let (r_off, r_info, addend) = (u64le(&self.image, o), u64le(&self.image, o + 8), u64le(&self.image, o + 16) as i64);
                let t = (r_info & 0xFFFF_FFFF) as u32;
                let typ = match t {
                    257 => RelType::Abs64,
                    1025 => RelType::GlobDat,
                    1026 => RelType::JumpSlot,
                    1027 => RelType::Relative,
                    x => RelType::Other(x),
                };
                self.relocs.push(Reloc { offset: r_off, typ, sym: (r_info >> 32) as u32, addend });
                off += 24;
            }
        }
        let (ia, isz) = (self.d(DT_INIT_ARRAY), self.d(DT_INIT_ARRAYSZ));
        self.init_array = (0..isz / 8).map(|i| u64le(&self.image, (ia + 8 * i) as usize)).collect();
        self.eh_funcs = self.eh_table();
        // (nso2.py: DT_RELR, the packed relative relocations, as RELATIVE entries whose addend is the stored offset)
        let (start, size) = (self.d(DT_RELR), self.d(DT_RELRSZ));
        if start != 0 && size != 0 {
            let ent = self.d(DT_RELRENT);
            if ent != 0 && ent != 8 {
                return Err(format!("{}: RELRENT {ent}", self.name));
            }
            let mut where_ = 0u64;
            let mut off = start;
            while off < start + size {
                let e = u64le(&self.image, off as usize);
                off += 8;
                if e & 1 == 0 {
                    let a = u64le(&self.image, e as usize) as i64;
                    self.relocs.push(Reloc { offset: e, typ: RelType::Relative, sym: 0, addend: a });
                    where_ = e + 8;
                } else {
                    let mut bits = e >> 1;
                    let mut i = 0;
                    while bits != 0 {
                        if bits & 1 != 0 {
                            let o = where_ + 8 * i;
                            let a = u64le(&self.image, o as usize) as i64;
                            self.relocs.push(Reloc { offset: o, typ: RelType::Relative, sym: 0, addend: a });
                        }
                        bits >>= 1;
                        i += 1;
                    }
                    where_ += 63 * 8;
                }
            }
        }
        Ok(())
    }

    fn eh_table(&self) -> Vec<u64> {
        let (s, e) = self.eh_frame_hdr;
        if e <= s {
            return Vec::new();
        }
        let h = &self.image[s as usize..s as usize + 4];
        if h[0] != 1 || h[3] != 0x3B || h[2] != 0x03 {
            return Vec::new();
        }
        if !matches!(h[1] & 0x0F, 0x0B | 0x03) {
            return Vec::new();
        }
        let p = s as usize + 8;
        let count = u32le(&self.image, p) as usize;
        (0..count).map(|i| (s as i64 + i32::from_le_bytes(self.image[p + 4 + 8 * i..p + 8 + 8 * i].try_into().unwrap()) as i64) as u64).collect()
    }

    pub fn code_range(&self) -> (u64, u64) {
        (self.segments[0].0, self.segments[0].0 + self.segments[0].1)
    }
    pub fn dyn_first(&self, k: i64) -> Option<u64> {
        self.dynamic.get(&k).map(|v| v[0])
    }
    /// the relocation's symbol name (None: index 0)
    pub fn rel_sym(&self, r: &Reloc) -> Option<&str> {
        (r.sym != 0).then(|| self.symbols[r.sym as usize].name.as_str())
    }
}

/// the exefs's modules in rtld's order (nso.load_exefs)
pub fn load_exefs(dir: &Path) -> Result<Vec<Nso>, String> {
    MODULE_ORDER.iter().map(|n| dir.join(n)).filter(|p| p.exists()).map(|p| Nso::load(&p)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lz4_round_trip() {
        let data: Vec<u8> = (0..4096u32).map(|i| (i % 7) as u8).collect();
        let c = lz4_flex::block::compress(&data);
        assert_eq!(lz4_block(&c, data.len()).unwrap(), data);
        assert!(lz4_block(&c, data.len() + 1).is_err());
    }
}
