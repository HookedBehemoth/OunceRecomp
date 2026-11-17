//! Game-free tests: made-up packages, files and modules in temporary folders.

use crate::extract::{self, Output, Progress};
use crate::nso::{self, Unpacked};
use crate::nup::{self, Nup, Part, Title, NONE};
use crate::standin;
use crate::zstd::{self, Format, MAGIC};
use crate::Dump;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::io;
use std::path::{Path, PathBuf};

fn put32(b: &mut [u8], o: usize, v: u32) {
    b[o..o + 4].copy_from_slice(&v.to_le_bytes());
}
fn put64(b: &mut [u8], o: usize, v: u64) {
    b[o..o + 8].copy_from_slice(&v.to_le_bytes());
}
fn get64(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
fn pad16(b: &mut Vec<u8>) {
    b.resize(b.len().next_multiple_of(16), 0);
}

fn zs(data: &[u8]) -> Vec<u8> {
    zstd::compress(Format::Std, data, 3, true)
}

/// A temporary folder, removed on drop.
struct Temp(PathBuf);
impl Temp {
    fn new() -> Temp {
        use std::sync::atomic::{AtomicUsize, Ordering};
        static N: AtomicUsize = AtomicUsize::new(0);
        let p = std::env::temp_dir().join(format!("ounce-dump-{}-{}", std::process::id(), N.fetch_add(1, Ordering::Relaxed)));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        Temp(p)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// One directory per title (its root and its romfs), its files below it: (title, name, logical bytes, codec);
/// identical stored bytes share an extent. Returns the package and where the file records start.
fn flat(rows: &[(usize, &str, Vec<u8>, u8)], titles: usize) -> (Vec<u8>, usize) {
    let files_at = 0xb0 + 64 * titles + 24 * titles;
    let strings: Vec<u8> = rows.iter().flat_map(|r| r.1.bytes()).collect();
    let strings_at = files_at + 80 * rows.len() + 4 * rows.len();
    let blob_at = (strings_at + strings.len()).next_multiple_of(16);
    let mut b = vec![0u8; blob_at];
    b[..4].copy_from_slice(b"NUP\x01");
    b[0x80..0x84].copy_from_slice(b"TOC\x01");
    b[4..6].copy_from_slice(&0x80u16.to_le_bytes());
    b[6..8].copy_from_slice(&1u16.to_le_bytes());
    put64(&mut b, 8, 0x80);
    put64(&mut b, 16, (strings_at + strings.len() - 0x80) as u64);
    put32(&mut b, 0x84, titles as u32);
    put64(&mut b, 0x88, titles as u64);
    put64(&mut b, 0x90, rows.len() as u64);
    put64(&mut b, 0x98, (strings_at - 0x80) as u64);
    put64(&mut b, 0xa0, strings.len() as u64);
    put64(&mut b, 0x38, blob_at as u64);
    b[strings_at..strings_at + strings.len()].copy_from_slice(&strings);
    let mut slot = 0;
    for t in 0..titles {
        let o = 0xb0 + 64 * t;
        put64(&mut b, o, 0x1000 + t as u64);
        put32(&mut b, o + 8, t as u32);
        put32(&mut b, o + 12, NONE);
        put32(&mut b, o + 16, t as u32);
        put32(&mut b, o + 20, t as u32 + 1);
        let members: Vec<usize> = (0..rows.len()).filter(|&i| rows[i].0 == t).collect();
        let d = 0xb0 + 64 * titles + 24 * t;
        put32(&mut b, d + 8, slot as u32);
        put32(&mut b, d + 12, members.len() as u32);
        for i in members {
            put32(&mut b, files_at + 80 * rows.len() + 4 * slot, (titles + i) as u32);
            slot += 1;
        }
    }
    let (mut name_at, mut seen) = (0, BTreeMap::new());
    for (i, (_, name, logical, codec)) in rows.iter().enumerate() {
        let stored = if *codec == 1 { zs(logical) } else { logical.clone() };
        let at = *seen.entry((stored.clone(), *codec)).or_insert_with(|| {
            pad16(&mut b);
            let at = b.len();
            b.extend_from_slice(&stored);
            at
        });
        let o = files_at + 80 * i;
        put32(&mut b, o, name_at);
        put32(&mut b, o + 4, name.len() as u32);
        put64(&mut b, o + 8, logical.len() as u64);
        put64(&mut b, o + 16, at as u64);
        put64(&mut b, o + 24, stored.len() as u64);
        b[o + 32..o + 64].copy_from_slice(&Sha256::digest(logical));
        b[o + 64] = *codec;
        name_at += name.len() as u32;
    }
    let n = b.len() as u64;
    put64(&mut b, 0x40, n);
    (b, files_at)
}

/// The table of contents' hash written.
fn seal(b: &mut [u8]) {
    let (o, s) = (get64(b, 8) as usize, get64(b, 16) as usize);
    let h = Sha256::digest(&b[o..o + s]);
    b[24..56].copy_from_slice(&h);
}

/// A package of titles, each a list of (path below the title's root, e.g. "exefs/main", stored bytes, logical
/// size, codec).
pub(crate) fn tree(titles: &[Vec<(String, Vec<u8>, u64, u8)>]) -> Vec<u8> {
    // dirs: (name, children as node numbers); files: (name, row)
    let mut dirs: Vec<(String, Vec<usize>)> = Vec::new();
    let mut files: Vec<(String, (usize, usize))> = Vec::new();
    let mut roots = Vec::new();
    let mut parts = Vec::new();
    let mut child_dirs: Vec<BTreeMap<String, usize>> = Vec::new();
    for (t, rows) in titles.iter().enumerate() {
        dirs.push((String::new(), vec![]));
        child_dirs.push(BTreeMap::new());
        let root = dirs.len() - 1;
        roots.push(root);
        for (r, (path, _, _, _)) in rows.iter().enumerate() {
            let comps: Vec<&str> = path.split('/').collect();
            let mut d = root;
            for c in &comps[..comps.len() - 1] {
                d = match child_dirs[d].get(*c) {
                    Some(&k) => k,
                    None => {
                        dirs.push((c.to_string(), vec![]));
                        child_dirs.push(BTreeMap::new());
                        let k = dirs.len() - 1;
                        child_dirs[d].insert(c.to_string(), k);
                        dirs[d].1.push(k);
                        k
                    }
                };
            }
            files.push((comps[comps.len() - 1].to_string(), (t, r)));
            dirs[d].1.push(usize::MAX - (files.len() - 1));
        }
        let find = |n: &str| child_dirs[root].get(n).copied().map_or(NONE, |k| k as u32);
        parts.push((find("exefs"), find("romfs")));
    }
    let (nt, nd, nf) = (titles.len(), dirs.len(), files.len());
    let mut strings = Vec::new();
    let mut name_at = |s: &str| {
        strings.extend_from_slice(s.as_bytes());
        (strings.len() - s.len()) as u32
    };
    let dir_names: Vec<u32> = dirs.iter().map(|d| name_at(&d.0)).collect();
    let file_names: Vec<u32> = files.iter().map(|f| name_at(&f.0)).collect();
    let nk: usize = dirs.iter().map(|d| d.1.len()).sum();
    let dirs_at = 0xb0 + 0x40 * nt;
    let files_at = dirs_at + 24 * nd;
    let kids_at = files_at + 80 * nf;
    let strings_at = kids_at + 4 * nk;
    let mut b = vec![0u8; strings_at];
    b.extend_from_slice(&strings);
    let toc_end = b.len();
    pad16(&mut b);
    let blob_at = b.len();
    b[..4].copy_from_slice(b"NUP\x01");
    put64(&mut b, 8, 0x80);
    put64(&mut b, 16, (toc_end - 0x80) as u64);
    put64(&mut b, 0x38, blob_at as u64);
    b[0x80..0x84].copy_from_slice(b"TOC\x01");
    put32(&mut b, 0x84, nt as u32);
    put64(&mut b, 0x88, nd as u64);
    put64(&mut b, 0x90, nf as u64);
    put64(&mut b, 0x98, (strings_at - 0x80) as u64);
    put64(&mut b, 0xa0, strings.len() as u64);
    for t in 0..nt {
        let o = 0xb0 + 0x40 * t;
        put64(&mut b, o, 0x0100_0000_0000_0000 + t as u64);
        put32(&mut b, o + 8, roots[t] as u32);
        put32(&mut b, o + 12, parts[t].0);
        put32(&mut b, o + 16, parts[t].1);
        put32(&mut b, o + 20, t as u32);
    }
    let mut k = kids_at;
    for (i, d) in dirs.iter().enumerate() {
        let o = dirs_at + 24 * i;
        put32(&mut b, o, dir_names[i]);
        put32(&mut b, o + 4, d.0.len() as u32);
        put32(&mut b, o + 8, ((k - kids_at) / 4) as u32);
        put32(&mut b, o + 12, d.1.len() as u32);
        for &c in &d.1 {
            let node = if c > usize::MAX / 2 { nd + (usize::MAX - c) } else { c };
            put32(&mut b, k, node as u32);
            k += 4;
        }
    }
    for (i, (name, (t, r))) in files.iter().enumerate() {
        let (_, stored, size, codec) = &titles[*t][*r];
        pad16(&mut b);
        let at = b.len();
        b.extend_from_slice(stored);
        let o = files_at + 80 * i;
        put32(&mut b, o, file_names[i]);
        put32(&mut b, o + 4, name.len() as u32);
        put64(&mut b, o + 8, *size);
        put64(&mut b, o + 16, if *size == 0 { 0 } else { at as u64 });
        put64(&mut b, o + 24, if *size == 0 { 0 } else { stored.len() as u64 });
        b[o + 64] = *codec;
    }
    let n = b.len() as u64;
    put64(&mut b, 0x40, n);
    seal(&mut b);
    b
}

fn open(tmp: &Temp, b: &[u8]) -> Nup {
    let p = tmp.0.join("fixture.nup");
    std::fs::write(&p, b).unwrap();
    Nup::open(&p, false).unwrap()
}

/// The one member of a one-file package, copied as extraction does.
fn copy_one(name: &str, logical_len: u64, stored: &[u8], codec: u8, extent: Option<u64>) -> (io::Result<()>, Vec<u8>) {
    let t = Temp::new();
    let mut b = tree(&[vec![(name.to_string(), stored.to_vec(), logical_len, codec)]]);
    // (the tail holds what a truncated extent leaves out)
    b.extend(vec![0u8; logical_len as usize + 64]);
    let n = b.len() as u64;
    put64(&mut b, 0x40, n);
    let nup = open(&t, &b);
    let (rel, i) = nup.walk(0).unwrap().remove(0);
    assert_eq!(rel, name);
    let mut r = nup.files[i].clone();
    if let Some(e) = extent {
        r.stored = e;
    }
    let mut out = Vec::new();
    (nup.copy_to(&r, &mut out, &rel, None), out)
}

#[test]
fn codecs_extents_and_limits() {
    let plain: Vec<u8> = [vec![0u8; 32], b"ordinary payload, not another Zstd frame".repeat(100)].concat();
    let packed = zs(&plain);
    let n = plain.len() as u64;
    assert_eq!(copy_one("member.zs", n, &packed, 1, None).1, plain, "explicit outer codec ignored for .zs plaintext");
    let game = zs(&b"game asset content".repeat(200));
    assert_eq!(copy_one("asset.zs", game.len() as u64, &game, 0, None).1, game, "raw game zstd was decompressed");
    assert_eq!(copy_one("legacy.bin", n, &packed, 0, None).1, plain, "legacy outer-frame detection changed");
    assert!(copy_one("truncated.zs", n, &packed, 1, Some(packed.len() as u64 - 1)).0.is_err(), "explicit extent overread or incomplete frame accepted");
    let (r, out) = copy_one("oversized.bin", 8, &zs(&[0u8; 65536]), 1, None);
    assert!(r.unwrap_err().to_string().contains("expanded past its declared size"));
    assert!(out.is_empty(), "wrote {} bytes past the declared size", out.len());
}

#[test]
fn classification_by_name() {
    // (a made-up frame whose stored and decoded lengths are both 49)
    let frame = (0..49).map(|i| u8::from_str_radix(&"28b52ffd20314501001402000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f00010041c013"[2 * i..2 * i + 2], 16).unwrap()).collect::<Vec<u8>>();
    let frame = &frame[..49];
    let payload: Vec<u8> = (0..32).chain(std::iter::repeat_n(0, 17)).collect();
    assert_eq!(zstd::decompress(Format::Std, frame, 49).unwrap(), payload);
    let t = Temp::new();
    let check = |data: &[u8], name: &str, size: u64, codec: u8, want: &[u8]| {
        let nup = open(&t, &tree(&[vec![(name.to_string(), data.to_vec(), size, codec)]]));
        let (rel, i) = nup.walk(0).unwrap().remove(0);
        assert_eq!(nup.read(&nup.files[i], &rel).unwrap(), want, "{name}");
        nup
    };
    let nup = check(frame, "x.bcul.zs", 49, 0, frame);
    let r = &nup.files[0];
    assert!(!nup.is_packed(r, "x.bcul.zs").unwrap());
    assert!(nup.is_packed(r, "main").unwrap(), "the name given decides");
    let plain = b"not another Zstd frame".repeat(3);
    check(&zs(&plain), "x.bcul.zs", plain.len() as u64, 1, &plain);
    check(frame, "main", 49, 0, &payload);
    let nso = [&b"NSO0"[..], &[0; 40]].concat();
    check(&nso, "main", nso.len() as u64, 0, &nso);
    let nested = zstd::compress(Format::Std, frame, 3, false);
    check(&nested, "x.bcul.zs", 49, 0, frame);
}

/// verify over a sealed package: (result, its lines)
fn verify(b: &mut [u8], titles: Option<&[Title]>, only: Option<Part>) -> nup::Verified {
    seal(b);
    let t = Temp::new();
    let p = t.0.join("v.nup");
    std::fs::write(&p, &*b).unwrap();
    match Nup::open(&p, true) {
        Ok(n) => nup::verify(&n, titles, only),
        Err(e) => nup::Verified { bad: 1, lines: vec![format!("BAD package: {e}")], ..Default::default() },
    }
}

#[test]
fn verification() {
    let game = zs(&b"self-authored game payload".repeat(500));
    let (mut good, _) = flat(&[(0, "raw.bin", b"raw bytes".to_vec(), 0), (0, "raw.zs", game.clone(), 0), (0, "outer.zs", game.clone(), 1)], 1);
    let v = verify(&mut good, None, None);
    assert_eq!((v.bad, v.checked), (0, 3), "{:?}", v.lines);
    let (mut empty, at) = flat(&[(0, "empty.bin", vec![], 0)], 1);
    put64(&mut empty, at + 16, 0);
    put64(&mut empty, at + 24, 0);
    assert_eq!(verify(&mut empty, None, None).bad, 0);
    // (a hash-valid zero start: the container's integrity alone must not pass the game's file)
    let (mut damaged, _) = flat(&[(0, "bad.zs", [vec![0u8; 12288], b"not a Zstd frame".to_vec()].concat(), 1)], 1);
    let v = verify(&mut damaged, None, None);
    assert!(v.bad == 1 && v.lines[0].contains("expected game Zstd header"), "{:?}", v.lines);
    let (mut dedup, at) = flat(&[(0, "one.bin", b"shared bytes".to_vec(), 0), (0, "two.bin", b"shared bytes".to_vec(), 0)], 1);
    dedup[at + 80 + 32] ^= 1;
    let v = verify(&mut dedup, None, None);
    assert!(v.bad == 1 && v.decoded == 1 && v.lines[0].contains("SHA-256 mismatch"), "{v:?}");
    for problem in ["checksum", "truncated", "trailing", "bounds", "codec", "flags", "reserved"] {
        let (mut b, at) = flat(&[(0, "test.zs", game.clone(), 1)], 1);
        let (off, size) = (get64(&b, at + 16) as usize, get64(&b, at + 24));
        match problem {
            "checksum" => b[off + size as usize - 1] ^= 1,
            "truncated" => put64(&mut b, at + 24, size - 1),
            "trailing" => {
                b.push(b'x');
                let n = b.len() as u64;
                put64(&mut b, 0x40, n);
                put64(&mut b, at + 24, size + 1);
            }
            "bounds" => {
                let n = b.len().next_multiple_of(16) as u64;
                put64(&mut b, at + 16, n)
            }
            p => b[at + [("codec", 64), ("flags", 65), ("reserved", 66)].iter().find(|x| x.0 == p).unwrap().1] = 9,
        }
        assert_eq!(verify(&mut b, None, None).bad, 1, "{problem}");
    }
    let (mut b, _) = flat(&[(0, "bad.zs", vec![0; 32], 0), (1, "good.zs", game.clone(), 0)], 2);
    assert_eq!(verify(&mut b, None, None).bad, 1);
    let selected = Title { index: 1, id: 0x1001, version: 2, root: 1, exefs: NONE, romfs: 1 };
    assert_eq!(verify(&mut b, Some(&[selected.clone()]), None).bad, 0);
    assert_eq!(verify(&mut b, None, Some(Part::Romfs)).bad, 1);
    assert_eq!(verify(&mut b, Some(&[selected]), Some(Part::Romfs)).bad, 0);
    // (a rehashed table of contents that does not hold its tables, and broken trees)
    for (problem, reason) in [
        ("short-toc", "exceed declared TOC"),
        ("strings", "exceed declared TOC"),
        ("child", "child index"),
        ("directory-range", "child range"),
        ("title", "invalid directory index"),
        ("cycle", "directory cycle"),
        ("orphan", "orphan"),
        ("utf8", "utf-8"),
    ] {
        let (mut b, at) = flat(&[(0, "test.zs", game.clone(), 0)], 1);
        let (dirs_at, kids_at) = (0xb0 + 64, at + 80);
        match problem {
            "short-toc" => put64(&mut b, 0x10, 0x30),
            "strings" => put64(&mut b, 0xa0, 1 << 20),
            "child" => put32(&mut b, kids_at, 99),
            "directory-range" => put32(&mut b, dirs_at + 8, 1),
            "title" => put32(&mut b, 0xb0 + 8, 1),
            "cycle" => put32(&mut b, kids_at, 0),
            "orphan" => put32(&mut b, dirs_at + 12, 0),
            _ => b[kids_at + 4] = 0xff,
        }
        let v = verify(&mut b, None, None);
        assert!(v.bad == 1 && v.lines[0].starts_with("BAD package:") && v.lines[0].contains(reason), "{problem}: {:?}", v.lines);
    }
    let (mut b, _) = flat(&[(0, "one.bin", b"one".to_vec(), 0), (1, "two.bin", b"two".to_vec(), 0)], 2);
    put32(&mut b, 0xb0 + 16, 1);
    assert!(verify(&mut b, None, None).lines[0].contains("subtree is not reachable"));
    put32(&mut b, 0xb0 + 16, 0);
    put32(&mut b, 0xb0 + 64 + 8, 0);
    put32(&mut b, 0xb0 + 64 + 16, 0);
    assert!(verify(&mut b, None, None).lines[0].contains("orphan"));
    let (mut b, at) = flat(&[(0, "test.zs", game, 0)], 1);
    seal(&mut b);
    put32(&mut b, at + 80, 2); // (an invalid child and a table of contents not rehashed)
    let t = Temp::new();
    std::fs::write(t.0.join("x.nup"), &b).unwrap();
    assert!(Nup::open(&t.0.join("x.nup"), true).err().unwrap().contains("TOC SHA-256 mismatch"));
}

#[test]
fn incomplete_package() {
    let t = Temp::new();
    let mut b = tree(&[vec![("romfs/a.bin".into(), b"abc".to_vec(), 3, 0)]]);
    let n = b.len() as u64;
    put64(&mut b, 0x40, n + (64 << 20));
    std::fs::write(t.0.join("x.nup"), &b).unwrap();
    assert!(Nup::open(&t.0.join("x.nup"), false).err().unwrap().contains("the .nup file is incomplete"));
}

const DATA: &[u8] = b"authored fixture";

/// `extract` of made-up files, each written as `DATA`.
fn extract_names(out: &Path, names: &[&str]) -> Result<extract::Written, String> {
    let files: Vec<(String, u64)> = names.iter().map(|n| (n.to_string(), DATA.len() as u64)).collect();
    extract::extract(out, Part::Romfs, &files, &mut |_, o: &mut Output| io::Write::write_all(o, DATA), &mut Progress::new(None, ""))
}

/// Every path below `root`: a link's target, a file's bytes.
fn snapshot(root: &Path) -> BTreeMap<PathBuf, String> {
    let mut m = BTreeMap::new();
    let mut pending = vec![root.to_path_buf()];
    while let Some(d) = pending.pop() {
        for e in std::fs::read_dir(&d).unwrap().flatten() {
            let t = e.file_type().unwrap();
            let p = e.path();
            let v = if t.is_symlink() {
                format!("link {}", std::fs::read_link(&p).unwrap().display())
            } else if t.is_dir() {
                pending.push(p.clone());
                "dir".into()
            } else {
                format!("{:?}", std::fs::read(&p).unwrap())
            };
            m.insert(p, v);
        }
    }
    m
}

struct Fixture {
    t: Temp,
    out: PathBuf,
    sentinel: PathBuf,
}

fn fixture() -> Fixture {
    let t = Temp::new();
    let out = t.0.join("out");
    std::fs::create_dir(&out).unwrap();
    let sentinel = t.0.join("outside.bin");
    std::fs::write(&sentinel, b"keep outside data").unwrap();
    Fixture { t, out, sentinel }
}

impl Fixture {
    fn reject(&self, names: &[&str], msg: &str) {
        let before = snapshot(&self.t.0);
        let e = extract_names(&self.out, names).unwrap_err();
        assert!(e.starts_with(msg), "{e}");
        assert_eq!(before, snapshot(&self.t.0));
    }
}

#[test]
fn nested_names_and_resume() {
    let f = fixture();
    let names = ["Map/Forest/data.bin", "Text/Français.bin", "com10.txt", ".hidden", "conifer.txt"];
    assert_eq!(extract_names(&f.out, &names).unwrap(), extract::Written { files: 5, bytes: 5 * DATA.len() as u64 });
    for n in names {
        assert_eq!(std::fs::read(f.out.join("romfs").join(n)).unwrap(), DATA);
    }
    assert_eq!(extract_names(&f.out, &names).unwrap(), extract::Written::default());
}

#[test]
fn paths_cannot_leave_the_output() {
    let f = fixture();
    for name in [
        "", ".", "..", "../../outside.bin", "nested/../../outside.bin", "nested/../file.bin", "/rooted.bin", "//server/share/file.bin", r"\rooted.bin",
        r"\\server\share\file.bin", r"C:\outside.bin", "C:/outside.bin", "C:outside.bin", r"nested\..\outside.bin", "nested//file.bin",
        "nested/.. /outside.bin", "file:stream", "trailing.", "trailing ", "control\x00.bin", "CON", "aux.txt", "NUL.bin", "COM1", "LPT9.txt",
        "COM¹.txt", "COM¹/file", "CONIN$", "conout$", "nested/CONIN$", "nested/ConOut$",
    ] {
        f.reject(&[name], &format!("unsafe path in the package: {name:?}"));
    }
}

#[test]
fn links_are_refused() {
    use std::os::unix::fs::symlink;
    let f = fixture();
    let outside = f.t.0.join("outside");
    std::fs::create_dir(&outside).unwrap();
    symlink(&outside, f.out.join("romfs")).unwrap();
    f.reject(&["new.bin"], "extraction path is a link or reparse point: ");
    std::fs::remove_file(f.out.join("romfs")).unwrap();
    std::fs::create_dir(f.out.join("romfs")).unwrap();
    symlink(&outside, f.out.join("romfs/nested")).unwrap();
    f.reject(&["nested/new.bin"], "extraction path is a link or reparse point: ");
    // (the file itself, before a resume could take it as done; its temporary)
    std::fs::write(&f.sentinel, DATA).unwrap();
    symlink(&f.sentinel, f.out.join("romfs/file.bin")).unwrap();
    f.reject(&["file.bin"], "extraction path is a link or reparse point: ");
    std::fs::remove_file(f.out.join("romfs/file.bin")).unwrap();
    symlink(&f.sentinel, f.out.join("romfs/file.bin.part")).unwrap();
    f.reject(&["file.bin"], "extraction path is a link or reparse point: ");
    std::fs::remove_file(f.out.join("romfs/file.bin.part")).unwrap();
    std::fs::hard_link(&f.sentinel, f.out.join("romfs/file.bin.part")).unwrap();
    f.reject(&["file.bin"], "extraction temporary file has other hard links: ");
}

#[test]
fn partial_files_and_failures() {
    let f = fixture();
    let romfs = f.out.join("romfs");
    std::fs::create_dir(&romfs).unwrap();
    std::fs::write(romfs.join("file.bin.part"), b"an interrupted extraction").unwrap();
    extract_names(&f.out, &["file.bin"]).unwrap();
    assert_eq!(std::fs::read(romfs.join("file.bin")).unwrap(), DATA);
    assert!(!romfs.join("file.bin.part").exists());
    // (a failed file keeps the one before it, frees its temporary, and names itself and the room left)
    let target = romfs.join("Model/authored.zs");
    std::fs::create_dir_all(target.parent().unwrap()).unwrap();
    let files = vec![("Model/authored.zs".to_string(), 12345u64)];
    for (code, want) in [(libc::ENOSPC, "No space left on device writing romfs/Model/authored.zs (12345 bytes): "), (libc::EACCES, "romfs/Model/authored.zs: ")] {
        std::fs::write(&target, b"previous file").unwrap();
        let e = extract::extract(
            &f.out,
            Part::Romfs,
            &files,
            &mut |_, o: &mut Output| {
                io::Write::write_all(o, b"some bytes")?;
                Err(io::Error::from_raw_os_error(code))
            },
            &mut Progress::new(None, ""),
        )
        .unwrap_err();
        assert!(e.starts_with(want), "{e}");
        if code == libc::ENOSPC {
            assert!(e.ends_with(" GB free there"), "{e}");
        }
        assert_eq!(std::fs::read(&target).unwrap(), b"previous file");
        assert!(!target.with_file_name("authored.zs.part").exists());
    }
    let w = extract::extract(&f.out, Part::Romfs, &files, &mut |_, o: &mut Output| io::Write::write_all(o, &[7; 12345]), &mut Progress::new(None, "")).unwrap();
    assert_eq!(w.files, 1);
}

/// A package whose update (title 1) lacks main; its base (title 0) holds `module`.
fn healing(module: &[u8]) -> Vec<u8> {
    tree(&[vec![("exefs/main".into(), module.to_vec(), module.len() as u64, 0)], vec![("exefs/rtld".into(), b"NSO0rtld".to_vec(), 8, 0)]])
}

#[test]
fn heal_takes_a_module_from_another_title() {
    let f = fixture();
    let nup = open(&f.t, &healing(b"NSO0authored module"));
    std::fs::create_dir(f.out.join("exefs")).unwrap();
    let t = nup.titles[1].clone();
    assert_eq!(nso::heal(&nup, &t, &f.out).unwrap(), ["main: taken from title 0 (v0)"]);
    assert_eq!(std::fs::read(f.out.join("exefs/main")).unwrap(), b"NSO0authored module");
    let before = snapshot(&f.t.0);
    nso::heal(&nup, &t, &f.out).unwrap();
    assert_eq!(before, snapshot(&f.t.0));
    // (a hard link at the module is replaced, not written through)
    std::fs::remove_file(f.out.join("exefs/main")).unwrap();
    std::fs::hard_link(&f.sentinel, f.out.join("exefs/main")).unwrap();
    nso::heal(&nup, &t, &f.out).unwrap();
    assert_eq!(std::fs::read(&f.sentinel).unwrap(), b"keep outside data");
    assert_eq!(std::fs::read(f.out.join("exefs/main")).unwrap(), b"NSO0authored module");
}

#[test]
fn heal_and_unpack_refuse_links() {
    use std::os::unix::fs::symlink;
    let nup_bytes = healing(b"NSO0authored module");
    for (at, suffix, msg) in [
        ("exefs", None, "extraction path is a link or reparse point: "),
        ("exefs/main", None, "extraction path is a link or reparse point: "),
        ("exefs/main.part", None, "extraction path is a link or reparse point: "),
        ("exefs/main.part", Some("hard"), "extraction temporary file has other hard links: "),
        ("exefs/main.plain", None, "extraction path is a link or reparse point: "),
        ("exefs/main.plain", Some("hard"), "extraction temporary file has other hard links: "),
    ] {
        let f = fixture();
        let nup = open(&f.t, &nup_bytes);
        let t = nup.titles[1].clone();
        if at != "exefs" {
            std::fs::create_dir(f.out.join("exefs")).unwrap();
        }
        let link = f.out.join(at);
        if suffix == Some("hard") {
            std::fs::hard_link(&f.sentinel, &link).unwrap();
        } else if at == "exefs" {
            let outside = f.t.0.join("outside");
            std::fs::create_dir(&outside).unwrap();
            symlink(&outside, &link).unwrap();
        } else {
            symlink(&f.sentinel, &link).unwrap();
        }
        let before = snapshot(&f.t.0);
        let e = if at.ends_with(".plain") {
            std::fs::write(f.out.join("exefs/main"), zbic_nso(&[])).unwrap();
            nso::unpack_exefs(&f.out).unwrap_err()
        } else {
            let e = nso::heal(&nup, &t, &f.out).unwrap_err();
            assert_eq!(before, snapshot(&f.t.0), "{at}");
            e
        };
        assert!(e.starts_with(msg), "{at}: {e}");
        assert_eq!(std::fs::read(&f.sentinel).unwrap(), b"keep outside data");
    }
}

/// An NSO whose segments (text, ro, data) are ZBIC-compressed, with their hashes.
fn zbic_nso(segs: &[&[u8]]) -> Vec<u8> {
    let mut h = vec![0u8; 0x100];
    h[..4].copy_from_slice(b"NSO0");
    let mut flags = 1 << 7;
    let mut body = b"module-name\0".to_vec();
    for i in 0..3 {
        let plain = segs.get(i).copied().unwrap_or(&[]);
        let off = 0x100 + body.len();
        let stored = if plain.is_empty() {
            vec![]
        } else {
            flags |= (1 << i) | (8 << i);
            h[0xa0 + 0x20 * i..0xc0 + 0x20 * i].copy_from_slice(&Sha256::digest(plain));
            zstd::compress(Format::Zbic, plain, 9, false)
        };
        put32(&mut h, 0x10 + 0x10 * i, off as u32);
        put32(&mut h, 0x10 + 0x10 * i + 4, 0x1000 * i as u32);
        put32(&mut h, 0x10 + 0x10 * i + 8, plain.len() as u32);
        put32(&mut h, 0x60 + 4 * i, stored.len() as u32);
        body.extend_from_slice(&stored);
    }
    put32(&mut h, 0xc, flags);
    [h, body].concat()
}

#[test]
fn zbic_modules_become_plain() {
    let f = fixture();
    let exefs = f.out.join("exefs");
    std::fs::create_dir(&exefs).unwrap();
    // (an empty-segment module, and one with data in each segment)
    std::fs::write(exefs.join("rtld"), zbic_nso(&[])).unwrap();
    let text: Vec<u8> = (0..300_000u32).map(|i| (i * 7 / 13) as u8).collect();
    let (ro, data) = (b"read-only ".repeat(5000), vec![3u8; 4096]);
    std::fs::write(exefs.join("main"), zbic_nso(&[&text, &ro, &data])).unwrap();
    std::fs::write(exefs.join("sdk"), b"NSO0").unwrap();
    std::fs::write(exefs.join("main.plain"), b"interrupted unpack").unwrap();
    assert_eq!(nso::unpack_exefs(&f.out).unwrap(), ["rtld: ZBIC -> plain NSO", "main: ZBIC -> plain NSO"]);
    let m = std::fs::read(exefs.join("main")).unwrap();
    assert_eq!(u32::from_le_bytes(m[0xc..0x10].try_into().unwrap()), (8 | 16 | 32));
    let seg = |i: usize| {
        let (o, n) = (u32::from_le_bytes(m[0x10 + 0x10 * i..][..4].try_into().unwrap()) as usize, u32::from_le_bytes(m[0x60 + 4 * i..][..4].try_into().unwrap()) as usize);
        &m[o..o + n]
    };
    assert_eq!((seg(0), seg(1), seg(2)), (&text[..], &ro[..], &data[..]));
    assert_eq!(&m[0x100..0x10c], b"module-name\0");
    assert!(!exefs.join("main.plain").exists());
    assert_eq!(u32::from_le_bytes(std::fs::read(exefs.join("rtld")).unwrap()[0xc..0x10].try_into().unwrap()), 0);
    assert_eq!(nso::unpack_nso(&exefs.join("main"), &exefs.join("x")).unwrap(), Unpacked::Kept);
    // (a segment that does not match its hash)
    let mut bad = zbic_nso(&[&text]);
    bad[0xa0] ^= 1;
    std::fs::write(exefs.join("main"), bad).unwrap();
    assert!(nso::unpack_exefs(&f.out).unwrap_err().contains("hash mismatch after ZBIC"));
}

#[test]
fn stand_ins() {
    let t = Temp::new();
    let romfs = &t.0;
    let w = |p: &str, d: Vec<u8>| {
        let p = romfs.join(p);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, d).unwrap();
    };
    let z = |c: u8, n: usize| [&MAGIC[..], &vec![c; n]].concat();
    w("Tex/Rock_Temple_Gate_M_01_Nrm.bntx.zs", [vec![0; 400], b"rest".repeat(30)].concat());
    w("Tex/Rock_Temple_Gate_P_01_Nrm.bntx.zs", z(b'P', 500)); // the nearest name: chosen
    w("Tex/Rock_Temple_Gate_M_01_Alb.bntx.zs", z(b'A', 4000)); // another kind of texture
    w("Tex/Rock_Temple_Gate_A_Nrm.bntx.zs", z(b'A', 4000)); // as close a name, a size further off
    w("Tex/Rock_Temple_Gate_M_02_Nrm.bntx.zs", vec![0; 64]); // damaged too: never a stand-in
    w("Tex/Bake_Stage_G_2_1_d0_t0.bntx.zs", vec![0; 64]);
    w("Tex/Bake_Stage_G_2_2_d0_t0.bntx.zs", z(b't', 60));
    w("Tex/Bake_Stage_G_2_2_d1_un3.bntx.zs", z(b'u', 60));
    w("Lonely/Only_One_Nrm.bntx.zs", vec![0; 16]); // nothing of its kind beside it
    w("Other/plain.txt", vec![0; 16]); // not a .zs: not looked at
    w("Other/Skip_A.bntx.zs", [&b"\x5a\x2a\x4d\x18"[..], &[0; 8], &MAGIC].concat()); // a skippable frame first
    let rel = |p: &PathBuf| p.strip_prefix(romfs).unwrap().to_string_lossy().into_owned();
    let damaged: Vec<String> = standin::damaged_game_files(romfs, &mut |_, _| {}).unwrap().iter().map(rel).collect();
    assert_eq!(damaged, ["Lonely/Only_One_Nrm.bntx.zs", "Tex/Bake_Stage_G_2_1_d0_t0.bntx.zs", "Tex/Rock_Temple_Gate_M_01_Nrm.bntx.zs", "Tex/Rock_Temple_Gate_M_02_Nrm.bntx.zs"]);
    let done: BTreeMap<String, Option<String>> = standin::stand_in_damaged(romfs, &mut |_, _| {})
        .unwrap()
        .iter()
        .map(|(p, o)| (rel(p), o.as_ref().map(|o| o.file_name().unwrap().to_string_lossy().into_owned())))
        .collect();
    let get = |k: &str| done[k].as_deref();
    assert_eq!(get("Tex/Rock_Temple_Gate_M_01_Nrm.bntx.zs"), Some("Rock_Temple_Gate_P_01_Nrm.bntx.zs"));
    assert_eq!(get("Tex/Rock_Temple_Gate_M_02_Nrm.bntx.zs"), Some("Rock_Temple_Gate_P_01_Nrm.bntx.zs"));
    assert_eq!(get("Tex/Bake_Stage_G_2_1_d0_t0.bntx.zs"), Some("Bake_Stage_G_2_2_d0_t0.bntx.zs"));
    assert_eq!(get("Lonely/Only_One_Nrm.bntx.zs"), None);
    assert_eq!(std::fs::read(romfs.join("Tex/Rock_Temple_Gate_M_01_Nrm.bntx.zs")).unwrap(), z(b'P', 500));
    assert!(extract::list_folder(romfs).unwrap().iter().all(|(p, _)| !p.ends_with(".part")));
    let again: Vec<String> = standin::stand_in_damaged(romfs, &mut |_, _| {}).unwrap().iter().map(|(p, _)| rel(p)).collect();
    assert_eq!(again, ["Lonely/Only_One_Nrm.bntx.zs"]);
}

#[test]
fn dump_from_a_package_and_from_a_folder() {
    let t = Temp::new();
    let text = b"text of the module ".repeat(1000);
    let main = zbic_nso(&[&text]);
    let romfs_file = zs(&b"a game file ".repeat(100));
    let pkg = tree(&[
        vec![("exefs/main".into(), b"NSO0old".to_vec(), 7, 0), ("romfs/Old/x.zs".into(), romfs_file.clone(), romfs_file.len() as u64, 0)],
        vec![
            ("exefs/main".into(), zs(&main), main.len() as u64, 1),
            ("exefs/main.npdm".into(), b"META".to_vec(), 4, 0),
            ("romfs/Map/a.zs".into(), romfs_file.clone(), romfs_file.len() as u64, 0),
            ("romfs/Map/Sub/b.bin".into(), zs(b"bee"), 3, 1),
            ("romfs/Map/c_Nrm.bntx.zs".into(), vec![0; 40], 40, 0),
            ("romfs/Map/d_Nrm.bntx.zs".into(), romfs_file.clone(), romfs_file.len() as u64, 0),
        ],
    ]);
    std::fs::write(t.0.join("d.nup"), &pkg).unwrap();
    let out = t.0.join("out");
    let d = Dump::open(&t.0.join("d.nup"), None, &out).unwrap();
    let mut p = Progress::new(None, "");
    let e = d.exefs(&mut p).unwrap();
    assert_eq!((e.written.files, e.notes.clone()), (2, vec!["main: ZBIC -> plain NSO".to_string()]));
    assert!(!out.join("romfs").exists(), "the exefs comes alone");
    let r = d.romfs(&mut p).unwrap();
    assert_eq!(r.written.files, 4);
    assert_eq!(r.notes.len(), 1, "{:?}", r.notes);
    assert_eq!(std::fs::read(out.join("romfs/Map/c_Nrm.bntx.zs")).unwrap(), romfs_file);
    assert_eq!(std::fs::read(out.join("romfs/Map/Sub/b.bin")).unwrap(), b"bee");
    assert!(std::fs::read(out.join("exefs/main")).unwrap().windows(text.len()).any(|w| w == text));
    // (the folder just written, read as a player's folder dump: the same tree)
    let out2 = t.0.join("out2");
    let d2 = Dump::open(&out, None, &out2).unwrap();
    d2.exefs(&mut p).unwrap();
    d2.romfs(&mut p).unwrap();
    assert_eq!(extract::list_folder(&out).unwrap(), extract::list_folder(&out2).unwrap());
    for (f, _) in extract::list_folder(&out).unwrap() {
        assert_eq!(std::fs::read(out.join(&f)).unwrap(), std::fs::read(out2.join(&f)).unwrap(), "{f}");
    }
    // (in place: nothing written)
    let d3 = Dump::open(&out, None, &out).unwrap();
    assert!(d3.romfs(&mut p).unwrap().in_place);
}

/// A .nup of a real exefs and a part of a romfs (HOOKSHOT_NUP_FROM=<dump folder>, HOOKSHOT_NUP_TO=<file>,
/// HOOKSHOT_NUP_ROMFS_MB=<romfs MB>): main ZBIC-compressed, members alternately raw and codec 1. For the reader's
/// proof on game data; nothing of it is kept in the tree.
#[test]
#[ignore]
fn make_proof_nup() {
    let from = PathBuf::from(std::env::var("HOOKSHOT_NUP_FROM").unwrap());
    let to = PathBuf::from(std::env::var("HOOKSHOT_NUP_TO").unwrap());
    let budget: u64 = std::env::var("HOOKSHOT_NUP_ROMFS_MB").map_or(1024, |v| v.parse().unwrap()) << 20;
    let mut rows = Vec::new();
    for (rel, size) in extract::list_folder(&from.join("exefs")).unwrap() {
        let mut data = std::fs::read(from.join("exefs").join(&rel)).unwrap();
        if rel == "main" {
            data = zbic_of_plain(&data);
        }
        rows.push((format!("exefs/{rel}"), zs(&data), if rel == "main" { data.len() as u64 } else { size }, 1));
    }
    let mut used = 0;
    for (i, (rel, size)) in extract::list_folder(&from.join("romfs")).unwrap().into_iter().enumerate() {
        if used + size > budget {
            continue;
        }
        used += size;
        let data = std::fs::read(from.join("romfs").join(&rel)).unwrap();
        let (stored, codec) = if i % 2 == 0 { (zs(&data), 1) } else { (data, 0) };
        rows.push((format!("romfs/{rel}"), stored, size, codec));
    }
    std::fs::write(&to, tree(&[rows])).unwrap();
}

/// A plain NSO with each segment ZBIC-compressed (flags: ZBIC and each segment's compressed bit).
fn zbic_of_plain(raw: &[u8]) -> Vec<u8> {
    let g = |o: usize| u32::from_le_bytes(raw[o..o + 4].try_into().unwrap());
    let mut h = raw[..0x100].to_vec();
    let first = (0..3).map(|i| g(0x10 + 0x10 * i) as usize).min().unwrap();
    let mut body = raw[0x100..first].to_vec();
    for i in 0..3 {
        let (o, n) = (g(0x10 + 0x10 * i) as usize, g(0x60 + 4 * i) as usize);
        let z = zstd::compress(Format::Zbic, &raw[o..o + n], 19, false);
        assert_eq!(zstd::decompress(Format::Zbic, &z, n).as_deref(), Ok(&raw[o..o + n]), "segment {i}: the ZBIC encoder's frame does not decode");
        put32(&mut h, 0x10 + 0x10 * i, (0x100 + body.len()) as u32);
        put32(&mut h, 0x60 + 4 * i, z.len() as u32);
        body.extend_from_slice(&z);
    }
    let flags = g(0xc) | (1 << 7) | 7;
    put32(&mut h, 0xc, flags);
    [h, body].concat()
}
