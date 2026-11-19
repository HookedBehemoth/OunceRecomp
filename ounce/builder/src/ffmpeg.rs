//! `ounce tool ffmpeg [<dir>] [-j N]`: the FFmpeg the game links statically, for the movies' decoders
//! (runtime/nn_video.c): libavcodec with FFmpeg's VP9 decoder (DKB's and MKW's movies) and dav1d for AV1 (OoT's; FFmpeg's
//! own AV1 decoder needs a hardware decoder), libswscale for the RGBA reads and libavutil. The games demux their WebM
//! movies themselves and their sound is a separate stream, so there is no libavformat, no parser beyond VP9's and no
//! audio. Both sources are pinned release tarballs, fetched once and checked by SHA-256; they are built unmodified with
//! clang at the host C's target flags against the build's sysroot (sysroot.rs), static and PIC, with nothing
//! autodetected (no library but the sysroot's libc, libm and pthread).
//!
//! The result is `<dir>/<key>/`: `include/`, `lib/` (the four archives), `licenses/` and `ffmpeg.txt` (the sources,
//! their hashes and the configure lines: what the LGPL asks to say about a library linked in). `<key>` names the
//! recipe, so a changed recipe builds beside the old one. The default `<dir>` is the cache folder's `ffmpeg/`, shared
//! by every checkout; a release ships the folder as `prebuilt/ffmpeg/`. About a minute on 16 threads.

use crate::fail;
use crate::fetch::{fetch, run, Pinned};
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

struct Tarball {
    name: &'static str,
    src: Pinned,
}

const FFMPEG: Tarball = Tarball {
    name: "ffmpeg-9.0.2",
    src: Pinned { file: "ffmpeg-9.0.2.tar.xz", url: "https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz",
    sha256: "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e" },
};
const DAV1D: Tarball = Tarball {
    name: "dav1d-1.5.4",
    src: Pinned { file: "dav1d-1.5.4.tar.xz", url: "https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz",
    sha256: "686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5" },
};

/// dav1d's Meson options: the library alone, 8-bit (every AV1 movie is 8-bit 4:2:0)
const DAV1D_OPTIONS: &[&str] = &[
    "--buildtype=release", "--default-library=static", "-Db_staticpic=true", "-Dbitdepths=8", "-Denable_tools=false",
    "-Denable_examples=false", "-Denable_tests=false", "-Denable_docs=false", "-Dlogging=false",
];

/// FFmpeg's configure options: everything off, then the two decoders (VP9 brings its parser and superframe filter)
const CONFIGURE: &[&str] = &[
    "--cc=clang", "--enable-pic", "--enable-static", "--disable-shared", "--disable-autodetect", "--disable-iconv",
    "--disable-everything", "--disable-programs", "--disable-doc", "--disable-network", "--disable-avdevice",
    "--disable-avformat", "--disable-avfilter", "--disable-swresample", "--disable-debug", "--enable-libdav1d",
    "--enable-decoder=vp9,libdav1d",
];

/// The archives, in the link's order
pub const LIBS: [&str; 4] = ["libavcodec.a", "libdav1d.a", "libswscale.a", "libavutil.a"];

/// A release's FFmpeg, built with it (the AppImage's), under its sources' root
pub const PREBUILT: &str = "prebuilt/ffmpeg";

/// The C flags of both builds: the host C's target (the CPU), sections for --gc-sections; the sysroot (sysroot.rs)
/// is given apart, as its path differs from PC to PC
fn cflags() -> String {
    let mut f = crate::host::target();
    f.extend(["-ffunction-sections", "-fdata-sections"].map(String::from));
    f.join(" ")
}

/// ffmpeg.txt: what was built from what, and how. It is also the folder's stamp.
fn recipe() -> String {
    let mut t = String::from("FFmpeg as linked into the game (ounce tool ffmpeg), from unmodified upstream sources:\n");
    for s in [&FFMPEG, &DAV1D] {
        t += &format!("{} {} sha256 {}\n", s.name, s.src.url, s.src.sha256);
    }
    t += &format!("sysroot: {} (ounce tool sysroot)\nCFLAGS: {}\ndav1d: meson setup {}\nFFmpeg: configure {}\n", crate::sysroot::key(), cflags(), DAV1D_OPTIONS.join(" "), CONFIGURE.join(" "));
    t += "FFmpeg is LGPL 2.1 or later and dav1d BSD 2-clause (licenses/). The game is linked from its objects on the PC \
          that builds it, so it can be linked again with another build of these libraries.\n";
    t
}

pub fn key() -> String {
    let h = Sha256::digest(recipe().as_bytes());
    format!("{}-{:02x}{:02x}{:02x}{:02x}", FFMPEG.name, h[0], h[1], h[2], h[3])
}

/// The folder holds a whole build of this recipe
fn complete(out: &Path) -> bool {
    std::fs::read_to_string(out.join("ffmpeg.txt")).is_ok_and(|t| t == recipe()) && LIBS.iter().all(|l| out.join("lib").join(l).is_file())
}

/// The folder of this recipe's build under `dir`, built first unless it is there
pub fn ensure(dir: &Path, jobs: usize, sysroot: &Path) -> PathBuf {
    let out = dir.join(key());
    if complete(&out) {
        return out;
    }
    let t = std::time::Instant::now();
    for (tool, arg) in [("clang", "--version"), ("meson", "--version"), ("ninja", "--version"), ("nasm", "-v"), ("make", "--version"), ("pkg-config", "--version"), ("tar", "--version")] {
        if Command::new(tool).arg(arg).stdout(Stdio::null()).stderr(Stdio::null()).status().is_err() {
            fail(format!("{tool} is not installed: FFmpeg's build (ounce tool ffmpeg) needs it"));
        }
    }
    // (a fresh tree per build, so nothing of an interrupted or older one is left in it)
    let work = dir.join(format!("build-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work).unwrap_or_else(|e| fail(format!("{}: {e}", work.display())));
    let log = work.join("build.log");
    println!("ffmpeg: building {} (the log is {})", out.display(), log.display());
    let stage = work.join("out");
    for t in [&FFMPEG, &DAV1D] {
        let file = fetch(dir, &t.src, &log);
        let mut c = Command::new("tar");
        c.arg("-xf").arg(&file).arg("-C").arg(&work);
        run(c, &format!("the unpacking of {}", t.name), &log);
    }
    let (ff_src, dav_src) = (work.join(FFMPEG.name), work.join(DAV1D.name));
    let mut c = Command::new("meson");
    c.arg("setup").arg(work.join("dav1d-build")).arg(&dav_src).arg(format!("--prefix={}", stage.display())).arg("--libdir=lib");
    let sr = format!("--sysroot={}", sysroot.display());
    c.args(DAV1D_OPTIONS).env("CC", "clang").env("CFLAGS", format!("{} {sr}", cflags())).env("LDFLAGS", &sr);
    run(c, "dav1d's meson setup", &log);
    let mut c = Command::new("ninja");
    c.arg("-C").arg(work.join("dav1d-build")).arg("-j").arg(jobs.to_string()).arg("install");
    run(c, "dav1d's build", &log);
    let ff_build = work.join("ffmpeg-build");
    std::fs::create_dir_all(&ff_build).unwrap_or_else(|e| fail(e));
    let mut c = Command::new(ff_src.join("configure"));
    c.current_dir(&ff_build).arg(format!("--prefix={}", stage.display())).args(CONFIGURE).arg(format!("--extra-cflags={} {sr}", cflags())).arg(format!("--extra-ldflags={sr}"));
    // (only the dav1d just built: no system library is found)
    c.env("PKG_CONFIG_LIBDIR", stage.join("lib/pkgconfig")).env_remove("PKG_CONFIG_PATH");
    run(c, "FFmpeg's configure", &log);
    let mut c = Command::new("make");
    c.current_dir(&ff_build).arg(format!("-j{jobs}")).arg("install");
    run(c, "FFmpeg's build", &log);
    // (what ships: the headers, the archives, the licenses and the recipe; pkg-config's files name this tree)
    let _ = std::fs::remove_dir_all(stage.join("lib/pkgconfig"));
    let _ = std::fs::remove_dir_all(stage.join("share"));
    let lic = stage.join("licenses");
    std::fs::create_dir_all(&lic).unwrap_or_else(|e| fail(e));
    for (from, to) in [(ff_src.join("COPYING.LGPLv2.1"), "FFmpeg-COPYING.LGPLv2.1"), (ff_src.join("LICENSE.md"), "FFmpeg-LICENSE.md"), (dav_src.join("COPYING"), "dav1d-COPYING")] {
        std::fs::copy(&from, lic.join(to)).unwrap_or_else(|e| fail(format!("{}: {e}", from.display())));
    }
    std::fs::write(stage.join("ffmpeg.txt"), recipe()).unwrap_or_else(|e| fail(e));
    if !complete(&stage) {
        fail(format!("FFmpeg's build made no {}", LIBS.join(", ")));
    }
    let _ = std::fs::remove_dir_all(&out);
    std::fs::rename(&stage, &out).unwrap_or_else(|e| fail(format!("{}: {e}", out.display())));
    let _ = std::fs::remove_dir_all(&work);
    let size: u64 = LIBS.iter().map(|l| std::fs::metadata(out.join("lib").join(l)).map_or(0, |m| m.len())).sum();
    println!("ffmpeg: {} ({:.1} MB of archives) in {:.0} s", out.display(), size as f64 / 1e6, t.elapsed().as_secs_f64());
    out
}

/// The FFmpeg a build links: a release's prebuilt one under `root`, else this recipe's in the cache folder (built
/// the first time)
pub fn find(root: &Path, cache_dir: &Path, jobs: usize, sysroot: &Path) -> PathBuf {
    let prebuilt = root.join(PREBUILT);
    if prebuilt.join("ffmpeg.txt").is_file() {
        if !complete(&prebuilt) {
            fail(format!("{} is not this ounce's FFmpeg (ounce tool ffmpeg makes it)", prebuilt.display()));
        }
        return prebuilt;
    }
    ensure(&cache_dir.join("ffmpeg"), jobs, sysroot)
}

pub fn command(args: &[String]) {
    let (mut dir, mut jobs) = (None::<PathBuf>, std::thread::available_parallelism().map_or(1, |n| n.get()));
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "-j" => jobs = it.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| fail("-j N")),
            _ if dir.is_none() && !a.starts_with('-') => dir = Some(a.into()),
            _ => fail("tool ffmpeg [<dir>] [-j N]"),
        }
    }
    let config = crate::config();
    let dir = dir.unwrap_or_else(|| config.cache_dir().join("ffmpeg"));
    let sysroot = crate::sysroot::find(&ounce_core::games::root().unwrap_or_else(|e| fail(e)), &config.cache_dir());
    println!("ffmpeg: {}", ensure(&dir, jobs, &sysroot).display());
}
