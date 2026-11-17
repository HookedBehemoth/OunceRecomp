//! ounce-lift [<export dir>] <out dir> [-j N] [--parts a,b,...] [--skip] [--check] [--golden <dir>] [--game <dir>]
//! [--work <dir>] [--exefs <dir>] [--root <dir>]: the whole lift from the dump's exefs, the host's C and the game
//! description: gen/, image/image.bin and one .ll per partition (EXPORT.md).
//! --skip: a function using a template not ported yet is left out and counted ("skipped: template X").
//! --check: compare the scan, the image, the plan, every summary and record with Python's (the export) and stop.
//! --golden: gen/ and image.bin against the golden's sha256.

use ounce_lift::game::Game;
use ounce_lift::gen;
use ounce_lift::patch;
use ounce_lift::pipeline;
use ounce_lift::session::Opts;
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::Mutex;

mod tools;

#[cfg(feature = "count-alloc")]
mod count {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
    pub static N: AtomicU64 = AtomicU64::new(0);
    pub static BYTES: AtomicU64 = AtomicU64::new(0);
    pub struct Counting;
    unsafe impl GlobalAlloc for Counting {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            N.fetch_add(1, Relaxed);
            BYTES.fetch_add(l.size() as u64, Relaxed);
            System.alloc(l)
        }
        unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
            System.dealloc(p, l)
        }
        unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
            N.fetch_add(1, Relaxed);
            BYTES.fetch_add(n.saturating_sub(l.size()) as u64, Relaxed);
            System.realloc(p, l, n)
        }
    }
    #[global_allocator]
    static A: Counting = Counting;
}

/// the generated files (gen/) and image.bin against the golden's sha256 (diffout --store's meta.json: "files" for gen/,
/// "image_sha256"); the partitions and facts.tsv are diffout's to compare. The count of files that differ or that
/// only one side has
fn check_golden(golden: &std::path::Path, files: &[(String, Vec<u8>)], image: &[u8]) -> usize {
    use sha2::{Digest, Sha256};
    let meta: serde_json::Value = serde_json::from_slice(&std::fs::read(golden.join("meta.json")).expect("golden meta.json")).unwrap();
    let hex = |d: &[u8]| Sha256::digest(d).iter().map(|b| format!("{b:02x}")).collect::<String>();
    let want: BTreeMap<&str, &str> = meta["files"]
        .as_object()
        .expect("golden files")
        .iter()
        .filter(|(k, _)| k.starts_with("gen/"))
        .map(|(k, v)| (k.as_str(), v.as_str().unwrap_or("")))
        .collect();
    let (mut bad, mut equal) = (0, 0);
    for (name, data) in files {
        match want.get(name.as_str()) {
            Some(w) if *w == hex(data) => equal += 1,
            Some(_) => {
                bad += 1;
                println!("golden: {name} differs");
            }
            None => {
                bad += 1;
                println!("golden: {name} is not in the golden");
            }
        }
    }
    for name in want.keys().filter(|k| !files.iter().any(|f| f.0 == **k)) {
        bad += 1;
        println!("golden: {name} is in the golden only");
    }
    if meta["image_sha256"].as_str() == Some(hex(image).as_str()) {
        equal += 1;
    } else {
        bad += 1;
        println!("golden: image/image.bin differs");
    }
    println!("golden: {equal} of {} files equal (gen/, image.bin)", want.len() + 1);
    bad
}

/// `ounce-lift records [--game <dir>] [--root <dir>] <object>...`: the host objects' hos_patch records against the scan
fn records(argv: Vec<String>) {
    let (mut game_dir, mut root, mut objs) = (PathBuf::from("games/oot"), PathBuf::from("."), Vec::new());
    let mut args = argv.into_iter();
    while let Some(a) = args.next() {
        match a.as_str() {
            "--game" => game_dir = PathBuf::from(args.next().unwrap()),
            "--root" => root = PathBuf::from(args.next().unwrap()),
            _ => objs.push(PathBuf::from(a)),
        }
    }
    let game = Game::load(&game_dir).unwrap_or_else(|e| fail(e));
    match patch::check_records(&root, &game.host, &objs) {
        Ok(line) => println!("{line}"),
        Err(e) => fail(e),
    }
}

/// `ounce-lift rt64 <dir> [--root <dir>]`: the guest model (a64.h, a64_rt.c), which needs no dump
fn rt64(argv: Vec<String>) {
    let (mut root, mut out) = (PathBuf::from("."), None);
    let mut args = argv.into_iter();
    while let Some(a) = args.next() {
        match a.as_str() {
            "--root" => root = PathBuf::from(args.next().unwrap()),
            _ => out = Some(PathBuf::from(a)),
        }
    }
    let out = out.unwrap_or_else(|| fail("usage: ounce-lift rt64 <dir> [--root <dir>]"));
    std::fs::create_dir_all(&out).unwrap_or_else(|e| fail(e));
    for (name, text) in gen::rt64(&root).unwrap_or_else(|e| fail(e)) {
        std::fs::write(out.join(name), text).unwrap_or_else(|e| fail(e));
    }
}

/// the options every subcommand that lifts takes
const OPTS: &str = "[--game <dir>] [--work <dir>] [--root <dir>] [--exefs <dir>]";

/// takes one of OPTS (and its value) into o: false when a is none of them
fn opt_arg(o: &mut Opts, a: &str, args: &mut impl Iterator<Item = String>) -> bool {
    let mut val = || PathBuf::from(args.next().unwrap_or_else(|| fail(format!("{a} wants a value"))));
    match a {
        "--game" => o.game = val(),
        "--work" => o.work = val(),
        "--root" => o.root = val(),
        "--exefs" => o.exefs = Some(val()),
        "--max-func" => o.max_func = val().to_string_lossy().parse().unwrap_or_else(|_| fail("--max-func N")),
        _ => return false,
    }
    true
}

/// the message on stderr, exit status 1
fn fail(msg: impl std::fmt::Display) -> ! {
    eprintln!("ounce-lift: {msg}");
    std::process::exit(1)
}

const HELP: &str = "ounce-lift: the lifter. Subcommands (each but rt64, records and scan reads the dump; OPTS: --game <dir> (games/oot), --work <dir> (work), --root <dir> (.), --exefs <dir>):
  [lift] <out> [-j N] [--parts a,b] [--skip] [--golden <dir>] OPTS   the lift: <out>/gen, image/image.bin, lift_*.ll
  rt64 <dir> [--root <dir>]                                         the guest model (a64.h, a64_rt.c)
  records [--game] [--root] <object>...                             the host objects' hos_patch records against the scan
  scan [--game] [--root] [<out.json>]                               the host's declarations as the lift reads them, as JSON
  site <address|label>... OPTS                                      what a patch site is: paths, liveness, record, a PATCH_AT to paste
  check [<file:line> | - | --all] OPTS                              the declarations against the plan; one: its cost (the summaries' difference)
  report OPTS                                                       every declaration's local cost, costliest first
  dump <address|label>... [--tree] [--opt O1|O2] [--asm] [-o <dir>] OPTS   a function's guest code, IR and LLVM IR";

fn main() {
    let mut argv: Vec<String> = std::env::args().skip(1).collect();
    match argv.first().map(String::as_str) {
        Some("records") => records(argv.split_off(1)),
        Some("rt64") => rt64(argv.split_off(1)),
        Some("scan") => tools::scan(argv.split_off(1)),
        Some("site") => tools::site(argv.split_off(1)),
        Some("check") => tools::check(argv.split_off(1)),
        Some("report") => tools::report(argv.split_off(1)),
        Some("dump") => tools::dump(argv.split_off(1)),
        Some("-h" | "--help" | "help") => println!("{}", HELP),
        Some("lift") => lift(argv.split_off(1)),
        _ => lift(argv),
    }
}

/// `ounce-lift [lift] ...`: the whole lift into a folder (pipeline::lift)
fn lift(argv: Vec<String>) {
    let mut args = argv.into_iter();
    let mut out = None::<PathBuf>;
    let mut r = pipeline::Run { jobs: std::thread::available_parallelism().map_or(1, |n| n.get()), ..Default::default() };
    let mut golden = None::<PathBuf>;
    while let Some(a) = args.next() {
        match a.as_str() {
            "-j" => r.jobs = args.next().unwrap().parse().unwrap(),
            "--parts" => r.only = Some(args.next().unwrap().split(',').map(String::from).collect()),
            "--skip" => r.skip = true,
            "--golden" => golden = Some(PathBuf::from(args.next().unwrap())),
            _ if opt_arg(&mut r.opts, &a, &mut args) => {}
            _ if out.is_none() && !a.starts_with('-') => out = Some(PathBuf::from(a)),
            _ => fail(format!("lift: what is {a}? usage: ounce-lift [lift] <out dir> [-j N] [--parts a,b] [--skip] [--golden <dir>] {OPTS}")),
        }
    }
    let out = out.unwrap_or_else(|| fail(format!("usage: ounce-lift [lift] <out dir> [-j N] [--parts a,b] [--skip] [--golden <dir>] {OPTS}")));
    std::fs::create_dir_all(&out).unwrap_or_else(|e| fail(format!("{}: {e}", out.display())));
    let dir = Dir { root: out, gen: Mutex::new(Vec::new()), image: Mutex::new(Vec::new()) };
    pipeline::lift(&r, &dir).unwrap_or_else(|e| fail(e));
    if let Some(g) = golden {
        let (gen, image) = (dir.gen.into_inner().unwrap(), dir.image.into_inner().unwrap());
        if check_golden(&g, &gen, &image) > 0 {
            std::process::exit(1);
        }
    }
}

/// The lift's output as files under `root`; gen/ and the image also kept for the golden check
struct Dir {
    root: PathBuf,
    gen: Mutex<Vec<(String, Vec<u8>)>>,
    image: Mutex<Vec<u8>>,
}

impl pipeline::Out for Dir {
    fn file(&self, rel: &str, data: Vec<u8>) {
        let p = self.root.join(rel);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(&p, &data).unwrap_or_else(|e| fail(format!("{}: {e}", p.display())));
        if rel.starts_with("gen/") {
            self.gen.lock().unwrap().push((rel.to_string(), data));
        } else if rel == "image/image.bin" {
            *self.image.lock().unwrap() = data;
        }
    }

    fn part(&self, name: &str, head: &[u8], body: &[u8]) {
        use std::io::Write;
        let p = self.root.join(format!("{name}.ll"));
        let mut f = std::fs::File::create(&p).unwrap_or_else(|e| fail(format!("{}: {e}", p.display())));
        f.write_all(head).and_then(|_| f.write_all(body)).unwrap_or_else(|e| fail(format!("{}: {e}", p.display())));
    }
}
