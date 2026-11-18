// The BC encoders (src/encoders.cpp over bc7enc_rdo: rgbcx, bc7decomp and bc7e.ispc) built into one static library.
// The sources: OUNCE_BC7ENC (for packaging), else third_party/bc7enc_rdo; ISPC (a tool, not vendored): the user's config
// `tools.ispc` (ounce config set tools.ispc <path>), else `ispc` on the PATH. The ISPC targets and options decide the
// BC7 bytes: changing them changes every prepared BC7 texture.
use std::path::PathBuf;
use std::process::Command;

fn ispc() -> PathBuf {
    let config = ounce_core::config::Config::default_path();
    println!("cargo:rerun-if-changed={}", config.display());
    let c = ounce_core::config::Config::load().unwrap_or_else(|e| panic!("{e}"));
    c.show("tools.ispc").map_or_else(|| "ispc".into(), PathBuf::from)
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
    let exe = ispc();
    let st = Command::new(&exe)
        .arg("-O2")
        .arg(src.join("bc7e.ispc"))
        .arg("-o")
        .arg(out.join("bc7e.o"))
        .arg("-h")
        .arg(out.join("bc7e_ispc.h"))
        .args(["--target=sse2,sse4,avx,avx2", "--opt=fast-math", "--opt=disable-assertions", "--pic", "--woff"])
        .status()
        .unwrap_or_else(|e| panic!("{}: {e} (ispc: ounce config set tools.ispc <path>)", exe.display()));
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
