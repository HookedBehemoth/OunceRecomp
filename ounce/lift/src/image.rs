//! The guest image as the lift built it (nx2/image.py EdenImage, written by nx2/lift.py as image.bin and image.json):
//! its bytes, modules, code ranges and hooks; the function starts of each module's .eh_frame_hdr; and the extra entries
//! at the code addresses ADR / ADRP+ADD form (nx2/adr_entries.py).

use serde::Deserialize;
use std::path::Path;

#[derive(Deserialize, Debug, Clone)]
pub struct Segment {
    pub name: String,
    pub addr: u64,
    pub size: u64,
}

#[derive(Deserialize, Debug, Clone)]
pub struct Module {
    pub name: String,
    pub base: u64,
    pub size: u64,
    pub segments: Vec<Segment>,
}

#[derive(Deserialize, Debug, Clone)]
pub struct Hook {
    pub addr: u64,
    pub c: String,
    pub sym: String,
}

#[derive(Deserialize)]
struct Raw {
    base: u64,
    end: u64,
    modules: Vec<Module>,
    hooks: Vec<Hook>,
}

pub struct Image {
    pub base: u64,
    pub end: u64,
    pub mem: Vec<u8>,
    pub modules: Vec<Module>,
    pub hooks: Vec<Hook>,
    /// [lo, hi) of each module's code (its first segment, nso code_range)
    pub code: Vec<(u64, u64)>,
}

impl Image {
    pub fn load(dir: &Path) -> Result<Image, String> {
        let j = dir.join("image.json");
        let raw: Raw = serde_json::from_slice(&std::fs::read(&j).map_err(|e| format!("{}: {e}", j.display()))?)
            .map_err(|e| format!("{}: {e}", j.display()))?;
        let b = dir.join("image.bin");
        let mem = std::fs::read(&b).map_err(|e| format!("{}: {e}", b.display()))?;
        assert_eq!(mem.len() as u64, raw.end - raw.base, "image.bin's size");
        let code = raw.modules.iter().map(|m| (m.segments[0].addr, m.segments[0].addr + m.segments[0].size)).collect();
        Ok(Image { base: raw.base, end: raw.end, mem, modules: raw.modules, hooks: raw.hooks, code })
    }

    #[inline]
    pub fn in_code(&self, a: u64) -> bool {
        a & 3 == 0 && self.code.iter().any(|&(lo, hi)| lo <= a && a < hi)
    }
    /// the word at a (in the image)
    #[inline]
    pub fn word(&self, a: u64) -> u32 {
        self.try_word(a).unwrap_or_else(|| panic!("word outside the image: {a:#x}"))
    }
    pub fn try_word(&self, a: u64) -> Option<u32> {
        let o = a.checked_sub(self.base)? as usize;
        let b = self.mem.get(o..o + 4)?;
        Some(u32::from_le_bytes(b.try_into().unwrap()))
    }
    /// n bytes at a, or None outside [base, end) (NxImage.read's ValueError)
    pub fn read(&self, a: u64, n: u64) -> Option<&[u8]> {
        if !(self.base <= a && a.checked_add(n)? <= self.end) {
            return None;
        }
        let o = (a - self.base) as usize;
        Some(&self.mem[o..o + n as usize])
    }
    /// [a, a + n) lies in a module's .text or .rodata (Discovery.read_only)
    pub fn read_only(&self, a: u64, n: u64) -> bool {
        self.modules.iter().any(|m| {
            m.segments.len() >= 2 && m.segments[0].addr <= a && a + n <= m.segments[1].addr + m.segments[1].size
        })
    }
    fn u32_at(&self, a: u64) -> u32 {
        self.word(a)
    }
    fn i32_at(&self, a: u64) -> i64 {
        self.word(a) as i32 as i64
    }

    /// function starts from a module's .eh_frame_hdr table (nso._eh_frame_hdr_table), absolute
    pub fn eh_funcs(&self, s: u64, e: u64) -> Vec<u64> {
        if e <= s {
            return Vec::new();
        }
        let h = &self.mem[(s - self.base) as usize..(s - self.base) as usize + 4];
        let (ver, ptr_enc, cnt_enc, tbl_enc) = (h[0], h[1], h[2], h[3]);
        if ver != 1 || tbl_enc != 0x3B || cnt_enc != 0x03 {
            return Vec::new();
        }
        let p = s + 8;
        if !matches!(ptr_enc & 0x0F, 0x0B | 0x03) {
            return Vec::new();
        }
        let count = self.u32_at(p) as u64;
        let p = p + 4;
        (0..count).map(|i| (s as i64 + self.i32_at(p + 8 * i)) as u64).collect()
    }

    /// (start, end) of every function the .eh_frame_hdr tables have an FDE for, sorted (adr_entries.fde_spans);
    /// hdrs: (module, start, end) absolute
    pub fn fde_spans(&self, hdrs: &[(String, u64, u64)]) -> Vec<(u64, u64)> {
        let mut spans = Vec::new();
        for (name, s, e) in hdrs {
            let (s, e) = (*s, *e);
            if e <= s {
                continue;
            }
            let m = self.modules.iter().find(|m| &m.name == name).expect("eh_frame_hdr of no module");
            let h = self.read(s, 4).unwrap();
            if h[0] != 1 || h[3] != 0x3B || h[2] != 0x03 {
                continue;
            }
            let count = self.u32_at(s + 8) as u64;
            let mend = m.base + m.size;
            for i in 0..count {
                let loc = (s as i64 + self.i32_at(s + 12 + 8 * i)) as u64;
                let fde = (s as i64 + self.i32_at(s + 16 + 8 * i)) as u64;
                if fde + 16 > mend {
                    continue;
                }
                let length = self.u32_at(fde);
                let cie = self.u32_at(fde + 4);
                let begin = self.i32_at(fde + 8);
                let rng = self.u32_at(fde + 12) as u64;
                if length < 12 || cie == 0 || (fde as i64 + 8 + begin) as u64 != loc {
                    continue;
                }
                spans.push((loc, loc + rng));
            }
        }
        spans.sort_unstable();
        spans
    }

    /// the 4-aligned code addresses ADR / ADRP+ADD form in the .text segments, less those strictly inside a function
    /// spans covers (adr_entries.code_address_refs)
    pub fn code_address_refs(&self, spans: &[(u64, u64)]) -> Vec<u64> {
        let texts: Vec<(u64, u64)> = self
            .modules
            .iter()
            .flat_map(|m| m.segments.iter().filter(|s| s.name == ".text").map(|s| (s.addr, s.addr + s.size)))
            .collect();
        let mut found: Vec<i64> = Vec::new();
        for &(lo, hi) in &texts {
            let n = ((hi - lo) / 4) as usize;
            let w = |i: usize| self.word(lo + 4 * i as u64) as i64;
            let imm = |w: i64| {
                let v = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3);
                if v & (1 << 20) != 0 {
                    v - (1 << 21)
                } else {
                    v
                }
            };
            for i in 0..n {
                let wi = w(i);
                let pc = lo as i64 + 4 * i as i64;
                if wi & 0x9F000000 == 0x10000000 {
                    found.push(pc + imm(wi));
                }
                if wi & 0x9F000000 == 0x90000000 {
                    let page = (pc & !0xFFF) + (imm(wi) << 12);
                    for d in 1..=4 {
                        let j = i + d;
                        if j >= n {
                            continue;
                        }
                        let y = w(j);
                        if y & 0xFFC00000 == 0x91000000 && (y >> 5) & 31 == wi & 31 {
                            found.push(page + ((y >> 10) & 0xFFF));
                        }
                    }
                }
            }
        }
        let mut t: Vec<u64> = found
            .into_iter()
            .filter(|&a| a % 4 == 0 && texts.iter().any(|&(lo, hi)| lo as i64 <= a && a < hi as i64))
            .map(|a| a as u64)
            .collect();
        t.sort_unstable();
        t.dedup();
        if spans.is_empty() {
            return t;
        }
        // the latest-ending span among those starting at or before t (spans may nest or repeat)
        let mut best = Vec::with_capacity(spans.len());
        let mut m = 0;
        for &(_, h) in spans {
            m = m.max(h);
            best.push(m);
        }
        let los: Vec<u64> = spans.iter().map(|s| s.0).collect();
        t.retain(|&a| {
            let k = los.partition_point(|&l| l <= a);
            if k == 0 {
                return true;
            }
            let within = a < best[k - 1] && los.binary_search(&a).is_err();
            !within
        });
        t
    }
}
