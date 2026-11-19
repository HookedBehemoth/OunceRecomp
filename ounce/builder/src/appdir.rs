//! `ounce tool appdir <dir> [--llvm <dir>] [--appimage <file>] [-j N]`: the release a player double-clicks. Everything
//! in it is built against the build's sysroot (sysroot.rs), so it runs on any Linux with glibc 2.35 or later:
//!
//! - `usr/bin/ounce` (LLVM, lld and clang linked statically, from `tool static-llvm`'s tree) and `usr/bin/ounce-gui`,
//!   which is `AppRun`; `usr/lib/clang/<major>/include`, clang's resource headers, where the builder looks for them;
//! - `usr/share/ounce`: the sources a build reads (`games/`, `runtime/`, `renderer/api`, the parts of `third_party/`
//!   the host's C includes, `ounce/lift/data`), `VERSION`, and `prebuilt/`: `ounce_renderer.so` (built with Meson
//!   against the sysroot, the static libc++ and pinned Vulkan headers), `ffmpeg/`, `sdl/` and `sysroot/` (without its
//!   C++ library);
//! - the desktop file and the icon.
//!
//! No game data is in it. Vulkan is the system's loader (`libvulkan.so.1`, loaded at run time); SDL3 is the release's
//! own (`tool sdl`, `prebuilt/sdl`), which a build puts beside the game. Every program is checked with `tool portable` before it is copied. The AppImage
//! is the AppDir as a zstd squashfs (`mksquashfs`) behind AppImage's type-2 runtime, pinned by its hash. The build's
//! own files go to `<checkout>/work/release/`.

use crate::fail;
use crate::fetch::{fetch, run, Pinned};
use std::path::{Path, PathBuf};
use std::process::Command;

const RUNTIME: Pinned = Pinned {
    file: "appimage-runtime-x86_64-20251108",
    url: "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64",
    sha256: "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d",
};

/// The Vulkan headers the renderer is built with (in a sysroot build the system's /usr/include cannot be on the path)
const VULKAN_HEADERS: Pinned = Pinned {
    file: "Vulkan-Headers-1.4.363.tar.gz",
    url: "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz",
    sha256: "cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68",
};

/// The sources a build reads, under the checkout and the AppDir's usr/share/ounce
const SOURCES: &[&str] = &["games", "runtime", "renderer/api", "third_party/opus", "third_party/a64-kit", "third_party/zstd", "ounce/lift/data", "LICENSE"];

/// What a game's link and the host's C never read of the sysroot: its C++ library (the static builds' only)
const SYSROOT_LEFT_OUT: &[&str] = &["usr/include/c++", "usr/include/x86_64-linux-gnu/c++", "usr/lib/gcc/x86_64-linux-gnu/12/include"];
const SYSROOT_LEFT_OUT_PREFIX: &[&str] = &["libstdc++", "libsupc++"];

const DESKTOP: &str = "[Desktop Entry]
Type=Application
Name=OunceRecomp
Comment=Builds your own game dump into a native Linux game
Exec=ounce-gui
Icon=ounce
Categories=Game;
Terminal=false
";

/// `from` copied to `to` (folders whole, links as links), leaving out what `skip` says
fn copy(from: &Path, to: &Path, skip: &dyn Fn(&Path) -> bool) -> Result<(), String> {
    let err = |p: &Path, e: std::io::Error| format!("{}: {e}", p.display());
    let m = std::fs::symlink_metadata(from).map_err(|e| err(from, e))?;
    if skip(from) {
        return Ok(());
    }
    if let Some(d) = to.parent() {
        std::fs::create_dir_all(d).map_err(|e| err(d, e))?;
    }
    if m.is_dir() {
        std::fs::create_dir_all(to).map_err(|e| err(to, e))?;
        for e in std::fs::read_dir(from).map_err(|e| err(from, e))?.flatten() {
            copy(&e.path(), &to.join(e.file_name()), skip)?;
        }
    } else if m.file_type().is_symlink() {
        let t = std::fs::read_link(from).map_err(|e| err(from, e))?;
        std::os::unix::fs::symlink(t, to).map_err(|e| err(to, e))?;
    } else {
        std::fs::copy(from, to).map_err(|e| format!("{} -> {}: {e}", from.display(), to.display()))?;
    }
    Ok(())
}

/// The icon: a ring on a rounded square, 256 by 256
fn icon() -> Vec<u8> {
    const N: usize = 256;
    let mut px = vec![0u8; N * N * 4];
    for y in 0..N {
        for x in 0..N {
            let (fx, fy) = (x as f32 + 0.5 - 128.0, y as f32 + 0.5 - 128.0);
            // (the square's corners rounded at radius 40)
            let (qx, qy) = ((fx.abs() - 88.0).max(0.0), (fy.abs() - 88.0).max(0.0));
            let corner = 40.0 - (qx * qx + qy * qy).sqrt();
            let r = (fx * fx + fy * fy).sqrt();
            let ring = 14.0 - (r - 66.0).abs();
            let p = &mut px[(y * N + x) * 4..][..4];
            let a = corner.clamp(0.0, 1.0);
            let w = ring.clamp(0.0, 1.0);
            let (br, bg, bb) = (34.0, 92.0, 110.0);
            p[0] = (br + (245.0 - br) * w) as u8;
            p[1] = (bg + (236.0 - bg) * w) as u8;
            p[2] = (bb + (200.0 - bb) * w) as u8;
            p[3] = (a * 255.0) as u8;
        }
    }
    let mut out = Vec::new();
    let mut e = png::Encoder::new(&mut out, N as u32, N as u32);
    e.set_color(png::ColorType::Rgba);
    e.set_depth(png::BitDepth::Eight);
    e.write_header().and_then(|mut w| w.write_image_data(&px)).unwrap_or_else(|e| fail(e));
    out
}

pub fn command(args: &[String]) {
    let root = ounce_core::games::root().unwrap_or_else(|e| fail(e));
    let (mut out, mut llvm, mut image, mut jobs) = (None::<PathBuf>, root.join("work/llvm-static"), None::<PathBuf>, crate::gate::default_jobs());
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            // (absolute: Meson runs in its own build folder)
            "--llvm" => llvm = std::path::absolute(val()).unwrap_or_else(|e| fail(e)),
            "--appimage" => image = Some(val().into()),
            "-j" => jobs = val().parse().unwrap_or_else(|_| fail("-j N")),
            _ if out.is_none() && !a.starts_with('-') => out = Some(a.into()),
            _ => fail("tool appdir <dir> [--llvm <static LLVM dir>] [--appimage <file>] [-j N]"),
        }
    }
    let Some(out) = out else { fail("tool appdir <dir> [--llvm <static LLVM dir>] [--appimage <file>] [-j N]") };
    let t0 = std::time::Instant::now();
    let config = crate::config();
    let work = root.join("work/release");
    std::fs::create_dir_all(&work).unwrap_or_else(|e| fail(format!("{}: {e}", work.display())));
    let log = work.join("appdir.log");
    std::fs::write(&log, "").unwrap_or_else(|e| fail(e));
    let (llvm_build, libcxx) = (llvm.join("build"), llvm.join(crate::static_llvm::LIBCXX));
    if !libcxx.join("lib/libc++.a").is_file() {
        fail(format!("{} has no libc++: run `ounce tool static-llvm {}`", llvm.display(), llvm.display()));
    }
    let sysroot = crate::sysroot::find(&root, &config.cache_dir());
    let ffmpeg = crate::ffmpeg::find(&root, &config.cache_dir(), jobs, &sysroot);
    let sdl = crate::sdl::ensure(&config.cache_dir().join("sdl"), jobs, &sysroot);
    let s = |p: &Path| p.to_string_lossy().into_owned();
    let sr = format!("--sysroot={}", s(&sysroot));

    // (the renderer: Meson against the sysroot, libc++ and the pinned Vulkan headers)
    println!("appdir: the renderer (the log is {})", log.display());
    // (outside the sources: Meson takes no absolute include folder inside them)
    let dl = config.cache_dir().join("appimage");
    let vk = dl.join(VULKAN_HEADERS.file.trim_end_matches(".tar.gz"));
    if !vk.join("include/vulkan/vulkan.h").is_file() {
        let tar = fetch(&dl, &VULKAN_HEADERS, &log);
        let mut c = Command::new("tar");
        c.arg("-xf").arg(&tar).arg("-C").arg(&dl);
        run(c, "the unpacking of the Vulkan headers", &log);
    }
    let rdir = work.join("renderer");
    let mut c = Command::new("meson");
    c.current_dir(&root).arg("setup").arg("--native-file").arg("renderer/meson/clang.ini").arg(&rdir);
    if rdir.join("build.ninja").is_file() {
        c.arg("--reconfigure");
    }
    c.arg(format!("-Dsysroot={}", s(&sysroot))).arg(format!("-Dlibcxx={}", s(&libcxx))).arg(format!("-Dvulkan_headers={}", s(&vk)));
    run(c, "the renderer's meson setup", &log);
    let mut c = Command::new("ninja");
    c.arg("-C").arg(&rdir).arg("-j").arg(jobs.to_string());
    run(c, "the renderer's build", &log);
    let renderer = rdir.join("renderer/ounce_renderer.so");

    // (ounce and its window: the static LLVM, the sysroot's start files and libraries, no debug information)
    println!("appdir: ounce and ounce-gui");
    let target = work.join("target");
    for (profile, pkg, features) in [("release", "ounce", "static-llvm,clang"), ("gui", "ounce-gui", "")] {
        let mut c = Command::new("cargo");
        c.args(["build", "--profile", profile, "-p", pkg, "--manifest-path"]).arg(root.join("ounce/Cargo.toml"));
        if !features.is_empty() {
            c.args(["--features", features]);
        }
        c.env("CARGO_TARGET_DIR", &target).env("LLVM_STATIC_DIR", &llvm_build);
        c.env("CC", "clang").env("CXX", "clang++").env("CFLAGS", &sr).env("CXXFLAGS", &sr);
        // (no libstdc++ named by the cc crate (ounce-tex's encoder): the builder links it statically, build.rs)
        c.env("CXXSTDLIB", "");
        c.env("CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER", "clang");
        c.env("CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_RUSTFLAGS", format!("-C link-arg={sr} -C link-arg=-fuse-ld=lld"));
        c.env("CARGO_PROFILE_RELEASE_DEBUG", "0").env("CARGO_PROFILE_RELEASE_STRIP", "symbols");
        run(c, &format!("the build of {pkg}"), &log);
    }
    let bins = [target.join("release/ounce"), target.join("gui/ounce-gui")];
    let mut bad = Vec::new();
    for p in bins.iter().chain([&renderer]) {
        match crate::elf::check(p) {
            Ok(l) => println!("appdir: {l}"),
            Err(l) => bad.push(l),
        }
    }
    if !bad.is_empty() {
        fail(bad.join("\n"));
    }

    // (the AppDir, made again from the start)
    println!("appdir: {}", out.display());
    let _ = std::fs::remove_dir_all(&out);
    let (bin, share) = (out.join("usr/bin"), out.join("usr/share/ounce"));
    let none = |_: &Path| false;
    for (from, to) in [(&bins[0], bin.join("ounce")), (&bins[1], bin.join("ounce-gui"))] {
        copy(from, &to, &none).unwrap_or_else(|e| fail(e));
    }
    let major = crate::LLVM_MAJOR.to_string();
    copy(&llvm_build.join("lib/clang").join(&major).join("include"), &out.join("usr/lib/clang").join(&major).join("include"), &none).unwrap_or_else(|e| fail(e));
    for src in SOURCES {
        copy(&root.join(src), &share.join(src), &none).unwrap_or_else(|e| fail(e));
    }
    let prebuilt = share.join("prebuilt");
    copy(&renderer, &share.join(crate::commands::PREBUILT_RENDERER), &none).unwrap_or_else(|e| fail(e));
    copy(&ffmpeg, &share.join(crate::ffmpeg::PREBUILT), &none).unwrap_or_else(|e| fail(e));
    copy(&sdl, &share.join(crate::sdl::PREBUILT), &none).unwrap_or_else(|e| fail(e));
    let cxx = |p: &Path| {
        let rel = p.strip_prefix(&sysroot).unwrap_or(p);
        let name = p.file_name().map_or(String::new(), |n| n.to_string_lossy().into_owned());
        SYSROOT_LEFT_OUT.iter().any(|d| rel == Path::new(d)) || SYSROOT_LEFT_OUT_PREFIX.iter().any(|x| name.starts_with(x))
    };
    copy(&sysroot, &share.join(crate::sysroot::PREBUILT), &cxx).unwrap_or_else(|e| fail(e));
    let version = crate::version::release(&root);
    std::fs::write(share.join("VERSION"), format!("{version}\n")).unwrap_or_else(|e| fail(e));
    std::fs::write(out.join("ounce.desktop"), DESKTOP).unwrap_or_else(|e| fail(e));
    std::fs::write(out.join("ounce.png"), icon()).unwrap_or_else(|e| fail(e));
    for (link, to) in [("AppRun", "usr/bin/ounce-gui"), (".DirIcon", "ounce.png")] {
        std::os::unix::fs::symlink(to, out.join(link)).unwrap_or_else(|e| fail(e));
    }
    println!("appdir: {} MB in {} (the sysroot {} MB)", crate::sysroot::size(&out) >> 20, out.display(), crate::sysroot::size(&prebuilt.join("sysroot")) >> 20);

    // (the AppImage: the type-2 runtime, then the squashfs; the runtime mounts it, or extracts it without FUSE)
    let image = image.unwrap_or_else(|| out.parent().unwrap_or(Path::new(".")).join("OunceRecomp-x86_64.AppImage"));
    let rt = fetch(&dl, &RUNTIME, &log);
    let fs = work.join("appdir.squashfs");
    let _ = std::fs::remove_file(&fs);
    let mut c = Command::new("mksquashfs");
    c.arg(&out).arg(&fs).args(["-root-owned", "-noappend", "-no-xattrs", "-comp", "zstd", "-Xcompression-level", "19", "-b", "1M", "-processors"]).arg(jobs.to_string());
    run(c, "mksquashfs", &log);
    let mut bytes = std::fs::read(&rt).unwrap_or_else(|e| fail(e));
    bytes.extend(std::fs::read(&fs).unwrap_or_else(|e| fail(e)));
    let tmp = image.with_extension("part");
    std::fs::write(&tmp, &bytes).unwrap_or_else(|e| fail(format!("{}: {e}", tmp.display())));
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o755)).unwrap_or_else(|e| fail(e));
    std::fs::rename(&tmp, &image).unwrap_or_else(|e| fail(e));
    let _ = std::fs::remove_file(&fs);
    println!("appdir: {} ({:.1} MB, {version}) in {:.0} s", image.display(), bytes.len() as f64 / 1e6, t0.elapsed().as_secs_f64());
}
