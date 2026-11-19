//! `ounce tool sdl [<dir>]`: the SDL3 a release puts beside the game (runtime/media/hos_sdl.c loads
//! `libSDL3.so.0` from beside the executable before the system's), so a player's PC needs no SDL of its own.
//!
//! SDL 3.4.20, the pinned release tarball checked by SHA-256, built unmodified by clang against the build's sysroot
//! (sysroot.rs). SDL loads its back ends at run time (X11, Wayland, PipeWire, PulseAudio, ALSA, udev, D-Bus), so it
//! needs their headers to build and nothing of them to link: the headers come from this PC's `/usr/include` after the
//! sysroot's (`-idirafter`), and `--no-undefined` makes a function that the sysroot's glibc lacks a failed build.
//!
//! The result is `<dir>/<key>/`: `libSDL3.so.0`, `LICENSE.txt` (zlib) and `sdl.txt`, the recipe and its stamp. The
//! default `<dir>` is the cache folder's `sdl/`; a release ships it as `prebuilt/sdl`, and `ounce build` copies the
//! library into the game folder.

use crate::fail;
use crate::fetch::{fetch, run, Pinned};
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::Command;

const SDL: Pinned = Pinned {
    file: "SDL3-3.4.20.tar.gz",
    url: "https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz",
    sha256: "f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f",
};

/// The library's name: what the game loads beside itself
pub const LIB: &str = "libSDL3.so.0";

/// A release's SDL under its sources' root (the AppImage's)
pub const PREBUILT: &str = "prebuilt/sdl";

const OPTIONS: &[&str] = &[
    "CMAKE_BUILD_TYPE=Release", "CMAKE_C_COMPILER=clang", "CMAKE_CXX_COMPILER=clang++", "SDL_SHARED=ON", "SDL_STATIC=OFF",
    "SDL_TEST_LIBRARY=OFF", "SDL_TESTS=OFF", "SDL_EXAMPLES=OFF", "SDL_DEPS_SHARED=ON", "SDL_INSTALL=OFF", "SDL_RPATH=OFF",
];

fn recipe() -> String {
    format!(
        "SDL as the game loads it (ounce tool sdl), unmodified: {} sha256 {}\nsysroot: {}\ncmake: {}\nflags: -march=x86-64-v3 --sysroot -idirafter /usr/include; link --no-undefined\n",
        SDL.url,
        SDL.sha256,
        crate::sysroot::key(),
        OPTIONS.join(" ")
    )
}

fn key() -> String {
    let h = Sha256::digest(recipe().as_bytes());
    format!("SDL3-3.4.20-{:02x}{:02x}{:02x}{:02x}", h[0], h[1], h[2], h[3])
}

fn complete(dir: &Path) -> bool {
    std::fs::read_to_string(dir.join("sdl.txt")).is_ok_and(|t| t == recipe()) && dir.join(LIB).is_file()
}

/// The folder of this recipe's build under `dir`, built first unless it is there
pub fn ensure(dir: &Path, jobs: usize, sysroot: &Path) -> PathBuf {
    let out = dir.join(key());
    if complete(&out) {
        return out;
    }
    let t = std::time::Instant::now();
    let work = dir.join(format!("build-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work).unwrap_or_else(|e| fail(format!("{}: {e}", work.display())));
    let log = work.join("build.log");
    println!("sdl: building {} (the log is {})", out.display(), log.display());
    let tar = fetch(dir, &SDL, &log);
    let mut c = Command::new("tar");
    c.arg("-xf").arg(&tar).arg("-C").arg(&work);
    run(c, "the unpacking of SDL", &log);
    let (src, build) = (work.join(SDL.file.trim_end_matches(".tar.gz")), work.join("build"));
    let sr = format!("--sysroot={}", sysroot.display());
    let mut c = Command::new("cmake");
    c.args(["-G", "Ninja", "-S"]).arg(&src).arg("-B").arg(&build).args(OPTIONS.iter().map(|o| format!("-D{o}")));
    c.arg(format!("-DCMAKE_C_FLAGS=-march=x86-64-v3 {sr} -idirafter /usr/include"));
    c.arg(format!("-DCMAKE_SHARED_LINKER_FLAGS={sr} -fuse-ld=lld -Wl,--no-undefined"));
    run(c, "SDL's configure", &log);
    let mut c = Command::new("ninja");
    c.arg("-C").arg(&build).arg("-j").arg(jobs.to_string()).arg("SDL3-shared");
    run(c, "SDL's build", &log);
    let stage = work.join("out");
    std::fs::create_dir_all(&stage).unwrap_or_else(|e| fail(e));
    // (the library itself: libSDL3.so.0 is a link to libSDL3.so.0.<version>)
    let built = std::fs::canonicalize(build.join(LIB)).unwrap_or_else(|e| fail(format!("{}: {e}", build.join(LIB).display())));
    std::fs::copy(&built, stage.join(LIB)).unwrap_or_else(|e| fail(e));
    std::fs::copy(src.join("LICENSE.txt"), stage.join("LICENSE.txt")).unwrap_or_else(|e| fail(e));
    crate::elf::check(&stage.join(LIB)).unwrap_or_else(|e| fail(e));
    std::fs::write(stage.join("sdl.txt"), recipe()).unwrap_or_else(|e| fail(e));
    let _ = std::fs::remove_dir_all(&out);
    std::fs::rename(&stage, &out).unwrap_or_else(|e| fail(format!("{}: {e}", out.display())));
    let _ = std::fs::remove_dir_all(&work);
    println!("sdl: {} ({:.1} MB) in {:.0} s", out.display(), std::fs::metadata(out.join(LIB)).map_or(0, |m| m.len()) as f64 / 1e6, t.elapsed().as_secs_f64());
    out
}

/// A release's SDL under `root`, None in a checkout (the developer's PC has its own)
pub fn prebuilt(root: &Path) -> Option<PathBuf> {
    let p = root.join(PREBUILT);
    if !p.join("sdl.txt").is_file() {
        return None;
    }
    if !complete(&p) {
        fail(format!("{} is not this ounce's SDL (ounce tool sdl makes it)", p.display()));
    }
    Some(p.join(LIB))
}

pub fn command(args: &[String]) {
    let (mut dir, mut jobs) = (None::<PathBuf>, std::thread::available_parallelism().map_or(1, |n| n.get()));
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "-j" => jobs = it.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| fail("-j N")),
            _ if dir.is_none() && !a.starts_with('-') => dir = Some(a.into()),
            _ => fail("tool sdl [<dir>] [-j N]"),
        }
    }
    let config = crate::config();
    let dir = dir.unwrap_or_else(|| config.cache_dir().join("sdl"));
    let sysroot = crate::sysroot::find(&ounce_core::games::root().unwrap_or_else(|e| fail(e)), &config.cache_dir());
    println!("sdl: {}", ensure(&dir, jobs, &sysroot).display());
}
