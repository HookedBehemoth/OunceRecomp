//! hookshot-builder: a player's dump -> a portable game folder (docs/builder-plan.md).

mod compile;
mod llvm;

use std::path::PathBuf;
use std::time::Instant;

const HELP: &str = "hookshot-builder <command> ...
  compile-ll <in.ll> <out.o> [--hot]   one partition's IR -> its object, as nx2/build.py compiles it
  llvm-version                         the libLLVM this process uses";

fn fail(msg: impl std::fmt::Display) -> ! {
    eprintln!("hookshot-builder: {msg}");
    std::process::exit(1)
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    match argv.first().map(String::as_str) {
        Some("compile-ll") => {
            let hot = argv.iter().any(|a| a == "--hot");
            let files: Vec<&String> = argv[1..].iter().filter(|a| !a.starts_with("--")).collect();
            if files.len() != 2 {
                fail(HELP);
            }
            let (src, obj) = (PathBuf::from(files[0]), PathBuf::from(files[1]));
            compile::init().unwrap_or_else(|e| fail(e));
            let ir = std::fs::read(&src).unwrap_or_else(|e| fail(format!("{}: {e}", src.display())));
            let t = Instant::now();
            let level = if hot { compile::Level::Hot } else { compile::Level::Cold };
            let o = if argv.iter().any(|a| a == "--codegen-only") {
                // (a diagnosis: the IR as given, code generation alone)
                llvm::compile(&ir, "<stdin>", &llvm::Recipe { cpu: compile::CPU, pipeline: "", level: if hot { 2 } else { 1 }, slp: false, rotation: None })
            } else {
                compile::partition(&ir, "<stdin>", level)
            }
            .unwrap_or_else(|e| fail(e));
            std::fs::write(&obj, &o).unwrap_or_else(|e| fail(format!("{}: {e}", obj.display())));
            println!("{}: {} bytes in {:.2} s", obj.display(), o.len(), t.elapsed().as_secs_f64());
        }
        Some("compile-all") => compile_all(&argv[1..]),
        Some("llvm-version") => println!("{}", llvm::version()),
        _ => println!("{HELP}"),
    }
}

/// `compile-all <lift dir> <out dir> [-j N] [--check <obj dir>]`: every lift_*.ll of a lift, compiled as build.py
/// compiles it (lift_h*: hot, the rest cold); --check compares each object with build.py's of the same name
fn compile_all(args: &[String]) {
    let mut jobs = std::thread::available_parallelism().map_or(1, |n| n.get());
    let (mut dirs, mut check) = (Vec::new(), None::<PathBuf>);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "-j" => jobs = it.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| fail("-j N")),
            "--check" => check = Some(PathBuf::from(it.next().unwrap_or_else(|| fail("--check <obj dir>")))),
            _ => dirs.push(PathBuf::from(a)),
        }
    }
    let [src, out] = &dirs[..] else { fail("compile-all <lift dir> <out dir> [-j N] [--check <obj dir>]") };
    std::fs::create_dir_all(out).unwrap_or_else(|e| fail(format!("{}: {e}", out.display())));
    let mut parts: Vec<(PathBuf, u64, compile::Level)> = std::fs::read_dir(src)
        .unwrap_or_else(|e| fail(format!("{}: {e}", src.display())))
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "ll") && p.file_name().unwrap().to_string_lossy().starts_with("lift_"))
        .map(|p| {
            let hot = p.file_name().unwrap().to_string_lossy().starts_with("lift_h");
            let n = std::fs::metadata(&p).map_or(0, |m| m.len());
            (p, n, if hot { compile::Level::Hot } else { compile::Level::Cold })
        })
        .collect();
    // (the hot ones first, at their rotation threshold, then the cold ones; the largest first in each)
    parts.sort_by_key(|p| (p.2 == compile::Level::Cold, std::cmp::Reverse(p.1)));
    compile::init().unwrap_or_else(|e| fail(e));
    let t0 = Instant::now();
    let next = std::sync::atomic::AtomicUsize::new(0);
    let res = std::sync::Mutex::new((0usize, 0usize, 0f64));
    std::thread::scope(|s| {
        for _ in 0..jobs.max(1) {
            s.spawn(|| loop {
                let k = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                let Some((p, _, level)) = parts.get(k) else { break };
                let stem = p.file_stem().unwrap().to_string_lossy().into_owned();
                let ir = std::fs::read(p).unwrap_or_else(|e| fail(format!("{}: {e}", p.display())));
                let t = Instant::now();
                let o = compile::partition(&ir, "<stdin>", *level).unwrap_or_else(|e| fail(format!("{stem}: {e}")));
                let dt = t.elapsed().as_secs_f64();
                drop(ir);
                std::fs::write(out.join(format!("{stem}.o")), &o).unwrap();
                let same = check.as_ref().map(|c| std::fs::read(c.join(format!("{stem}.o"))).ok().as_deref() == Some(&o[..]));
                let mut r = res.lock().unwrap();
                r.0 += 1;
                r.2 += dt;
                if same == Some(false) {
                    r.1 += 1;
                }
                println!(
                    "{stem}: {:?} {:.1} s{} [{}/{}, {:.0} s]",
                    level,
                    dt,
                    match same { Some(true) => ", same", Some(false) => ", DIFFERS", None => "" },
                    r.0,
                    parts.len(),
                    t0.elapsed().as_secs_f64()
                );
            });
        }
    });
    let r = res.into_inner().unwrap();
    println!("compile: {} partitions, {:.0} s wall, {:.0} s in compiles{}", r.0, t0.elapsed().as_secs_f64(), r.2,
             if check.is_some() { format!(", {} differ", r.1) } else { String::new() });
    if r.1 > 0 {
        std::process::exit(1);
    }
}
