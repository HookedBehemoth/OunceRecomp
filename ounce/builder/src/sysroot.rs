//! `ounce tool sysroot [<dir>]`: the sysroot every C compile and link of a build is made against, so the game,
//! the renderer and ounce itself run on any Linux with glibc 2.35 or later, whatever the building PC has.
//!
//! It is Ubuntu 22.04's (jammy's) glibc 2.35, Linux 5.15 headers and GCC 12's start files, libgcc and libstdc++,
//! taken unmodified from the release pocket's packages, which the archive keeps for as long as the release exists.
//! Each package is pinned by its SHA-256 (from the release's signed package index). Only the parts a link or a
//! compile reads are kept: the headers, the start files, the libraries' link names, the shared libraries they name,
//! libgcc and libstdc++. The packages' absolute symbolic links are made relative, so nothing points out of the
//! folder; the linker scripts (libc.so, libm.so) keep their absolute paths, which clang and lld read inside
//! `--sysroot`.
//!
//! The result is `<dir>/<key>/`, with `sysroot.txt` (the packages and their hashes) as its stamp; the default
//! `<dir>` is the cache folder's `sysroot/`, shared by every checkout. A release ships it as `prebuilt/sysroot`.
//! The packages' glibc has known security bugs, which do not matter here: the game is linked against it, and runs
//! with the player's own glibc.

use crate::fail;
use crate::fetch::{fetch, run, Pinned};
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::Command;

/// The oldest glibc the game, its renderer and ounce run on
pub const GLIBC: (u32, u32) = (2, 35);

/// A release's sysroot under its sources' root (the AppImage's)
pub const PREBUILT: &str = "prebuilt/sysroot";

/// GCC's folder inside the sysroot: crtbeginS.o, crtendS.o, libgcc.a, libstdc++.a
pub const GCC: &str = "usr/lib/gcc/x86_64-linux-gnu/12";
/// glibc's: the start files, the link names
pub const LIB: &str = "usr/lib/x86_64-linux-gnu";

macro_rules! deb {
    ($path:literal, $file:literal, $sha:literal) => {
        Pinned { file: $file, url: concat!("http://archive.ubuntu.com/ubuntu/pool/", $path, $file), sha256: $sha }
    };
}

const PACKAGES: &[Pinned] = &[
    deb!("main/g/glibc/", "libc6_2.35-0ubuntu3_amd64.deb", "ea9a27e0ebdd0cfc9c750d94f8074f3a35d1f97dcc77ae04c370fb498a6b6db2"),
    deb!("main/g/glibc/", "libc6-dev_2.35-0ubuntu3_amd64.deb", "cc37cab5c60bcfe4bbf289a8002f369949a41ed46e8b51a0503a001099370c56"),
    deb!("main/l/linux/", "linux-libc-dev_5.15.0-25.25_amd64.deb", "1b2a93020593c9e94a25f750ce442da5a6e8ff48a20f52cec92dfc3fa35336d8"),
    deb!("main/g/gcc-12/", "libgcc-12-dev_12-20220319-1ubuntu1_amd64.deb", "a83374135068a144b49e9a3e518c6efc3a8f566b67a63610086318a17b299ccc"),
    deb!("main/g/gcc-12/", "libgcc-s1_12-20220319-1ubuntu1_amd64.deb", "c9a93721431be07557c810d7e1b24c3b3c51f398334ded104362a68a415b9e61"),
    deb!("universe/g/gcc-12/", "libstdc++-12-dev_12-20220319-1ubuntu1_amd64.deb", "27767620a793cd97f58580e795592aa8f49122116f74b45fa8a5fc9ad62eb07e"),
    deb!("main/g/gcc-12/", "libstdc++6_12-20220319-1ubuntu1_amd64.deb", "035cfe195ddf3a091abbc0c323285772ccbdbb2fb75279e44a0915748b4e5726"),
];

/// What is left out of the packages: documentation, configuration, the sanitizers' and other runtimes a build
/// never links, glibc's static archive and its plug-ins
const DROP: &[&str] = &[
    "etc", "usr/share", "usr/bin", "usr/sbin", "sbin", "usr/libexec", "usr/lib/x86_64-linux-gnu/gconv", "usr/lib/x86_64-linux-gnu/audit",
    "usr/lib/x86_64-linux-gnu/pkgconfig", "usr/lib/x86_64-linux-gnu/libc.a", "usr/lib/x86_64-linux-gnu/libm-2.35.a",
    "usr/lib/x86_64-linux-gnu/libmvec.a", "usr/lib/x86_64-linux-gnu/libm.a", "usr/lib/x86_64-linux-gnu/libresolv.a",
];
/// GCC's runtimes no build links (in GCC's folder, and their shared libraries' link names)
const DROP_GCC: &[&str] = &["libasan", "libtsan", "liblsan", "libubsan", "libitm", "libgomp", "libquadmath", "libatomic", "libgcov", "libbacktrace"];

/// sysroot.txt: what the folder is made of. It is also the folder's stamp.
fn recipe() -> String {
    let mut t = format!("The sysroot of ounce's builds (ounce tool sysroot): glibc {}.{}, Ubuntu 22.04's packages, unmodified:\n", GLIBC.0, GLIBC.1);
    for p in PACKAGES {
        t += &format!("{} sha256 {}\n", p.url, p.sha256);
    }
    t += &format!("left out: {}; GCC's {}\n", DROP.join(" "), DROP_GCC.join(" "));
    t
}

/// The recipe's name: the folder a build of it is in, and what an FFmpeg or object built against it names
pub fn key() -> String {
    let h = Sha256::digest(recipe().as_bytes());
    format!("glibc-{}.{}-{:02x}{:02x}{:02x}{:02x}", GLIBC.0, GLIBC.1, h[0], h[1], h[2], h[3])
}

fn complete(dir: &Path) -> bool {
    std::fs::read_to_string(dir.join("sysroot.txt")).is_ok_and(|t| t == recipe())
        && [format!("{LIB}/Scrt1.o"), format!("{GCC}/crtbeginS.o"), "usr/include/stdio.h".into(), "usr/include/linux/udmabuf.h".into()]
            .iter()
            .all(|f| dir.join(f).is_file())
}

/// A Debian package's data member (an ar archive's `data.tar.*`) written to `to`
fn deb_data(deb: &Path, to: &Path) -> Result<PathBuf, String> {
    let d = std::fs::read(deb).map_err(|e| format!("{}: {e}", deb.display()))?;
    if !d.starts_with(b"!<arch>\n") {
        return Err(format!("{} is not an ar archive", deb.display()));
    }
    let mut at = 8;
    while at + 60 <= d.len() {
        let h = &d[at..at + 60];
        let name = String::from_utf8_lossy(&h[..16]).trim().trim_end_matches('/').to_string();
        let size: usize = String::from_utf8_lossy(&h[48..58]).trim().parse().map_err(|_| format!("{}: a bad member header", deb.display()))?;
        let body = d.get(at + 60..at + 60 + size).ok_or_else(|| format!("{}: cut short", deb.display()))?;
        if let Some(ext) = name.strip_prefix("data.tar") {
            let out = to.join(format!("data.tar{ext}"));
            std::fs::write(&out, body).map_err(|e| format!("{}: {e}", out.display()))?;
            return Ok(out);
        }
        at += 60 + size + (size & 1);
    }
    Err(format!("{} has no data.tar member", deb.display()))
}

/// Every absolute symbolic link under `root` made relative to its own folder, inside `root`
fn relative_links(root: &Path, dir: &Path) -> Result<(), String> {
    for e in std::fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))?.flatten() {
        let p = e.path();
        let ft = e.file_type().map_err(|e| e.to_string())?;
        if ft.is_dir() {
            relative_links(root, &p)?;
        } else if ft.is_symlink() {
            let target = std::fs::read_link(&p).map_err(|e| e.to_string())?;
            if target.is_absolute() {
                let inside = root.join(target.strip_prefix("/").unwrap());
                let from = p.parent().unwrap().strip_prefix(root).unwrap();
                let up: PathBuf = from.components().map(|_| "..").collect();
                let rel = up.join(inside.strip_prefix(root).unwrap());
                std::fs::remove_file(&p).map_err(|e| e.to_string())?;
                std::os::unix::fs::symlink(&rel, &p).map_err(|e| format!("{}: {e}", p.display()))?;
            }
        }
    }
    Ok(())
}

/// The folder of this recipe's sysroot under `dir`, made first unless it is there
pub fn ensure(dir: &Path) -> PathBuf {
    let out = dir.join(key());
    if complete(&out) {
        return out;
    }
    let t = std::time::Instant::now();
    let work = dir.join(format!("make-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&work);
    let stage = work.join("root");
    std::fs::create_dir_all(&stage).unwrap_or_else(|e| fail(format!("{}: {e}", stage.display())));
    let log = work.join("sysroot.log");
    println!("sysroot: making {} from {} packages", out.display(), PACKAGES.len());
    for p in PACKAGES {
        let deb = fetch(dir, p, &log);
        let data = deb_data(&deb, &work).unwrap_or_else(|e| fail(e));
        let mut c = Command::new("tar");
        c.arg("-xf").arg(&data).arg("-C").arg(&stage);
        run(c, &format!("the unpacking of {}", p.file), &log);
        let _ = std::fs::remove_file(&data);
    }
    for d in DROP {
        let p = stage.join(d);
        let _ = if p.is_dir() { std::fs::remove_dir_all(&p) } else { std::fs::remove_file(&p) };
    }
    for dir in [stage.join(GCC), stage.join(LIB)] {
        for e in std::fs::read_dir(&dir).into_iter().flatten().flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if DROP_GCC.iter().any(|d| n.starts_with(&format!("{d}.")) || n.starts_with(&format!("{d}_"))) {
                let _ = std::fs::remove_file(e.path());
            }
        }
    }
    relative_links(&stage, &stage).unwrap_or_else(|e| fail(e));
    std::fs::write(stage.join("sysroot.txt"), recipe()).unwrap_or_else(|e| fail(e));
    if !complete(&stage) {
        fail(format!("the packages made no whole sysroot (the log is {})", log.display()));
    }
    let _ = std::fs::remove_dir_all(&out);
    std::fs::rename(&stage, &out).unwrap_or_else(|e| fail(format!("{}: {e}", out.display())));
    let _ = std::fs::remove_dir_all(&work);
    println!("sysroot: {} ({:.1} MB) in {:.0} s", out.display(), size(&out) as f64 / 1e6, t.elapsed().as_secs_f64());
    out
}

/// The bytes of the files under `dir` (links not followed)
pub fn size(dir: &Path) -> u64 {
    std::fs::read_dir(dir).into_iter().flatten().flatten().map(|e| match e.file_type() {
        Ok(t) if t.is_dir() => size(&e.path()),
        Ok(t) if t.is_file() => e.metadata().map_or(0, |m| m.len()),
        _ => 0,
    }).sum()
}

/// The sysroot a build uses: a release's prebuilt one under `root`, else this recipe's in the cache folder (made the
/// first time)
pub fn find(root: &Path, cache_dir: &Path) -> PathBuf {
    let prebuilt = root.join(PREBUILT);
    if prebuilt.join("sysroot.txt").is_file() {
        if !complete(&prebuilt) {
            fail(format!("{} is not this ounce's sysroot (ounce tool sysroot makes it)", prebuilt.display()));
        }
        return prebuilt;
    }
    ensure(&cache_dir.join("sysroot"))
}

pub fn command(args: &[String]) {
    let dir = match args {
        [] => crate::config().cache_dir().join("sysroot"),
        [d] if !d.starts_with('-') => PathBuf::from(d),
        _ => fail("tool sysroot [<dir>]"),
    };
    println!("sysroot: {}", ensure(&dir).display());
}
