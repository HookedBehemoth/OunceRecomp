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
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        return windows();
    }
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

/// Windows (COFF has no hidden visibility to localize): each build of zstd.c and its shim joined as LLVM IR, every
/// symbol but the shim's prefixed functions internalized (opt -internalize), then compiled to one object. The tools are
/// LLVM's, beside the llvm-config the builder takes (LLVM_CONFIG, else the PATH's).
fn windows() {
    let src = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../../third_party/zbic");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    println!("cargo:rerun-if-changed=src/zstd_shim.c");
    println!("cargo:rerun-if-changed={}", src.join("zstd.c").display());
    println!("cargo:rerun-if-env-changed=LLVM_CONFIG");
    let config = std::env::var("LLVM_CONFIG").unwrap_or_else(|_| "llvm-config".into());
    let bin = Command::new(&config).arg("--bindir").output().unwrap_or_else(|e| panic!("{config}: {e}"));
    let bin = PathBuf::from(String::from_utf8(bin.stdout).unwrap().trim());
    let tool = |n: &str| bin.join(format!("{n}.exe"));
    let shim = std::fs::read_to_string("src/zstd_shim.c").unwrap();
    // (the shim's functions: `API <type> F(<name>)(`)
    let names: Vec<&str> = shim.lines().filter(|l| l.starts_with("API ")).filter_map(|l| l.split("F(").nth(1)?.split(')').next()).collect();
    let mut joined = Vec::new();
    for (name, zbic) in [("zs_std", "0"), ("zs_zbic", "1")] {
        let mut bcs = Vec::new();
        for (f, stem) in [(src.join("zstd.c"), "zstd"), (PathBuf::from("src/zstd_shim.c"), "shim")] {
            let bc = out.join(format!("{name}_{stem}.bc"));
            run(Command::new(tool("clang")).args(["-c", "-emit-llvm", "-O3", "-march=x86-64-v3", "-w"]).arg(format!("-I{}", src.display()))
                .args([format!("-DZSTD_ZBIC_SUPPORT={zbic}"), "-DZSTDLIB_VISIBILITY=".into(), "-DZSTDERRORLIB_VISIBILITY=".into()])
                .args(["-DZDICTLIB_VISIBILITY=".to_string(), format!("-DPFX={name}_"), "-D_CRT_SECURE_NO_WARNINGS".into()])
                .arg(&f).arg("-o").arg(&bc));
            bcs.push(bc);
        }
        let linked = out.join(format!("{name}.bc"));
        run(Command::new(tool("llvm-link")).args(&bcs).arg("-o").arg(&linked));
        let keep: Vec<String> = names.iter().map(|n| format!("{name}_{n}")).collect();
        let opt = out.join(format!("{name}.opt.bc"));
        run(Command::new(tool("opt")).arg("-passes=internalize,globaldce").arg(format!("-internalize-public-api-list={}", keep.join(",")))
            .arg(&linked).arg("-o").arg(&opt));
        let o = out.join(format!("{name}.obj"));
        run(Command::new(tool("clang")).args(["-c", "-O3", "-march=x86-64-v3"]).arg(&opt).arg("-o").arg(&o));
        joined.push(o);
    }
    let lib = out.join("zs.lib");
    let _ = std::fs::remove_file(&lib);
    run(Command::new(tool("llvm-lib")).arg(format!("/out:{}", lib.display())).args(&joined));
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=zs");
}
