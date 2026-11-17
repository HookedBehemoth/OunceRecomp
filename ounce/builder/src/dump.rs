//! The dump step: `hookshot-builder dump`, and the exefs-then-romfs order `build --dump` runs.

use crate::fail;
use ounce_dump::extract::Progress;
use ounce_dump::nup::{self, Nup, Part};
use ounce_dump::{Done, Dump};
use ounce_lift::exefs::Exefs;
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

/// The exefs, read in memory (written into `<dir>/exefs` too when `write_exefs`: a developer's dump folder) and checked
/// against the game description; the dump stays open for its romfs, which goes into `<dir>/romfs`.
pub fn exefs(src: &Path, title: Option<&str>, dir: &Path, game: &Path, write_exefs: bool) -> Result<(Dump, Exefs), String> {
    let d = Dump::open(src, title, dir)?;
    println!("dump: {}", d.describe());
    let t = Instant::now();
    let ex = if write_exefs {
        report("exefs", &d.exefs(&mut Progress::new(None, ""))?, t.elapsed().as_secs_f64());
        Exefs::read_dir(&dir.join("exefs"))?
    } else {
        let (files, notes) = d.exefs_files()?;
        println!("dump: exefs read in memory: {} files, {:.0} MB in {:.1} s", files.len(), files.values().map(|f| f.len()).sum::<usize>() as f64 / 1e6, t.elapsed().as_secs_f64());
        for n in &notes {
            println!("  {n}");
        }
        Exefs { files }
    };
    ounce_lift::game::Game::load(game)?.check_exefs(&ex)?;
    Ok((d, ex))
}

/// The thread writing the dump's romfs (`<dump's dir>/romfs`), then copying it into `copy_to/romfs` when given (a
/// developer's dump folder and the game folder). Files named in `stale` (romfs paths, the game folder's hollowed
/// files of an earlier build) are removed first, so they are written whole again.
pub fn romfs(d: Dump, copy_to: Option<PathBuf>, stale: Vec<String>) -> JoinHandle<Result<(), String>> {
    std::thread::spawn(move || {
        let dest = copy_to.as_deref().unwrap_or(&d.out).join("romfs");
        for f in &stale {
            let p = ounce_dump::extract::extraction_path(dest.parent().unwrap(), "romfs", f, ".part")?;
            let _ = std::fs::remove_file(p);
        }
        let t = Instant::now();
        let r = d.romfs(&mut Progress::new(Some(Duration::from_secs(30)), "romfs: "))?;
        report("romfs", &r, t.elapsed().as_secs_f64());
        if let Some(to) = copy_to {
            let t = Instant::now();
            let c = Dump::open(&d.out, None, &to)?;
            let r = c.romfs(&mut Progress::new(Some(Duration::from_secs(30)), "romfs copy: "))?;
            report("romfs copy", &r, t.elapsed().as_secs_f64());
        }
        Ok(())
    })
}

pub fn finish_romfs(h: JoinHandle<Result<(), String>>) -> Result<(), String> {
    if !h.is_finished() {
        println!("dump: waiting for the romfs");
    }
    h.join().map_err(|_| "the romfs thread panicked".to_string())?
}

/// `dump <src> --check`: `@@dump <what it is>`, then `@@identity pass <build id>` or `@@identity fail <why>` (exit 1):
/// the exefs read in memory and checked against the game description, as `build --dump` does first
fn check(src: &Path, title: Option<&str>, game: &Path) {
    let r = Dump::open(src, title, Path::new(".")).and_then(|d| {
        println!("@@dump {}", d.describe());
        let (files, _) = d.exefs_files()?;
        ounce_lift::game::Game::load(game)?.check_exefs(&Exefs { files })
    });
    match r {
        Ok(id) => println!("@@identity pass {}", id.build_id),
        Err(e) => {
            println!("@@identity fail {}", e.replace('\n', "; "));
            std::process::exit(1)
        }
    }
}

/// `dump <file.nup|folder> [--verify] [--check [--game <dir>]] [--extract <dir>] [--title <t>] [--only exefs|romfs]`
pub fn command(args: &[String]) {
    let (mut src, mut out, mut verify, mut title, mut only) = (None::<PathBuf>, None::<PathBuf>, false, None::<String>, None::<Part>);
    let (mut identity, mut game) = (false, PathBuf::from("games/oot"));
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "--verify" => verify = true,
            "--check" => identity = true,
            "--game" => game = val().into(),
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
    let Some(src) = src else { fail("dump <file.nup|folder> [--verify] [--check [--game <dir>]] [--extract <dir>] [--title <t>] [--only exefs|romfs]") };
    if identity {
        return check(&src, title.as_deref(), &game);
    }
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
