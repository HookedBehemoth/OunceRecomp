//! `ounce tool static-llvm [<dir>]`: the static LLVM the shipped ounce links (`--features static-llvm,clang`). LLVM,
//! clang and lld of one pinned release, X86 only, the libraries alone (no tools, tests, docs, bindings, terminfo,
//! libxml2, zlib or zstd), built for x86-64-v3 like the game, in `<dir>/build` from a shallow clone in `<dir>/src`
//! (default work/llvm-static). Then: `LLVM_STATIC_DIR=<dir>/build cargo build --release -p ounce --features
//! static-llvm,clang`. About 10 minutes on 16 threads and 4.4 GB at the peak with 12 jobs (-j), 3.9 GB on disk.
//! LLVM is built with git, CMake, Ninja, clang and lld, run as child processes.

use crate::fail;
use std::path::{Path, PathBuf};
use std::process::Command;

const TAG: &str = "llvmorg-23.1.5";

/// the libraries ounce links and what they need, llvm-config for its flags, clang's resource headers
const TARGETS: &[&str] = &[
    "llvm-config", "clang-resource-headers", "clangCodeGen", "clangFrontend", "clangDriver", "clangSerialization",
    "clangParse", "clangSema", "clangAnalysis", "clangAST", "clangEdit", "clangLex", "clangBasic", "clangAPINotes",
    "clangSupport", "clangOptions", "lldELF", "lldCommon", "LLVMX86CodeGen", "LLVMX86AsmParser", "LLVMX86Desc",
    "LLVMX86Info", "LLVMPasses", "LLVMIRReader",
];

const CMAKE_OPTIONS: &[&str] = &[
    "CMAKE_BUILD_TYPE=Release", "CMAKE_C_COMPILER=clang", "CMAKE_CXX_COMPILER=clang++", "LLVM_USE_LINKER=lld",
    "CMAKE_C_FLAGS=-march=x86-64-v3 -ffunction-sections -fdata-sections",
    "CMAKE_CXX_FLAGS=-march=x86-64-v3 -ffunction-sections -fdata-sections",
    "LLVM_TARGETS_TO_BUILD=X86", "LLVM_ENABLE_PROJECTS=clang;lld", "LLVM_DEFAULT_TARGET_TRIPLE=x86_64-pc-linux-gnu",
    "BUILD_SHARED_LIBS=OFF", "LLVM_BUILD_LLVM_DYLIB=OFF", "LLVM_LINK_LLVM_DYLIB=OFF",
    "LLVM_BUILD_TOOLS=OFF", "CLANG_BUILD_TOOLS=OFF", "LLD_BUILD_TOOLS=OFF", "LLVM_BUILD_UTILS=OFF",
    "LLVM_INCLUDE_EXAMPLES=OFF", "LLVM_INCLUDE_TESTS=OFF", "LLVM_INCLUDE_BENCHMARKS=OFF", "LLVM_INCLUDE_DOCS=OFF",
    "LLVM_ENABLE_BINDINGS=OFF", "CLANG_INCLUDE_TESTS=OFF", "CLANG_INCLUDE_DOCS=OFF", "CLANG_BUILD_EXAMPLES=OFF",
    "CLANG_ENABLE_STATIC_ANALYZER=OFF", "CLANG_ENABLE_OBJC_REWRITER=OFF", "CLANG_PLUGIN_SUPPORT=OFF",
    "LLVM_ENABLE_TERMINFO=OFF", "LLVM_ENABLE_LIBXML2=OFF", "LLVM_ENABLE_ZSTD=OFF", "LLVM_ENABLE_ZLIB=OFF",
    "LLVM_ENABLE_LIBEDIT=OFF", "LLVM_ENABLE_LIBPFM=OFF", "LLVM_ENABLE_CURL=OFF", "LLVM_ENABLE_HTTPLIB=OFF",
    "LLVM_ENABLE_ASSERTIONS=OFF", "LLVM_ENABLE_PLUGINS=OFF", "LLVM_PARALLEL_LINK_JOBS=2",
];

fn run(mut c: Command, what: &str) {
    let prog = c.get_program().to_string_lossy().into_owned();
    let st = c.status().unwrap_or_else(|e| fail(format!("{what}: {prog}: {e}")));
    if !st.success() {
        fail(format!("{what} failed ({st})"));
    }
}

pub fn command(args: &[String]) {
    let (mut dir, mut jobs) = (None::<PathBuf>, 12usize);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "-j" => jobs = it.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| fail("-j N")),
            _ if dir.is_none() && !a.starts_with('-') => dir = Some(a.into()),
            _ => fail("tool static-llvm [<dir>] [-j N]"),
        }
    }
    let dir = dir.unwrap_or_else(|| ounce_core::games::root().unwrap_or_else(|e| fail(e)).join("work/llvm-static"));
    let src = dir.join("src");
    std::fs::create_dir_all(&dir).unwrap_or_else(|e| fail(format!("{}: {e}", dir.display())));
    if !src.join(".git").is_dir() {
        let mut c = Command::new("git");
        c.args(["clone", "-q", "--depth", "1", "--branch", TAG, "--single-branch", "https://github.com/llvm/llvm-project.git"]).arg(&src);
        run(c, "the clone of LLVM");
    }
    let have = Command::new("git").arg("-C").arg(&src).args(["describe", "--tags"]).output().ok().map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string());
    if have.as_deref() != Some(TAG) {
        fail(format!("{} is {}, not {TAG}: remove it and run again", src.display(), have.unwrap_or_default()));
    }
    let build = dir.join("build");
    let log = std::fs::File::create(dir.join("configure.log")).unwrap_or_else(|e| fail(e));
    let mut c = Command::new("cmake");
    c.args(["-G", "Ninja", "-S"]).arg(src.join("llvm")).arg("-B").arg(&build).args(CMAKE_OPTIONS.iter().map(|o| format!("-D{o}"))).stdout(log);
    run(c, "LLVM's configure (configure.log)");
    let mut c = Command::new("ninja");
    c.arg("-C").arg(&build).arg("-j").arg(jobs.to_string()).args(TARGETS);
    run(c, "LLVM's build");
    let version = Command::new(Path::new(&build).join("bin/llvm-config")).arg("--version").output().map_or(String::new(), |o| String::from_utf8_lossy(&o.stdout).trim().to_string());
    println!("static-llvm: {} ({version})", build.display());
}
