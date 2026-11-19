//! `ounce tool lift-stats`: the lift and compile of one game measured, as numbers only, and two measurements
//! compared. CI's lift profile runs it on a public log: the lift's own output (which names the game's functions and
//! can quote its IR in an error) goes to a log file that is never printed, and what is printed and written is
//! counts, sizes and seconds.

use crate::fail;
use serde_json::{json, Map, Value};
use std::path::{Path, PathBuf};

/// The numbers of one lift and compile, from `tool lift-compile --stats`: (name, kind, value). Kinds: `time`
/// (seconds), `memory` (MB), `size` (bytes) and `count`.
pub fn metrics(d: &crate::lift::Done, wall_s: f64, cpu_s: f64, peak_mb: f64) -> Value {
    let l = &d.lift;
    let rows: Vec<(&str, &str, f64)> = vec![
        ("lift.image_s", "time", l.image_s),
        ("lift.discovery_s", "time", l.discovery_s),
        ("lift.plan_s", "time", l.plan_s),
        ("lift.summaries_s", "time", l.summaries_s),
        ("lift.partitions_s", "time", l.lift_s),
        ("lift.wall_s", "time", l.wall_s),
        ("lift.cpu_s", "time", l.cpu_s),
        ("compile.sum_s", "time", d.compile_s),
        ("total.wall_s", "time", wall_s),
        ("total.cpu_s", "time", cpu_s),
        ("total.peak_mb", "memory", peak_mb),
        ("functions.found", "count", l.functions as f64),
        ("functions.lifted", "count", l.lifted as f64),
        ("functions.skipped", "count", l.skipped as f64),
        ("partitions", "count", l.partitions as f64),
        ("objects", "count", d.objects.len() as f64),
        ("objects.hot_bytes", "size", d.hot_bytes as f64),
        ("objects.cold_bytes", "size", d.cold_bytes as f64),
        ("objects.bytes", "size", (d.hot_bytes + d.cold_bytes) as f64),
    ];
    let mut m = Map::new();
    for (name, kind, v) in rows {
        m.insert(name.into(), json!({ "kind": kind, "value": (v * 1000.0).round() / 1000.0 }));
    }
    Value::Object(m)
}

const USAGE: &str = "tool lift-stats <game> --exefs <dir> --out <stats.json> [--bench <file>] [-j N] [--work <dir>]
       tool lift-stats --compare <baseline.json> <stats.json> [--warn <pct>] [--fail <pct>]";

pub fn command(args: &[String]) {
    if args.first().is_some_and(|a| a == "--compare") {
        return compare(&args[1..]);
    }
    let mut jobs = std::thread::available_parallelism().map_or(1, |n| n.get());
    let (mut game, mut exefs, mut out, mut work) = (None::<String>, None::<PathBuf>, None::<PathBuf>, None::<PathBuf>);
    let mut bench = None::<PathBuf>;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "-j" => jobs = val().parse().unwrap_or_else(|_| fail("-j N")),
            "--exefs" => exefs = Some(val().into()),
            "--out" => out = Some(val().into()),
            "--work" => work = Some(val().into()),
            "--bench" => bench = Some(val().into()),
            _ if game.is_none() && !a.starts_with('-') => game = Some(a.clone()),
            _ => fail(USAGE),
        }
    }
    let (Some(game), Some(exefs), Some(out)) = (game, exefs, out) else { fail(USAGE) };
    let root = ounce_core::games::root().unwrap_or_else(|e| fail(e));
    let game_dir = root.join("games").join(&game);
    if !game_dir.join("game.toml").is_file() {
        fail(format!("no game {game} in {}", root.join("games").display()));
    }
    let work = work.unwrap_or_else(|| std::env::temp_dir().join(format!("ounce-lift-stats-{}", std::process::id())));
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work).unwrap_or_else(|e| fail(format!("{}: {e}", work.display())));
    let (log, stats) = (work.join("lift.log"), work.join("stats.json"));
    let file = std::fs::File::create(&log).unwrap_or_else(|e| fail(format!("{}: {e}", log.display())));
    let err = file.try_clone().unwrap_or_else(|e| fail(e));
    let exe = std::env::current_exe().unwrap_or_else(|e| fail(e));
    // (no object cache: every module is compiled, so the compile is measured and nothing made from the game is kept)
    let st = std::process::Command::new(exe)
        .args(["tool", "lift-compile"])
        .arg(work.join("obj"))
        .arg("--game")
        .arg(&game_dir)
        .arg("--root")
        .arg(&root)
        .arg("--exefs")
        .arg(&exefs)
        .args(["-j", &jobs.to_string(), "--stats"])
        .arg(&stats)
        .stdin(std::process::Stdio::null())
        .stdout(file)
        .stderr(err)
        .status()
        .unwrap_or_else(|e| fail(format!("lift-compile: {e}")));
    if !st.success() {
        fail(format!("{game}: the lift and compile failed ({st}); its log, which is not printed, is {}", log.display()));
    }
    let metrics: Value = read_json(&stats);
    let doc = json!({
        "format": 1,
        "game": game,
        "cpu": cpu_model(),
        "threads": std::thread::available_parallelism().map_or(1, |n| n.get()),
        "jobs": jobs,
        "metrics": metrics,
    });
    for f in [Some(&out), bench.as_ref()].into_iter().flatten() {
        if let Some(d) = f.parent().filter(|d| !d.as_os_str().is_empty()) {
            std::fs::create_dir_all(d).unwrap_or_else(|e| fail(format!("{}: {e}", d.display())));
        }
    }
    std::fs::write(&out, serde_json::to_string_pretty(&doc).unwrap() + "\n").unwrap_or_else(|e| fail(format!("{}: {e}", out.display())));
    // (the objects and the log are the game's: removed once the numbers are out)
    std::fs::remove_dir_all(&work).unwrap_or_else(|e| fail(format!("{}: {e}", work.display())));
    let cpu = doc["cpu"].as_str().unwrap_or("").to_string();
    println!("lift-stats {game}: {jobs} jobs on {cpu}");
    let mut table = format!("### lift-stats {game}\n\n{jobs} jobs on {cpu}\n\n| metric | value |\n|---|---:|\n");
    let mut points = Vec::new();
    for (name, m) in doc["metrics"].as_object().unwrap() {
        let (kind, v) = (m["kind"].as_str().unwrap_or(""), m["value"].as_f64().unwrap_or(0.0));
        println!("  {name:<20} {:>14}", show(kind, v));
        table.push_str(&format!("| {name} | {} |\n", show(kind, v)));
        let unit = match kind { "time" => "s", "memory" => "MB", "size" => "bytes", _ => "count" };
        points.push(json!({ "name": name, "unit": unit, "value": v, "extra": format!("{jobs} jobs on {cpu}") }));
    }
    summary(&table);
    // (github-action-benchmark's customSmallerIsBetter: the history CI keeps on gh-pages)
    if let Some(b) = bench {
        std::fs::write(&b, serde_json::to_string_pretty(&points).unwrap() + "\n").unwrap_or_else(|e| fail(format!("{}: {e}", b.display())));
    }
    println!("lift-stats: {}", out.display());
}

/// `table` (Markdown) appended to the job's summary on GitHub (GITHUB_STEP_SUMMARY)
fn summary(table: &str) {
    if let Some(path) = std::env::var_os("GITHUB_STEP_SUMMARY") {
        use std::io::Write;
        if let Ok(mut f) = std::fs::OpenOptions::new().append(true).create(true).open(path) {
            let _ = writeln!(f, "{table}");
        }
    }
}

fn read_json(p: &Path) -> Value {
    let t = std::fs::read_to_string(p).unwrap_or_else(|e| fail(format!("{}: {e}", p.display())));
    serde_json::from_str(&t).unwrap_or_else(|e| fail(format!("{}: {e}", p.display())))
}

fn cpu_model() -> String {
    let t = std::fs::read_to_string("/proc/cpuinfo").unwrap_or_default();
    t.lines().find(|l| l.starts_with("model name")).and_then(|l| l.split(':').nth(1)).map_or("unknown".into(), |m| m.trim().to_string())
}

fn show(kind: &str, v: f64) -> String {
    match kind {
        "time" => format!("{v:.2} s"),
        "memory" => format!("{v:.0} MB"),
        "size" => format!("{:.2} MB", v / 1e6),
        _ => format!("{v:.0}"),
    }
}

/// a time below this many seconds is too short to compare: its noise is larger than any threshold
const MIN_TIME_S: f64 = 2.0;

/// What a change of one metric is
#[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Clone, Copy)]
pub enum Verdict {
    Same,
    /// changed, but its kind is not judged (a count), or a time not comparable (another CPU, or too short)
    Info,
    Better,
    Warn,
    Fail,
}

/// The verdict of `old` -> `new` for a metric of `kind`: times and memory regress when they grow by more than `warn`
/// or `fail` percent, sizes warn only; `comparable`: the same CPU and jobs, without which times and memory are not
/// judged
pub fn judge(kind: &str, old: f64, new: f64, comparable: bool, warn: f64, fail: f64) -> Verdict {
    if old == new {
        return Verdict::Same;
    }
    let pct = if old == 0.0 { f64::INFINITY } else { (new - old) / old * 100.0 };
    match kind {
        "time" | "memory" if !comparable || (kind == "time" && old.max(new) < MIN_TIME_S) => Verdict::Info,
        "time" | "memory" | "size" if pct < -warn => Verdict::Better,
        "time" | "memory" if pct > fail => Verdict::Fail,
        "time" | "memory" | "size" if pct > warn => Verdict::Warn,
        "time" | "memory" | "size" => Verdict::Same,
        _ => Verdict::Info,
    }
}

fn compare(args: &[String]) {
    let (mut files, mut warn, mut fail_pct) = (Vec::new(), 10.0f64, 25.0f64);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().and_then(|v| v.parse::<f64>().ok()).unwrap_or_else(|| fail(format!("{a} <pct>")));
        match a.as_str() {
            "--warn" => warn = val(),
            "--fail" => fail_pct = val(),
            _ => files.push(PathBuf::from(a)),
        }
    }
    let [base, new] = &files[..] else { fail(USAGE) };
    let (b, n) = (read_json(base), read_json(new));
    let comparable = b["cpu"] == n["cpu"] && b["jobs"] == n["jobs"];
    let game = n["game"].as_str().unwrap_or("?").to_string();
    let gha = std::env::var("GITHUB_ACTIONS").is_ok_and(|v| v == "true");
    let mut table = format!(
        "### lift-stats {game}\n\nwarn above {warn}%, fail above {fail_pct}%{}\n\n| metric | baseline | now | change | |\n|---|---:|---:|---:|---|\n",
        if comparable { String::new() } else { "; times and memory not judged: another CPU or job count than the baseline's".into() }
    );
    println!("lift-stats {game}: against {}{}", base.display(), if comparable { "" } else { " (another CPU or job count: times and memory not judged)" });
    let mut worst = Verdict::Same;
    let empty = Map::new();
    let old_m = b["metrics"].as_object().unwrap_or(&empty);
    for (name, m) in n["metrics"].as_object().unwrap_or(&empty) {
        let kind = m["kind"].as_str().unwrap_or("");
        let v = m["value"].as_f64().unwrap_or(0.0);
        let Some(o) = old_m.get(name).and_then(|o| o["value"].as_f64()) else {
            println!("  {name:<20} {:>14} (new)", show(kind, v));
            continue;
        };
        let verdict = judge(kind, o, v, comparable, warn, fail_pct);
        worst = worst.max(verdict);
        let pct = if o == 0.0 { 0.0 } else { (v - o) / o * 100.0 };
        let mark = match verdict {
            Verdict::Same => "",
            Verdict::Info => "changed",
            Verdict::Better => "better",
            Verdict::Warn => "WARN",
            Verdict::Fail => "FAIL",
        };
        println!("  {name:<20} {:>14} -> {:>14} {pct:>+7.1}% {mark}", show(kind, o), show(kind, v));
        table.push_str(&format!("| {name} | {} | {} | {pct:+.1}% | {mark} |\n", show(kind, o), show(kind, v)));
        if gha && matches!(verdict, Verdict::Warn | Verdict::Fail) {
            let level = if verdict == Verdict::Fail { "error" } else { "warning" };
            println!("::{level} title=lift-stats {game}::{name} {} -> {} ({pct:+.1}%)", show(kind, o), show(kind, v));
        }
    }
    summary(&table);
    if worst == Verdict::Fail {
        fail(format!("{game}: a time or the memory grew by more than {fail_pct}%"));
    }
}

#[cfg(test)]
mod tests {
    use super::{judge, Verdict};

    #[test]
    fn verdicts() {
        assert_eq!(judge("time", 100.0, 100.0, true, 10.0, 25.0), Verdict::Same);
        assert_eq!(judge("time", 100.0, 105.0, true, 10.0, 25.0), Verdict::Same);
        assert_eq!(judge("time", 100.0, 115.0, true, 10.0, 25.0), Verdict::Warn);
        assert_eq!(judge("time", 100.0, 130.0, true, 10.0, 25.0), Verdict::Fail);
        assert_eq!(judge("time", 100.0, 80.0, true, 10.0, 25.0), Verdict::Better);
        // (another CPU, or a stage too short to time)
        assert_eq!(judge("time", 100.0, 200.0, false, 10.0, 25.0), Verdict::Info);
        assert_eq!(judge("time", 0.5, 1.5, true, 10.0, 25.0), Verdict::Info);
        assert_eq!(judge("memory", 1000.0, 1300.0, true, 10.0, 25.0), Verdict::Fail);
        // (sizes warn, counts are told)
        assert_eq!(judge("size", 1000.0, 2000.0, true, 10.0, 25.0), Verdict::Warn);
        assert_eq!(judge("size", 1000.0, 2000.0, false, 10.0, 25.0), Verdict::Warn);
        assert_eq!(judge("count", 10.0, 11.0, true, 10.0, 25.0), Verdict::Info);
    }
}
