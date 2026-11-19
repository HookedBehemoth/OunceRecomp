//! The Windows build's toolchain, in the place the Linux build's sysroot (sysroot.rs), FFmpeg (ffmpeg.rs) and SDL
//! (sdl.rs) take there:
//!
//! - the C runtime and the system's headers and libraries are MSVC's and the Windows SDK's, found through the Visual
//!   Studio developer environment (VCToolsInstallDir, WindowsSdkDir, WindowsSDKVersion: `vcvars64.bat`, or the
//!   variables set by hand); the game links the static C runtime (libcmt), so it needs no redistributable;
//! - FFmpeg is a pinned LGPL build of FFmpeg 9.0.2 with dav1d (BtbN's), linked through its import libraries, its DLLs
//!   installed beside the game; `OUNCE_FFMPEG=<folder>` takes an unpacked one instead (bin/, include/, lib/);
//! - SDL is SDL 3.4.20's official Windows build (the version sdl.rs builds on Linux), SDL3.dll beside the game.
//!
//! Each download is fetched once into the cache folder and checked by its SHA-256 (fetch.rs), and unpacked with the
//! system's tar (bsdtar, which reads zip files).

use crate::fail;
use crate::fetch::{fetch, run, Pinned};
use std::path::{Path, PathBuf};
use std::process::Command;

/// The target every host compile and the lifted code are built for on Windows
pub const TRIPLE: &str = "x86_64-pc-windows-msvc";

const FFMPEG: Pinned = Pinned {
    file: "ffmpeg-n9.0.2-57-gd838f9f9ae-win64-lgpl-shared-9.0.zip",
    url: "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-11-17-12-40/ffmpeg-n9.0.2-57-gd838f9f9ae-win64-lgpl-shared-9.0.zip",
    sha256: "8388f846a8b56a9316ad8f24d9eedf186097a59555816b7b222de392611b0279",
};
/// the import libraries the game links, and the DLLs it then loads (avcodec's dav1d is inside avcodec-63.dll)
pub const FFMPEG_LIBS: [&str; 3] = ["avcodec.lib", "swscale.lib", "avutil.lib"];
pub const FFMPEG_DLLS: [&str; 4] = ["avcodec-63.dll", "swscale-10.dll", "avutil-61.dll", "swresample-7.dll"];

const SDL: Pinned = Pinned {
    file: "SDL3-devel-3.4.20-VC.zip",
    url: "https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-devel-3.4.20-VC.zip",
    sha256: "1ec3009655ee37b1f73e52b3b120acc5afd8abaf9532f3303b1bf1dfe91b5969",
};
pub const SDL_DLL: &str = "SDL3.dll";

/// MSVC's and the Windows SDK's folders
pub struct Msvc {
    /// the compiler's include folders (-imsvc each)
    pub include: Vec<PathBuf>,
    /// the linker's library folders (/LIBPATH each)
    pub lib: Vec<PathBuf>,
}

fn env_dir(name: &str) -> Option<PathBuf> {
    std::env::var_os(name).map(PathBuf::from).filter(|p| p.is_dir())
}

/// MSVC and the SDK from the developer environment's variables
pub fn msvc() -> Msvc {
    let why = "run ounce from a Visual Studio developer prompt (x64), or call vcvars64.bat first: VCToolsInstallDir, \
               WindowsSdkDir and WindowsSDKVersion name MSVC and the Windows SDK";
    let vc = env_dir("VCToolsInstallDir").unwrap_or_else(|| fail(format!("no MSVC: {why}")));
    let sdk = env_dir("WindowsSdkDir").unwrap_or_else(|| fail(format!("no Windows SDK: {why}")));
    let ver = std::env::var("WindowsSDKVersion").unwrap_or_default().trim_end_matches(['\\', '/']).to_string();
    if ver.is_empty() {
        fail(format!("no Windows SDK version: {why}"));
    }
    let inc = sdk.join("Include").join(&ver);
    let lib = sdk.join("Lib").join(&ver);
    let m = Msvc {
        include: vec![vc.join("include"), inc.join("ucrt"), inc.join("um"), inc.join("shared")],
        lib: vec![vc.join("lib/x64"), lib.join("ucrt/x64"), lib.join("um/x64")],
    };
    if let Some(missing) = m.include.iter().chain(&m.lib).find(|d| !d.is_dir()) {
        fail(format!("{} is missing (MSVC's x64 tools and the Windows SDK must be installed): {why}", missing.display()));
    }
    m
}

/// A pinned zip unpacked once into `dir/<name>` (its one top folder's contents); the folder
fn unpacked(dir: &Path, p: &Pinned, name: &str) -> PathBuf {
    let out = dir.join(name);
    let stamp = out.join("ounce-unpacked.txt");
    if std::fs::read_to_string(&stamp).is_ok_and(|t| t.trim() == p.sha256) {
        return out;
    }
    std::fs::create_dir_all(dir).unwrap_or_else(|e| fail(format!("{}: {e}", dir.display())));
    let log = dir.join(format!("{name}.log"));
    let zip = fetch(dir, p, &log);
    let work = dir.join(format!("unpack-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work).unwrap_or_else(|e| fail(format!("{}: {e}", work.display())));
    let mut c = Command::new("tar");
    c.arg("-xf").arg(&zip).arg("-C").arg(&work);
    run(c, &format!("the unpacking of {}", p.file), &log);
    let tops: Vec<PathBuf> = std::fs::read_dir(&work).into_iter().flatten().flatten().map(|e| e.path()).collect();
    let [top] = &tops[..] else { fail(format!("{}: not one folder at its top", p.file)) };
    let _ = std::fs::remove_dir_all(&out);
    std::fs::rename(top, &out).unwrap_or_else(|e| fail(format!("{}: {e}", out.display())));
    let _ = std::fs::remove_dir_all(&work);
    std::fs::write(&stamp, format!("{}\n", p.sha256)).unwrap_or_else(|e| fail(format!("{}: {e}", stamp.display())));
    out
}

/// The FFmpeg a Windows build links: OUNCE_FFMPEG's folder, else the pinned build in the cache folder
pub fn ffmpeg(cache_dir: &Path) -> PathBuf {
    let dir = match std::env::var_os("OUNCE_FFMPEG") {
        Some(d) => PathBuf::from(d),
        None => unpacked(&cache_dir.join("ffmpeg"), &FFMPEG, "win64-9.0.2"),
    };
    for f in FFMPEG_LIBS.iter().map(|l| dir.join("lib").join(l)).chain(FFMPEG_DLLS.iter().map(|d| dir.join("bin").join(d))) {
        if !f.is_file() {
            fail(format!("{} is missing: not FFmpeg 9.0.2's Windows build (OUNCE_FFMPEG)", f.display()));
        }
    }
    dir
}

/// SDL3.dll of the pinned build in the cache folder
pub fn sdl(cache_dir: &Path) -> PathBuf {
    let dll = unpacked(&cache_dir.join("sdl"), &SDL, "win64-3.4.20").join("lib/x64").join(SDL_DLL);
    if !dll.is_file() {
        fail(format!("{} is missing", dll.display()));
    }
    dll
}
