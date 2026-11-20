//! The commands a user types: `ounce build <game>`, `ounce run <game>`, `ounce test <game>` and `ounce config`. Each
//! finds the game under the checkout's games/, its dump in the config and its folders in the layout (ounce_core).

use crate::{config, fail};
use ounce_core::config::Config;
use ounce_core::games::{self, Game};
use ounce_core::layout::Layout;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// The checkout, the game named by the first argument, and the arguments after it.
fn game_args<'a>(cmd: &str, args: &'a [String]) -> (Layout, Game, &'a [String]) {
    let root = games::root().unwrap_or_else(|e| fail(e));
    let Some(name) = args.first().filter(|a| !a.starts_with('-')) else {
        fail(format!("{cmd} <game>: the games are {}", games::list(&root).join(", ")))
    };
    let game = Game::load(&root, name).unwrap_or_else(|e| fail(e));
    (Layout::new(root, name), game, &args[1..])
}

/// Runs `cmd`, its output shown and appended to `log`; fails naming `what` and the log.
fn logged(mut cmd: Command, what: &str, log: &Path) {
    let prog = cmd.get_program().to_string_lossy().into_owned();
    let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().unwrap_or_else(|e| {
        fail(if e.kind() == std::io::ErrorKind::NotFound { format!("{what}: {prog} is not installed") } else { format!("{what}: {prog}: {e}") })
    });
    let file = std::fs::OpenOptions::new().create(true).append(true).open(log).unwrap_or_else(|e| fail(format!("{}: {e}", log.display())));
    let file = std::sync::Arc::new(std::sync::Mutex::new(file));
    let threads: Vec<_> = [
        Box::new(child.stdout.take().unwrap()) as Box<dyn std::io::Read + Send>,
        Box::new(child.stderr.take().unwrap()),
    ]
    .into_iter()
    .enumerate()
    .map(|(k, pipe)| {
        let file = file.clone();
        std::thread::spawn(move || {
            for line in BufReader::new(pipe).split(b'\n').map_while(Result::ok) {
                let _ = writeln!(file.lock().unwrap(), "{}", String::from_utf8_lossy(&line));
                if k == 0 {
                    let _ = writeln!(std::io::stdout(), "{}", String::from_utf8_lossy(&line));
                } else {
                    let _ = writeln!(std::io::stderr(), "{}", String::from_utf8_lossy(&line));
                }
            }
        })
    })
    .collect();
    for t in threads {
        let _ = t.join();
    }
    let st = child.wait().unwrap_or_else(|e| fail(e));
    if !st.success() {
        fail(format!("{what} failed ({st}); the log is {}", log.display()));
    }
}

/// A release's renderer, built with it (the AppImage's): used instead of the checkout's Meson build when it is there
pub const PREBUILT_RENDERER: &str = if cfg!(windows) { "prebuilt/ounce_renderer.dll" } else { "prebuilt/ounce_renderer.so" };
/// the renderer's file name in its build folder and in the game folder
pub const RENDERER: &str = if cfg!(windows) { "ounce_renderer.dll" } else { "ounce_renderer.so" };

/// The renderer: a release's prebuilt one, else Meson and Ninja in the checkout's work/renderer: the same build for every game). `meson setup`
/// runs when the folder has no build.ninja yet; on Windows with the Vulkan SDK's headers (VULKAN_SDK). Returns
/// ounce_renderer.so (.dll).
fn renderer(layout: &Layout, log: &Path) -> PathBuf {
    // (a release ships it built: no Meson, Ninja or C++ compiler on a player's PC)
    let prebuilt = layout.root.join(PREBUILT_RENDERER);
    if prebuilt.is_file() {
        println!("the renderer: {} (prebuilt)", prebuilt.display());
        return prebuilt;
    }
    let dir = layout.renderer_build();
    for (tool, why) in [("meson", "the renderer's build"), ("ninja", "the renderer's build"), ("clang++", "the renderer's compiler (renderer/meson/clang.ini)")] {
        if Command::new(tool).arg("--version").stdout(Stdio::null()).stderr(Stdio::null()).status().is_err() {
            fail(format!("{tool} is not installed: {why} needs it"));
        }
    }
    if !dir.join("build.ninja").is_file() {
        let mut c = Command::new("meson");
        c.current_dir(&layout.root).arg("setup").arg("--native-file").arg("renderer/meson/clang.ini").arg(&dir);
        if cfg!(windows) {
            // (no system include folder holds the Vulkan headers on Windows: the Vulkan SDK's)
            let sdk = std::env::var_os("VULKAN_SDK").map(PathBuf::from).filter(|d| d.join("Include/vulkan/vulkan.h").is_file());
            let sdk = sdk.unwrap_or_else(|| fail("the Vulkan SDK is not installed (VULKAN_SDK names no Include/vulkan/vulkan.h): the renderer's build needs its headers"));
            c.arg(format!("-Dvulkan_headers={}", sdk.display()));
        }
        logged(c, "meson setup", log);
    }
    let mut c = Command::new("ninja");
    c.arg("-C").arg(&dir);
    logged(c, "the renderer's build", log);
    dir.join("renderer").join(RENDERER)
}

/// `ounce build <game> [-j N] [--gen-parts]`, or `ounce build <game> --out <dir> --dump <src> [--quality q]`: the
/// renderer and FFmpeg (ffmpeg.rs), then the lift, the compiles and the link (`ounce tool build` as a child, its output in build.log).
pub fn build(args: &[String]) {
    let (layout, game, rest) = game_args("build", args);
    let config = config();
    let (mut out, mut dump, mut pass) = (None::<PathBuf>, None::<String>, Vec::<String>::new());
    let mut jobs = config.jobs();
    let mut it = rest.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "-j" => jobs = Some(val().parse().unwrap_or_else(|_| fail("-j N"))),
            "--out" => out = Some(val().into()),
            "--dump" => dump = Some(val()),
            "--quality" => pass.extend(["--quality".into(), val()]),
            "--gen-parts" | "--progress" | "--shaders" => pass.push(a.clone()),
            _ => fail(format!("build: what is {a}? (ounce help)")),
        }
    }
    if out.is_some() != dump.is_some() {
        fail("build: --out and --dump go together (a player's game folder made from a dump)");
    }
    // (a missing dump stops the build before anything runs)
    let exefs = if dump.is_none() { Some(config.dump(&game.name).unwrap_or_else(|e| fail(e)).join("exefs")) } else { None };
    std::fs::create_dir_all(layout.work()).unwrap_or_else(|e| fail(format!("{}: {e}", layout.work().display())));
    let log = layout.build_log();
    std::fs::write(&log, "").unwrap_or_else(|e| fail(format!("{}: {e}", log.display())));
    let so = renderer(&layout, &log);
    // (Linux: the sysroot, and FFmpeg built against it; Windows: MSVC's, and the pinned FFmpeg and SDL, windows.rs)
    #[cfg(windows)]
    let (sysroot, ffmpeg) = (None::<PathBuf>, crate::windows::ffmpeg(&config.cache_dir()));
    #[cfg(not(windows))]
    let sysroot = Some(crate::sysroot::find(&layout.root, &config.cache_dir()));
    #[cfg(not(windows))]
    let ffmpeg = crate::ffmpeg::find(&layout.root, &config.cache_dir(), jobs.unwrap_or_else(|| std::thread::available_parallelism().map_or(1, |n| n.get())), sysroot.as_ref().unwrap());
    let s = |p: &Path| p.to_string_lossy().into_owned();
    let mut a: Vec<String> = vec!["tool".into(), "build".into()];
    match (&out, &dump) {
        (Some(o), Some(d)) => a.extend([s(o), "--dump".into(), d.clone()]),
        _ => {
            let lift = layout.lift_dir();
            a.extend([s(&layout.game_dir()), "--exefs".into(), s(exefs.as_ref().unwrap()), "--gen".into(), s(&lift)]);
            a.extend(["--map".into(), s(&lift.join(format!("{}.map", game.exe)))]);
        }
    }
    a.extend(["--direct".into(), s(&so), "--ffmpeg".into(), s(&ffmpeg), "--game".into(), s(&game.dir), "--root".into(), s(&layout.root)]);
    a.extend(["--obj".into(), s(&layout.obj_dir())]);
    if let Some(r) = &sysroot {
        a.extend(["--sysroot".into(), s(r)]);
    }
    #[cfg(windows)]
    let sdl = Some(crate::windows::sdl(&config.cache_dir()));
    #[cfg(not(windows))]
    let sdl = crate::sdl::prebuilt(&layout.root);
    if let Some(l) = sdl {
        a.extend(["--sdl".into(), s(&l)]);
    }
    if let Some(j) = jobs {
        a.extend(["-j".into(), j.to_string()]);
    }
    a.extend(pass);
    let mut c = Command::new(std::env::current_exe().unwrap_or_else(|e| fail(e)));
    c.args(&a);
    logged(c, "the build", &log);
    println!("built {}; the log is {}", out.as_deref().map_or(layout.game_dir(), |o| o.to_path_buf()).display(), log.display());
}

/// `key=value`
fn switch(v: &str) -> (String, String) {
    let (k, v) = v.split_once('=').unwrap_or_else(|| fail(format!("--set {v}: key=value")));
    (k.to_string(), v.to_string())
}

/// `ounce run <game> ...`
pub fn run(args: &[String]) {
    let (layout, game, rest) = game_args("run", args);
    let config = config();
    let mut o = ounce_core::run::Options::default();
    let mut it = rest.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "--headless" => o.headless = true,
            "--seconds" => o.seconds = Some(val().parse().unwrap_or_else(|_| fail("--seconds N"))),
            "--record" => o.record = Some(val()),
            "--replay" => o.replay = Some(val()),
            "--bench" => o.bench = Some(val().parse().unwrap_or_else(|_| fail("--bench <frames>"))),
            "--shots" => o.shot_every = Some(val().parse().unwrap_or_else(|_| fail("--shots <every N presents>"))),
            "--name" => o.name = Some(val()),
            "--set" => o.set.push(switch(&val())),
            "--build" => o.build = Some(PathBuf::from(val())),
            "--superluminal" => o.superluminal = Some(PathBuf::from(val())),
            _ => fail(format!("run: what is {a}? (ounce help)")),
        }
    }
    if o.record.is_some() && o.replay.is_some() {
        fail("run: --record or --replay, not both");
    }
    let r = ounce_core::run::run(&layout, &config, &game, &o).unwrap_or_else(|e| fail(e));
    println!("{}; the log is {}", r.summary, r.dir.join("log.txt").display());
    if o.bench.is_some() {
        let log = std::fs::read_to_string(r.dir.join("log.txt")).unwrap_or_default();
        for l in log.lines().filter(|l| l.contains("bench: ") && !l.contains("not ready") && !l.contains("bench: messages")) {
            println!("{}", l.split_once("] ").map_or(l, |x| x.1));
        }
    }
}

/// `ounce test <game> [--name T] [--golden | --golden-reset] [--keep-shots] [--set NAME=VALUE]...`: the smoke test T
/// of games/<game>/tests.toml. One headless run with the test's settings, then the verdict (smoke.rs) against the data
/// folder's `<game>/golden/<T>/` (--golden adds the run to the golden set, --golden-reset starts the set with it).
/// `--golden-kept` adds the test's last run, kept with --keep-shots and checked by eye, without a new run.
/// Exits 0 when it passes.
pub fn test(args: &[String]) {
    let (layout, game, rest) = game_args("test", args);
    let config = config();
    let (mut name, mut golden, mut reset, mut keep, mut set) = (game.default_test.clone(), false, false, false, Vec::new());
    let mut kept = false;
    let mut it = rest.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "--name" => name = Some(val()),
            "--golden" => golden = true,
            "--golden-reset" => (golden, reset) = (true, true),
            "--keep-shots" => keep = true,
            "--golden-kept" => kept = true,
            "--set" => set.push(switch(&val())),
            _ => fail(format!("test: what is {a}? (ounce help)")),
        }
    }
    let tests: Vec<&str> = game.tests.keys().map(String::as_str).collect();
    if tests.is_empty() {
        fail(format!("test: {} has no smoke tests (games/{}/tests.toml)", game.name, game.name));
    }
    let Some(name) = name else { fail(format!("test: games/{}/tests.toml names no default test: pass --name, one of {}", game.name, tests.join(", "))) };
    let Some(t) = game.tests.get(&name) else {
        fail(format!("test: no test {name} for {} (games/{}/tests.toml has {})", game.name, game.name, tests.join(", ")))
    };
    let gd = layout.golden(&config).join(&name);
    if kept {
        let dir = layout.run_dir(&format!("test-{name}"));
        if !dir.join("shots").is_dir() {
            fail(format!("test: {} has no shots (run with --keep-shots first)", dir.display()));
        }
        let (lines, _) = crate::smoke::verdict(&dir, &gd, t.smoke_skip as u64, true, false).unwrap_or_else(|e| fail(e));
        for l in &lines {
            println!("{l}");
        }
        return;
    }
    let gds = gd.to_string_lossy();
    let mut settings: Vec<(String, String)> = t.set.iter().map(|s| switch(&s.replace("{golden}", &gds))).collect();
    settings.extend(set);
    let o = ounce_core::run::Options {
        headless: true,
        seconds: Some(t.seconds),
        name: Some(format!("test-{name}")),
        shot_every: Some(120),
        set: settings,
        ..Default::default()
    };
    let r = ounce_core::run::run(&layout, &config, &game, &o).unwrap_or_else(|e| fail(e));
    std::fs::write(r.dir.join("run.out"), format!("{}\n", r.summary)).unwrap_or_else(|e| fail(e));
    // (the saves and caches the run made)
    let _ = std::fs::remove_dir_all(r.dir.join("user"));
    let (lines, pass) = crate::smoke::verdict(&r.dir, &gd, t.smoke_skip as u64, golden, reset).unwrap_or_else(|e| fail(e));
    for l in &lines {
        println!("{l}");
    }
    let _ = std::fs::write(r.dir.join("verdict.txt"), lines.join("\n") + "\n");
    if !keep && !golden {
        let _ = std::fs::remove_dir_all(r.dir.join("shots"));
    }
    std::process::exit(if pass { 0 } else { 1 });
}

/// `ounce config [path | get <key> | set <key> <value>]`
pub fn config_cmd(args: &[String]) {
    let mut c: Config = config();
    match args.iter().map(String::as_str).collect::<Vec<_>>()[..] {
        [] => {
            println!("# {}", c.path.display());
            print!("{}", c.text());
        }
        ["path"] => println!("{}", c.path.display()),
        ["get", key] => match c.show(key) {
            Some(v) => println!("{v}"),
            None => fail(format!("{key} is not set in {}", c.path.display())),
        },
        ["set", key, value] => {
            c.set(key, value).unwrap_or_else(|e| fail(e));
            println!("{key} = {value} (in {})", c.path.display());
        }
        _ => fail("config [path | get <key> | set <key> <value>]"),
    }
}
