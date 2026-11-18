// The BC encoders (src/encoders.cpp over bc7enc_rdo: rgbcx, bc7decomp and bc7e.ispc) built into one static library.
// The sources: OUNCE_BC7ENC (for packaging), else third_party/bc7enc_rdo; ISPC (a tool, not vendored): `ispc` on the
// PATH, or ISPC (as CC and CXX). The ISPC version, targets and options decide the BC7 bytes: the version goes to the
// crate as OUNCE_ISPC_VERSION, into the texture store's key (run.rs), so another ISPC makes a new store.
use std::path::PathBuf;
use std::process::Command;

/// ISPC and its version ("1.24.0")
fn ispc() -> (PathBuf, String) {
    println!("cargo:rerun-if-env-changed=ISPC");
    println!("cargo:rerun-if-env-changed=PATH");
    let exe = std::env::var_os("ISPC").filter(|v| !v.is_empty()).map_or_else(|| PathBuf::from("ispc"), PathBuf::from);
    let out = Command::new(&exe).arg("--version").output();
    let Ok(out) = out.as_ref().map(|o| String::from_utf8_lossy(&o.stdout).into_owned()) else {
        panic!("ispc not found: install it (e.g. pacman -S ispc), or set ISPC=<path>")
    };
    // ("Intel(r) Implicit SPMD Program Compiler (Intel(r) ISPC), 1.24.0 (build ...)")
    let version = out.split(", ").nth(1).and_then(|r| r.split_whitespace().next()).unwrap_or(out.trim()).to_string();
    (exe, version)
}

fn main() {
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let src = std::env::var_os("OUNCE_BC7ENC").map(PathBuf::from).unwrap_or_else(|| root.join("third_party/bc7enc_rdo"));
    println!("cargo:rerun-if-changed=src/encoders.cpp");
    println!("cargo:rerun-if-env-changed=OUNCE_BC7ENC");
    for f in ["rgbcx.cpp", "bc7decomp.cpp", "bc7e.ispc"] {
        assert!(src.join(f).exists(), "bc7enc_rdo sources not found in {} (OUNCE_BC7ENC)", src.display());
        println!("cargo:rerun-if-changed={}", src.join(f).display());
    }
    let (exe, version) = ispc();
    println!("cargo:rustc-env=OUNCE_ISPC_VERSION={version}");
    let st = Command::new(&exe)
        .arg("-O2")
        .arg(src.join("bc7e.ispc"))
        .arg("-o")
        .arg(out.join("bc7e.o"))
        .arg("-h")
        .arg(out.join("bc7e_ispc.h"))
        .args(["--target=sse2,sse4,avx,avx2", "--opt=fast-math", "--opt=disable-assertions", "--pic", "--woff"])
        .status()
        .unwrap_or_else(|e| panic!("{}: {e}", exe.display()));
    assert!(st.success(), "ISPC failed on bc7e.ispc");
    let mut b = cc::Build::new();
    b.cpp(true)
        .std("c++17")
        .opt_level(2)
        .warnings(false)
        // (the builder's CPU level; the C++ parts are integer-only where the store uses them, and no contraction keeps
        // the rest as written)
        .flag("-march=x86-64-v3")
        .flag("-ffp-contract=off")
        .include(&out)
        .include(&src)
        .file("src/encoders.cpp")
        .file(src.join("rgbcx.cpp"))
        .file(src.join("bc7decomp.cpp"));
    for t in ["", "_sse2", "_sse4", "_avx", "_avx2"] {
        b.object(out.join(format!("bc7e{t}.o")));
    }
    b.compile("hk_encoders");
}
