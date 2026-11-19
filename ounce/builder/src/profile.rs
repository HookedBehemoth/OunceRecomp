//! `ounce profile <game>`: the scenes of games/<game>/profile.toml, one headless benchmark run each (runtime/bench.c),
//! read into a table a scene and a summary across them. A run reports the frame time, the CPU time a frame by part
//! (guest logic, guest draw, host draw, other) and the GPU's time a frame by pass (renderer/vk_passtime.h, passtime.csv).
//! With --shaders a second run per scene takes HOS_DBG=pstats:bench's per-draw times of one frame and names the costliest
//! programs of the costliest passes, with ACO's statistics from the newest shader census (tools/shadercensus).

use crate::{config, fail};
use ounce_core::games::{self, Game, Scene};
use ounce_core::layout::Layout;
use std::collections::{BTreeMap, HashMap};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::process::Command;

const HELP: &str = "profile <game> [--scene NAME]... [--size WxH] [--frames N] [--shaders] [--census <stages.jsonl>]";

struct Opts {
    scenes: Vec<String>,
    size: Option<String>,
    frames: u32,
    shaders: bool,
    census: Option<PathBuf>,
}

/// One pass's line of passtime.csv
#[derive(Clone, Default)]
struct Pass {
    what: String,
    kind: u32,
    lr: String,
    rt: u32,
    w: u32,
    h: u32,
    targets: u32,
    format: i32,
    depth: i32,
    pass_key: String,
    prog: String,
    count: u64,
    draws: u64,
    jobs: u64,
    ns: f64,
}

#[derive(Default)]
struct SceneResult {
    ready: Option<String>,
    not_ready: Option<String>,
    frame: Option<[f64; 3]>,  // (mean, median, p99 ms)
    cpu: Option<[f64; 4]>,    // (guest logic, guest draw, host draw, other: ms a frame)
    gpu_frames: f64,
    busy_ns: f64,
    idle_ns: f64,
    passes: Vec<Pass>,
    shaders: Vec<String>,
}

fn kind_name(k: u32) -> &'static str {
    ["packet", "load", "job", "layer", "gate", "async compute", "queue head", "immediate"].get(k as usize).copied().unwrap_or("?")
}

fn format_name(f: i32) -> String {
    match f {
        -1 => "-".into(),
        37 => "rgba8".into(),
        43 => "rgba8 srgb".into(),
        44 => "bgra8".into(),
        64 => "rgb10a2".into(),
        76 => "r16f".into(),
        83 => "rg16f".into(),
        97 => "rgba16f".into(),
        98 => "r32ui".into(),
        100 => "r32f".into(),
        103 => "rg32f".into(),
        109 => "rgba32f".into(),
        122 => "r11g11b10f".into(),
        124 => "d16".into(),
        126 => "d32".into(),
        129 => "d24s8".into(),
        130 => "d32s8".into(),
        9 => "r8".into(),
        16 => "rg8".into(),
        f => format!("vk{f}"),
    }
}

impl Pass {
    /// A short name: what it is, its pass's shape
    fn label(&self) -> String {
        match self.what.as_str() {
            "render" => {
                let mut s = format!("{} rt{} {}x{} {}", kind_name(self.kind), self.rt, self.w, self.h, format_name(self.format));
                if self.targets > 1 {
                    let _ = write!(s, " x{}", self.targets);
                }
                if self.depth >= 0 {
                    let _ = write!(s, " +{}", format_name(self.depth));
                }
                if self.jobs > 0 {
                    let _ = write!(s, " key {} ({:.0} jobs)", self.pass_key, self.jobs as f64 / self.count.max(1) as f64);
                }
                s
            }
            "dispatch" => format!("dispatch ({} {}) {} grid {}x{}x{}", kind_name(self.kind), self.lr, &self.prog[..self.prog.len().min(8)], self.w, self.h, self.targets),
            "task" => format!("task step ({}) rt{}{}", kind_name(self.kind), self.rt, if self.pass_key.len() > 3 { format!(" key {}", self.pass_key) } else { String::new() }),
            "copy" => format!("copy ({}) rt{} {}x{}", kind_name(self.kind), self.rt, self.w, self.h),
            "clear" => format!("texture clear ({}) {}x{}", kind_name(self.kind), self.w, self.h),
            "present" => format!("present {}x{}", self.w, self.h),
            w => w.to_string(),
        }
    }
    /// Its group in the summary
    fn group(&self) -> String {
        match self.what.as_str() {
            "render" => format!("renderings ({})", kind_name(self.kind)),
            "dispatch" => format!("dispatches ({})", kind_name(self.kind)),
            "task" => "task steps".into(),
            "copy" => "copies".into(),
            "clear" => "texture clears".into(),
            w => w.to_string(),
        }
    }
}

fn read_passtime(path: &Path, r: &mut SceneResult) {
    let Ok(text) = std::fs::read_to_string(path) else { return };
    let mut lines = text.lines();
    if let Some(h) = lines.next() {
        for kv in h.trim_start_matches('#').split_whitespace() {
            if let Some((k, v)) = kv.split_once('=') {
                let v: f64 = v.parse().unwrap_or(0.0);
                match k {
                    "presents" => r.gpu_frames = v,
                    "busy_ns" => r.busy_ns = v,
                    "idle_ns" => r.idle_ns = v,
                    _ => {}
                }
            }
        }
    }
    lines.next();
    for l in lines {
        let c: Vec<&str> = l.split(',').collect();
        if c.len() < 15 {
            continue;
        }
        let n = |i: usize| c[i].parse::<u64>().unwrap_or(0);
        r.passes.push(Pass {
            what: c[0].into(),
            kind: n(1) as u32,
            lr: c[2].into(),
            rt: n(3) as u32,
            w: n(4) as u32,
            h: n(5) as u32,
            targets: n(6) as u32,
            format: c[7].parse().unwrap_or(-1),
            depth: c[8].parse().unwrap_or(-1),
            pass_key: c[9].into(),
            prog: c[10].into(),
            count: n(11),
            draws: n(12),
            jobs: n(13),
            ns: c[14].parse().unwrap_or(0.0),
        });
    }
}

/// The numbers after `key` in a log line, as floats (the first `n`)
fn nums_after(line: &str, key: &str, n: usize) -> Vec<f64> {
    let Some(rest) = line.split_once(key).map(|x| x.1) else { return Vec::new() };
    rest.split(|c: char| !(c.is_ascii_digit() || c == '.')).filter(|s| !s.is_empty() && *s != ".").filter_map(|s| s.parse().ok()).take(n).collect()
}

fn read_log(dir: &Path, r: &mut SceneResult) {
    let log = String::from_utf8_lossy(&std::fs::read(dir.join("log.txt")).unwrap_or_default()).into_owned();
    for l in log.lines() {
        let l = l.split_once("] ").map_or(l, |x| x.1);
        if l.starts_with("bench: ready at") {
            r.ready = Some(nums_after(l, "ready at", 1).first().map_or("?".into(), |s| format!("{s:.1} s")));
        } else if l.starts_with("bench: not ready") {
            r.not_ready = Some(l.trim_start_matches("bench: ").to_string());
        } else if l.starts_with("bench: ") && l.contains(" frames in ") {
            let g = |k: &str| nums_after(l, k, 1).first().copied().unwrap_or(0.0);
            r.frame = Some([g("frame mean"), g("median"), g("p99 ")]);
        } else if l.starts_with("bench: CPU a frame:") {
            let g = |k: &str| nums_after(l, k, 1).first().copied().unwrap_or(0.0);
            r.cpu = Some([g("guest logic"), g("guest draw"), g("host draw"), g("other")]);
        }
    }
    read_passtime(&dir.join("passtime.csv"), r);
}

/// The play size: the game folder's Output resolution (ounce_options.ini output_res) when it names one, else 2560x1440
fn play_size(layout: &Layout) -> String {
    let ini = std::fs::read_to_string(layout.game_dir().join("ounce_options.ini")).unwrap_or_default();
    let res = ini.lines().find_map(|l| l.trim().strip_prefix("output_res=")).and_then(|v| v.trim().parse::<u32>().ok());
    match res {
        Some(2) => "3840x2160",
        Some(3) => "1920x1080",
        Some(4) => "5120x1440",
        _ => "2560x1440",
    }
    .into()
}

/// The scene's overlay folder in the data folder, made when missing (tools/dump/make_chisel_overlay.py)
fn overlay(layout: &Layout, dir: &Path, s: &Scene) -> Option<PathBuf> {
    let banc = s.banc.as_ref()?;
    let ov = dir.join("overlay");
    if !ov.join("Sequence").is_dir() {
        let py = layout.root.join("work/venv/bin/python");
        let py = if py.is_file() { py } else { PathBuf::from("python3") };
        let mut c = Command::new(py);
        c.arg("-I").arg(layout.root.join("tools/dump/make_chisel_overlay.py")).arg(&ov).args(["--route", "newgame", "--banc", banc]);
        if !s.start.is_empty() {
            c.args(["--start", &s.start]);
        }
        let out = c.output().unwrap_or_else(|e| fail(format!("make_chisel_overlay.py: {e}")));
        if !out.status.success() {
            fail(format!("the overlay of {banc}: {}", String::from_utf8_lossy(&out.stderr)));
        }
    }
    Some(ov)
}

/// ACO's statistics of the census's stages by code key: per stage the census compiled with it, its name and
/// "VGPRs/SGPRs, waves a SIMD, instructions, latency"
fn census_stats(path: &Path, keys: &[String]) -> HashMap<String, Vec<(String, String)>> {
    let mut out = HashMap::new();
    let Ok(text) = std::fs::read_to_string(path) else { return out };
    for line in text.lines() {
        let Some(k) = line.split_once("\"key\":\"").and_then(|x| x.1.get(..16)) else { continue };
        if !keys.iter().any(|w| w == k) {
            continue;
        }
        let Ok(v) = serde_json::from_str::<serde_json::Value>(line) else { continue };
        let mut stages = Vec::new();
        for st in v["radv"].as_array().into_iter().flatten() {
            let g = |f: &str| st[f].as_u64().unwrap_or(0);
            let name = st["name"].as_str().unwrap_or("?").to_string();
            let mut s = format!("{name} {}v/{}s {}w {}i lat {}", g("VGPRs"), g("SGPRs"), g("Subgroups per SIMD"), g("Instructions"), g("Latency"));
            if g("LDS size") > 0 {
                let _ = write!(s, " lds {}", g("LDS size"));
            }
            if g("Spilled VGPRs") + g("Scratch size") > 0 {
                let _ = write!(s, " spills {} scratch {}", g("Spilled VGPRs"), g("Scratch size"));
            }
            stages.push((name, s));
        }
        out.insert(k.to_string(), stages);
    }
    out
}

fn hex(s: &str) -> u64 {
    u64::from_str_radix(s.trim_start_matches("0x"), 16).unwrap_or(0)
}

/// The costliest programs of the costliest passes, from HOS_DBG=pstats:bench's pstats.csv (one frame, every command
/// bracketed): a rendering's own draws (its list's SetRenderTargets number) and its jobs' (their pass key)
fn shaders_of(dir: &Path, r: &SceneResult, census: Option<&Path>) -> Vec<String> {
    let Ok(text) = std::fs::read_to_string(dir.join("pstats.csv")) else { return vec!["  (no pstats.csv)".into()] };
    struct Row {
        what: String,
        key: String,
        codes: [String; 2],
        kind: u32,
        pass: u64,
        ns: f64,
        fs: u64,
    }
    let mut rows = Vec::new();
    for l in text.lines().skip(1) {
        let c: Vec<&str> = l.split(',').collect();
        if c.len() < 27 || c[15] != "1" {
            continue;
        }
        let (tb, te): (f64, f64) = (c[24].parse().unwrap_or(-1.0), c[25].parse().unwrap_or(-1.0));
        if tb < 0.0 || te < tb {
            continue;
        }
        rows.push(Row { what: c[1].into(), key: c[2].into(), codes: [c[3].into(), c[4].into()], kind: c[6].parse().unwrap_or(0), pass: hex(c[26]),
                        ns: te - tb, fs: c[21].parse().unwrap_or(0) });
    }
    // (per pick: by program, its time, fragment invocations, commands)
    let mut picks: Vec<(String, Vec<(&Row, f64, u64, u64)>)> = Vec::new();
    let mut group = |label: String, sel: &dyn Fn(&Row) -> bool| {
        let mut by: BTreeMap<&str, (&Row, f64, u64, u64)> = BTreeMap::new();
        for x in rows.iter().filter(|x| sel(x)) {
            let e = by.entry(&x.key).or_insert((x, 0.0, 0, 0));
            e.1 += x.ns;
            e.2 += x.fs;
            e.3 += 1;
        }
        let mut v: Vec<_> = by.into_values().collect();
        v.sort_by(|a, b| b.1.total_cmp(&a.1));
        v.truncate(4);
        picks.push((label, v));
    };
    for p in r.passes.iter().filter(|p| p.what == "render").take(3) {
        let (rt, kind, key) = (p.rt as u64, p.kind, hex(&p.pass_key));
        group(p.label(), &|x: &Row| x.what.contains("draw") && ((x.kind == 2 && key != 0 && x.pass == key) || (x.kind == kind && x.pass == rt)));
    }
    group("dispatches and task steps".into(), &|x: &Row| x.what.contains("dispatch") || x.what.contains("task step"));
    let want: Vec<String> = picks.iter().flat_map(|(_, v)| v.iter().flat_map(|(x, ..)| x.codes.iter().cloned())).collect();
    let stats = census.map(|c| census_stats(c, &want)).unwrap_or_default();
    let mut out = Vec::new();
    for (label, progs) in picks {
        out.push(format!("  {label}:"));
        for (x, ns, fs, n) in progs {
            // (the first stage from its own code's record, the fragment stage from the fragment code's)
            let first = stats.get(&x.codes[0]).and_then(|v| v.iter().find(|s| s.0 != "fragment").or(v.first())).map(|s| s.1.clone());
            let frag = stats.get(&x.codes[1]).and_then(|v| v.iter().find(|s| s.0 == "fragment")).map(|s| s.1.clone());
            let st: Vec<String> = [first, frag].into_iter().flatten().collect();
            out.push(format!("    {:<18} {} {:>7.3} ms {:>4} cmds {:>6.2} Mpix  {}", x.what, &x.key[..8.min(x.key.len())], ns / 1e6, n, fs as f64 / 1e6,
                             if st.is_empty() { "(no census record)".into() } else { st.join("; ") }));
        }
    }
    out
}

fn table(name: &str, s: &Scene, r: &SceneResult, top: usize) -> String {
    let mut o = String::new();
    let _ = writeln!(o, "== {name}: {}", s.note);
    let Some(f) = r.frame else {
        let _ = writeln!(o, "  not measured: {}", r.not_ready.as_deref().unwrap_or("no bench lines (see the run's log.txt)"));
        return o;
    };
    let _ = writeln!(o, "  frame {:.2} ms mean, {:.2} median, {:.2} p99 ({:.0} fps); ready at {}", f[0], f[1], f[2], 1000.0 / f[0], r.ready.as_deref().unwrap_or("?"));
    if let Some(c) = r.cpu {
        let _ = writeln!(o, "  CPU a frame: guest logic {:.2} ms, guest draw {:.2}, host draw {:.2}, other {:.2}", c[0], c[1], c[2], c[3]);
    }
    if r.gpu_frames > 0.0 {
        let fr = r.gpu_frames;
        let _ = writeln!(o, "  GPU a frame: busy {:.2} ms, idle {:.2} ms (over {} frames)", r.busy_ns / 1e6 / fr, r.idle_ns / 1e6 / fr, fr);
        let mut groups: BTreeMap<String, (f64, u64)> = BTreeMap::new();
        for p in &r.passes {
            let g = groups.entry(p.group()).or_default();
            g.0 += p.ns;
            g.1 += p.count;
        }
        let mut gs: Vec<_> = groups.into_iter().collect();
        gs.sort_by(|a, b| b.1 .0.total_cmp(&a.1 .0));
        let _ = writeln!(o, "  by kind:");
        for (g, (ns, n)) in gs {
            let _ = writeln!(o, "    {:<30} {:>6.3} ms  {:>6.1} a frame", g, ns / 1e6 / fr, n as f64 / fr);
        }
        let _ = writeln!(o, "  top passes (ms a frame, units a frame, draws a frame):");
        let mut shown = 0.0;
        for p in r.passes.iter().take(top) {
            shown += p.ns;
            let _ = writeln!(o, "    {:>6.3}  {:>5.1}  {:>6.0}  {}{}", p.ns / 1e6 / fr, p.count as f64 / fr, p.draws as f64 / fr, p.label(),
                             if p.what == "render" && p.prog.len() >= 8 { format!(" (last prog {})", &p.prog[..8]) } else { String::new() });
        }
        let rest = r.busy_ns - shown;
        let _ = writeln!(o, "    {:>6.3}  {:>5.1}          the other {} labels", rest / 1e6 / fr,
                         r.passes.iter().skip(top).map(|p| p.count).sum::<u64>() as f64 / fr, r.passes.len().saturating_sub(top));
    }
    if !r.shaders.is_empty() {
        let _ = writeln!(o, "  costliest programs (one frame, HOS_DBG=pstats: time with every draw bracketed; ACO: VGPRs/SGPRs, waves a SIMD, instructions, latency):");
        for l in &r.shaders {
            let _ = writeln!(o, "{l}");
        }
    }
    o
}

/// Runs one headless benchmark of the scene; with `shaders`, the per-draw run (HOS_DBG=pstats:bench) instead
fn run_scene(layout: &Layout, game: &Game, name: &str, s: &Scene, dir: &Path, size: &str, frames: u32, shaders: bool) -> PathBuf {
    let config = config();
    let mut set: Vec<(String, String)> = vec![("HOS_WINDOW_SIZE".into(), size.into())];
    if let Some(ov) = overlay(layout, dir, s) {
        set.push(("HOS_FS_OVERLAY".into(), ov.to_string_lossy().into_owned()));
    }
    let play = if s.play { "" } else { ",noplay" };
    let run_name = if shaders { format!("profile-{name}-shaders") } else { format!("profile-{name}") };
    if shaders {
        set.push(("HOS_BENCH".into(), format!("30,60,0{play}")));
        set.push(("HOS_DBG".into(), "pstats:bench".into()));
    } else {
        set.push(("HOS_BENCH".into(), format!("{frames},60,{}{play}", frames.saturating_sub(8).min(300))));
    }
    // (the benchmark run keeps a picture of the scene beside the overlay, taken while it measured: every `frames - 8`
    // presents puts one shot in any `frames` presents in a row, at least 8 before the run's end, past the shot's
    // three-present write; the newest is the measured one)
    let shot_every = if shaders { None } else { Some(frames.saturating_sub(8).max(1)) };
    let o = ounce_core::run::Options { headless: true, seconds: Some(s.seconds), name: Some(run_name), set, shot_every, ..Default::default() };
    let r = ounce_core::run::run(layout, &config, game, &o).unwrap_or_else(|e| fail(e));
    if !shaders {
        let mut shots: Vec<PathBuf> = std::fs::read_dir(r.dir.join("shots")).into_iter().flatten().flatten().map(|e| e.path()).collect();
        shots.sort();
        if let Some(last) = shots.last() {
            let _ = std::fs::copy(last, dir.join(format!("{name}.png")));
        }
    }
    let _ = std::fs::remove_dir_all(r.dir.join("user"));
    let _ = std::fs::remove_dir_all(r.dir.join("shots"));
    println!("  {}", r.summary);
    r.dir
}

/// The newest shader census of the game (work/shadercensus/*/census_<game>/stages.jsonl, in this checkout or the main one)
fn find_census(layout: &Layout, game: &str) -> Option<PathBuf> {
    let mut found: Vec<(std::time::SystemTime, PathBuf)> = Vec::new();
    for root in [Some(layout.root.clone()), crate::textures::main_checkout(&layout.root)].into_iter().flatten() {
        for e in std::fs::read_dir(root.join("work/shadercensus")).into_iter().flatten().flatten() {
            let c = e.path().join(format!("census_{game}/stages.jsonl"));
            if let Ok(t) = c.metadata().and_then(|m| m.modified()) {
                found.push((t, c));
            }
        }
    }
    found.into_iter().max().map(|x| x.1)
}

pub fn command(args: &[String]) {
    let root = games::root().unwrap_or_else(|e| fail(e));
    let Some(gname) = args.first().filter(|a| !a.starts_with('-')) else { fail(HELP) };
    let game = Game::load(&root, gname).unwrap_or_else(|e| fail(e));
    let layout = Layout::new(root, gname);
    let mut o = Opts { scenes: Vec::new(), size: None, frames: 600, shaders: false, census: None };
    let mut it = args[1..].iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "--scene" => o.scenes.push(val()),
            "--size" => o.size = Some(val()),
            "--frames" => o.frames = val().parse().unwrap_or_else(|_| fail("--frames N")),
            "--shaders" => o.shaders = true,
            "--census" => o.census = Some(val().into()),
            _ => fail(format!("profile: what is {a}? ({HELP})")),
        }
    }
    if game.scenes.is_empty() {
        fail(format!("profile: {} has no scenes (games/{}/profile.toml)", game.name, game.name));
    }
    for s in &o.scenes {
        if !game.scenes.iter().any(|(n, _)| n == s) {
            fail(format!("profile: no scene {s} (games/{}/profile.toml has {})", game.name, game.scenes.iter().map(|x| x.0.as_str()).collect::<Vec<_>>().join(", ")));
        }
    }
    let size = o.size.clone().unwrap_or_else(|| play_size(&layout));
    let census = o.census.clone().or_else(|| find_census(&layout, &game.name));
    let data = config().data_dir().join(&game.name).join("profile");
    let mut report = format!("ounce profile {}: {} scenes at {size}, {} measured frames each\n", game.name,
                             if o.scenes.is_empty() { game.scenes.len() } else { o.scenes.len() }, o.frames);
    let mut rows = Vec::new();
    for (name, s) in game.scenes.iter().filter(|(n, _)| o.scenes.is_empty() || o.scenes.contains(n)) {
        println!("profile: {name}");
        let dir = data.join(name);
        std::fs::create_dir_all(&dir).unwrap_or_else(|e| fail(format!("{}: {e}", dir.display())));
        let run = run_scene(&layout, &game, name, s, &dir, &size, o.frames, false);
        let mut r = SceneResult::default();
        read_log(&run, &mut r);
        if o.shaders && r.frame.is_some() {
            let cap = run_scene(&layout, &game, name, s, &dir, &size, o.frames, true);
            r.shaders = shaders_of(&cap, &r, census.as_deref());
        }
        let t = table(name, s, &r, 12);
        print!("{t}");
        report += &t;
        rows.push((name.clone(), r));
    }
    let mut sum = String::from("\n== summary (ms a frame)\n  scene              frame   fps   GPU busy  idle   logic  g.draw  h.draw  other  costliest pass\n");
    for (name, r) in &rows {
        let (Some(f), c) = (r.frame, r.cpu.unwrap_or_default()) else {
            let _ = writeln!(sum, "  {name:<18} not measured");
            continue;
        };
        let fr = r.gpu_frames.max(1.0);
        let top = r.passes.first().map_or(String::new(), |p| format!("{:.2} {}", p.ns / 1e6 / fr, p.label()));
        let _ = writeln!(sum, "  {name:<18} {:>5.2} {:>5.0}   {:>6.2} {:>5.2}   {:>5.2}  {:>5.2}   {:>5.2}  {:>5.2}  {top}", f[0], 1000.0 / f[0],
                         r.busy_ns / 1e6 / fr, r.idle_ns / 1e6 / fr, c[0], c[1], c[2], c[3]);
    }
    print!("{sum}");
    report += &sum;
    let out = layout.work().join("profile.txt");
    std::fs::write(&out, &report).unwrap_or_else(|e| fail(format!("{}: {e}", out.display())));
    println!("written to {}", out.display());
}
