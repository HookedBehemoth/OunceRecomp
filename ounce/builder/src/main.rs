//! hookshot-builder: a player's dump -> a portable game folder (docs/builder-plan.md).

#[cfg(feature = "clang")]
mod clang;
mod cache;
#[cfg(feature = "clang")]
mod checks;
mod compile;
mod dump;
mod gate;
#[cfg(feature = "clang")]
mod host;
mod labels;
mod lift;
mod link;
mod llvm;
mod objeq;
mod progress;
mod textures;
#[cfg(feature = "clang")]
mod version;

use std::path::PathBuf;
use std::time::Instant;

/// the LLVM the lifter's IR text is written for
#[cfg(feature = "clang")]
const LLVM_MAJOR: u32 = 23;

const HELP: &str = "hookshot-builder <command> ...
  compile-ll <in.ll> <out.o> [--hot]   one partition's IR -> its object
  compile-all <lift dir> <out dir> [-j N] [--check <obj dir>]
  lift-compile <out dir> [-j N] [--check <obj dir>] [--cache <dir>] [--game <dir>] [--work <dir>] [--root <dir>] [--exefs <dir>]
                                       the lift and the compile of every partition and generated module, in memory
  build <out dir> --direct <hookshot_direct.so> [-j N] [--cache <dir>] [--version <text>] [--check <obj dir>]
        [--gen <lift dir> [--gen-parts]] [--progress] [--game <dir>] [--work <dir>] [--root <dir>] [--exefs <dir>]
        [--dump <file.nup|folder> [--dump-dir <dir>] [--title <t>]] [--prepared <dir> [--fast-textures] [--romfs <dir>]]
                                       the game folder: the lift, every compile and the link in this process (feature clang);
                                       --dump reads the dump into --dump-dir (default <work>/dump), the romfs while the lift runs;
                                       --prepared writes the texture store (<dir>/tex) and the overlay (<dir>/terrain)
  dump <file.nup|folder> [--verify] [--extract <dir>] [--title <index|version|id>] [--only exefs|romfs]
                                       a dump: its titles, its checks (--verify), or written out as exefs/ and romfs/
  link <out> <objs.rsp> [--order <hot_order.bin>] [--map <file>] [--labels <file>] [--print] [-- <libs>...]
                                       the game's link (a list of objects), lld in this process
  textures <out dir> [--romfs <dir>] [--game <dir>] [--fast] [-j N] [--only <prefix>]...
                                       the prepared texture store from a romfs; --compare <a> <b> checks two stores
  overlay <out dir> [--romfs <dir>] [--game <dir>]
                                       the game's own files made linear (its overlay_steps), served over the romfs
  objeq <a.o> <b.o>                    equal as linker input: allocated sections, symbols but FILE ones, relocations
  compile-c <out.o> -- <clang args>... a C file compiled in this process by clang's libraries (feature clang)
  version [<root>]                     the version text a build stamps (feature clang)
  llvm-version                         the libLLVM this process uses";

pub fn fail(msg: impl std::fmt::Display) -> ! {
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
        Some("link") => link_cmd(&argv[1..]),
        Some("lift-compile") => lift_compile(&argv[1..]),
        Some("dump") => dump::command(&argv[1..]),
        Some("textures") => textures::command(&argv[1..]),
        Some("overlay") => textures::overlay_command(&argv[1..]),
        #[cfg(feature = "clang")]
        Some("build") => build(&argv[1..]),
        Some("objeq") => {
            let [a, b] = &argv[1..] else { fail("objeq <a.o> <b.o>") };
            let read = |p: &String| std::fs::read(p).unwrap_or_else(|e| fail(format!("{p}: {e}")));
            let (same, lines) = objeq::compare(&read(a), &read(b)).unwrap_or_else(|e| fail(e));
            println!("{}", lines.join("\n"));
            std::process::exit(if same { 0 } else { 1 });
        }
        #[cfg(feature = "clang")]
        Some("compile-c") => {
            let Some(k) = argv.iter().position(|a| a == "--") else { fail("compile-c <out.o> -- <clang args>...") };
            let [out] = &argv[1..k] else { fail("compile-c <out.o> -- <clang args>...") };
            let t = Instant::now();
            let o = clang::compile(&argv[k + 1..]).unwrap_or_else(|e| fail(e));
            std::fs::write(out, &o).unwrap_or_else(|e| fail(format!("{out}: {e}")));
            println!("{out}: {} bytes in {:.2} s", o.len(), t.elapsed().as_secs_f64());
        }
        #[cfg(feature = "clang")]
        Some("version") => println!("{}", version::describe(std::path::Path::new(argv.get(1).map_or(".", |s| s.as_str())))),
        Some("llvm-version") => println!("{}", llvm::version()),
        _ => println!("{HELP}"),
    }
}

/// `compile-all <lift dir> <out dir> [-j N] [--check <obj dir>]`: every lift_*.ll and gen/*.ll of a lift, compiled
/// (lift_h*: hot, the rest cold); --check compares each object with the one of the same name there
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
    // (the partitions, lift_*.ll, and the generated code, gen/*.ll, compiled as lift_answers is)
    let list = |d: &std::path::Path| -> Vec<PathBuf> {
        std::fs::read_dir(d).unwrap_or_else(|e| fail(format!("{}: {e}", d.display()))).filter_map(|e| e.ok()).map(|e| e.path()).collect()
    };
    let mut parts: Vec<(PathBuf, u64, compile::Level)> = list(src)
        .into_iter()
        .filter(|p| p.file_name().unwrap().to_string_lossy().starts_with("lift_"))
        .chain(list(&src.join("gen")))
        .filter(|p| p.extension().is_some_and(|x| x == "ll"))
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

/// `link <out> <objs.rsp> [--order <hot_order.bin>] [--map <file>] [--print] [-- <libs>...]`
fn link_cmd(args: &[String]) {
    let (mut pos, mut order, mut map, mut libs, mut print) = (Vec::new(), None::<PathBuf>, None::<PathBuf>, Vec::new(), false);
    let mut label_file = None::<PathBuf>;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--order" => order = it.next().map(PathBuf::from),
            "--map" => map = it.next().map(PathBuf::from),
            "--print" => print = true,
            "--labels" => label_file = it.next().map(PathBuf::from),
            "--" => libs.extend(it.by_ref().cloned()),
            _ => pos.push(PathBuf::from(a)),
        }
    }
    let [out, rsp] = &pos[..] else { fail("link <out> <objs.rsp> [--order <hot_order.bin>] [--map <file>] [-- <libs>...]") };
    let objects: Vec<PathBuf> = std::fs::read_to_string(rsp)
        .unwrap_or_else(|e| fail(format!("{}: {e}", rsp.display())))
        .lines()
        .map(|l| PathBuf::from(l.trim().trim_matches('"')))
        .filter(|p| !p.as_os_str().is_empty())
        .collect();
    let sys = link::Sysroot::find().unwrap_or_else(|e| fail(e));
    let dir = out.parent().filter(|d| !d.as_os_str().is_empty()).unwrap_or(std::path::Path::new("."));
    let order = order.map(|b| link::order_file(&b, dir).unwrap_or_else(|e| fail(e)));
    let x = link::Exe { out, map: map.as_deref(), objects: &objects, order: order.as_deref(), libs: &libs };
    let a = link::exe_args(&x, &sys);
    if print {
        println!("{}", a.join(" "));
    }
    let t = Instant::now();
    match link::lld(&a) {
        Ok(m) => {
            print!("{m}");
            println!("linked {}: {} MB in {:.1} s", out.display(), std::fs::metadata(out).map_or(0, |m| m.len() >> 20), t.elapsed().as_secs_f64());
        }
        Err(m) => fail(format!("the link failed:\n{m}")),
    }
    if let Some(f) = label_file {
        let l = labels::read(&f).unwrap_or_else(|e| fail(e));
        let (n, missing) = labels::apply(out, &l).unwrap_or_else(|e| fail(e));
        println!("labels: {n} functions named{}", if missing.is_empty() { String::new() } else {
            format!("; no function for {}", missing.iter().map(|a| format!("{a:08x}")).collect::<Vec<_>>().join(", ")) });
    }
}

/// `lift-compile <out dir> [-j N] [--check <obj dir>] OPTS`: ounce-lift's pipeline and the compile in this process
fn lift_compile(args: &[String]) {
    let jobs = std::thread::available_parallelism().map_or(1, |n| n.get());
    let mut run = ounce_lift::pipeline::Run { jobs, hot_first: true, ..Default::default() };
    let (mut cjobs, mut out, mut check, mut cache_dir) = (jobs, None::<PathBuf>, None::<PathBuf>, None::<PathBuf>);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || PathBuf::from(it.next().unwrap_or_else(|| fail(format!("{a} wants a value"))));
        match a.as_str() {
            "-j" => cjobs = val().to_string_lossy().parse().unwrap_or_else(|_| fail("-j N")),
            "--check" => check = Some(val()),
            "--cache" => cache_dir = Some(val()),
            "--game" => run.opts.game = val(),
            "--work" => run.opts.work = val(),
            "--root" => run.opts.root = val(),
            "--exefs" => run.opts.exefs = Some(val()),
            _ if out.is_none() && !a.starts_with('-') => out = Some(PathBuf::from(a)),
            _ => fail(format!("lift-compile: what is {a}?")),
        }
    }
    let out = out.unwrap_or_else(|| fail("lift-compile <out dir> [-j N] [--check <obj dir>] [--game <dir>] [--work <dir>] [--root <dir>] [--exefs <dir>]"));
    let t = Instant::now();
    let cache = cache_dir.map(|d| cache::Cache::open(&d).unwrap_or_else(|e| fail(e)));
    let gate = gate::Gate::new();
    let aux = lift::Aux { gen: None, gen_parts: false, gate: &gate, progress: None };
    let d = lift::lift_compile(&run, &out, cjobs, cache.as_ref(), check.as_deref(), &aux).unwrap_or_else(|e| fail(e));
    let (cpu, hwm) = ounce_lift::pipeline::usage();
    println!(
        "lift-compile: {} objects, {:.0} s wall, {:.0} s in compiles, {:.0} s CPU, peak {:.0} MB{}",
        d.objects.len(),
        t.elapsed().as_secs_f64(),
        d.compile_s,
        cpu,
        hwm,
        if check.is_some() { format!(", {} differ", d.differ.len()) } else { String::new() }
    );
    if !d.differ.is_empty() {
        println!("differ: {}", d.differ.join(" "));
        std::process::exit(1);
    }
}

/// the clang resource folder: --resource-dir, else the static LLVM's build tree's
#[cfg(feature = "clang")]
const RESOURCE_DIR: &str = env!("HK_CLANG_RESOURCE_DIR");

/// `build <out dir> --direct <so> ...`: the game folder (hookshot, hookshot_direct.so, image.bin) from the dump,
/// and its checks
#[cfg(feature = "clang")]
/// Puts `from` (moved, or copied beside `to` first) at `to` as a new file: synced, then renamed over the old one. A game
/// running from the folder maps hookshot_direct.so and the executable; a copy over them in place (truncate + write)
/// changed its code under it and crashed it. The rename leaves it the old files.
fn install(from: &std::path::Path, to: &std::path::Path, moved: bool) -> Result<(), String> {
    let err = |p: &std::path::Path, e: std::io::Error| format!("{}: {e}", p.display());
    let tmp = if moved { from.to_path_buf() } else { to.with_extension("install.tmp") };
    if !moved {
        std::fs::copy(from, &tmp).map_err(|e| format!("{} -> {}: {e}", from.display(), tmp.display()))?;
    }
    std::fs::File::open(&tmp).and_then(|f| f.sync_all()).map_err(|e| err(&tmp, e))?;
    std::fs::rename(&tmp, to).map_err(|e| err(to, e))?;
    let dir = to.parent().filter(|d| !d.as_os_str().is_empty()).unwrap_or(std::path::Path::new("."));
    std::fs::File::open(dir).and_then(|f| f.sync_all()).map_err(|e| err(dir, e))
}

fn build(args: &[String]) {
    let jobs = gate::default_jobs();
    let mut run = ounce_lift::pipeline::Run { jobs: std::thread::available_parallelism().map_or(1, |n| n.get()), hot_first: true, ..Default::default() };
    let (mut cjobs, mut out, mut check, mut direct) = (jobs, None::<PathBuf>, None::<PathBuf>, None::<PathBuf>);
    let (mut cache_dir, mut version, mut resource) = (None::<PathBuf>, None::<String>, PathBuf::from(RESOURCE_DIR));
    let (mut gen, mut gen_parts, mut progress) = (None::<PathBuf>, false, false);
    let (mut dump_src, mut dump_dir, mut title) = (None::<PathBuf>, None::<PathBuf>, None::<String>);
    let (mut prepared, mut fast_textures, mut romfs_dir) = (None::<PathBuf>, false, None::<PathBuf>);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "-j" => cjobs = val().parse().unwrap_or_else(|_| fail("-j N")),
            "--check" => check = Some(val().into()),
            "--direct" => direct = Some(val().into()),
            "--cache" => cache_dir = Some(val().into()),
            "--version" => version = Some(val()),
            "--resource-dir" => resource = val().into(),
            "--gen" => gen = Some(val().into()),
            "--gen-parts" => gen_parts = true,
            "--progress" => progress = true,
            "--game" => run.opts.game = val().into(),
            "--work" => run.opts.work = val().into(),
            "--root" => run.opts.root = val().into(),
            "--exefs" => run.opts.exefs = Some(val().into()),
            "--dump" => dump_src = Some(val().into()),
            "--dump-dir" => dump_dir = Some(val().into()),
            "--title" => title = Some(val()),
            "--prepared" => prepared = Some(val().into()),
            "--fast-textures" => fast_textures = true,
            "--romfs" => romfs_dir = Some(val().into()),
            _ if out.is_none() && !a.starts_with('-') => out = Some(PathBuf::from(a)),
            _ => fail(format!("build: what is {a}?")),
        }
    }
    let (Some(out), Some(direct)) = (out, direct) else { fail("build <out dir> --direct <hookshot_direct.so> [...]") };
    let major = llvm::version().split('.').next().and_then(|m| m.parse::<u32>().ok());
    if major != Some(LLVM_MAJOR) {
        fail(format!("this process runs LLVM {}, but the lifter's IR is written for LLVM {LLVM_MAJOR}", llvm::version()));
    }
    let t0 = Instant::now();
    // (the exefs first, so the lift starts while the romfs is written)
    let dump_root = dump_dir.clone().unwrap_or_else(|| run.opts.work.join("dump"));
    let romfs_dir = romfs_dir.unwrap_or_else(|| dump_root.join("romfs"));
    let romfs = dump_src.map(|src| {
        let dir = dump_dir.unwrap_or_else(|| run.opts.work.join("dump"));
        let (exefs, romfs) = dump::exefs_then_romfs(&src, title.as_deref(), &dir, &run.opts.game).unwrap_or_else(|e| fail(e));
        run.opts.exefs = Some(exefs);
        romfs
    });
    let root = run.opts.root.clone();
    let version = version.unwrap_or_else(|| version::describe(&root));
    let obj = out.join("obj");
    let rt64 = out.join("rt64");
    for d in [&obj, &rt64] {
        std::fs::create_dir_all(d).unwrap_or_else(|e| fail(format!("{}: {e}", d.display())));
    }
    for (name, text) in ounce_lift::gen::rt64(&root).unwrap_or_else(|e| fail(e)) {
        std::fs::write(rt64.join(name), text).unwrap_or_else(|e| fail(e));
    }
    let cache_dir = cache_dir.unwrap_or_else(|| run.opts.work.join("builder-cache"));
    let cache = cache::Cache::open(&cache_dir).unwrap_or_else(|e| fail(e));
    let gate = gate::Gate::new();
    let prog = progress::Progress::new(progress, cjobs, &cache_dir);
    if gen_parts && gen.is_none() {
        fail("--gen-parts wants --gen <dir>");
    }
    if let Some(g) = gen.as_deref() {
        // (an earlier lift's partitions: the count changes with the code, and without --gen-parts none are current)
        for e in std::fs::read_dir(g).into_iter().flatten().flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n.starts_with("lift_") && n.ends_with(".ll") {
                let _ = std::fs::remove_file(e.path());
            }
        }
    }
    let aux = lift::Aux { gen: gen.as_deref(), gen_parts, gate: &gate, progress: Some(&prog) };
    println!("build: {version}; {cjobs} jobs, the memory gate {}", if std::env::var("HOOKSHOT_MEM_GATE").is_ok_and(|v| v == "0") { "off" } else { "on" });
    compile::init().unwrap_or_else(|e| fail(e));
    let game = ounce_lift::game::Game::load(&run.opts.game).unwrap_or_else(|e| fail(e));
    // (the host's C at LLVM's defaults, then the runtime at its two -mllvm options, then the lift and its compile)
    let t = Instant::now();
    let host_jobs = host::host_jobs(&root, &rt64, &game.host, &version).unwrap_or_else(|e| fail(e));
    let rt = host::rt_job(&root, &rt64);
    let hits = host::compile(&host_jobs, &obj, cjobs, Some(&cache), &resource, &aux).unwrap_or_else(|e| fail(e))
        + host::compile(std::slice::from_ref(&rt), &obj, 1, Some(&cache), &resource, &aux).unwrap_or_else(|e| fail(e));
    println!("host C: {} files ({hits} from the cache) in {:.1} s", host_jobs.len() + 1, t.elapsed().as_secs_f64());
    // (the patch declarations the lift scans, against what the compiler made of them)
    let host_objs: Vec<PathBuf> = host_jobs.iter().filter(|j| j.obj.starts_with("host_")).map(|j| obj.join(&j.obj)).collect();
    match ounce_lift::patch::check_records(&root, &game.host, &host_objs) {
        Ok(line) => println!("{line}"),
        Err(e) => fail(e),
    }
    let d = lift::lift_compile(&run, &obj, cjobs, Some(&cache), check.as_deref(), &aux).unwrap_or_else(|e| fail(e));
    println!(
        "lift and compile: {} modules ({} from the cache), the lift's state freed at {:.0} s; {:.0} s",
        d.objects.len(),
        d.cached,
        d.lift_s,
        t0.elapsed().as_secs_f64()
    );
    if !d.differ.is_empty() {
        fail(format!("objects unlike the check folder's: {}", d.differ.join(" ")));
    }
    prog.save();
    // (the link's order: the partitions largest IR first, the generated code, the runtime, the host)
    let mut parts: Vec<&(String, usize)> = d.objects.iter().filter(|o| o.0.starts_with("lift_")).collect();
    parts.sort_by_key(|o| std::cmp::Reverse(o.1));
    let mut objects: Vec<PathBuf> = parts.iter().map(|o| obj.join(format!("{}.o", o.0))).collect();
    for g in ["a64_table", "a64_hooks", "a64_code_slots", "a64_stubs", "hos_image", "nvn2_exports"] {
        objects.push(obj.join(format!("{g}.o")));
    }
    objects.push(obj.join(&rt.obj));
    objects.extend(host_jobs.iter().map(|j| obj.join(&j.obj)));
    // (linked and labelled beside, then renamed over the old one: see install)
    let exe = out.join("hookshot.new");
    let map = out.join("hookshot.map");
    let game_dir = if run.opts.game.is_dir() { run.opts.game.clone() } else { run.opts.game.parent().unwrap().to_path_buf() };
    let order_bin = game_dir.join("hot_order.bin");
    let order = order_bin.exists().then(|| link::order_file(&order_bin, &out).unwrap_or_else(|e| fail(e)));
    let libs: Vec<String> = ["-lavcodec", "-lswscale", "-lswresample", "-lavutil"].map(String::from).to_vec();
    let sys = link::Sysroot::find().unwrap_or_else(|e| fail(e));
    let x = link::Exe { out: &exe, map: Some(&map), objects: &objects, order: order.as_deref(), libs: &libs };
    let t = Instant::now();
    if let Err(m) = link::lld(&link::exe_args(&x, &sys)) {
        fail(format!("the link failed:\n{m}"));
    }
    let l = labels::read(&game.labels).unwrap_or_else(|e| fail(e));
    let (n, _) = labels::apply(&exe, &l).unwrap_or_else(|e| fail(e));
    println!("linked {} ({} MB, {n} functions named) in {:.1} s", out.join("hookshot").display(), std::fs::metadata(&exe).map_or(0, |m| m.len() >> 20), t.elapsed().as_secs_f64());
    checks::backend_symbols(&direct, &exe).unwrap_or_else(|e| fail(e));
    // (the folder the game runs from: the image beside the executable, where hos_main.c looks without an argument)
    for (from, to) in [(direct.clone(), out.join("hookshot_direct.so")), (obj.join("image.bin"), out.join("image.bin"))] {
        if from != to {
            install(&from, &to, false).unwrap_or_else(|e| fail(e));
        }
    }
    install(&exe, &out.join("hookshot"), true).unwrap_or_else(|e| fail(e));
    if let Some(r) = romfs {
        dump::finish_romfs(r).unwrap_or_else(|e| fail(e));
    }
    // (the prepared assets: the texture store and the game's overlay, from the romfs)
    if let Some(p) = &prepared {
        textures::make(&romfs_dir, &p.join("tex"), &game, fast_textures, cjobs, Vec::new()).unwrap_or_else(|e| fail(e));
        textures::overlay(&romfs_dir, &p.join("terrain"), &game).unwrap_or_else(|e| fail(e));
    }
    let days: f64 = std::env::var("HOOKSHOT_OBJCACHE_DAYS").ok().and_then(|v| v.parse().ok()).unwrap_or(3.0);
    let (pn, pf, pk) = cache.prune(days);
    let (cpu, hwm) = ounce_lift::pipeline::usage();
    println!(
        "build: {:.1} s wall, {cpu:.0} s CPU, peak {hwm:.0} MB; cache: {pn} files unused for {days} days removed ({} MB), {} MB kept",
        t0.elapsed().as_secs_f64(),
        pf >> 20,
        pk >> 20
    );
}

#[cfg(test)]
mod tests {
    use std::io::{Read, Seek};

    #[test]
    fn install_leaves_an_open_file_its_old_contents() {
        let d = std::env::temp_dir().join(format!("hk-install-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let (new, live) = (d.join("new.so"), d.join("live.so"));
        std::fs::write(&live, b"old contents").unwrap();
        std::fs::write(&new, b"new").unwrap();
        let mut held = std::fs::File::open(&live).unwrap();
        super::install(&new, &live, false).unwrap();
        assert!(new.exists(), "a copy keeps its source");
        let mut s = String::new();
        held.seek(std::io::SeekFrom::Start(0)).unwrap();
        held.read_to_string(&mut s).unwrap();
        assert_eq!(s, "old contents");
        assert_eq!(std::fs::read(&live).unwrap(), b"new");
        super::install(&new, &live, true).unwrap();
        assert!(!new.exists(), "a move takes its source");
        assert_eq!(std::fs::read_dir(&d).unwrap().count(), 1, "no temporary file left");
        std::fs::remove_dir_all(&d).unwrap();
    }
}
