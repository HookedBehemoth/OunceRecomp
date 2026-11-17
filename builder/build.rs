// The C++ shims over libLLVM and lld (src/llvm_shim.cpp, src/lld_shim.cpp), built against the LLVM that llvm-config names (LLVM_CONFIG, else
// llvm-config on the PATH); libLLVM is linked dynamically.
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
    println!("cargo:rerun-if-env-changed=LLVM_CONFIG");
    let version = llvm_config(&["--version"]);
    let major: u32 = version.split('.').next().unwrap().parse().unwrap();
    assert_eq!(major, LLVM_MAJOR, "LLVM {version}: the lifter's IR is written for LLVM {LLVM_MAJOR}");
    let mut b = cc::Build::new();
    b.cpp(true).file("src/llvm_shim.cpp").file("src/lld_shim.cpp").warnings(false);
    for f in llvm_config(&["--cxxflags"]).split_whitespace() {
        b.flag(f);
    }
    b.compile("llvm_shim");
    println!("cargo:rustc-link-search=native={}", llvm_config(&["--libdir"]));
    for l in llvm_config(&["--link-shared", "--libs"]).split_whitespace() {
        println!("cargo:rustc-link-lib=dylib={}", l.trim_start_matches("-l"));
    }
    // lld's ELF driver: its libraries beside libLLVM's
    for l in ["lldELF", "lldCommon"] {
        println!("cargo:rustc-link-lib=dylib={l}");
    }
    println!("cargo:rustc-link-lib=dylib=stdc++");
}
