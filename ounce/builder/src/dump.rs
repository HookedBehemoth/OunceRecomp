//! The dump step: `hookshot-builder dump`, and the exefs-then-romfs order `build --dump` runs.

use crate::fail;
use ounce_dump::extract::Progress;
use ounce_dump::nup::{self, Nup, Part};
use ounce_dump::{Done, Dump};
use std::path::{Path, PathBuf};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

fn report(part: &str, d: &Done, secs: f64) {
    if d.in_place {
        println!("dump: {part} read in place");
    } else {
        println!("dump: {part}: {} files written, {:.2} GB in {secs:.1} s", d.written.files, d.written.bytes as f64 / 1e9);
    }
    for n in &d.notes {
        println!("  {n}");
    }
}

/// Writes `dir/exefs` and checks it is the supported game's; returns it and the thread writing `dir/romfs`.
pub fn exefs_then_romfs(src: &Path, title: Option<&str>, dir: &Path, game: &Path) -> Result<(PathBuf, JoinHandle<Result<(), String>>), String> {
    let d = Dump::open(src, title, dir)?;
    println!("dump: {}", d.describe());
    let t = Instant::now();
    report("exefs", &d.exefs(&mut Progress::new(None, ""))?, t.elapsed().as_secs_f64());
    let exefs = dir.join("exefs");
    ounce_lift::game::Game::load(game)?.check_exefs(&exefs)?;
    let romfs = std::thread::spawn(move || {
        let t = Instant::now();
        let r = d.romfs(&mut Progress::new(Some(Duration::from_secs(30)), "romfs: "))?;
        report("romfs", &r, t.elapsed().as_secs_f64());
        Ok(())
    });
    Ok((exefs, romfs))
}

pub fn finish_romfs(h: JoinHandle<Result<(), String>>) -> Result<(), String> {
    if !h.is_finished() {
        println!("dump: waiting for the romfs");
    }
    h.join().map_err(|_| "the romfs thread panicked".to_string())?
}

/// `dump <file.nup|folder> [--verify] [--extract <dir>] [--title <t>] [--only exefs|romfs]`
pub fn command(args: &[String]) {
    let (mut src, mut out, mut verify, mut title, mut only) = (None::<PathBuf>, None::<PathBuf>, false, None::<String>, None::<Part>);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "--verify" => verify = true,
            "--extract" => out = Some(val().into()),
            "--title" => title = Some(val()),
            "--only" => {
                only = Some(match val().as_str() {
                    "exefs" => Part::Exefs,
                    "romfs" => Part::Romfs,
                    o => fail(format!("--only exefs|romfs, not {o}")),
                })
            }
            _ if src.is_none() && !a.starts_with('-') => src = Some(a.into()),
            _ => fail(format!("dump: what is {a}?")),
        }
    }
    let Some(src) = src else { fail("dump <file.nup|folder> [--verify] [--extract <dir>] [--title <t>] [--only exefs|romfs]") };
    if verify {
        if out.is_some() {
            fail("--verify does not extract; run the extraction separately");
        }
        let nup = Nup::open(&src, true).unwrap_or_else(|e| {
            println!("BAD package: {e}");
            std::process::exit(1)
        });
        let titles = title.map(|t| vec![nup.pick_title(Some(&t)).unwrap_or_else(|e| fail(e)).clone()]);
        let v = nup::verify(&nup, titles.as_deref(), only);
        for l in &v.lines {
            println!("{l}");
        }
        println!("verified {} records, {} unique extents: {} bad; game .zs headers only (not inner decode/authenticity)", v.checked, v.decoded, v.bad);
        std::process::exit(i32::from(v.bad > 0));
    }
    let Some(out) = out else {
        list(&src);
        return;
    };
    let d = Dump::open(&src, title.as_deref(), &out).unwrap_or_else(|e| fail(e));
    let free = ounce_dump::extract::free_gb(&nearest_dir(&out)).unwrap_or(0.0);
    println!("{}; writing to {} ({free:.0} GB free)", d.describe(), out.display());
    let mut p = Progress::new(Some(Duration::from_secs(2)), "");
    if only != Some(Part::Romfs) {
        let t = Instant::now();
        report("exefs", &d.exefs(&mut p).unwrap_or_else(|e| fail(e)), t.elapsed().as_secs_f64());
    }
    if only != Some(Part::Exefs) {
        let t = Instant::now();
        report("romfs", &d.romfs(&mut p).unwrap_or_else(|e| fail(e)), t.elapsed().as_secs_f64());
    }
}

/// The deepest existing folder of `p`, where its free space is measured.
fn nearest_dir(p: &Path) -> PathBuf {
    let mut p = std::path::absolute(p).unwrap_or_else(|_| p.to_path_buf());
    while !p.is_dir() && p.pop() {}
    p
}

fn list(src: &Path) {
    if src.is_dir() {
        for part in ["exefs", "romfs"] {
            let files = ounce_dump::extract::list_folder(&src.join(part)).unwrap_or_else(|e| fail(e));
            println!("{part}: {} files, {:.2} GB", files.len(), files.iter().map(|f| f.1).sum::<u64>() as f64 / 1e9);
        }
        return;
    }
    let nup = Nup::open(src, false).unwrap_or_else(|e| fail(e));
    println!("{}: {:.2} GB, {} titles, {} dirs, {} files", src.display(), nup.file_size as f64 / 1e9, nup.titles.len(), nup.dirs.len(), nup.files.len());
    for t in &nup.titles {
        let walk = |d: u32| if d == nup::NONE { vec![] } else { nup.walk(d).unwrap_or_else(|e| fail(e)) };
        let (ex, ro) = (walk(t.exefs), walk(t.romfs));
        let ex: Vec<String> = ex.iter().map(|(p, i)| format!("{p} {}", nup.files[*i].size)).collect();
        println!(
            "title {}: {:016x} v{}  exefs {} files ({})  romfs {} files, {:.2} GB",
            t.index,
            t.id,
            t.version,
            ex.len(),
            ex.join(", "),
            ro.len(),
            ro.iter().map(|(_, i)| nup.files[*i].size).sum::<u64>() as f64 / 1e9
        );
    }
}
