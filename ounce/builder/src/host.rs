//! The host's C, compiled in this process by clang's libraries: host/*.c (the game description's list and the
//! platform files), libopus 1.5.2 and the guest model's runtime (rt64/a64_rt.c), each with the flags nx2/build.py
//! gives it, -fstack-protector-strong explicit (the distribution's clang has it on by default; ours does not).
//! Objects are cached by the files the compile read (its dependency list) and the flags.

use crate::cache::{Cache, Key};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

pub struct CJob {
    /// the object's name in the build folder (build.py's: host_<file>.o, opus_<path>.o, a64_rt.o)
    pub obj: String,
    pub src: PathBuf,
    pub flags: Vec<String>,
}

const CPU: [&str; 2] = ["-march=x86-64-v3", "-ffp-contract=off"];

fn s(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}

/// host/*.c and libopus, in build.py's order; `version` is hos_main.c's HOOKSHOT_VERSION
pub fn host_jobs(root: &Path, rt64: &Path, host: &[String], version: &str) -> Result<Vec<CJob>, String> {
    let h = root.join("host");
    let opus = root.join("third_party/opus");
    let mut fl: Vec<String> = CPU.iter().map(|x| x.to_string()).collect();
    fl.extend([format!("-I{}", s(&h.join("os"))), "-D_GNU_SOURCE".into(), format!("-I{}", s(&h)), format!("-I{}", s(rt64))]);
    fl.extend(["-Wno-everything", "-O2", "-g0", "-mcx16", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types"].map(String::from));
    fl.push(format!("-I{}", s(&opus.join("include"))));
    let srcs: Vec<PathBuf> = host
        .iter()
        .map(|f| h.join(f))
        .chain([h.join("posix/display_sdl.c"), h.join("os/os_linux.c"), h.join("os/os_guest_linux.c")])
        .collect();
    let mut jobs: Vec<CJob> = srcs
        .into_iter()
        .map(|src| {
            let stem = src.file_stem().unwrap().to_string_lossy().into_owned();
            let mut f = fl.clone();
            if stem == "hos_main" {
                f.push(format!("-DHOOKSHOT_VERSION=\"{version}\""));
            }
            CJob { obj: format!("host_{stem}.o"), src, flags: f }
        })
        .collect();
    let mut of: Vec<String> = CPU.iter().map(|x| x.to_string()).collect();
    of.extend(["-O2", "-g0", "-Wno-everything", "-DOPUS_BUILD", "-DUSE_ALLOCA", "-DHAVE_LRINTF", "-DHAVE_LRINT"].map(String::from));
    of.extend(["include", "celt", "silk", "silk/float", "src"].map(|d| format!("-I{}", s(&opus.join(d)))));
    let list = std::fs::read_to_string(opus.join("sources.txt")).map_err(|e| format!("{}: {e}", opus.join("sources.txt").display()))?;
    for rel in list.split_whitespace() {
        jobs.push(CJob { obj: format!("opus_{}.o", rel.trim_end_matches(".c").replace('/', "_")), src: opus.join(rel), flags: of.clone() });
    }
    Ok(jobs)
}

/// rt64/a64_rt.c at the generated code's flags (build.py flags(): -O1 and two -mllvm options)
pub fn rt_job(root: &Path, rt64: &Path) -> CJob {
    let mut f: Vec<String> = CPU.iter().map(|x| x.to_string()).collect();
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
pub fn compile(jobs: &[CJob], out: &Path, threads: usize, cache: Option<&Cache>, resource_dir: &Path) -> Result<usize, String> {
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
                match one(j, cache, resource_dir) {
                    Ok((o, hit)) => {
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
fn args(j: &CJob, resource_dir: &Path, deps: &Path) -> Vec<String> {
    let mut a = j.flags.clone();
    a.extend(["-fstack-protector-strong".into(), format!("-resource-dir={}", s(resource_dir))]);
    a.extend(["-MD".into(), "-MF".into(), s(deps), "-c".into(), s(&j.src)]);
    a
}

/// one job through the cache: the manifest (by source and flags) names the files the last compile read; the object
/// is found by their contents. A miss compiles and records both.
fn one(j: &CJob, cache: Option<&Cache>, resource_dir: &Path) -> Result<(Vec<u8>, bool), String> {
    // (the manifest is found by the flags as given: which files a compile reads depends on the include folders. The
    // object is found by the flags less those folders and the files' contents: what they hold is in the list)
    let flags: Vec<&str> = j.flags.iter().map(String::as_str).filter(|f| !f.starts_with("-I")).collect();
    let base = |kind: &str| {
        let k = Key::new(kind).add(s(&j.src).as_bytes()).add(b"-fstack-protector-strong");
        if kind == "manifest" { k.add(j.flags.join("\0").as_bytes()) } else { k.add(flags.join("\0").as_bytes()) }
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
    let o = crate::clang::compile(&args(j, resource_dir, &tmp)).map_err(|e| format!("{}: {e}", j.src.display()))?;
    let text = std::fs::read_to_string(&tmp).map_err(|e| format!("{}: {e}", tmp.display()))?;
    let _ = std::fs::remove_file(&tmp);
    if let Some(c) = cache {
        // (make's rule: "<obj>: <dep> <dep> \" ...; the first word is the target)
        let deps: Vec<String> = text.replace("\\\n", " ").split_whitespace().skip(1).map(String::from).collect();
        c.put_manifest(base("manifest"), &deps)?;
        if let Some(k) = content_key(&deps) {
            c.put(k, &o)?;
        }
    }
    Ok((o, false))
}
