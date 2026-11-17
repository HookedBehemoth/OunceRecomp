// third_party/zbic's zstd.c built twice: the standard format (the .nup's compressed members) and ZBIC (the exefs's
// modules on firmware 22+). Each build and its shim are joined into one object whose only global symbols are the
// shim's prefixed functions (`ld -r`, then `objcopy --localize-hidden`).
use std::path::PathBuf;
use std::process::Command;

fn run(cmd: &mut Command) {
    let st = cmd.status().unwrap_or_else(|e| panic!("{cmd:?}: {e}"));
    assert!(st.success(), "{cmd:?} failed");
}

fn main() {
    let src = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../../third_party/zbic");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    println!("cargo:rerun-if-changed=src/zstd_shim.c");
    println!("cargo:rerun-if-changed={}", src.join("zstd.c").display());
    let mut joined = Vec::new();
    for (name, zbic) in [("zs_std", "0"), ("zs_zbic", "1")] {
        let objs = cc::Build::new()
            .file(src.join("zstd.c"))
            .file("src/zstd_shim.c")
            .include(&src)
            .define("ZSTD_ZBIC_SUPPORT", zbic)
            .define("ZSTDLIB_VISIBILITY", "")
            .define("ZSTDERRORLIB_VISIBILITY", "")
            .define("ZDICTLIB_VISIBILITY", "")
            .define("PFX", Some(&*format!("{name}_")))
            .flag("-fvisibility=hidden")
            .opt_level(3)
            .warnings(false)
            .compile_intermediates();
        let o = out.join(format!("{name}.o"));
        run(Command::new("ld").arg("-r").arg("-o").arg(&o).args(&objs));
        run(Command::new("objcopy").arg("--localize-hidden").arg(&o));
        joined.push(o);
    }
    let lib = out.join("libzs.a");
    let _ = std::fs::remove_file(&lib);
    run(Command::new("ar").arg("crs").arg(&lib).args(&joined));
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=zs");
    println!("cargo:rustc-link-lib=dylib=pthread");
}
