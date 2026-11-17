//! Two stores compared texture by texture: every index field but where the frames lie in the packs, and every frame's
//! compressed bytes.

use crate::convert::FLAG_DEDUP;
use crate::store::{read_index, FrameRef, PackReader};
use rayon::prelude::*;
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Debug, Default)]
pub struct Diff {
    pub files: usize,
    pub textures: usize,
    pub frames: usize,
    /// field -> (count, first examples)
    pub fields: BTreeMap<String, (usize, Vec<String>)>,
}

impl Diff {
    fn add(&mut self, field: &str, what: String) {
        let e = self.fields.entry(field.to_string()).or_default();
        e.0 += 1;
        if e.1.len() < 5 {
            e.1.push(what);
        }
    }

    pub fn equal(&self) -> bool {
        self.fields.is_empty()
    }
}

fn frame_hash(r: &mut PackReader, f: &FrameRef) -> Result<u64, String> {
    Ok(crate::convert::hash64(&[&r.read(f).map_err(|e| e.to_string())?]))
}

/// a's and b's differences; the dedup flag is counted apart (which of equal textures holds the frames).
pub fn compare(a: &Path, b: &Path) -> Result<Diff, String> {
    let ia = read_index(&a.join("index.bin"))?;
    let ib = read_index(&b.join("index.bin"))?;
    let mut d = Diff::default();
    if ia.cfg_hash != ib.cfg_hash {
        d.add("header config hash", format!("{:#x} / {:#x}", ia.cfg_hash, ib.cfg_hash));
    }
    let mb: BTreeMap<&str, Vec<&crate::store::FileEntry>> = ib.entries.iter().fold(BTreeMap::new(), |mut m, e| {
        m.entry(e.path.as_str()).or_insert_with(Vec::new).push(e);
        m
    });
    let ma: BTreeMap<&str, Vec<&crate::store::FileEntry>> = ia.entries.iter().fold(BTreeMap::new(), |mut m, e| {
        m.entry(e.path.as_str()).or_insert_with(Vec::new).push(e);
        m
    });
    for p in ma.keys().filter(|p| !mb.contains_key(*p)) {
        d.add("file only in a", p.to_string());
    }
    for p in mb.keys().filter(|p| !ma.contains_key(*p)) {
        d.add("file only in b", p.to_string());
    }
    let order_a: Vec<&str> = ia.entries.iter().map(|e| e.path.as_str()).collect();
    let order_b: Vec<&str> = ib.entries.iter().map(|e| e.path.as_str()).collect();
    if order_a != order_b {
        d.add("file order", String::new());
    }
    // (frame pairs whose bytes are compared, hashed in parallel below)
    let mut pairs: Vec<(String, FrameRef, FrameRef)> = Vec::new();
    for (p, ea) in &ma {
        let Some(eb) = mb.get(p) else { continue };
        for (fa, fb) in ea.iter().zip(eb.iter()) {
            d.files += 1;
            let file_fields = |e: &crate::store::FileEntry| (e.src_size, e.src_hash, e.mtime, e.bntx_size, e.head_len, e.rlt_off, e.flags);
            if file_fields(fa) != file_fields(fb) {
                d.add("file record", p.to_string());
            }
            if (fa.meta.raw_size, fa.meta.csize) != (fb.meta.raw_size, fb.meta.csize) {
                d.add("metadata frame size", p.to_string());
            } else {
                pairs.push((format!("{p} metadata"), fa.meta, fb.meta));
            }
            if fa.textures.len() != fb.textures.len() {
                d.add("texture count", p.to_string());
                continue;
            }
            for (ta, tb) in fa.textures.iter().zip(&fb.textures) {
                d.textures += 1;
                let what = format!("{p} {}", ta.rec.name);
                let (mut ra, mut rb) = (ta.rec.clone(), tb.rec.clone());
                if (ra.flags ^ rb.flags) & FLAG_DEDUP != 0 {
                    d.add("dedup flag (which equal texture holds the frames)", what.clone());
                }
                ra.flags &= !FLAG_DEDUP;
                rb.flags &= !FLAG_DEDUP;
                if (ra.psnr, ra.psnr_min, ra.psnr_first) != (rb.psnr, rb.psnr_min, rb.psnr_first) {
                    d.add("psnr fields", format!("{what}: {} {} {} / {} {} {}", ra.psnr, ra.psnr_min, ra.psnr_first, rb.psnr, rb.psnr_min, rb.psnr_first));
                }
                (ra.psnr, ra.psnr_min, ra.psnr_first) = (0.0, 0.0, 0.0);
                (rb.psnr, rb.psnr_min, rb.psnr_first) = (0.0, 0.0, 0.0);
                if ra != rb {
                    d.add("texture record", format!("{what}: {ra:?} / {rb:?}"));
                }
                if ta.mips != tb.mips {
                    d.add("mip records", what.clone());
                }
                let shape = |fs: &[FrameRef]| fs.iter().map(|f| (f.raw_off, f.raw_size)).collect::<Vec<_>>();
                if shape(&ta.frames) != shape(&tb.frames) {
                    d.add("frame split", what.clone());
                    continue;
                }
                for (x, y) in ta.frames.iter().zip(&tb.frames) {
                    if x.csize != y.csize {
                        d.add("frame compressed size", what.clone());
                    } else {
                        pairs.push((what.clone(), *x, *y));
                    }
                }
            }
        }
    }
    d.frames = pairs.len();
    let bad: Vec<String> = pairs
        .par_chunks(256)
        .map(|chunk| {
            let (mut ra, mut rb) = (PackReader::new(a), PackReader::new(b));
            let mut bad = Vec::new();
            for (what, x, y) in chunk {
                match (frame_hash(&mut ra, x), frame_hash(&mut rb, y)) {
                    (Ok(p), Ok(q)) if p == q => {}
                    _ => bad.push(what.clone()),
                }
            }
            bad
        })
        .flatten()
        .collect();
    for w in bad {
        d.add("frame bytes", w);
    }
    Ok(d)
}
