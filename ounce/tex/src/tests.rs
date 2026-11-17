//! The converter on synthetic textures, no game data: the block-linear layout against an independent reading of the GOB
//! rule, the ASTC block scan, the targets by channel swizzle, the frame split, a whole run over a made-up romfs read
//! back, the fallback to exact texels, and BC7 keeping clear texels clear.

use crate::bntx::{self, block_linear_size, mip_block_height, surface_layout, to_block_linear, to_linear, Tex};
use crate::convert::{Settings, Status, FLAG_DEDUP};
use crate::plan::{choose_target, compared_channels, frames_of, prepare_input, Enc, Kind, SW_A, SW_B, SW_G, SW_ONE, SW_R};
use crate::run::{GameFiles, Options};
use crate::store::{read_index, PackReader};
use crate::{native, store};
use std::path::{Path, PathBuf};

/// a small deterministic generator (xorshift)
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn bytes(&mut self, n: usize) -> Vec<u8> {
        (0..n).map(|_| self.next() as u8).collect()
    }
}

/// byte (x, y) of a block-linear surface, written out from the GOB rule (the Switch 2's)
fn ref_offset(x: u32, y: u32, width_bytes: u32, bh: u32) -> usize {
    let gobs_x = width_bytes.div_ceil(64);
    let block_rows = 8 << bh;
    let block = (y / block_rows) * gobs_x + x / 64;
    let in_gob = ((x % 64) / 32) * 256 + ((y % 8) / 4) * 128 + ((x % 32) / 16) * 64 + ((y % 4) / 2) * 32 + (y % 2) * 16 + x % 16;
    (block * 512 * (1 << bh) + ((y % block_rows) / 8) * 512 + in_gob) as usize
}

#[test]
fn block_linear() {
    let mut rng = Rng(5);
    for (wb, rows, bh) in [(16, 4, 0), (64, 8, 0), (8, 3, 0), (200, 37, 2), (4096, 64, 4), (48, 129, 5), (1, 1, 0), (128, 16, 1), (6, 17, 3)] {
        let lin = rng.bytes((wb * rows) as usize);
        let bl = to_block_linear(&lin, wb, rows, bh);
        assert_eq!(bl.len() as u64, block_linear_size(wb, rows, bh));
        let mut want = vec![0u8; bl.len()];
        for y in 0..rows {
            for x in 0..wb {
                want[ref_offset(x, y, wb, bh)] = lin[(y * wb + x) as usize];
            }
        }
        assert!(bl == want, "swizzle differs from the GOB rule: {wb} x {rows} bh {bh}");
        assert_eq!(to_linear(&bl, wb, rows, bh), lin, "round trip {wb} x {rows} bh {bh}");
    }
    // (a short source reads as zeros past its end)
    let short = to_linear(&[7u8; 100], 64, 8, 0);
    for y in 0..8 {
        for x in 0..64 {
            assert_eq!(short[(y * 64 + x) as usize], if ref_offset(x, y, 64, 0) < 100 { 7 } else { 0 });
        }
    }
    assert_eq!(
        [mip_block_height(16, 1), mip_block_height(8, 1), mip_block_height(64, 4), mip_block_height(1, 5), mip_block_height(129, 4)],
        [1, 0, 3, 0, 4]
    );
}

fn astc_block(bm: u128, parts: u128, cem: u128, low6: u128) -> [u8; 16] {
    let v = bm | (parts << 11) | if parts == 0 { cem << 13 } else { low6 << 23 };
    v.to_le_bytes()
}

fn void_extent(c: [u8; 4], hdr: bool) -> [u8; 16] {
    let mut v: u128 = 0x1FC | ((hdr as u128) << 9) | (3 << 10) | (((1u128 << 52) - 1) << 12);
    for (i, &x) in c.iter().enumerate() {
        v |= (x as u128 * 257) << (64 + 16 * i);
    }
    v.to_le_bytes()
}

#[test]
fn astc_scan() {
    let ok_mode = 0x42; // (4x4 weights of 2 bits: a valid 2D mode)
    let blocks: Vec<u8> = [
        void_extent([1, 2, 3, 4], false),
        void_extent([1, 2, 3, 4], true),
        astc_block(ok_mode, 0, 15, 0),
        astc_block(ok_mode, 0, 8, 0),
        astc_block(0, 0, 0, 0),
        astc_block(ok_mode, 1, 0, 3 << 2), // (2 partitions, shared CEM 3: HDR)
        astc_block(ok_mode, 1, 0, 10 << 2),
    ]
    .concat();
    let mut c = [0u64; 4];
    bntx::astc_scan(&blocks, &mut c);
    assert_eq!(c, [2, 1, 1, 1]);
}

#[test]
fn targets() {
    let (astc55u, astc85s) = ((0x2f << 8) | 1, (0x32 << 8) | 6);
    let t = choose_target(astc55u, [2, 5, 0, 1], 0).unwrap(); // normal map: R and A
    assert_eq!((t.kind, t.vk, t.c0, t.c1, t.map, t.dropped), (Kind::Bc5, 141, 0, 3, [SW_R, SW_R, SW_R, SW_G], 0b0110));
    let t = choose_target(astc55u, [2, 2, 2, 2], 0).unwrap();
    assert_eq!((t.kind, t.vk, t.c0, t.map), (Kind::Bc4, 139, 0, [SW_R; 4]));
    assert_eq!(choose_target(astc55u, [5, 5, 5, 5], 0).unwrap().c0, 3);
    let t = choose_target(astc85s, [2, 3, 4, 5], 0).unwrap();
    assert_eq!((t.kind, t.vk, t.force_alpha, t.map), (Kind::Bc7, 146, false, [SW_R, SW_G, SW_B, SW_A]));
    let t = choose_target(astc55u, [2, 3, 4, 1], 0).unwrap(); // alpha unused
    assert_eq!((t.kind, t.vk, t.force_alpha, t.dropped), (Kind::Bc7, 145, true, 8));
    let t = choose_target(astc85s, [2, 2, 2, 5], 0).unwrap(); // sRGB luminance-alpha: BC7
    assert_eq!((t.kind, t.vk, t.fill), (Kind::Bc7, 146, Some(0)));
    let mut px = [10u8, 20, 30, 40];
    prepare_input(&mut px, &t);
    assert_eq!(px, [10, 10, 10, 40]);
    for (f, kind, vk) in [
        ((0x1a << 8) | 6, Kind::CopyBc, 134),
        ((0x1f << 8) | 0xa, Kind::CopyBc, 143),
        ((0x1e << 8) | 2, Kind::CopyBc, 142),
        ((0x0a << 8) | 1, Kind::Copy, 70),
        ((0x15 << 8) | 5, Kind::Copy, 97),
        ((0x0f << 8) | 5, Kind::Copy, 122),
    ] {
        let t = choose_target(f, [2, 3, 4, 5], 0).unwrap();
        assert_eq!((t.kind, t.vk), (kind, vk), "{f:#x}");
    }
    assert!(choose_target(0x7701, [2, 3, 4, 5], 0).is_err());
    let t = choose_target(astc55u, [2, 5, 0, 1], 1).unwrap();
    assert_eq!((t.kind, t.vk, t.map, t.used.clone()), (Kind::Bc7, 145, [SW_R, SW_G, SW_B, SW_A], vec![0, 3]));
    let rgba = [SW_R, SW_G, SW_B, SW_A];
    for (sel, srgb, vk, pack, m) in [
        ([2, 5, 0, 1], false, 16, vec![0, 3], [SW_R, SW_R, SW_R, SW_G]),
        ([2, 2, 2, 2], false, 9, vec![0], [SW_R; 4]),
        ([5, 5, 5, 5], false, 9, vec![3], [SW_R; 4]),
        ([3, 3, 3, 3], false, 9, vec![1], [SW_R; 4]),
        ([2, 3, 4, 1], false, 37, vec![0, 1, 2, 3], rgba),
        ([2, 3, 4, 5], false, 37, vec![0, 1, 2, 3], rgba),
        ([2, 2, 2, 2], true, 15, vec![0], [SW_R; 4]),
        ([5, 5, 5, 5], true, 9, vec![3], [SW_R; 4]),
        ([2, 3, 4, 5], true, 43, vec![0, 1, 2, 3], rgba),
        ([2, 5, 0, 1], true, 43, vec![0, 1, 2, 3], rgba),
    ] {
        let t = choose_target((0x2f << 8) | if srgb { 6 } else { 1 }, sel, 2).unwrap();
        assert_eq!((t.kind, t.vk, t.pack.clone(), t.map), (Kind::Raw, vk, pack, m), "{sel:?} {srgb}");
    }
    assert_eq!(choose_target(astc55u, [2, 5, 0, 1], 2).unwrap().dropped, 0b0110);
    assert_eq!(choose_target((0x1a << 8) | 1, [2, 3, 4, 5], 2).unwrap().kind, Kind::CopyBc); // (rules only touch ASTC)
    assert_ne!(SW_ONE, SW_R);
}

#[test]
fn frames() {
    let m = |a: u64, n: u64| (a, n);
    let mips = [m(0, 3 << 20), m(3 << 20, 1 << 20), m(4 << 20, 300 << 10), m((4 << 20) + (300 << 10), 60 << 10), m((4 << 20) + (360 << 10), 4 << 10)];
    let total = mips[4].0 + mips[4].1;
    let fr = frames_of(total, &mips, 1 << 20, 64 << 10);
    let mut pos = 0;
    for &(a, n) in &fr {
        assert!(a == pos && n > 0 && n <= 1 << 20 && a % 16 == 0, "{fr:?}");
        pos += n;
    }
    assert_eq!(pos, total);
    assert_eq!(fr[..5].iter().map(|f| f.0).collect::<Vec<_>>(), [0, 1 << 20, 2 << 20, 3 << 20, 4 << 20]);
    assert_eq!((fr.len(), fr[5]), (6, ((4 << 20) + (300 << 10), 64 << 10)));
}

// ---- a made-up romfs ----
struct Spec {
    name: String,
    fmt: u32,
    w: u32,
    h: u32,
    layers: u32,
    mips: u32,
    bh: u32,
    sel: [u8; 4],
    target: u8,
    /// linear blocks per (mip, layer), mips outer
    lin: Vec<Vec<u8>>,
}

fn spec(name: &str, fmt: u32, (w, h, layers, mips, bh): (u32, u32, u32, u32, u32), sel: [u8; 4], target: u8, fill: &mut dyn FnMut(usize) -> Vec<u8>) -> Spec {
    let (_, bw, bhp, bpb) = bntx::fmt(fmt >> 8).unwrap();
    let mut lin = Vec::new();
    for m in 0..mips {
        let (mw, mh) = ((w >> m).max(1), (h >> m).max(1));
        for _ in 0..layers {
            lin.push(fill((mw.div_ceil(bw) * mh.div_ceil(bhp) * bpb) as usize));
        }
    }
    Spec { name: name.into(), fmt, w, h, layers, mips, bh, sel, target, lin }
}

fn tex_of(s: &Spec) -> Tex {
    Tex {
        off: 0,
        mips: s.mips,
        samples: 1,
        format: s.fmt,
        width: s.w,
        height: s.h,
        depth: 1,
        layers: s.layers,
        bh_log2: s.bh,
        image_size: 0,
        alignment: 512,
        comp_sel: s.sel,
        target: s.target,
        name: s.name.clone(),
        mip_offsets: Vec::new(),
    }
}

fn put<const N: usize>(b: &mut [u8], o: usize, v: [u8; N]) {
    b[o..o + N].copy_from_slice(&v);
}

/// A BNTX as the game's: header, BRTIs, names, mip tables, BRTD with the block-linear data, _RLT.
fn make_bntx(texs: &[Spec]) -> Vec<u8> {
    let n = texs.len();
    let tbl = 0x60;
    let brti0 = (tbl + 8 * n).div_ceil(16) * 16;
    let mut pos = brti0 + 0xA0 * n;
    let mut names = Vec::new();
    let mut strs = Vec::new();
    for t in texs {
        names.push(pos);
        strs.push((pos, t.name.clone()));
        pos = (pos + 2 + t.name.len() + 1).div_ceil(8) * 8;
    }
    let mut mipt = Vec::new();
    for t in texs {
        mipt.push(pos);
        pos += 8 * t.mips as usize;
    }
    let brtd = pos.div_ceil(0x1000).max(1) * 0x1000;
    let mut data = Vec::new();
    let mut starts = Vec::new();
    let mut layouts = Vec::new();
    for t in texs {
        let (lay, stride) = surface_layout(&tex_of(t));
        while (brtd + 0x10 + data.len()) % 512 != 0 {
            data.push(0);
        }
        starts.push(brtd + 0x10 + data.len());
        let mut img = vec![0u8; (stride * t.layers as u64) as usize];
        let bpb = bntx::fmt(t.fmt >> 8).unwrap().3;
        for (m, mip) in lay.iter().enumerate() {
            for l in 0..t.layers as usize {
                let bl = to_block_linear(&t.lin[m * t.layers as usize + l], mip.wb * bpb, mip.hb, mip.bh);
                let o = (l as u64 * stride + mip.off) as usize;
                img[o..o + mip.size as usize].copy_from_slice(&bl);
            }
        }
        data.extend_from_slice(&img);
        layouts.push((lay, stride));
    }
    let rlt = (brtd + 0x10 + data.len()).div_ceil(8) * 8;
    let total = rlt + 0x70;
    let mut b = vec![0u8; total];
    b[brtd + 0x10..brtd + 0x10 + data.len()].copy_from_slice(&data);
    b[0..4].copy_from_slice(b"BNTX");
    b[8..12].copy_from_slice(&[1, 1, 4, 0]);
    put(&mut b, 0x18, (rlt as u32).to_le_bytes());
    put(&mut b, 0x1c, (total as u32).to_le_bytes());
    b[0x20..0x24].copy_from_slice(b"Ounc");
    put(&mut b, 0x24, (n as u32).to_le_bytes());
    put(&mut b, 0x28, (tbl as u64).to_le_bytes());
    put(&mut b, 0x30, (brtd as u64).to_le_bytes());
    b[brtd..brtd + 4].copy_from_slice(b"BRTD");
    b[rlt..rlt + 4].copy_from_slice(b"_RLT");
    for (p, s) in strs {
        put(&mut b, p, (s.len() as u16).to_le_bytes());
        b[p + 2..p + 2 + s.len()].copy_from_slice(s.as_bytes());
    }
    for (i, t) in texs.iter().enumerate() {
        let o = brti0 + 0xA0 * i;
        put(&mut b, tbl + 8 * i, (o as u64).to_le_bytes());
        b[o..o + 4].copy_from_slice(b"BRTI");
        put(&mut b, o + 0x16, (t.mips as u16).to_le_bytes());
        put(&mut b, o + 0x18, 1u32.to_le_bytes());
        put(&mut b, o + 0x1c, t.fmt.to_le_bytes());
        for (k, v) in [t.w, t.h, 1, t.layers, t.bh].iter().enumerate() {
            put(&mut b, o + 0x24 + 4 * k, v.to_le_bytes());
        }
        let (lay, stride) = &layouts[i];
        put(&mut b, o + 0x50, ((stride * t.layers as u64) as u32).to_le_bytes());
        put(&mut b, o + 0x54, 512u32.to_le_bytes());
        b[o + 0x58..o + 0x5c].copy_from_slice(&t.sel);
        b[o + 0x5c] = t.target;
        put(&mut b, o + 0x60, (names[i] as u64).to_le_bytes());
        put(&mut b, o + 0x70, (mipt[i] as u64).to_le_bytes());
        for (m, mip) in lay.iter().enumerate() {
            put(&mut b, mipt[i] + 8 * m, ((starts[i] as u64) + mip.off).to_le_bytes());
        }
    }
    b
}

fn astc_fill(colour: impl Fn(usize) -> [u8; 4]) -> impl FnMut(usize) -> Vec<u8> {
    move |n| (0..n / 16).flat_map(|i| void_extent(colour(i), false)).collect()
}

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("ounce-tex-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn write_romfs(romfs: &Path, files: &[(&str, &[Spec])]) {
    for (path, texs) in files {
        let p = romfs.join(path);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, ounce_dump::zstd::compress(ounce_dump::zstd::Format::Std, &make_bntx(texs), 3, false)).unwrap();
    }
}

fn run_store(romfs: &Path, out: &Path, min_psnr: f64) -> crate::run::Stats {
    let admit = |_mb: u64| -> Box<dyn crate::run::Held> { Box::new(()) };
    let progress = |_: &str| {};
    let o = Options {
        romfs: romfs.to_path_buf(),
        out: out.to_path_buf(),
        game: GameFiles { zstd_dictionaries: None, skip_dirs: vec!["Sound".into()] },
        settings: Settings { bc7_level: 1, bc45_radius: 2, zstd_level: 3, min_psnr, frame_kb: 64 },
        jobs: 4,
        pack_limit: 1 << 20,
        only: Vec::new(),
        admit: &admit,
        progress: &progress,
    };
    crate::run::run(&o).unwrap()
}

/// a texture's raw data from its frames
fn raw_of(out: &Path, t: &store::TexEntry) -> Vec<u8> {
    let mut r = PackReader::new(out);
    let mut raw = vec![0u8; t.rec.raw_size as usize];
    for f in &t.frames {
        let d = ounce_dump::zstd::decompress(ounce_dump::zstd::Format::Std, &r.read(f).unwrap(), f.raw_size as usize).unwrap();
        raw[f.raw_off as usize..][..d.len()].copy_from_slice(&d);
    }
    raw
}

#[test]
fn whole_run() {
    let d = tmp("run");
    let (romfs, out) = (d.join("romfs"), d.join("store"));
    let mut rng = Rng(1);
    let mut rnd = |n| rng.bytes(n);
    let kept = [
        spec("Kept_BC1", (0x1a << 8) | 6, (100, 60, 1, 5, 2), [2, 3, 4, 5], 1, &mut rnd),
        spec("Kept_RGBA8_Array", (0x0b << 8) | 1, (33, 17, 4, 3, 1), [2, 3, 4, 5], 5, &mut rnd),
        spec("Kept_BC4_Big", (0x1d << 8) | 1, (2048, 1024, 1, 3, 4), [2, 3, 4, 5], 1, &mut rnd),
        spec("Kept_R16_Odd", (0x0a << 8) | 1, (5, 3, 3, 3, 0), [2, 3, 4, 5], 5, &mut rnd),
    ];
    let colour = |i: usize| [(i * 7 % 256) as u8, (i * 13 % 256) as u8, (i * 3 % 256) as u8, (255 - i % 256) as u8];
    let conv = [
        spec("Nrm", (0x2f << 8) | 1, (40, 35, 1, 3, 0), [2, 5, 0, 1], 1, &mut astc_fill(colour)),
        spec("Alb", (0x32 << 8) | 6, (64, 30, 2, 2, 0), [2, 3, 4, 5], 5, &mut astc_fill(colour)),
        spec("AO", (0x2d << 8) | 1, (16, 16, 1, 1, 0), [2, 2, 2, 2], 1, &mut astc_fill(colour)),
    ];
    let copy = [spec("AO_Copy", (0x2d << 8) | 1, (16, 16, 1, 1, 0), [2, 2, 2, 2], 1, &mut astc_fill(colour))];
    let mut copy_named = copy;
    copy_named[0].name = "AO_Copy".into();
    let files: Vec<(&str, &[Spec])> = vec![
        ("Tex/Kept_BC1.bntx.zs", &kept[..1]),
        ("Tex/Kept_Two.bntx.zs", &kept[1..3]),
        ("Tex/Kept_R16.bntx.zs", &kept[3..]),
        ("Tex/Conv.bntx.zs", &conv[..]),
        ("Tex/AO_Copy.bntx.zs", &copy_named[..]),
        ("Sound/Skipped.bntx.zs", &kept[..1]),
    ];
    write_romfs(&romfs, &files);
    std::fs::create_dir_all(romfs.join("Other")).unwrap();
    std::fs::write(romfs.join("Other/not_a_texture.bin"), b"nothing here".repeat(100)).unwrap();
    let st = run_store(&romfs, &out, 35.0);
    assert!(st.errors.is_empty() && st.files == 6 && st.bntx_files == 5, "{st:?}");
    let idx = read_index(&out.join("index.bin")).unwrap();
    assert!(idx.n_packs >= 2, "packs roll over at 1 MiB: {}", idx.n_packs);
    let mut paths: Vec<&str> = idx.entries.iter().map(|e| e.path.as_str()).collect();
    paths.sort();
    assert_eq!(paths, ["Tex/AO_Copy.bntx.zs", "Tex/Conv.bntx.zs", "Tex/Kept_BC1.bntx.zs", "Tex/Kept_R16.bntx.zs", "Tex/Kept_Two.bntx.zs"]);
    let mut r = PackReader::new(&out);
    for (path, texs) in &files[..5] {
        let e = idx.entries.iter().find(|e| e.path == *path).unwrap();
        let b = make_bntx(texs);
        let meta = ounce_dump::zstd::decompress(ounce_dump::zstd::Format::Std, &r.read(&e.meta).unwrap(), e.meta.raw_size as usize).unwrap();
        assert_eq!(meta, [&b[..e.head_len as usize], &b[e.rlt_off as usize..]].concat(), "{path}");
        assert_eq!(e.bntx_size, b.len() as u64);
        for (s, t) in texs.iter().zip(&e.textures) {
            assert_eq!((t.rec.name.as_str(), t.rec.status), (s.name.as_str(), Status::Ok));
            let raw = raw_of(&out, t);
            let (_, bw, bhp, _) = bntx::fmt(s.fmt >> 8).unwrap();
            let plan = choose_target(s.fmt, s.sel, 0).unwrap();
            for m in 0..s.mips as usize {
                let (mo, lb) = t.mips[m];
                assert_eq!(mo % 4, 0);
                let (w, h) = ((s.w >> m).max(1), (s.h >> m).max(1));
                for l in 0..s.layers as usize {
                    let seg = &raw[(mo + l as u64 * lb as u64) as usize..][..lb as usize];
                    let src = &s.lin[m * s.layers as usize + l];
                    if plan.enc.is_none() {
                        assert!(seg == &src[..], "{} mip {m} layer {l}", s.name);
                        continue;
                    }
                    let want = native::astc_decode(src, w, h, bw, bhp).unwrap();
                    let dec = native::decode(plan.enc.unwrap(), w, h, seg);
                    for (sc, dc) in compared_channels(&plan) {
                        let worst = dec.chunks_exact(4).zip(want.chunks_exact(4)).map(|(d, x)| (d[dc] as i32 - x[sc] as i32).abs()).max().unwrap();
                        assert!(worst <= 40, "{} mip {m} layer {l} channel {sc}: {worst}", s.name);
                    }
                }
            }
            if plan.enc.is_some() {
                assert!(t.rec.psnr >= 35.0 && [139, 141, 146].contains(&t.rec.vk_format), "{:?}", t.rec);
            }
            assert!(t.frames.iter().all(|f| f.raw_size <= 64 << 10));
        }
    }
    let rec = |n: &str| idx.entries.iter().flat_map(|e| &e.textures).find(|t| t.rec.name == n).unwrap();
    assert_eq!((rec("Nrm").rec.kind, rec("Alb").rec.kind, rec("AO").rec.kind), (Kind::Bc5, Kind::Bc7, Kind::Bc4));
    assert_eq!(rec("AO").frames, rec("AO_Copy").frames);
    // (the first in index order holds the frames; the other is flagged)
    assert_eq!((rec("AO_Copy").rec.flags & FLAG_DEDUP, rec("AO").rec.flags & FLAG_DEDUP), (0, FLAG_DEDUP));
    let same = crate::compare::compare(&out, &out).unwrap();
    assert!(same.equal() && same.textures == 8, "{same:?}");
    let _ = std::fs::remove_dir_all(&d);
}

#[test]
fn fallback_to_exact() {
    let d = tmp("fallback");
    let (romfs, out) = (d.join("romfs"), d.join("store"));
    let mut rng = Rng(9);
    let mut noise = |n: usize| (0..n / 16).flat_map(|_| void_extent([rng.next() as u8, rng.next() as u8, rng.next() as u8, rng.next() as u8], false)).collect();
    let specs = [
        spec("NrmHard", (0x2f << 8) | 1, (40, 35, 1, 3, 0), [2, 5, 0, 1], 1, &mut noise),
        spec("OneHard", (0x2d << 8) | 1, (32, 32, 1, 2, 0), [2, 2, 2, 2], 1, &mut noise),
        spec("AlbHard", (0x2d << 8) | 6, (32, 20, 2, 2, 0), [2, 3, 4, 1], 5, &mut noise),
    ];
    write_romfs(&romfs, &[("Tex/Hard.bntx.zs", &specs[..])]);
    // (no lossy target reaches 99.5 dB: every one ends exact)
    let st = run_store(&romfs, &out, 99.5);
    assert!(st.errors.is_empty());
    let idx = read_index(&out.join("index.bin")).unwrap();
    let want = [(16, vec![0, 3]), (9, vec![0]), (43, vec![0, 1, 2, 3])];
    for ((s, t), (vk, pack)) in specs.iter().zip(&idx.entries[0].textures).zip(want) {
        assert_eq!((t.rec.status, t.rec.kind, t.rec.rule, t.rec.vk_format), (Status::Ok, Kind::Raw, 2, vk), "{}", s.name);
        assert!(t.rec.psnr == 99.0 && t.rec.psnr_first < 99.5);
        let raw = raw_of(&out, t);
        let (_, bw, bhp, _) = bntx::fmt(s.fmt >> 8).unwrap();
        for m in 0..s.mips as usize {
            let (mo, lb) = t.mips[m];
            let (w, h) = ((s.w >> m).max(1), (s.h >> m).max(1));
            for l in 0..s.layers as usize {
                let mut src = native::astc_decode(&s.lin[m * s.layers as usize + l], w, h, bw, bhp).unwrap();
                let want: Vec<u8> = if s.name == "AlbHard" {
                    // (the swizzle reads R, G, B: the unused alpha is forced to 255)
                    src.chunks_exact_mut(4).for_each(|p| p[3] = 255);
                    src
                } else {
                    src.chunks_exact(4).flat_map(|p| pack.iter().map(move |&c| p[c])).collect()
                };
                assert_eq!(lb as usize, want.len());
                assert!(raw[(mo + l as u64 * lb as u64) as usize..][..lb as usize] == want[..], "{} mip {m} layer {l}", s.name);
            }
        }
    }
    let _ = std::fs::remove_dir_all(&d);
}

/// BC7 at the store's level keeps alpha 0 at 0 under colour that varies (with the alpha weighted as the colour, the
/// encoder gave some of these texels an alpha above 0: lines over a clear area)
#[test]
fn bc7_keeps_clear_texels_clear() {
    let mut rng = Rng(7);
    let noise: Vec<u8> = (0..64 * 64).flat_map(|_| [rng.next() as u8, rng.next() as u8, rng.next() as u8, 0]).collect();
    let stripes: Vec<u8> = (0..64u32 * 64)
        .flat_map(|i| {
            let (x, y) = (i % 64, i / 64);
            [((x % 3) * 120) as u8, ((x % 5) * 60) as u8, ((y % 2) * 200) as u8, 0]
        })
        .collect();
    for (name, rgba) in [("noise", noise), ("stripes", stripes)] {
        let q = Settings::QUALITY;
        let dec = native::decode(Enc::Bc7, 64, 64, &native::encode(Enc::Bc7, 64, 64, &rgba, 0, 1, q.bc7_level, q.bc45_radius));
        let left = dec.chunks_exact(4).filter(|p| p[3] != 0).count();
        assert_eq!(left, 0, "BC7, {name} under alpha 0: {left} texels decode with an alpha above 0");
    }
}

/// bands of block rows give the same blocks as one call over the whole image (bc7e's batches of 64 blocks then start
/// elsewhere)
#[test]
fn bands_match_whole() {
    let mut rng = Rng(3);
    let (w, h) = (1030u32, 300u32);
    let rgba = rng.bytes((w * h * 4) as usize);
    for e in [Enc::Bc4, Enc::Bc5, Enc::Bc7] {
        let banded = native::encode(e, w, h, &rgba, 0, 1, 1, 1);
        assert!(banded == native::encode_whole(e, w, h, &rgba, 0, 1, 1, 1), "{e:?}");
    }
}

extern "C" {
    fn hk_bc4_rgbcx(out: *mut u8, px16: *const u8, radius: i32);
    fn hk_bc4_fast(out: *mut u8, px16: *const u8, radius: i32);
}

/// the vectorized BC4 search gives rgbcx's blocks
#[test]
fn bc4_search_matches_rgbcx() {
    let mut rng = Rng(11);
    for n in 0..200_000u32 {
        let mut px = [0u8; 16];
        let (base, spread) = ((rng.next() % 256) as i32, [0, 1, 3, 8, 40, 255][n as usize % 6]);
        for p in px.iter_mut() {
            *p = (base + (rng.next() % (spread as u64 + 1)) as i32 - spread / 2).clamp(0, 255) as u8;
        }
        let r = 1 + (n % 5) as i32;
        let (mut a, mut b) = ([0u8; 8], [0u8; 8]);
        // SAFETY: 16 texels in, 8 bytes out
        unsafe {
            hk_bc4_rgbcx(a.as_mut_ptr(), px.as_ptr(), r);
            hk_bc4_fast(b.as_mut_ptr(), px.as_ptr(), r);
        }
        assert_eq!(a, b, "texels {px:?} radius {r}");
    }
}
