//! The smoke test's verdict (`ounce test`): did anything blow up in one run? About ten lines, never the log.
//!
//! Against the test's golden folder (the data folder's `<game>/golden/<test>/`: golden.json, known.txt) it checks:
//! - fatal signs in log.txt (device lost, Vulkan errors, crashes, traps) and a run that stopped presenting;
//! - pictures from present `skip` on: a 64-bit DCT perceptual hash of each screenshot against the golden runs' shot of
//!   the same present (Hamming distance; two near-black shots are equal, one black and one not a failure), and its
//!   colours in a 16 x 9 thumbnail, which catch a missing layer the hash does not (Kokiri's grass);
//! - rough numbers: steady fps and replay times from the renderer's log lines, peak RSS, the least free and the most
//!   shared memory; shown beside the golden run's;
//! - warning and error groups: log lines that look like one, normalised (numbers and addresses masked), less the known
//!   ones (known.txt: one normalised line each, or `re:<regex>`; `#` comments). New ones are listed; new error-like
//!   ones fail.
//!
//! `record` adds the run to the golden set instead (its hashes, its numbers, and its warning groups as known). The
//! hash and the normalised lines are made exactly as the Python verdict before it made them, so older golden sets hold.

use regex::Regex;
use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, HashMap};
use std::path::Path;

/// of 64 bits
const HASH_BAD: u32 = 12;
/// the mean luminance below which a shot counts as black
const DARK: f64 = 8.0;
/// The colour thumbnail's cells (16 x 9: 80-pixel squares at 1280 x 720). A cell is off when a channel of its mean
/// differs from the golden shot's by more than CELL_BAD, and a shot is bad when CELLS_BAD cells are off against every
/// golden run. The hash misses a layer that changes colour but not shape: Kokiri's grass gone moves 14 to 17 cells,
/// golden runs of the same scene at most 4.
const THUMB_W: usize = 16;
const THUMB_H: usize = 9;
const CELL_BAD: u8 = 12;
const CELLS_BAD: usize = 8;

struct Patterns {
    fatal: Regex,
    warn: Regex,
    errish: Regex,
    lead: Regex,
    hex: Regex,
    long_hex: Regex,
    num: Regex,
    presents: Regex,
    replay: Regex,
}

impl Patterns {
    fn new() -> Patterns {
        let r = |s: &str| Regex::new(s).unwrap();
        Patterns {
            fatal: r(r"(?i)device lost|DEVICE_LOST|VK_ERROR_|segmentation fault|SIGSEGV|SIGABRT|a64_trap|unhandled exception|stack overflow|terminate called|Assertion .* failed"),
            warn: r(r"(?i)\b(warn(ing)?|error|fail(ed|s|ure)?|assert|abort|unhandled|unimplemented|stub(bed)?|timeout|timed out|invalid|mismatch(es)?|stale|lost)\b"),
            errish: r(r"(?i)\b(error|assert|abort|unhandled|lost)\b"),
            lead: r(r"^\[[^\]]*\]\s*"),
            hex: r(r"0x[0-9a-fA-F]+"),
            long_hex: r(r"\b[0-9a-f]{8,}\b"),
            num: r(r"\d+(\.\d+)?"),
            presents: r(r"vk: (\d+) presents, ([\d.]+) fps"),
            replay: r(r"replay time \(ms\): submit ([\d.]+).*?draw ([\d.]+)"),
        }
    }

    /// a log line as its group: the leading [time] gone, addresses and numbers masked, at most 200 characters
    fn norm(&self, line: &str) -> String {
        let l = self.lead.replace(line, "");
        let l = self.hex.replace_all(&l, "#");
        let l = self.long_hex.replace_all(&l, "#");
        let l = self.num.replace_all(&l, "N");
        l.chars().take(200).collect()
    }
}

fn clip(s: &str) -> String {
    s.trim().chars().take(200).collect()
}

/// PIL's 8-bit resampling coefficients for one axis (Resample.c: precompute_coeffs, normalize_coeffs_8bpc), Lanczos 3
fn coeffs(in_size: usize, out_size: usize) -> Vec<(usize, Vec<i64>)> {
    fn sinc(x: f64) -> f64 {
        if x == 0.0 { 1.0 } else { (x * std::f64::consts::PI).sin() / (x * std::f64::consts::PI) }
    }
    let lanczos = |x: f64| if (-3.0..3.0).contains(&x) { sinc(x) * sinc(x / 3.0) } else { 0.0 };
    let scale = in_size as f64 / out_size as f64;
    let fscale = scale.max(1.0);
    let support = 3.0 * fscale;
    (0..out_size)
        .map(|xx| {
            let center = (xx as f64 + 0.5) * scale;
            let xmin = ((center - support + 0.5) as i64).max(0) as usize;
            let xmax = ((center + support + 0.5) as i64).min(in_size as i64) as usize;
            let k: Vec<f64> = (xmin..xmax).map(|x| lanczos((x as f64 - center + 0.5) / fscale)).collect();
            let ww: f64 = k.iter().sum();
            let k = k.iter().map(|&w| {
                let w = if ww != 0.0 { w / ww } else { w };
                let f = w * (1u64 << 22) as f64;
                (if w < 0.0 { -0.5 + f } else { 0.5 + f }) as i64
            });
            (xmin, k.collect())
        })
        .collect()
}

fn clip8(v: i64) -> u8 {
    if v >= 255 << 22 {
        255
    } else if v <= 0 {
        0
    } else {
        (v >> 22) as u8
    }
}

/// PIL's `resize((w, h), LANCZOS)` of an 8-bit grey picture: horizontally, then vertically, rounded to 8 bits between
fn resize(px: &[u8], w: usize, h: usize, ow: usize, oh: usize) -> Vec<u8> {
    let (cx, cy) = (coeffs(w, ow), coeffs(h, oh));
    let mut tmp = vec![0u8; ow * h];
    for y in 0..h {
        for (xx, (xmin, k)) in cx.iter().enumerate() {
            let s = k.iter().enumerate().fold(1i64 << 21, |s, (i, &c)| s + px[y * w + xmin + i] as i64 * c);
            tmp[y * ow + xx] = clip8(s);
        }
    }
    let mut out = vec![0u8; ow * oh];
    for (yy, (ymin, k)) in cy.iter().enumerate() {
        for x in 0..ow {
            let s = k.iter().enumerate().fold(1i64 << 21, |s, (i, &c)| s + tmp[(ymin + i) * ow + x] as i64 * c);
            out[yy * ow + x] = clip8(s);
        }
    }
    out
}

/// A PNG as 8-bit RGB pixels, with its size.
fn rgb(path: &Path) -> Result<(Vec<[u8; 3]>, usize, usize), String> {
    let f = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let mut d = png::Decoder::new(std::io::BufReader::new(f));
    d.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
    let mut r = d.read_info().map_err(|e| e.to_string())?;
    let mut buf = vec![0; r.output_buffer_size().ok_or("too large")?];
    let info = r.next_frame(&mut buf).map_err(|e| e.to_string())?;
    let (w, h) = (info.width as usize, info.height as usize);
    let n = match info.color_type {
        png::ColorType::Grayscale => 1,
        png::ColorType::GrayscaleAlpha => 2,
        png::ColorType::Rgb => 3,
        png::ColorType::Rgba => 4,
        c => return Err(format!("{c:?} pictures")),
    };
    let px = buf[..info.buffer_size()]
        .chunks_exact(info.line_size)
        .flat_map(|row| row[..w * n].chunks_exact(n))
        .map(|p| if n < 3 { [p[0]; 3] } else { [p[0], p[1], p[2]] })
        .collect();
    Ok((px, w, h))
}

/// RGB pixels as 8-bit grey (PIL's convert("L"): ITU-R 601-2 in 16-bit fixed point).
fn grey(px: &[[u8; 3]]) -> Vec<u8> {
    px.iter().map(|p| ((p[0] as u32 * 19595 + p[1] as u32 * 38470 + p[2] as u32 * 7471 + 0x8000) >> 16) as u8).collect()
}

/// The shot's colours in THUMB_W x THUMB_H cells, each its pixels' mean, as hex (RGB per cell, rows from the top).
fn thumb(px: &[[u8; 3]], w: usize, h: usize) -> String {
    let mut out = String::with_capacity(THUMB_W * THUMB_H * 6);
    for cy in 0..THUMB_H {
        let (y0, y1) = (cy * h / THUMB_H, ((cy + 1) * h / THUMB_H).max(cy * h / THUMB_H + 1));
        for cx in 0..THUMB_W {
            let (x0, x1) = (cx * w / THUMB_W, ((cx + 1) * w / THUMB_W).max(cx * w / THUMB_W + 1));
            let mut sum = [0u64; 3];
            for y in y0..y1.min(h) {
                for p in &px[y * w + x0..y * w + x1.min(w)] {
                    for c in 0..3 {
                        sum[c] += p[c] as u64;
                    }
                }
            }
            let n = ((y1.min(h) - y0) * (x1.min(w) - x0)).max(1) as u64;
            for v in sum {
                out.push_str(&format!("{:02x}", (v + n / 2) / n));
            }
        }
    }
    out
}

/// How many cells of two thumbnails differ by more than CELL_BAD in a channel (None: one is not a thumbnail).
fn cells_off(a: &str, b: &str) -> Option<usize> {
    let dec = |s: &str| -> Option<Vec<u8>> {
        (s.len() == THUMB_W * THUMB_H * 6).then_some(())?;
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
    };
    let (a, b) = (dec(a)?, dec(b)?);
    Some(a.chunks_exact(3).zip(b.chunks_exact(3)).filter(|(x, y)| x.iter().zip(y.iter()).any(|(p, q)| p.abs_diff(*q) > CELL_BAD)).count())
}

/// The shot's perceptual hash (the 8x8 low frequencies of a 32x32 DCT, each above their median or not) and its mean.
fn hash_of(px: &[u8], w: usize, h: usize) -> (u64, f64) {
    let a: Vec<f64> = resize(px, w, h, 32, 32).into_iter().map(f64::from).collect();
    let c = |k: usize, n: usize| (std::f64::consts::PI * (2 * n + 1) as f64 * k as f64 / 64.0).cos();
    // (C a C^T, its top left 8x8)
    let mut ca = [[0f64; 32]; 8];
    for (k, row) in ca.iter_mut().enumerate() {
        for (j, v) in row.iter_mut().enumerate() {
            *v = (0..32).map(|n| c(k, n) * a[n * 32 + j]).sum();
        }
    }
    let mut d = [0f64; 64];
    for k in 0..8 {
        for l in 0..8 {
            d[k * 8 + l] = (0..32).map(|j| ca[k][j] * c(l, j)).sum();
        }
    }
    let mut rest = d[1..].to_vec();
    rest.sort_by(f64::total_cmp);
    let med = rest[rest.len() / 2];
    let hash = d.iter().fold(0u64, |h, &v| (h << 1) | (v > med) as u64);
    (hash, a.iter().sum::<f64>() / a.len() as f64)
}

/// A shot's signature: its hash and mean (None: unreadable), and its colour thumbnail.
type Shot = (Option<u64>, f64, String);

/// present -> its shot's signature
fn shots(run: &Path) -> BTreeMap<u64, Shot> {
    let mut out = BTreeMap::new();
    for e in std::fs::read_dir(run.join("shots")).into_iter().flatten().flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let Some(n) = name.strip_prefix("vk_").and_then(|r| r.strip_suffix(".png")).and_then(|n| n.parse().ok()) else { continue };
        let sig = match rgb(&e.path()) {
            Ok((px, w, h)) => {
                let (hash, mean) = hash_of(&grey(&px), w, h);
                (Some(hash), mean, thumb(&px, w, h))
            }
            Err(_) => (None, 0.0, String::new()),
        };
        out.insert(n, sig);
    }
    out
}

fn lines(path: &Path) -> Vec<String> {
    let raw = std::fs::read(path).unwrap_or_default();
    String::from_utf8_lossy(&raw).lines().map(String::from).collect()
}

/// the median of the last four, to two places (the steady state)
fn tail(v: &[f64]) -> Value {
    if v.is_empty() {
        return Value::Null;
    }
    let mut t = v[v.len().saturating_sub(4)..].to_vec();
    t.sort_by(f64::total_cmp);
    let m = if t.len() % 2 == 1 { t[t.len() / 2] } else { (t[t.len() / 2 - 1] + t[t.len() / 2]) / 2.0 };
    json!((m * 100.0).round() / 100.0)
}

const COLS: [&str; 7] = ["presents", "fps", "submit_ms", "draw_ms", "rss_mb", "min_avail_mb", "max_shmem_mb"];

fn numbers(p: &Patterns, run: &Path) -> Map<String, Value> {
    let (mut fps, mut submit, mut draw, mut presents) = (Vec::new(), Vec::new(), Vec::new(), 0u64);
    for l in lines(&run.join("log.txt")) {
        if let Some(c) = p.presents.captures(&l) {
            presents = c[1].parse().unwrap_or(0);
            fps.push(c[2].parse().unwrap_or(0.0));
        }
        if let Some(c) = p.replay.captures(&l) {
            submit.push(c[1].parse().unwrap_or(0.0));
            draw.push(c[2].parse().unwrap_or(0.0));
        }
    }
    let col = |file: &str, k: usize| -> Vec<u64> {
        lines(&run.join(file)).iter().filter_map(|l| l.split_whitespace().nth(k)?.parse().ok()).collect()
    };
    let rss = col("timeline.txt", 2).into_iter().max().unwrap_or(0);
    let avail = col("mem.txt", 0).into_iter().min();
    let shm = col("mem.txt", 1).into_iter().max().unwrap_or(0);
    let vals = [json!(presents), tail(&fps), tail(&submit), tail(&draw), json!(rss), avail.map_or(Value::Null, |a| json!(a)), json!(shm)];
    COLS.iter().map(|c| c.to_string()).zip(vals).collect()
}

/// (group -> count, group -> its first line, the first fatal lines)
fn groups(p: &Patterns, run: &Path) -> (HashMap<String, u64>, HashMap<String, String>, Vec<String>) {
    let (mut g, mut ex, mut fatal) = (HashMap::new(), HashMap::new(), Vec::new());
    for l in lines(&run.join("log.txt")) {
        if fatal.len() < 5 && p.fatal.is_match(&l) {
            fatal.push(clip(&l));
        }
        if p.warn.is_match(&l) {
            let k = p.norm(&l);
            *g.entry(k.clone()).or_insert(0) += 1;
            ex.entry(k).or_insert_with(|| clip(&l));
        }
    }
    (g, ex, fatal)
}

struct Known {
    exact: std::collections::HashSet<String>,
    res: Vec<Regex>,
}

impl Known {
    fn load(gd: &Path) -> Known {
        let mut k = Known { exact: Default::default(), res: Vec::new() };
        for l in lines(&gd.join("known.txt")) {
            if l.is_empty() || l.starts_with('#') {
                continue;
            }
            match l.strip_prefix("re:") {
                Some(r) => match Regex::new(r) {
                    Ok(r) => k.res.push(r),
                    Err(e) => eprintln!("known.txt: {r}: {e}"),
                },
                None => {
                    k.exact.insert(l);
                }
            }
        }
        k
    }

    fn has(&self, g: &str) -> bool {
        self.exact.contains(g) || self.res.iter().any(|r| r.is_match(g))
    }
}

/// golden.json's runs (an old file holding one run is that run)
fn golden_runs(gd: &Path) -> Vec<Value> {
    let Ok(text) = std::fs::read_to_string(gd.join("golden.json")) else { return Vec::new() };
    let Ok(v) = serde_json::from_str::<Value>(&text) else { return Vec::new() };
    match v.get("runs") {
        Some(Value::Array(r)) => r.clone(),
        _ if v.get("shots").is_some() => vec![v],
        _ => Vec::new(),
    }
}

fn show(v: Option<&Value>) -> String {
    match v {
        None | Some(Value::Null) => "-".into(),
        Some(v) => v.to_string(),
    }
}

/// `record`: the run joins the golden set (`reset`: the set starts anew with it). Returns the verdict's lines and
/// whether it passed; `skip` presents are left out before pictures are compared.
pub fn verdict(run: &Path, gd: &Path, skip: u64, record: bool, reset: bool) -> Result<(Vec<String>, bool), String> {
    let p = Patterns::new();
    std::fs::create_dir_all(gd).map_err(|e| format!("{}: {e}", gd.display()))?;
    let num = numbers(&p, run);
    let sh = shots(run);
    let (g, ex, fatal) = groups(&p, run);
    let mut out = Vec::new();
    if record {
        let mut runs = if reset { Vec::new() } else { golden_runs(gd) };
        let shots: Map<String, Value> = sh.iter().map(|(k, (h, m, t))| (k.to_string(), json!([h, m, t]))).collect();
        runs.push(json!({ "numbers": num, "shots": shots }));
        let n = runs.len();
        let text = serde_json::to_string_pretty(&json!({ "runs": runs })).map_err(|e| e.to_string())?;
        std::fs::write(gd.join("golden.json"), text).map_err(|e| format!("{}: {e}", gd.display()))?;
        let known = Known::load(gd);
        let mut add = format!("# from golden run {}\n", run.file_name().unwrap_or_default().to_string_lossy());
        let mut new: Vec<&String> = g.keys().filter(|k| !known.has(k)).collect();
        new.sort();
        for k in new {
            add.push_str(k);
            add.push('\n');
        }
        use std::io::Write;
        std::fs::OpenOptions::new().create(true).append(true).open(gd.join("known.txt")).and_then(|mut f| f.write_all(add.as_bytes())).map_err(|e| e.to_string())?;
        out.push(format!("golden recorded ({n} golden run(s) now): {} shots, {} warning groups as known; {}", sh.len(), g.len(), Value::Object(num)));
        if !fatal.is_empty() {
            out.push(format!("NOTE: the golden run has fatal lines:\n  {}", fatal.join("\n  ")));
        }
        return Ok((out, true));
    }
    let gruns = golden_runs(gd);
    let mut fails: Vec<&str> = Vec::new();
    let status = lines(&run.join("run.out")).pop();
    out.push(format!("run: {}", status.unwrap_or_else(|| run.file_name().unwrap_or_default().to_string_lossy().into_owned())));
    if !fatal.is_empty() {
        fails.push("fatal");
        out.push(format!("FATAL ({} shown):\n  {}", fatal.len(), fatal.join("\n  ")));
    }
    let set_of = |r: &Value| -> HashMap<u64, Shot> {
        r.get("shots").and_then(Value::as_object).into_iter().flatten().filter_map(|(k, v)| {
            let t = v.get(2).and_then(Value::as_str).unwrap_or_default().to_string();
            Some((k.parse().ok()?, (v.get(0).and_then(Value::as_u64), v.get(1).and_then(Value::as_f64).unwrap_or(0.0), t)))
        }).collect()
    };
    if let Some(last) = gruns.last() {
        // (several golden runs: a shot passes when it is close to any, so a scene's own motion is in the set)
        let gsets: Vec<_> = gruns.iter().map(set_of).collect();
        let gs = set_of(last);
        let mut common: Vec<u64> = gs.keys().filter(|p| **p >= skip && sh.contains_key(p)).copied().collect();
        common.sort();
        let (mut bad, mut worst) = (Vec::new(), (0u32, None::<u64>));
        // (the colours: shots compared, the worst shot's cells off and its present; golden runs older than the
        // thumbnails have none)
        let (mut ccount, mut cworst) = (0usize, (0usize, None::<u64>));
        for &pr in &common {
            let (h, m, ref t) = sh[&pr];
            let off = gsets.iter().filter_map(|g| cells_off(t, &g.get(&pr)?.2)).min();
            if let Some(off) = off {
                ccount += 1;
                if cworst.1.is_none() || off > cworst.0 {
                    cworst = (off, Some(pr));
                }
                if off >= CELLS_BAD {
                    bad.push((pr, format!("colours: {off} of {} cells off", THUMB_W * THUMB_H)));
                }
            }
            let mut best: Option<(u32, Option<String>)> = None;
            for gset in &gsets {
                let Some(&(gh, gm, _)) = gset.get(&pr) else { continue };
                let why = match (h, gh) {
                    (None, _) | (_, None) => (99, Some("unreadable".to_string())),
                    _ if m < DARK && gm < DARK => (0, None),
                    _ if (m < DARK) != (gm < DARK) => (98, Some(format!("black vs not (mean {m:.0} vs {gm:.0})"))),
                    (Some(a), Some(b)) => {
                        let d = (a ^ b).count_ones();
                        (d, (d > HASH_BAD).then(|| format!("hash distance {d}")))
                    }
                };
                if best.as_ref().is_none_or(|b| why.0 < b.0) {
                    best = Some(why);
                }
            }
            let (d, why) = best.unwrap();
            if d < 98 && (worst.1.is_none() || d > worst.0) {
                worst = (d, Some(pr));
            }
            if let Some(w) = why {
                bad.push((pr, w));
            }
        }
        bad.sort();
        let missing = gs.keys().filter(|p| **p >= skip && !sh.contains_key(p)).count();
        out.push(format!(
            "pictures: {} compared against {} golden run(s), worst distance {} (present {}), {} bad{}",
            common.len(),
            gsets.len(),
            worst.0,
            worst.1.map_or("-".into(), |p| p.to_string()),
            bad.len(),
            if missing > 0 { format!(", {missing} golden shots missing") } else { String::new() }
        ));
        out.push(match cworst.1 {
            Some(p) => format!("colours: {ccount} compared, worst {} of {} cells off (present {p}); bad from {CELLS_BAD}", cworst.0, THUMB_W * THUMB_H),
            None => "colours: no golden run has thumbnails (ounce test <game> --golden records one)".into(),
        });
        for (pr, why) in bad.iter().take(6) {
            out.push(format!("  present {pr}: {why}"));
        }
        if !bad.is_empty() {
            fails.push("pictures");
        }
        if missing > 0 && missing > gs.len() / 4 {
            fails.push("stall (few shots)");
        }
    } else {
        out.push("pictures: no golden run (ounce test <game> --golden records one)".into());
    }
    let gn = gruns.last().and_then(|r| r.get("numbers")).and_then(Value::as_object);
    out.push(format!(
        "numbers:  {}",
        COLS.iter().map(|c| format!("{c} {}{}", show(num.get(*c)), gn.map_or(String::new(), |g| format!(" (golden {})", show(g.get(*c)))))).collect::<Vec<_>>().join("  ")
    ));
    let gp = gn.and_then(|g| g.get("presents")).and_then(Value::as_u64).unwrap_or(0);
    if gp > 0 && (num["presents"].as_u64().unwrap_or(0) as f64) < 0.6 * gp as f64 {
        fails.push("stall (presents)");
    }
    let known = Known::load(gd);
    let mut new: Vec<(u64, &String)> = g.iter().filter(|(k, _)| !known.has(k)).map(|(k, &n)| (n, k)).collect();
    new.sort_by(|a, b| b.cmp(a));
    let newerr = new.iter().filter(|x| p.errish.is_match(x.1)).count();
    out.push(format!("log: {} warning/error groups, {} known, {} new ({newerr} error-like)", g.len(), g.len() - new.len(), new.len()));
    for (n, k) in new.iter().take(12) {
        out.push(format!("  {n:5} x {}", ex[*k]));
    }
    if newerr > 0 {
        fails.push("new errors");
    }
    let pass = fails.is_empty();
    out.push(format!("VERDICT: {}", if pass { "PASS".to_string() } else { format!("FAIL ({})", fails.join(", ")) }));
    Ok((out, pass))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lines_are_grouped_as_smoke_py_grouped_them() {
        let p = Patterns::new();
        assert_eq!(p.norm("[12.5] vk: warning at 0xdeadBEEF, 3 of 4.5 (cafebabe12)"), "vk: warning at #, N of N (#)");
        assert!(p.warn.is_match("a Stub here") && !p.warn.is_match("stubborn"));
        assert!(p.fatal.is_match("Assertion `x' failed"));
    }

    #[test]
    fn thumbnails_count_the_cells_off() {
        let (w, h) = (64, 36);
        let a = vec![[40u8, 90, 30]; w * h];
        let mut b = a.clone();
        for y in 0..h / 3 {
            for x in 0..w {
                b[y * w + x] = [40, 90 + CELL_BAD + 1, 30];
            }
        }
        let (ta, tb) = (thumb(&a, w, h), thumb(&b, w, h));
        assert_eq!(cells_off(&ta, &ta), Some(0));
        assert_eq!(cells_off(&ta, &tb), Some(THUMB_W * 3));
        assert_eq!(cells_off(&ta, ""), None);
    }

    #[test]
    fn resampling_keeps_a_flat_picture_flat() {
        let px = vec![77u8; 640 * 360];
        assert!(resize(&px, 640, 360, 32, 32).iter().all(|&v| v == 77));
    }
}
