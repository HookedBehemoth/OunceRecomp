//! A .nup package: a decrypted Switch 2 dump of one or more titles, each with an exefs/ and a romfs/ tree.
//!
//! The layout (little-endian):
//! - 0x000 "NUP\x01"; the table of contents' offset and size at 0x08, its SHA-256 at 0x18, the data's start at 0x38,
//!   the file's size at 0x40.
//! - 0x080 "TOC\x01", u32 titles, u64 dirs, u64 files, u64 strings offset (from 0x80), u64 strings size.
//! - 0x0b0 titles, 0x40 each: u64 title id, u32 root dir, u32 exefs dir, u32 romfs dir, u32 version.
//! - dirs, 24 bytes each: u32 name offset, u32 name length, u32 first child, u32 children, u64 0.
//! - files, 80 bytes each: u32 name offset, u32 name length, u64 size, u64 data offset, u64 stored size, 32-byte
//!   SHA-256, u8 codec (0 raw, 1 zstd), u8 flags, 14 bytes reserved.
//! - the child list, u32 each: below the dir count a dir, else (index - dirs) a file; then the names (not
//!   0-terminated).
//!
//! A member is stored as is, or as a zstd frame whose content is the member: codec 1 says so; packages from older
//! dumpers leave codec 0 and the frame is recognised by its content size. The game's own .zs files are zstd frames
//! too, stored raw.

use crate::zstd::{self, Decoder, Format, MAGIC};
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{self, Write};
#[cfg(unix)]
use std::os::unix::fs::FileExt;
#[cfg(windows)]
use std::os::windows::fs::FileExt;
use std::path::{Path, PathBuf};

pub const NONE: u32 = 0xffff_ffff;
const CHUNK: usize = 8 << 20;

#[derive(Clone, Debug)]
pub struct Title {
    pub index: usize,
    pub id: u64,
    pub root: u32,
    pub exefs: u32,
    pub romfs: u32,
    pub version: u32,
}

#[derive(Clone, Debug, Default)]
pub struct FileRec {
    pub name_off: u32,
    pub name_len: u32,
    pub size: u64,
    pub off: u64,
    pub stored: u64,
    pub sha256: [u8; 32],
    pub codec: u8,
    pub flags: u8,
    pub reserved: [u8; 14],
}

#[derive(Clone, Copy, Debug)]
pub struct Dir {
    pub name_off: u32,
    pub name_len: u32,
    pub first: u32,
    pub count: u32,
}

pub struct Nup {
    pub path: PathBuf,
    f: File,
    pub file_size: u64,
    toc_offset: u64,
    toc_size: u64,
    toc_sha256: [u8; 32],
    blob_offset: u64,
    pub titles: Vec<Title>,
    pub dirs: Vec<Dir>,
    pub files: Vec<FileRec>,
    kids: Vec<u32>,
    strings: Vec<u8>,
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

/// An error of the package's data (as opposed to the disk's).
pub fn bad(msg: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.into())
}

impl Nup {
    /// Reads the table of contents. `strict` also checks its bounds, its SHA-256 and the tree (`--verify`); without
    /// it a package whose header lacks them (older dumpers) still opens.
    pub fn open(path: &Path, strict: bool) -> Result<Nup, String> {
        let f = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let have = f.metadata().map_err(|e| e.to_string())?.len();
        let mut h = [0u8; 0x80];
        let n = read_full(&f, 0, &mut h).map_err(|e| e.to_string())?;
        if strict && n != 0x80 {
            return Err("truncated NUP header".into());
        }
        if h[..4] != *b"NUP\x01" {
            return Err(format!("{}: not a NUP package (magic {:02x?})", path.display(), &h[..4]));
        }
        let mut nup = Nup {
            path: path.into(),
            f,
            file_size: u64_at(&h, 0x40),
            toc_offset: u64_at(&h, 8),
            toc_size: u64_at(&h, 16),
            toc_sha256: h[24..56].try_into().unwrap(),
            blob_offset: u64_at(&h, 0x38),
            titles: vec![],
            dirs: vec![],
            files: vec![],
            kids: vec![],
            strings: vec![],
        };
        if strict {
            nup.verify_toc()?;
        }
        if nup.file_size != 0 && have < nup.file_size {
            return Err(format!(
                "the .nup file is incomplete: it is {} MB, its header says {} MB. Download or copy it again (wait for the download to finish) and check that the drive had room",
                have >> 20,
                nup.file_size >> 20
            ));
        }
        nup.parse()?;
        if strict {
            nup.verify_tree()?;
        }
        Ok(nup)
    }

    fn parse(&mut self) -> Result<(), String> {
        let mut t = [0u8; 0x30];
        if read_full(&self.f, 0x80, &mut t).map_err(|e| e.to_string())? != 0x30 || t[..4] != *b"TOC\x01" {
            return Err(format!("{}: no TOC at 0x80", self.path.display()));
        }
        let (nt, nd, nf, soff, ssz) = (u32_at(&t, 4) as u64, u64_at(&t, 8), u64_at(&t, 16), u64_at(&t, 24), u64_at(&t, 32));
        let short = || format!("{}: the table of contents is cut short", self.path.display());
        let dirs_at = 0x80 + 0x30 + 0x40 * nt;
        let files_at = nd.checked_mul(24).and_then(|d| d.checked_add(dirs_at)).ok_or_else(short)?;
        let kids_at = nf.checked_mul(80).and_then(|d| d.checked_add(files_at)).ok_or_else(short)?;
        let strings_at = soff.checked_add(0x80).ok_or_else(short)?;
        let end = strings_at.checked_add(ssz).ok_or_else(short)?;
        if strings_at < kids_at || end > self.f.metadata().map_err(|e| e.to_string())?.len() {
            return Err(short());
        }
        let mut meta = vec![0u8; end as usize];
        if read_full(&self.f, 0, &mut meta).map_err(|e| e.to_string())? != meta.len() {
            return Err(short());
        }
        self.strings = meta[strings_at as usize..].to_vec();
        self.titles = (0..nt as usize)
            .map(|i| {
                let o = 0xb0 + 0x40 * i;
                Title { index: i, id: u64_at(&meta, o), root: u32_at(&meta, o + 8), exefs: u32_at(&meta, o + 12), romfs: u32_at(&meta, o + 16), version: u32_at(&meta, o + 20) }
            })
            .collect();
        self.dirs = (0..nd as usize)
            .map(|i| {
                let o = dirs_at as usize + 24 * i;
                Dir { name_off: u32_at(&meta, o), name_len: u32_at(&meta, o + 4), first: u32_at(&meta, o + 8), count: u32_at(&meta, o + 12) }
            })
            .collect();
        self.files = (0..nf as usize)
            .map(|i| {
                let o = files_at as usize + 80 * i;
                FileRec {
                    name_off: u32_at(&meta, o),
                    name_len: u32_at(&meta, o + 4),
                    size: u64_at(&meta, o + 8),
                    off: u64_at(&meta, o + 16),
                    stored: u64_at(&meta, o + 24),
                    sha256: meta[o + 32..o + 64].try_into().unwrap(),
                    codec: meta[o + 64],
                    flags: meta[o + 65],
                    reserved: meta[o + 66..o + 80].try_into().unwrap(),
                }
            })
            .collect();
        let nk = (strings_at - kids_at) as usize / 4;
        self.kids = (0..nk).map(|i| u32_at(&meta, kids_at as usize + 4 * i)).collect();
        Ok(())
    }

    /// The declared bounds and the table of contents' SHA-256.
    pub fn verify_toc(&self) -> Result<(), String> {
        let physical = self.f.metadata().map_err(|e| e.to_string())?.len();
        if self.file_size != physical
            || self.toc_offset != 0x80
            || self.toc_size < 0x30
            || self.toc_offset + self.toc_size > self.blob_offset
            || self.blob_offset > physical
            || self.blob_offset % 16 != 0
        {
            return Err("invalid declared file/TOC/blob bounds".into());
        }
        let mut t = [0u8; 0x30];
        if read_full(&self.f, self.toc_offset, &mut t).map_err(|e| e.to_string())? != 0x30 || t[..4] != *b"TOC\x01" {
            return Err("invalid TOC header".into());
        }
        let (nt, nd, nf, soff, ssz) = (u32_at(&t, 4) as u128, u64_at(&t, 8) as u128, u64_at(&t, 16) as u128, u64_at(&t, 24) as u128, u64_at(&t, 32) as u128);
        let tables_end = 0x30 + 64 * nt + 24 * nd + 80 * nf;
        if nt == 0 || tables_end > soff || (soff - tables_end) % 4 != 0 || soff + ssz > self.toc_size as u128 {
            return Err("parsed title/directory/file/child/string tables exceed declared TOC".into());
        }
        let mut toc = vec![0u8; self.toc_size as usize];
        if read_full(&self.f, self.toc_offset, &mut toc).map_err(|e| e.to_string())? != toc.len() {
            return Err("truncated TOC".into());
        }
        if Sha256::digest(&toc)[..] != self.toc_sha256 {
            return Err("TOC SHA-256 mismatch".into());
        }
        Ok(())
    }

    /// Ranges, names, reachability, and no cycles.
    pub fn verify_tree(&self) -> Result<(), String> {
        let (nd, nf) = (self.dirs.len(), self.files.len());
        for d in &self.dirs {
            if (d.name_off as u64 + d.name_len as u64) > self.strings.len() as u64 || (d.first as u64 + d.count as u64) > self.kids.len() as u64 {
                return Err("directory name/child range outside table".into());
            }
            self.name(d.name_off, d.name_len)?;
        }
        for r in &self.files {
            if (r.name_off as u64 + r.name_len as u64) > self.strings.len() as u64 {
                return Err("file name outside string table".into());
            }
            self.name(r.name_off, r.name_len)?;
        }
        if self.kids.iter().any(|&k| k as usize >= nd + nf) {
            return Err("child index outside directory/file tables".into());
        }
        let (mut reached_dirs, mut reached_files) = (HashSet::new(), HashSet::new());
        for t in &self.titles {
            if t.root as usize >= nd || [t.exefs, t.romfs].iter().any(|&p| p != NONE && p as usize >= nd) {
                return Err(format!("title {} has an invalid directory index", t.index));
            }
            let (mut seen, mut active) = (HashSet::new(), HashSet::new());
            let mut pending = vec![(t.root, false)];
            while let Some((d, leaving)) = pending.pop() {
                if leaving {
                    active.remove(&d);
                    seen.insert(d);
                    continue;
                }
                if active.contains(&d) {
                    return Err(format!("directory cycle in title {}", t.index));
                }
                if seen.contains(&d) {
                    continue;
                }
                active.insert(d);
                pending.push((d, true));
                let dir = self.dirs[d as usize];
                for &k in &self.kids[dir.first as usize..(dir.first + dir.count) as usize] {
                    if (k as usize) < nd {
                        pending.push((k, false));
                    } else {
                        reached_files.insert(k as usize - nd);
                    }
                }
            }
            if [t.exefs, t.romfs].iter().any(|&p| p != NONE && !seen.contains(&p)) {
                return Err("title subtree is not reachable from its root".into());
            }
            reached_dirs.extend(seen);
        }
        if reached_dirs.len() != nd || reached_files.len() != nf {
            return Err("orphan directory/file records are not reachable from title roots".into());
        }
        Ok(())
    }

    pub fn name(&self, off: u32, n: u32) -> Result<&str, String> {
        let b = self.strings.get(off as usize..off as usize + n as usize).ok_or("a name outside the string table")?;
        std::str::from_utf8(b).map_err(|_| "a name is not valid utf-8".to_string())
    }

    /// (path under `dir`, file index) of every file below `dir`, in the package's order.
    pub fn walk(&self, dir: u32) -> Result<Vec<(String, usize)>, String> {
        let nd = self.dirs.len();
        let range = |d: usize| -> Result<&[u32], String> {
            let dd = self.dirs.get(d).ok_or("a directory index outside the table")?;
            Ok(self.kids.get(dd.first as usize..dd.first as usize + dd.count as usize).ok_or("a child range outside the table")?)
        };
        let mut out = Vec::new();
        // (each open directory: its index, its next child, its path)
        let mut stack = vec![(dir as usize, 0usize, String::new())];
        range(dir as usize)?;
        while let Some((d, i, prefix)) = stack.pop() {
            let kids = range(d)?;
            let Some(&k) = kids.get(i) else { continue };
            stack.push((d, i + 1, prefix.clone()));
            let k = k as usize;
            if k < nd {
                if stack.iter().any(|s| s.0 == k) {
                    return Err("directory cycle in the package".into());
                }
                range(k)?;
                let c = self.dirs[k];
                stack.push((k, 0, format!("{prefix}{}/", self.name(c.name_off, c.name_len)?)));
            } else {
                let r = self.files.get(k - nd).ok_or("a file index outside the table")?;
                out.push((format!("{prefix}{}", self.name(r.name_off, r.name_len)?), k - nd));
            }
        }
        Ok(out)
    }

    /// Whether the package keeps this member compressed: codec 1, or a frame whose content is the member. A .zs file
    /// is zstd in the game too, and a few happen to hold exactly as much as they take: that one is packed only if
    /// what it holds is zstd again.
    pub fn is_packed(&self, r: &FileRec, name: &str) -> io::Result<bool> {
        if r.codec == 1 {
            return Ok(true);
        }
        let mut head = [0u8; 18];
        let n = read_full(&self.f, r.off, &mut head)?;
        if head[..4] != MAGIC {
            return Ok(false);
        }
        let size = zstd::frame_content_size(&head[..n]);
        if name.ends_with(".zs") && (size.is_none() || size == Some(r.size)) {
            return Ok(size == Some(r.size) && self.inner_is_zstd(r)?);
        }
        if let Some(s) = size {
            return Ok(s == r.size);
        }
        // (a frame that states no size: the member when it unpacks to the listed size)
        let mut d = Decoder::new(Format::Std);
        let (mut got, mut left, mut pos) = (0u64, r.size, r.off);
        let mut buf = vec![0u8; 1 << 20];
        let mut out = vec![0u8; 1 << 17];
        while left > 0 && got <= r.size {
            let n = read_full(&self.f, pos, &mut buf[..left.min(1 << 20) as usize])?;
            if n == 0 {
                break;
            }
            left -= n as u64;
            pos += n as u64;
            match d.feed::<String>(&buf[..n], &mut out, |b| Ok(got += b.len() as u64)) {
                Ok(f) if f.done => break,
                Ok(_) => {}
                Err(_) => return Ok(false),
            }
        }
        Ok(got == r.size)
    }

    fn inner_is_zstd(&self, r: &FileRec) -> io::Result<bool> {
        let mut buf = vec![0u8; r.size.min(1 << 20) as usize];
        let n = read_full(&self.f, r.off, &mut buf)?;
        let mut head = Vec::new();
        let mut out = vec![0u8; 1 << 17];
        let res = Decoder::new(Format::Std).feed::<String>(&buf[..n], &mut out, |b| {
            head.extend_from_slice(&b[..b.len().min(4 - head.len().min(4))]);
            Ok(())
        });
        Ok(res.is_ok() && head.len() >= 4 && head[..4] == MAGIC)
    }

    /// The member, as itself, into `out`. `packed` None decides as extraction does (`is_packed`); Some is exact: the
    /// extent must hold the frame and nothing after it (verification).
    pub fn copy_to(&self, r: &FileRec, out: &mut dyn Write, name: &str, packed: Option<bool>) -> io::Result<()> {
        let exact = packed.is_some();
        let packed = match packed {
            Some(p) => p,
            None => self.is_packed(r, name)?,
        };
        let mut pos = r.off;
        if !packed {
            return self.copy_raw(pos, r.size, out);
        }
        // (an explicit extent must not read the next member; an old package's frame is read until it ends)
        let mut left = if r.codec == 1 { r.stored } else { r.size + (1 << 16) };
        let mut d = Decoder::new(Format::Std);
        let mut buf = vec![0u8; CHUNK.min(left as usize)];
        let mut obuf = vec![0u8; 1 << 17];
        let (mut got, mut done, mut unused) = (0u64, false, 0usize);
        while left > 0 && !done {
            let n = read_full(&self.f, pos, &mut buf[..left.min(CHUNK as u64) as usize])?;
            if n == 0 {
                break;
            }
            left -= n as u64;
            pos += n as u64;
            let fed = d.feed::<Stop>(&buf[..n], &mut obuf, |b| {
                got += b.len() as u64;
                if got > r.size {
                    return Err(Stop::Io(bad(format!("a compressed file expanded past its declared size of {} bytes", r.size))));
                }
                out.write_all(b).map_err(Stop::Io)
            });
            match fed {
                Ok(f) => {
                    done = f.done;
                    unused = n - f.used;
                }
                // (the end of the frame, and the next file's bytes after it)
                Err(Stop::Decode(_)) => break,
                Err(Stop::Io(e)) => return Err(e),
            }
        }
        if exact && (left > 0 || unused > 0) {
            return Err(bad("a compressed extent has trailing bytes"));
        }
        if r.codec == 1 && !done {
            return Err(bad("a compressed file ends before its frame completes"));
        }
        if got != r.size {
            return Err(bad(format!("a compressed file came out as {got} bytes, {} expected", r.size)));
        }
        Ok(())
    }

    fn copy_raw(&self, mut pos: u64, size: u64, out: &mut dyn Write) -> io::Result<()> {
        let mut left = size;
        let mut buf = vec![0u8; CHUNK.min(size as usize)];
        while left > 0 {
            let n = read_full(&self.f, pos, &mut buf[..left.min(CHUNK as u64) as usize])?;
            if n == 0 {
                return Err(bad(format!(
                    "the .nup file ends early (short read at {} of {} MB): it is incomplete. Download or copy it again",
                    pos >> 20,
                    self.f.metadata()?.len() >> 20
                )));
            }
            out.write_all(&buf[..n])?;
            left -= n as u64;
            pos += n as u64;
        }
        Ok(())
    }

    /// The whole member.
    pub fn read(&self, r: &FileRec, name: &str) -> io::Result<Vec<u8>> {
        let mut v = Vec::with_capacity(r.size as usize);
        self.copy_to(r, &mut v, name, None)?;
        Ok(v)
    }

    /// The title `want` names (its index, version or id), else the newest: an update over its base.
    pub fn pick_title(&self, want: Option<&str>) -> Result<&Title, String> {
        let Some(w) = want else {
            return self.titles.iter().max_by_key(|t| t.version).ok_or_else(|| "the package has no titles".to_string());
        };
        self.titles
            .iter()
            .find(|t| t.index.to_string() == w || t.version.to_string() == w || format!("{:016x}", t.id) == w.to_lowercase())
            .ok_or_else(|| format!("no title {w}"))
    }
}

/// Why a decode stopped: the data, or the output's error.
enum Stop {
    Decode(#[allow(dead_code)] String),
    Io(io::Error),
}

impl From<String> for Stop {
    fn from(s: String) -> Stop {
        Stop::Decode(s)
    }
}

#[cfg(unix)]
fn read_at(f: &File, buf: &mut [u8], pos: u64) -> io::Result<usize> {
    f.read_at(buf, pos)
}

/// (Windows: seek_read moves the file's position, which no reader here relies on)
#[cfg(windows)]
fn read_at(f: &File, buf: &mut [u8], pos: u64) -> io::Result<usize> {
    f.seek_read(buf, pos)
}

/// Reads up to `buf.len()` bytes at `pos`; fewer only at the end of the file.
pub fn read_full(f: &File, mut pos: u64, buf: &mut [u8]) -> io::Result<usize> {
    let mut n = 0;
    while n < buf.len() {
        match read_at(f, &mut buf[n..], pos) {
            Ok(0) => break,
            Ok(k) => {
                n += k;
                pos += k as u64;
            }
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
    Ok(n)
}

/// What `verify` found.
#[derive(Debug, Default)]
pub struct Verified {
    pub checked: usize,
    /// distinct extents decoded (members that share data are decoded once)
    pub decoded: usize,
    pub bad: usize,
    /// one line per bad member
    pub lines: Vec<String>,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Part {
    Exefs,
    Romfs,
}

impl Part {
    pub fn name(self) -> &'static str {
        match self {
            Part::Exefs => "exefs",
            Part::Romfs => "romfs",
        }
    }
    pub fn dir(self, t: &Title) -> u32 {
        match self {
            Part::Exefs => t.exefs,
            Part::Romfs => t.romfs,
        }
    }
}

struct HashSink {
    size: u64,
    prefix: Vec<u8>,
    hash: Sha256,
}

impl Write for HashSink {
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        self.size += b.len() as u64;
        self.hash.update(b);
        let want = 18usize.saturating_sub(self.prefix.len()).min(b.len());
        self.prefix.extend_from_slice(&b[..want]);
        Ok(b.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Read-only checks of the package: its table of contents, every member's extent, codec and SHA-256, and that each
/// .zs member starts with a game zstd header (not the inner data's integrity). `titles` None: all of them.
pub fn verify(nup: &Nup, titles: Option<&[Title]>, only: Option<Part>) -> Verified {
    let mut v = Verified::default();
    if let Err(e) = nup.verify_toc().and_then(|_| nup.verify_tree()) {
        v.bad = 1;
        v.lines.push(format!("BAD package: {e}"));
        return v;
    }
    let physical = nup.f.metadata().map_or(0, |m| m.len());
    let mut cache: HashMap<(u64, u64, u8, u64), (u64, [u8; 32], Vec<u8>, Option<String>)> = HashMap::new();
    for t in titles.unwrap_or(&nup.titles) {
        let (part, dir) = match only {
            Some(p) => (p.name(), p.dir(t)),
            None => ("", t.root),
        };
        if dir == NONE {
            continue;
        }
        let files = match nup.walk(dir) {
            Ok(f) => f,
            Err(e) => {
                v.bad += 1;
                v.lines.push(format!("BAD package: {e}"));
                continue;
            }
        };
        for (name, i) in files {
            v.checked += 1;
            let r = &nup.files[i];
            let mut why = Vec::new();
            if r.codec > 1 || r.flags != 0 || r.reserved.iter().any(|&b| b != 0) {
                why.push("unsupported codec/flags/reserved bytes".to_string());
            }
            if (r.size == 0 && (r.off != 0 || r.stored != 0 || r.codec != 0))
                || (r.size != 0 && (r.stored == 0 || r.off < nup.blob_offset || r.off % 16 != 0 || r.off > physical || r.stored > physical - r.off))
            {
                why.push("invalid extent bounds".into());
            }
            if r.codec == 0 && r.stored != r.size {
                why.push("raw extent length differs from member length".into());
            }
            if why.is_empty() {
                let key = (r.off, r.stored, r.codec, r.size);
                let (size, digest, prefix, error) = cache.entry(key).or_insert_with(|| {
                    let mut s = HashSink { size: 0, prefix: vec![], hash: Sha256::new() };
                    let e = nup.copy_to(r, &mut s, &name, Some(r.codec == 1)).err().map(|e| e.to_string());
                    (s.size, s.hash.finalize().into(), s.prefix, e)
                });
                if let Some(e) = error {
                    why.push(format!("member decode failed: {e}"));
                }
                if *size != r.size {
                    why.push("decoded length differs from member length".into());
                }
                if *digest != r.sha256 {
                    why.push("member SHA-256 mismatch".into());
                }
                if name.ends_with(".zs") && !(prefix.len() >= 4 && prefix[..4] == MAGIC && zstd::frame_header_ok(Format::Std, prefix)) {
                    why.push(".zs member lacks the expected game Zstd header".into());
                }
            }
            if !why.is_empty() {
                v.bad += 1;
                v.lines.push(format!("BAD title {} {:016x} v{} {part}/{name}: {}", t.index, t.id, t.version, why.join("; ")));
            }
        }
    }
    v.decoded = cache.len();
    v
}
