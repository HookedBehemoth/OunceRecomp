//! The game-free checks' helpers: the repository's files, functions cut out of source text, C and C++ fixtures compiled
//! with the system's compiler and run, the host's patch declarations as the lifter scans them. The checks themselves
//! are in tests/ (one test binary); their C and C++ fixtures in fixtures/, read when a check runs.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::OnceLock;

pub use ounce_lift::patch::{Func, Guest, PatchSet, Replaced, Row};

/// the repository's top folder
pub fn root() -> &'static Path {
    static ROOT: OnceLock<PathBuf> = OnceLock::new();
    ROOT.get_or_init(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap())
}

/// a file of the repository, by its path from the top
pub fn path(rel: &str) -> PathBuf {
    root().join(rel)
}

/// a file of the repository as text
pub fn read(rel: &str) -> String {
    std::fs::read_to_string(path(rel)).unwrap_or_else(|e| panic!("{rel}: {e}"))
}

/// a fixture (fixtures/<name>) as text
pub fn fixture(name: &str) -> String {
    read(&format!("ounce/tests/fixtures/{name}"))
}

/// a fixture's path
pub fn fixture_path(name: &str) -> PathBuf {
    path(&format!("ounce/tests/fixtures/{name}"))
}

/// where marker starts in text (it must be there)
pub fn find(text: &str, marker: &str) -> usize {
    text.find(marker).unwrap_or_else(|| panic!("the source has no {marker:?}"))
}

/// from marker to the brace that closes the first one after it
pub fn block<'a>(text: &'a str, marker: &str) -> &'a str {
    let start = find(text, marker);
    let b = text.as_bytes();
    let mut end = start + text[start..].find('{').unwrap_or_else(|| panic!("no brace after {marker:?}")) + 1;
    let mut depth = 1;
    while depth > 0 {
        match b[end] {
            b'{' => depth += 1,
            b'}' => depth -= 1,
            _ => {}
        }
        end += 1;
    }
    &text[start..end]
}

/// from marker to the end of the first line after it that starts with a closing brace (a top-level function)
pub fn block_top<'a>(text: &'a str, marker: &str) -> &'a str {
    let start = find(text, marker);
    let mut at = start;
    for line in text[start..].split_inclusive('\n') {
        if line.starts_with('}') {
            return &text[start..at + 1];
        }
        at += line.len();
    }
    panic!("{marker:?}: no closing brace at a line's start")
}

/// the text between the markers' starts (from, up to to)
pub fn between<'a>(text: &'a str, from: &str, to: &str) -> &'a str {
    let s = find(text, from);
    let e = s + find(&text[s..], to);
    &text[s..e]
}

/// a folder for one check's files, removed when dropped
pub struct Tmp(PathBuf);

impl Tmp {
    pub fn new(name: &str) -> Tmp {
        static N: AtomicU32 = AtomicU32::new(0);
        let p = std::env::temp_dir()
            .join(format!("ounce-test-{name}-{}-{}", std::process::id(), N.fetch_add(1, Ordering::Relaxed)));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        Tmp(p)
    }
    pub fn path(&self) -> &Path {
        &self.0
    }
    pub fn join(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
    /// writes a file into the folder; its path
    pub fn write(&self, name: &str, text: &str) -> PathBuf {
        let p = self.join(name);
        std::fs::write(&p, text).unwrap();
        p
    }
}

impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// a program on the PATH (the check fails without it)
pub fn tool(name: &str) -> PathBuf {
    which(name).unwrap_or_else(|| panic!("{name} is not on the PATH: the check needs it"))
}

/// a program on the PATH, if there is one
pub fn which(name: &str) -> Option<PathBuf> {
    let p = Path::new(name);
    if p.is_absolute() {
        return p.is_file().then(|| p.to_path_buf());
    }
    std::env::split_paths(&std::env::var_os("PATH")?).map(|d| d.join(name)).find(|p| p.is_file())
}

/// the C compiler: $CC, else clang
pub fn cc() -> String {
    std::env::var("CC").ok().filter(|s| !s.is_empty()).unwrap_or_else(|| "clang".into())
}

/// the C++ compiler: $CXX, else clang++
pub fn cxx() -> String {
    std::env::var("CXX").ok().filter(|s| !s.is_empty()).unwrap_or_else(|| "clang++".into())
}

/// the include folder of a repository path, as a -I flag
pub fn inc(rel: &str) -> String {
    format!("-I{}", path(rel).display())
}

/// a command's output; panics with all of it unless it succeeded
pub fn run(cmd: &mut Command) -> Output {
    let out = output(cmd);
    if !out.status.success() {
        panic!("{cmd:?} failed ({}):\n{}{}", out.status, text(&out.stdout), text(&out.stderr));
    }
    out
}

/// the longest a command may run before the check fails (a hung fixture must not hang cargo test)
const TIMEOUT: std::time::Duration = std::time::Duration::from_secs(300);

/// a command's output, whatever its status (no input); killed and failed after TIMEOUT
pub fn output(cmd: &mut Command) -> Output {
    use std::io::Read;
    let mut child = cmd
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap_or_else(|e| panic!("{cmd:?}: {e}"));
    let drain = |mut r: Box<dyn Read + Send>| {
        std::thread::spawn(move || {
            let mut b = Vec::new();
            let _ = r.read_to_end(&mut b);
            b
        })
    };
    let (out, err) = (drain(Box::new(child.stdout.take().unwrap())), drain(Box::new(child.stderr.take().unwrap())));
    let start = std::time::Instant::now();
    let status = loop {
        if let Some(s) = child.try_wait().unwrap() {
            break s;
        }
        if start.elapsed() > TIMEOUT {
            let _ = child.kill();
            let _ = child.wait();
            panic!("{cmd:?} still ran after {} s:\n{}{}", TIMEOUT.as_secs(), text(&out.join().unwrap()), text(&err.join().unwrap()));
        }
        std::thread::sleep(std::time::Duration::from_millis(2));
    };
    Output { status, stdout: out.join().unwrap(), stderr: err.join().unwrap() }
}

pub fn text(b: &[u8]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

/// compiles with compiler and args (sources, flags) into exe, panicking with the compiler's messages; exe. Every fixture
/// gets the settings' accessors (fixtures/settings_fixture.h: the defaults, or FIXTURE_SET's values)
pub fn compile<S: AsRef<std::ffi::OsStr>>(compiler: &str, args: &[S], exe: &Path) -> PathBuf {
    let settings = [inc("renderer/api"), "-include".into(), fixture_path("settings_fixture.h").display().to_string()];
    run(Command::new(compiler).args(settings).args(args).arg("-o").arg(exe));
    exe.to_path_buf()
}

/// a command of exe with no FIXTURE_SET of the test's environment (the settings a fixture is given are its own)
pub fn clean(exe: &Path) -> Command {
    let mut c = Command::new(exe);
    c.env_remove("FIXTURE_SET");
    c
}

/// the host's patch declarations as the lift reads them (ounce-lift scan), once per run
pub struct Scan {
    pub ps: PatchSet,
    /// At rows
    pub at: Vec<Row>,
    /// Value probes
    pub probes: Vec<Row>,
    effects: BTreeMap<String, Replaced>,
}

impl Scan {
    /// the declaration of a replaced function: its HLE, else HLE_EFFECTS, else NV
    pub fn effects(&self, symbol: &str) -> Option<&Replaced> {
        self.effects.get(symbol)
    }
}

pub fn scan() -> &'static Scan {
    static SCAN: OnceLock<Scan> = OnceLock::new();
    SCAN.get_or_init(|| {
        use ounce_lift::{game::Game, patch, session};
        let game = Game::load(&path("games/oot")).unwrap();
        let ps = patch::scan_texts(&session::host_texts(root(), &game).unwrap()).unwrap();
        let (probes, at) = patch::rows(&ps.sites).unwrap().into_iter().partition(|r| r.value);
        let mut effects = BTreeMap::new();
        for m in [&ps.nv, &ps.declared, &ps.hle] {
            for (_, r) in m.iter() {
                effects.insert(r.symbol.clone(), r.clone());
            }
        }
        Scan { ps, at, probes, effects }
    })
}

/// the At rows whose host function is fname
pub fn rows_of(fname: &str) -> Vec<&'static Row> {
    scan().at.iter().filter(|r| r.fname == fname).collect()
}

/// the guest model (a64.h, a64_rt.c: ounce-lift rt64) written into folder
pub fn rt64(folder: &Path) {
    std::fs::create_dir_all(folder).unwrap();
    for (name, text) in ounce_lift::gen::rt64(root()).unwrap() {
        std::fs::write(folder.join(name), text).unwrap();
    }
}

/// runs a Python script of the repository (python3 -I -B) with args; its output, panicking unless it succeeded
pub fn python(rel: &str, args: &[&str]) -> Output {
    run(Command::new(python3()).arg("-I").arg("-B").arg(path(rel)).args(args).current_dir(root()))
}

/// python3 on the PATH
pub fn python3() -> PathBuf {
    tool("python3")
}

/// the one `#define <name> ...` line of each name, joined (each must be defined once at a line's start)
pub fn defines(text: &str, names: &[&str]) -> String {
    let mut out = Vec::new();
    for name in names {
        let hits: Vec<&str> = text
            .lines()
            .filter(|l| {
                l.strip_prefix("#define ").and_then(|r| r.strip_prefix(name)).is_some_and(|r| r.starts_with([' ', '\t']))
            })
            .collect();
        assert_eq!(hits.len(), 1, "#define {name}: {hits:?}");
        out.push(hits[0]);
    }
    out.join("\n")
}
