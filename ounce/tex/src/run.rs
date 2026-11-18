//! A whole store made from a romfs: the files walked, converted in parallel (files admitted by the caller's memory
//! gate), the frames written to the packs as files finish, the index last.

use crate::convert::{self, hash64, BntxOut, Settings, Status, FLAG_DEDUP};
use crate::romfs::{find_bntx, Dicts};
use crate::store::{self, FileEntry, FrameRef, Packs, TexEntry};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;
use std::time::Instant;

/// What the game description says about its romfs.
#[derive(Debug, Clone, Default)]
pub struct GameFiles {
    /// the romfs file of zstd dictionaries (a zstd SARC of raw dictionaries), when the game has one
    pub zstd_dictionaries: Option<String>,
    /// top-level romfs folders that hold no BNTX
    pub skip_dirs: Vec<String>,
}

/// Where the texture files go with their texels zeroed (`hollow`): a romfs folder (the one read, or another), and
/// the zstd level they are compressed again at.
#[derive(Debug, Clone)]
pub struct HollowTo {
    pub dir: PathBuf,
    pub level: i32,
}

/// Anything held while a file is converted (a memory gate's lease).
pub trait Held {}
impl<T> Held for T {}

pub struct Options<'a> {
    pub romfs: PathBuf,
    pub out: PathBuf,
    pub game: GameFiles,
    pub settings: Settings,
    pub jobs: usize,
    pub pack_limit: u64,
    /// only romfs paths that start with one of these (none: all)
    pub only: Vec<String>,
    /// also writes each file holding BNTX texels there with them zeroed
    pub hollow: Option<HollowTo>,
    /// waits until a file needing this many MB may start; the value returned is held while it is converted
    pub admit: &'a (dyn Fn(u64) -> Box<dyn Held + 'a> + Sync),
    pub progress: &'a (dyn Fn(&str) + Sync),
    /// called as each file is done: (files done, files, bytes done, bytes, its romfs path)
    pub each: &'a (dyn Fn(usize, usize, u64, u64, &str) + Sync),
}

#[derive(Debug, Default)]
pub struct Stats {
    pub files: usize,
    pub bntx_files: usize,
    pub textures: usize,
    pub shared: usize,
    pub in_bytes: u64,
    pub raw: u64,
    pub packed: u64,
    pub by_kind: HashMap<&'static str, (usize, u64, u64)>,
    pub errors: Vec<String>,
    pub seconds: f64,
    /// the files written with their texels zeroed (romfs paths), their bytes before and after, the texel bytes
    /// zeroed, and BNTX files left whole (inside a compressed archive member)
    pub hollowed: Vec<String>,
    pub hollow_in: u64,
    pub hollow_out: u64,
    pub hollow_zeroed: u64,
    pub hollow_kept: u64,
}

/// (romfs-relative path, size, mtime in ns) of the files to look at.
pub fn walk(romfs: &Path, skip: &[String], only: &[String]) -> Result<Vec<(String, u64, u64)>, String> {
    use std::os::unix::fs::MetadataExt;
    let mut out = Vec::new();
    let mut top: Vec<_> = std::fs::read_dir(romfs).map_err(|e| format!("{}: {e}", romfs.display()))?.flatten().collect();
    top.sort_by_key(|e| e.file_name());
    let mut stack: Vec<PathBuf> = Vec::new();
    for e in top {
        let name = e.file_name().to_string_lossy().into_owned();
        if skip.contains(&name) || !e.path().is_dir() {
            continue;
        }
        stack.push(e.path());
        while let Some(d) = stack.pop() {
            let mut ents: Vec<_> = std::fs::read_dir(&d).map_err(|e| format!("{}: {e}", d.display()))?.flatten().collect();
            ents.sort_by_key(|e| e.file_name());
            for e in ents.into_iter().rev() {
                let p = e.path();
                let ft = e.file_type().map_err(|e| format!("{}: {e}", p.display()))?;
                if ft.is_dir() {
                    stack.push(p);
                    continue;
                }
                let rel = p.strip_prefix(romfs).unwrap().to_string_lossy().into_owned();
                if rel.ends_with(".hollow.tmp") {
                    continue; // (a file being zeroed when a build was stopped; written again and renamed this time)
                }
                if !only.is_empty() && !only.iter().any(|o| rel.starts_with(o.as_str())) {
                    continue;
                }
                let md = std::fs::metadata(&p).map_err(|e| format!("{}: {e}", p.display()))?;
                out.push((rel, md.len(), md.mtime() as u64 * 1_000_000_000 + md.mtime_nsec() as u64));
            }
        }
    }
    Ok(out)
}

/// The configuration hash in the index header: the store's bytes follow from these.
pub fn config_hash(s: &Settings) -> u64 {
    let text = format!(
        "ounce-tex 2 astc bc7 {} zstd {} frame {} ispc sse2,sse4,avx,avx2",
        s.bc7_level, s.zstd_level, s.frame_kb
    );
    hash64(&[text.as_bytes()])
}

/// MB a file of this size may take while it is converted (its BNTX decompressed, decoded images, raw and frames)
pub fn need_mb(size: u64) -> u64 {
    64 + 12 * (size >> 20)
}

struct Done {
    order: usize,
    path: String,
    size: u64,
    mtime: u64,
    hash: u64,
    bntx: Result<Vec<BntxOut>, String>,
    hollowed: Option<Result<(u64, crate::hollow::Hollowed), String>>,
}

/// `path` of the romfs written into `h.dir` with its texels zeroed (beside, then renamed over): its new size
fn hollow_file(dicts: &Dicts, data: &[u8], h: &HollowTo, path: &str) -> Result<(u64, crate::hollow::Hollowed), String> {
    let Some((out, st)) = crate::hollow::hollow(dicts, data, h.level)? else { return Ok((data.len() as u64, Default::default())) };
    let to = h.dir.join(path);
    let tmp = h.dir.join(format!("{path}.hollow.tmp"));
    if let Some(d) = to.parent() {
        std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
    }
    std::fs::write(&tmp, &out).and_then(|_| std::fs::rename(&tmp, &to)).map_err(|e| format!("{}: {e}", to.display()))?;
    Ok((out.len() as u64, st))
}

/// Makes the store in `o.out` (its packs and index replaced).
pub fn run(o: &Options) -> Result<Stats, String> {
    let t0 = Instant::now();
    std::fs::create_dir_all(&o.out).map_err(|e| format!("{}: {e}", o.out.display()))?;
    for e in std::fs::read_dir(&o.out).map_err(|e| e.to_string())?.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        if (n.starts_with("pack_") && n.ends_with(".bin")) || n == "index.bin" {
            std::fs::remove_file(e.path()).map_err(|e| e.to_string())?;
        }
    }
    let dicts = match &o.game.zstd_dictionaries {
        Some(p) => Dicts::load(&o.romfs.join(p))?,
        None => Dicts::default(),
    };
    let mut files = walk(&o.romfs, &o.game.skip_dirs, &o.only)?;
    files.sort_by(|a, b| a.0.cmp(&b.0));
    let mut todo: Vec<usize> = (0..files.len()).collect();
    todo.sort_by_key(|&i| std::cmp::Reverse(files[i].1));
    let bytes: u64 = files.iter().map(|f| f.1).sum();
    (o.progress)(&format!(
        "textures: {} files ({:.1} GB) from {}, {} threads",
        files.len(),
        bytes as f64 / 1e9,
        o.romfs.display(),
        o.jobs
    ));
    let pool = rayon::ThreadPoolBuilder::new().num_threads(o.jobs).build().map_err(|e| e.to_string())?;
    let (tx, rx) = mpsc::sync_channel::<Done>(o.jobs * 2);
    let next = AtomicUsize::new(0);
    let mut stats = Stats::default();
    let mut result: Result<(), String> = Ok(());
    std::thread::scope(|sc| {
        let files = &files;
        let (todo, next, dicts) = (&todo, &next, &dicts);
        sc.spawn(move || {
            pool.scope(|ps| {
                for _ in 0..o.jobs {
                    let tx = tx.clone();
                    ps.spawn(move |_| loop {
                        let k = next.fetch_add(1, Ordering::Relaxed);
                        let Some(&i) = todo.get(k) else { break };
                        let (path, size, mtime) = &files[i];
                        let _lease = (o.admit)(need_mb(*size));
                        let done = match std::fs::read(o.romfs.join(path)) {
                            Err(e) => Done { order: i, path: path.clone(), size: *size, mtime: *mtime, hash: 0, bntx: Err(e.to_string()), hollowed: None },
                            Ok(data) => {
                                let hash = hash64(&[&data]);
                                let found = find_bntx(dicts, &data, path, 0);
                                let hollowed = match (&o.hollow, &found) {
                                    (Some(h), Ok(f)) if !f.is_empty() => Some(hollow_file(dicts, &data, h, path)),
                                    _ => None,
                                };
                                drop(data);
                                let bntx = found.and_then(|found| found.iter().map(|(sub, b)| convert::convert_bntx(b, sub, &o.settings)).collect());
                                Done { order: i, path: path.clone(), size: *size, mtime: *mtime, hash, bntx, hollowed }
                            }
                        };
                        if tx.send(done).is_err() {
                            break;
                        }
                    });
                }
                drop(tx);
            });
        });
        result = write_all(o, rx, files.len(), bytes, &mut stats, t0);
    });
    result?;
    stats.seconds = t0.elapsed().as_secs_f64();
    Ok(stats)
}

type DedupKey = (u64, u32, u32, u32, u32, u32, u8, u32);

fn write_all(o: &Options, rx: mpsc::Receiver<Done>, n_files: usize, n_bytes: u64, st: &mut Stats, t0: Instant) -> Result<(), String> {
    let mut packs = Packs::new(&o.out, o.pack_limit);
    let mut done: Vec<Option<Vec<FileEntry>>> = vec![None; n_files];
    // (frames written once per content; every texture of a key in index order: (file order, entry, texture))
    let mut seen: HashMap<DedupKey, Vec<FrameRef>> = HashMap::new();
    let mut groups: HashMap<DedupKey, Vec<(usize, usize, usize)>> = HashMap::new();
    let mut last = Instant::now();
    let io = |e: std::io::Error| e.to_string();
    let mut bytes = 0;
    for d in rx {
        st.files += 1;
        bytes += d.size;
        (o.each)(st.files, n_files, bytes, n_bytes, &d.path);
        match d.hollowed {
            Some(Ok((n, h))) if h.zeroed > 0 => {
                st.hollowed.push(d.path.clone());
                st.hollow_in += d.size;
                st.hollow_out += n;
                st.hollow_zeroed += h.zeroed;
                st.hollow_kept += h.kept as u64;
            }
            Some(Err(e)) => st.errors.push(format!("{}: zeroing its texels: {e}", d.path)),
            _ => {}
        }
        let bntx = match d.bntx {
            Ok(b) => b,
            Err(e) => {
                st.errors.push(format!("{}: {e}", d.path));
                continue;
            }
        };
        if bntx.is_empty() {
            continue;
        }
        st.in_bytes += d.size;
        st.bntx_files += 1;
        let mut es = Vec::with_capacity(bntx.len());
        for (ei, b) in bntx.into_iter().enumerate() {
            let (pack, off) = packs.put(&b.meta.data).map_err(io)?;
            let meta = FrameRef { pack, off, csize: b.meta.data.len() as u32, raw_off: 0, raw_size: b.meta.raw_size };
            let mut textures = Vec::with_capacity(b.textures.len());
            for (ti, t) in b.textures.into_iter().enumerate() {
                st.textures += 1;
                let r = &t.rec;
                if r.status != Status::Ok {
                    textures.push(TexEntry { rec: t.rec, mips: t.mips, frames: Vec::new() });
                    continue;
                }
                let key = (r.tex_hash, r.nvn_format, r.width, r.height, r.layers, r.mips, r.kind as u8, r.vk_format);
                groups.entry(key).or_default().push((d.order, ei, ti));
                let frames = if let Some(f) = seen.get(&key) {
                    st.shared += 1;
                    f.clone()
                } else {
                    let mut fr = Vec::with_capacity(t.frames.len());
                    for f in &t.frames {
                        let (pack, off) = packs.put(&f.data).map_err(io)?;
                        fr.push(FrameRef { pack, off, csize: f.data.len() as u32, raw_off: f.raw_off, raw_size: f.raw_size });
                        st.packed += f.data.len() as u64;
                    }
                    st.raw += r.raw_size;
                    let k = st.by_kind.entry(r.kind.name()).or_default();
                    k.0 += 1;
                    k.1 += r.raw_size;
                    k.2 += t.frames.iter().map(|f| f.data.len() as u64).sum::<u64>();
                    seen.insert(key, fr.clone());
                    fr
                };
                textures.push(TexEntry { rec: t.rec, mips: t.mips, frames });
            }
            es.push(FileEntry {
                path: b.path,
                src_size: d.size,
                src_hash: d.hash,
                mtime: d.mtime,
                bntx_size: b.bntx_size,
                head_len: b.head_len,
                rlt_off: b.rlt_off,
                flags: b.flags,
                meta,
                textures,
            });
        }
        done[d.order] = Some(es);
        if last.elapsed().as_secs() >= 30 {
            last = Instant::now();
            (o.progress)(&format!(
                "textures: {}/{} files, {} textures, {:.0} MB raw, {:.0} MB packed, {:.0} s",
                st.files,
                n_files,
                st.textures,
                st.raw as f64 / 1e6,
                st.packed as f64 / 1e6,
                t0.elapsed().as_secs_f64()
            ));
        }
    }
    packs.close().map_err(io)?;
    // (a texture whose frames another of the same content holds is flagged; the first in index order is not)
    for v in groups.values_mut().filter(|v| v.len() > 1) {
        v.sort();
        for &(fo, ei, ti) in &v[1..] {
            done[fo].as_mut().unwrap()[ei].textures[ti].rec.flags |= FLAG_DEDUP;
        }
    }
    let entries: Vec<FileEntry> = done.into_iter().flatten().flatten().collect();
    store::write_index(&o.out.join("index.bin"), config_hash(&o.settings), &entries, packs.count).map_err(io)?;
    Ok(())
}
