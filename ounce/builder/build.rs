// The C++ shims over LLVM, lld and clang (src/*_shim.cpp). By default against the LLVM that llvm-config names (LLVM_CONFIG,
// else llvm-config on the PATH), libLLVM (and with `clang`, libclang-cpp) linked dynamically; with the `static-llvm`
// feature against a static build tree.
use std::process::Command;

const LLVM_MAJOR: u32 = 23;

fn llvm_config(args: &[&str]) -> String {
    let exe = std::env::var("LLVM_CONFIG").unwrap_or_else(|_| "llvm-config".into());
    let out = Command::new(&exe).args(args).output().unwrap_or_else(|e| panic!("{exe}: {e} (set LLVM_CONFIG)"));
    assert!(out.status.success(), "{exe} {args:?} failed");
    String::from_utf8(out.stdout).unwrap().trim().to_string()
}

fn main() {
    println!("cargo:rerun-if-changed=src/llvm_shim.cpp");
    println!("cargo:rerun-if-changed=src/lld_shim.cpp");
    println!("cargo:rerun-if-changed=src/clang_shim.cpp");
    println!("cargo:rerun-if-env-changed=LLVM_CONFIG");
    println!("cargo:rerun-if-env-changed=LLVM_STATIC_DIR");
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        return windows();
    }
    if std::env::var_os("CARGO_FEATURE_STATIC_LLVM").is_some() {
        return static_llvm();
    }
    let version = llvm_config(&["--version"]);
    let major: u32 = version.split('.').next().unwrap().parse().unwrap();
    assert_eq!(major, LLVM_MAJOR, "LLVM {version}: the lifter's IR is written for LLVM {LLVM_MAJOR}");
    let clang = std::env::var_os("CARGO_FEATURE_CLANG").is_some();
    let mut b = cc::Build::new();
    b.cpp(true).file("src/llvm_shim.cpp").file("src/lld_shim.cpp").warnings(false);
    if clang {
        b.file("src/clang_shim.cpp");
        // (the system clang's resource headers: the same version as its libclang-cpp)
        let res = std::path::PathBuf::from(llvm_config(&["--libdir"])).join("clang").join(major.to_string());
        println!("cargo:rustc-env=HK_CLANG_RESOURCE_DIR={}", res.display());
    }
    for f in llvm_config(&["--cxxflags"]).split_whitespace() {
        b.flag(f);
    }
    b.compile("llvm_shim");
    if clang {
        println!("cargo:rustc-link-lib=dylib=clang-cpp");
    }
    println!("cargo:rustc-link-search=native={}", llvm_config(&["--libdir"]));
    for l in llvm_config(&["--link-shared", "--libs"]).split_whitespace() {
        println!("cargo:rustc-link-lib=dylib={}", l.trim_start_matches("-l"));
    }
    // lld's ELF driver: its libraries beside libLLVM's, shared, or (Debian's and Ubuntu's packages) only as archives,
    // which then need LLVM's system libraries (zlib, zstd) themselves
    let libdir = std::path::PathBuf::from(llvm_config(&["--libdir"]));
    let shared = libdir.join("liblldELF.so").exists();
    for l in ["lldELF", "lldCommon"] {
        println!("cargo:rustc-link-lib={}={l}", if shared { "dylib" } else { "static" });
    }
    if !shared {
        // (`-lz`, or a library's path: Ubuntu's names /usr/lib/x86_64-linux-gnu/libz3.so)
        for l in llvm_config(&["--link-static", "--system-libs"]).split_whitespace() {
            match l.strip_prefix("-l") {
                Some(name) => println!("cargo:rustc-link-lib=dylib={name}"),
                None => println!("cargo:rustc-link-arg={l}"),
            }
        }
    }
    println!("cargo:rustc-link-lib=dylib=stdc++");
}

/// The `static-llvm` feature: LLVM and lld (and with `clang`, clang's libraries) linked statically from the build tree
/// LLVM_STATIC_DIR (its source tree beside it, ../src), every archive in one group so the linker takes only what is
/// used
fn static_llvm() {
    let dir = std::path::PathBuf::from(std::env::var("LLVM_STATIC_DIR").expect("static-llvm: set LLVM_STATIC_DIR to the LLVM build tree"));
    let src = dir.join("../src");
    let config = dir.join("bin/llvm-config");
    let run = |args: &[&str]| {
        let out = Command::new(&config).args(args).output().unwrap_or_else(|e| panic!("{}: {e}", config.display()));
        String::from_utf8(out.stdout).unwrap().trim().to_string()
    };
    let clang = std::env::var_os("CARGO_FEATURE_CLANG").is_some();
    let mut b = cc::Build::new();
    b.cpp(true).file("src/llvm_shim.cpp").file("src/lld_shim.cpp").warnings(false).cargo_metadata(false);
    if clang {
        // (no resource headers named: a release has its own beside the executable, main.rs resource_dir)
        b.file("src/clang_shim.cpp");
    }
    for f in run(&["--cxxflags"]).split_whitespace() {
        b.flag(f);
    }
    for inc in [src.join("lld/include"), src.join("clang/include"), dir.join("tools/clang/include")] {
        b.include(inc);
    }
    b.flag("-ffunction-sections").flag("-fdata-sections");
    b.compile("llvm_shim");
    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let mut archives: Vec<std::path::PathBuf> = std::fs::read_dir(dir.join("lib"))
        .unwrap()
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| {
            let n = p.file_name().unwrap().to_string_lossy();
            n.ends_with(".a") && (n.starts_with("libLLVM") || n.starts_with("liblld") || clang && n.starts_with("libclang"))
        })
        .collect();
    archives.sort();
    println!("cargo:rustc-link-arg=-Wl,--gc-sections");
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    println!("cargo:rustc-link-arg={}", out.join("libllvm_shim.a").display());
    for a in &archives {
        println!("cargo:rustc-link-arg={}", a.display());
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");
    for l in run(&["--link-static", "--system-libs"]).split_whitespace() {
        println!("cargo:rustc-link-arg={l}");
    }
    // (the C++ library statically too: the shipped ounce needs no libstdc++ of the player's)
    println!("cargo:rustc-link-arg=-l:libstdc++.a");
}

/// Windows (MSVC): LLVM, lld and clang linked statically from the release tree llvm-config names (LLVM_CONFIG, else
/// llvm-config on the PATH: the `clang+llvm-<version>-x86_64-pc-windows-msvc` archive holds every library). Its
/// libraries are built against the static C runtime (/MT), as the shims are here and the crates are with
/// `+crt-static` (.cargo/config.toml). zlib and zstd, which the archive's llvm-config names but does not ship, are the
/// libz-sys and zstd-sys crates' (Cargo.toml); libxml2, which only lld-link's manifest merger calls, is
/// src/xml_stub.c's (the game's link passes /manifest:no).
fn windows() {
    println!("cargo:rerun-if-changed=src/xml_stub.c");
    let version = llvm_config(&["--version"]);
    let major: u32 = version.split('.').next().unwrap().parse().unwrap();
    assert_eq!(major, LLVM_MAJOR, "LLVM {version}: the lifter's IR is written for LLVM {LLVM_MAJOR}");
    let clang = std::env::var_os("CARGO_FEATURE_CLANG").is_some();
    let libdir = std::path::PathBuf::from(llvm_config(&["--libdir"]));
    let mut b = cc::Build::new();
    b.cpp(true).file("src/llvm_shim.cpp").file("src/lld_shim.cpp").warnings(false).static_crt(true);
    if clang {
        b.file("src/clang_shim.cpp");
        let res = libdir.join("clang").join(major.to_string());
        println!("cargo:rustc-env=HK_CLANG_RESOURCE_DIR={}", res.display());
    }
    // (llvm-config's flags are cl's; the C and C++ of every crate are compiled by clang, .cargo/config.toml)
    for f in llvm_config(&["--cxxflags"]).split_whitespace() {
        match f {
            "/EHs-c-" => b.flag("-fno-exceptions"),
            "/GR-" => b.flag("-fno-rtti"),
            f if f.starts_with("-std:") => b.flag(f.replace("-std:", "-std=")),
            f => b.flag(f),
        };
    }
    b.compile("llvm_shim");
    cc::Build::new().file("src/xml_stub.c").static_crt(true).warnings(false).compile("xml_stub");
    println!("cargo:rustc-link-search=native={}", libdir.display());
    // (every component library; the DLLs' import libraries (LLVM-C, libclang, LTO, Remarks) and clangd's are left out)
    let mut libs: Vec<String> = std::fs::read_dir(&libdir)
        .unwrap()
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter_map(|n| n.strip_suffix(".lib").map(String::from))
        .filter(|n| {
            let llvm = n.starts_with("LLVM") && n.as_bytes().get(4).is_some_and(|c| c.is_ascii_alphabetic());
            llvm || n.starts_with("lld") || clang && n.starts_with("clang") && !n.starts_with("clangd") && !n.starts_with("clangTidy")
        })
        .collect();
    libs.sort();
    for l in &libs {
        println!("cargo:rustc-link-lib=static={l}");
    }
    // (the system's own libraries; zlib's, zstd's and libxml2's archives, named by the release's build machine, are
    // replaced as said above)
    for l in llvm_config(&["--system-libs"]).split_whitespace() {
        let name = l.strip_suffix(".lib").unwrap_or(l);
        if !name.contains(['/', '\\']) &&!["zs", "xml2s"].contains(&name) {
            println!("cargo:rustc-link-lib=dylib={name}");
        }
    }
}
