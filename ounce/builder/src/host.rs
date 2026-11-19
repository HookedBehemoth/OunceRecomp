//! The host's C, compiled in this process by clang's libraries: the runtime's files and the game's patch files
//! (Game::sources) against the bundled FFmpeg's headers (ffmpeg.rs), libopus 1.5.2 and the guest model's runtime (rt64/a64_rt.c), each with its flags, -fstack-protector-strong explicit (the distribution's clang has it on by default; ours does not).
//! Objects are cached by the files the compile read (its dependency list) and the flags.

use crate::cache::{Cache, Key};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

pub struct CJob {
    /// the object's name in the build folder (host_<file>.o, opus_<path>.o, a64_rt.o)
    pub obj: String,
    pub src: PathBuf,
    pub flags: Vec<String>,
}

const CPU: [&str; 2] = ["-march=x86-64-v3", "-ffp-contract=off"];

/// the target flags every C compile starts with: the CPU (and the build's --sysroot, sysroot.rs, given apart); on
/// Windows the MSVC target against the static C runtime (windows.rs)
pub fn target() -> Vec<String> {
    let mut t: Vec<String> = CPU.iter().map(|x| x.to_string()).collect();
    if cfg!(windows) {
        t.extend([format!("--target={}", crate::windows::TRIPLE), "-fms-runtime-lib=static".into()]);
    }
    t
}

fn s(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}

/// the runtime's and the patches' C (`sources`, Game::sources) and libopus, in the link's order; `version` is
/// hos_main.c's OUNCE_VERSION, `ffmpeg` the FFmpeg the game links (ffmpeg.rs)
pub fn host_jobs(root: &Path, rt64: &Path, sources: &[PathBuf], version: &str, ffmpeg: &Path) -> Result<Vec<CJob>, String> {
    let rt = root.join("runtime");
    let opus = root.join("third_party/opus");
    let mut fl = target();
    fl.extend([format!("-I{}", s(&rt.join("os"))), "-D_GNU_SOURCE".into(), format!("-I{}", s(&rt)), format!("-I{}", s(&rt.join("media")))]);
    fl.extend([format!("-I{}", s(&root.join("renderer/api"))), format!("-I{}", s(&root.join("third_party"))), format!("-I{}", s(rt64))]);
    fl.extend(["-Wno-everything", "-O2", "-g0", "-mcx16", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types"].map(String::from));
    if cfg!(windows) {
        // (the POSIX names the host's C calls, as the UCRT spells them: runtime/os/os_crt_windows.h)
        fl.extend([format!("-I{}", s(&rt.join("os/win"))), "-include".into(), s(&rt.join("os/os_crt_windows.h"))]);
        fl.extend(["-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_DEPRECATE"].map(String::from));
    }
    fl.push(format!("-I{}", s(&opus.join("include"))));
    fl.push(format!("-I{}", s(&ffmpeg.join("include"))));
    let mut jobs: Vec<CJob> = sources
        .iter()
        .cloned()
        .map(|src| {
            let stem = src.file_stem().unwrap().to_string_lossy().into_owned();
            let mut f = fl.clone();
            if stem == "hos_main" {
                f.push(format!("-DOUNCE_VERSION=\"{version}\""));
            }
            CJob { obj: format!("host_{stem}.o"), src, flags: f }
        })
        .collect();
    let mut of = target();
    of.extend(["-O2", "-g0", "-Wno-everything", "-DOPUS_BUILD", "-DUSE_ALLOCA", "-DHAVE_LRINTF", "-DHAVE_LRINT"].map(String::from));
    of.extend(["include", "celt", "silk", "silk/float", "src"].map(|d| format!("-I{}", s(&opus.join(d)))));
    let list = std::fs::read_to_string(opus.join("sources.txt")).map_err(|e| format!("{}: {e}", opus.join("sources.txt").display()))?;
    for rel in list.split_whitespace() {
        jobs.push(CJob { obj: format!("opus_{}.o", rel.trim_end_matches(".c").replace('/', "_")), src: opus.join(rel), flags: of.clone() });
    }
    Ok(jobs)
}

/// rt64/a64_rt.c at the generated code's flags (-O1 and two -mllvm options)
pub fn rt_job(root: &Path, rt64: &Path) -> CJob {
    let mut f = target();
    if cfg!(windows) {
        let rt = root.join("runtime");
        f.extend([format!("-I{}", s(&rt.join("os/win"))), "-include".into(), s(&rt.join("os/os_crt_windows.h")), "-D_CRT_SECURE_NO_WARNINGS".into()]);
    }
    f.extend([format!("-I{}", s(rt64)), format!("-I{}", s(&root.join("third_party/a64-kit/rt")))]);
    f.extend(["-Wno-everything", "-fno-strict-aliasing", "-mcx16", "-O1", "-g0", "-mllvm", "-rotation-max-header-size=0", "-ffunction-sections", "-mllvm", "-capture-tracking-max-uses-to-explore=5000"].map(String::from));
    CJob { obj: "a64_rt.o".into(), src: rt64.join("a64_rt.c"), flags: f }
}

/// the -mllvm options of a flag list, as (name, value)
fn mllvm(flags: &[String]) -> Vec<(String, u32)> {
    flags
        .windows(2)
        .filter(|w| w[0] == "-mllvm")
        .filter_map(|w| w[1].trim_start_matches('-').split_once('=').and_then(|(n, v)| Some((n.to_string(), v.parse().ok()?))))
        .collect()
}

/// Compiles `jobs` into `out` with `threads` threads. Every job's -mllvm options must be the same (they are LLVM's
/// globals, set for the phase). Returns how many came from the cache.
/// The compiler's own folders: clang's resource headers and the sysroot (sysroot.rs). An object's cache key names
/// neither by its path: the resource headers are clang's version, which the key names, and the sysroot is named by
/// its recipe (sysroot::key).
pub struct Toolchain<'a> {
    pub resource_dir: &'a Path,
    /// Linux: the sysroot; None on Windows, whose headers are MSVC's and the SDK's (`msvc`)
    pub sysroot: Option<&'a Path>,
    /// Windows: MSVC's and the SDK's include folders (windows.rs)
    pub msvc: &'a [PathBuf],
}

/// what names the system headers an object was compiled against in its cache key: the sysroot's recipe, or on
/// Windows MSVC's and the SDK's folders (their versions)
fn system_key(tc: &Toolchain) -> String {
    if tc.sysroot.is_some() { crate::sysroot::key() } else { tc.msvc.iter().map(|d| s(d)).collect::<Vec<_>>().join(";") }
}

pub fn compile(jobs: &[CJob], out: &Path, threads: usize, cache: Option<&Cache>, tc: &Toolchain, aux: &crate::lift::Aux) -> Result<usize, String> {
    let opts = jobs.first().map(|j| mllvm(&j.flags)).unwrap_or_default();
    if jobs.iter().any(|j| mllvm(&j.flags) != opts) {
        return Err("a phase of C compiles with different -mllvm options".into());
    }
    let over: Vec<(&str, u32)> = opts.iter().map(|(n, v)| (n.as_str(), *v)).collect();
    crate::compile::use_options(&over);
    let next = AtomicUsize::new(0);
    let (hits, err) = (AtomicUsize::new(0), Mutex::new(None::<String>));
    std::thread::scope(|sc| {
        for _ in 0..threads.max(1) {
            sc.spawn(|| loop {
                let k = next.fetch_add(1, Ordering::Relaxed);
                let Some(j) = jobs.get(k) else { break };
                let t = std::time::Instant::now();
                match one(j, cache, tc, aux.gate) {
                    Ok((o, hit)) => {
                        if let Some(p) = aux.progress {
                            p.done(&j.obj, t.elapsed().as_secs_f64(), hit);
                        }
                        hits.fetch_add(hit as usize, Ordering::Relaxed);
                        if let Err(e) = std::fs::write(out.join(&j.obj), o) {
                            *err.lock().unwrap() = Some(format!("{}: {e}", j.obj));
                        }
                    }
                    Err(e) => *err.lock().unwrap() = Some(e),
                }
            });
        }
    });
    crate::compile::use_options(crate::compile::LLVM_OPTIONS);
    match err.into_inner().unwrap() {
        Some(e) => Err(e),
        None => Ok(hits.into_inner()),
    }
}

/// the driver's arguments for a job (an -MD dependency file to `deps`)
fn args(j: &CJob, tc: &Toolchain, deps: &Path) -> Vec<String> {
    let mut a = j.flags.clone();
    a.extend(["-fstack-protector-strong".into(), format!("-resource-dir={}", s(tc.resource_dir))]);
    if let Some(r) = tc.sysroot {
        a.push(format!("--sysroot={}", s(r)));
    }
    for d in tc.msvc {
        a.extend(["-imsvc".into(), s(d)]);
    }
    a.extend(["-MD".into(), "-MF".into(), s(deps), "-c".into(), s(&j.src)]);
    a
}

/// one job through the cache: the manifest (by source and flags) names the files the last compile read; the object
/// is found by their contents. A miss compiles and records both.
fn one(j: &CJob, cache: Option<&Cache>, tc: &Toolchain, gate: &crate::gate::Gate) -> Result<(Vec<u8>, bool), String> {
    // (the manifest is found by the flags as given and the sysroot's path: which files a compile reads depends on the
    // include folders. The object is found by the flags less those folders, the sysroot's recipe and the files'
    // contents: what they hold is in the list)
    let flags: Vec<&str> = j.flags.iter().map(String::as_str).filter(|f| !f.starts_with("-I")).collect();
    let base = |kind: &str| {
        let k = Key::new(kind).add(s(&j.src).as_bytes()).add(b"-fstack-protector-strong").add(system_key(tc).as_bytes());
        let root = tc.sysroot.map(s).unwrap_or_default();
        if kind == "manifest" { k.add(j.flags.join("\0").as_bytes()).add(root.as_bytes()) } else { k.add(flags.join("\0").as_bytes()) }
    };
    let content_key = |deps: &[String]| -> Option<Key> {
        let mut k = base("c");
        for d in deps {
            k = k.add(&std::fs::read(d).ok()?);
        }
        Some(k)
    };
    if let Some(c) = cache {
        if let Some(deps) = c.manifest(base("manifest")) {
            if let Some(k) = content_key(&deps) {
                if let Some(o) = c.get(k) {
                    return Ok((o, true));
                }
            }
        }
    }
    let tmp = std::env::temp_dir().join(format!("hk-deps-{}-{}.d", std::process::id(), j.obj));
    let lease = gate.enter(crate::gate::NEED_C_MB);
    let o = crate::clang::compile(&args(j, tc, &tmp)).map_err(|e| format!("{}: {e}", j.src.display()))?;
    drop(lease);
    let text = std::fs::read_to_string(&tmp).map_err(|e| format!("{}: {e}", tmp.display()))?;
    let _ = std::fs::remove_file(&tmp);
    if let Some(c) = cache {
        // (make's rule: "<obj>: <dep> <dep> \" ...; the first word is the target)
        let deps: Vec<String> = make_words(&text).into_iter().skip(1).collect();
        c.put_manifest(base("manifest"), &deps)?;
        if let Some(k) = content_key(&deps) {
            c.put(k, &o)?;
        }
    }
    Ok((o, false))
}

/// The words of a make rule as clang's -MD writes it: whitespace apart, a backslash before a line end continues the line,
/// and one before a space is part of the word (a path with spaces: Windows' `C:\Program Files\...`, whose other
/// backslashes are its separators)
fn make_words(text: &str) -> Vec<String> {
    let (mut words, mut w) = (Vec::new(), String::new());
    let mut it = text.chars().peekable();
    while let Some(c) = it.next() {
        match c {
            '\\' if matches!(it.peek(), Some('\n') | Some('\r')) => {
                while matches!(it.peek(), Some('\n') | Some('\r')) {
                    it.next();
                }
                if !w.is_empty() {
                    words.push(std::mem::take(&mut w));
                }
            }
            '\\' if it.peek() == Some(&' ') => {
                w.push(' ');
                it.next();
            }
            c if c.is_whitespace() => {
                if !w.is_empty() {
                    words.push(std::mem::take(&mut w));
                }
            }
            c => w.push(c),
        }
    }
    if !w.is_empty() {
        words.push(w);
    }
    words
}

#[cfg(test)]
mod tests {
    #[test]
    fn make_words_keep_escaped_spaces() {
        let w = super::make_words("x.o: a.c C:\\Program\\ Files\\VC\\stdio.h \\\n  /usr/include/b.h\n");
        assert_eq!(w, ["x.o:", "a.c", "C:\\Program Files\\VC\\stdio.h", "/usr/include/b.h"]);
    }
}
