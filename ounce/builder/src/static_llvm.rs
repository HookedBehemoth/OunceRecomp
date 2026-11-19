//! `ounce tool static-llvm [<dir>]`: the static LLVM the shipped ounce links (`--features static-llvm,clang`). LLVM,
//! clang and lld of one pinned release, X86 only, the libraries alone (no tools, tests, docs, bindings, terminfo,
//! libxml2, zlib or zstd), built for x86-64-v3 like the game against the build's sysroot (sysroot.rs: they then run
//! on glibc 2.35), in `<dir>/build` from a shallow clone in `<dir>/src` (default work/llvm-static). Then:
//! `LLVM_STATIC_DIR=<dir>/build cargo build --release -p ounce --features static-llvm,clang` (`ounce tool appdir`
//! does it). It also builds libc++ from the same sources into `<dir>/libcxx`, which the release's renderer links.
//! About 10 minutes on 16 threads and 4.4 GB at the peak with 12 jobs (-j), 3.9 GB on disk. LLVM is built with git,
//! CMake, Ninja, clang and lld, run as child processes. `static-llvm.txt`, the recipe, stamps a finished folder: a
//! folder with this recipe's stamp is current and is left as it is. `--prune` then removes what neither the builder's
//! link (build.rs) nor `tool appdir` reads, the sources and the build's objects: 0.4 GB are left, what CI caches.

use crate::fail;
use sha2::{Digest, Sha256};
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

/// static-llvm.txt: what the folder is built from and how. It is also the folder's stamp.
fn recipe() -> String {
    format!(
        "LLVM, clang and lld as ounce links them (ounce tool static-llvm): {TAG}\ntargets: {}\ncmake: {}\nlibc++: {}\nsysroot: {}\n",
        TARGETS.join(" "),
        CMAKE_OPTIONS.join(" "),
        LIBCXX_OPTIONS.join(" "),
        crate::sysroot::key()
    )
}

/// The recipe's name (CI's cache key)
pub fn key() -> String {
    let h = Sha256::digest(recipe().as_bytes());
    format!("{TAG}-{:02x}{:02x}{:02x}{:02x}", h[0], h[1], h[2], h[3])
}

const STAMP: &str = "static-llvm.txt";

/// What `--prune` keeps, under the folder: llvm-config, the headers (the sources' and the generated ones), the
/// archives, clang's resource headers and libc++
const KEEP: &[&str] = &[
    "build/bin/llvm-config", "build/include", "build/lib/clang", "build/tools/clang/include", "src/llvm/include",
    "src/clang/include", "src/lld/include", LIBCXX, STAMP,
];

/// Removes everything under `dir` but KEEP and build/lib's archives
fn prune(dir: &Path, rel: &Path) -> Result<(), String> {
    for e in std::fs::read_dir(dir.join(rel)).map_err(|e| format!("{}: {e}", dir.join(rel).display()))?.flatten() {
        let r = rel.join(e.file_name());
        let name = e.file_name().to_string_lossy().into_owned();
        let p = e.path();
        if KEEP.iter().any(|k| Path::new(k) == r) || (rel == Path::new("build/lib") && name.ends_with(".a")) {
            continue;
        }
        let is_dir = e.file_type().is_ok_and(|t| t.is_dir());
        if is_dir && KEEP.iter().any(|k| Path::new(k).starts_with(&r)) {
            prune(dir, &r)?;
        } else if is_dir {
            std::fs::remove_dir_all(&p).map_err(|e| format!("{}: {e}", p.display()))?;
        } else {
            std::fs::remove_file(&p).map_err(|e| format!("{}: {e}", p.display()))?;
        }
    }
    Ok(())
}

pub fn command(args: &[String]) {
    let (mut dir, mut jobs, mut pruned) = (None::<PathBuf>, 12usize, false);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "-j" => jobs = it.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| fail("-j N")),
            "--prune" => pruned = true,
            _ if dir.is_none() && !a.starts_with('-') => dir = Some(a.into()),
            _ => fail("tool static-llvm [<dir>] [-j N] [--prune]"),
        }
    }
    let root = ounce_core::games::root().unwrap_or_else(|e| fail(e));
    let dir = dir.unwrap_or_else(|| root.join("work/llvm-static"));
    if std::fs::read_to_string(dir.join(STAMP)).is_ok_and(|t| t == recipe()) {
        println!("static-llvm: {} is current ({})", dir.display(), key());
        if pruned {
            prune(&dir, Path::new("")).unwrap_or_else(|e| fail(e));
        }
        return;
    }
    let sysroot = crate::sysroot::find(&root, &crate::config().cache_dir());
    let src = dir.join("src");
    std::fs::create_dir_all(&dir).unwrap_or_else(|e| fail(format!("{}: {e}", dir.display())));
    if !src.join(".git").is_dir() && src.exists() {
        fail(format!("{} has no clone (a pruned folder of another recipe?): remove {} and run again", src.display(), dir.display()));
    }
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
    c.arg(format!("-DCMAKE_SYSROOT={}", sysroot.display()));
    run(c, "LLVM's configure (configure.log)");
    let mut c = Command::new("ninja");
    c.arg("-C").arg(&build).arg("-j").arg(jobs.to_string()).args(TARGETS);
    run(c, "LLVM's build");
    let version = Command::new(Path::new(&build).join("bin/llvm-config")).arg("--version").output().map_or(String::new(), |o| String::from_utf8_lossy(&o.stdout).trim().to_string());
    println!("static-llvm: {} ({version})", build.display());
    // (libc++ for the release's renderer: the sysroot's libstdc++ is GCC 12's, which has no <format>)
    let rt = dir.join("libcxx-build");
    let log = std::fs::File::create(dir.join("libcxx-configure.log")).unwrap_or_else(|e| fail(e));
    let mut c = Command::new("cmake");
    c.args(["-G", "Ninja", "-S"]).arg(src.join("runtimes")).arg("-B").arg(&rt).args(LIBCXX_OPTIONS.iter().map(|o| format!("-D{o}"))).stdout(log);
    c.arg(format!("-DCMAKE_SYSROOT={}", sysroot.display())).arg(format!("-DCMAKE_INSTALL_PREFIX={}", dir.join(LIBCXX).display()));
    run(c, "libc++'s configure (libcxx-configure.log)");
    let mut c = Command::new("ninja");
    c.arg("-C").arg(&rt).arg("-j").arg(jobs.to_string()).args(["install-cxx", "install-cxxabi"]);
    run(c, "libc++'s build");
    println!("static-llvm: libc++ in {}", dir.join(LIBCXX).display());
    std::fs::write(dir.join(STAMP), recipe()).unwrap_or_else(|e| fail(e));
    if pruned {
        prune(&dir, Path::new("")).unwrap_or_else(|e| fail(e));
        println!("static-llvm: pruned to {} MB", crate::sysroot::size(&dir) >> 20);
    }
}

/// libc++ and libc++abi, static and PIC, on libgcc's unwinder: `<dir>/libcxx`, what the release's renderer links
/// (renderer's meson option `libcxx`)
pub const LIBCXX: &str = "libcxx";

const LIBCXX_OPTIONS: &[&str] = &[
    "CMAKE_BUILD_TYPE=Release", "CMAKE_C_COMPILER=clang", "CMAKE_CXX_COMPILER=clang++", "LLVM_ENABLE_RUNTIMES=libcxx;libcxxabi",
    "CMAKE_C_FLAGS=-march=x86-64-v3 -ffunction-sections -fdata-sections",
    "CMAKE_CXX_FLAGS=-march=x86-64-v3 -ffunction-sections -fdata-sections", "CMAKE_POSITION_INDEPENDENT_CODE=ON",
    "LIBCXX_ENABLE_SHARED=OFF", "LIBCXXABI_ENABLE_SHARED=OFF", "LIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON",
    "LIBCXXABI_USE_LLVM_UNWINDER=OFF", "LIBCXX_INCLUDE_TESTS=OFF", "LIBCXX_INCLUDE_BENCHMARKS=OFF", "LIBCXXABI_INCLUDE_TESTS=OFF",
    "LIBCXX_INCLUDE_DOCS=OFF", "LIBCXX_HARDENING_MODE=none", "LLVM_INCLUDE_TESTS=OFF",
];
