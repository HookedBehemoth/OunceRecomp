//! The prepared texture store (ounce-tex) as a builder step: `hookshot-builder textures`, and `build --textures`.

use crate::fail;
use crate::gate::Gate;
use ounce_tex::convert::Settings;
use ounce_tex::run::{GameFiles, Options, Stats};
use std::path::{Path, PathBuf};
use std::time::Instant;

pub const HELP: &str = "textures <out dir> [--romfs <dir>] [--game <dir>] [--fast] [-j N] [--only <romfs prefix>]...
  textures --compare <a> <b>";

/// Makes the store in `out` from `romfs` with the game description's settings for its romfs.
pub fn make(romfs: &Path, out: &Path, game: &ounce_lift::game::Game, fast: bool, jobs: usize, only: Vec<String>) -> Result<Stats, String> {
    let gate = Gate::new();
    let admit = |mb: u64| -> Box<dyn ounce_tex::run::Held + '_> { Box::new(gate.enter(mb)) };
    let progress = |s: &str| println!("{s}");
    let o = Options {
        romfs: romfs.to_path_buf(),
        out: out.to_path_buf(),
        game: GameFiles { zstd_dictionaries: game.assets.zstd_dictionaries.clone(), skip_dirs: game.assets.texture_skip_dirs.clone() },
        settings: if fast { Settings::FAST } else { Settings::QUALITY },
        jobs,
        pack_limit: 1 << 30,
        only,
        admit: &admit,
        progress: &progress,
    };
    let st = ounce_tex::run::run(&o)?;
    let mut kinds: Vec<_> = st.by_kind.iter().collect();
    kinds.sort();
    println!(
        "textures: {} files with BNTX ({:.0} MB of romfs), {} textures ({} sharing another's frames), {:.0} MB raw, {:.0} MB packed, {:.0} s",
        st.bntx_files,
        st.in_bytes as f64 / 1e6,
        st.textures,
        st.shared,
        st.raw as f64 / 1e6,
        st.packed as f64 / 1e6,
        st.seconds
    );
    for (k, (n, raw, packed)) in kinds {
        println!("  {k:8} {n:6} textures, {:8.0} MB raw, {:8.0} MB packed", *raw as f64 / 1e6, *packed as f64 / 1e6);
    }
    if !st.errors.is_empty() {
        return Err(format!("textures: {} files failed: {}", st.errors.len(), st.errors[..st.errors.len().min(10)].join("; ")));
    }
    Ok(st)
}

pub fn command(args: &[String]) {
    let (mut out, mut romfs, mut game, mut fast) = (None::<PathBuf>, None::<PathBuf>, PathBuf::from("games/oot"), false);
    let (mut jobs, mut only, mut compare) = (crate::gate::default_jobs(), Vec::new(), Vec::<PathBuf>::new());
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "--romfs" => romfs = Some(val().into()),
            "--game" => game = val().into(),
            "--fast" => fast = true,
            "-j" => jobs = val().parse().unwrap_or_else(|_| fail("-j N")),
            "--only" => only.push(val()),
            "--compare" => compare = vec![val().into(), val().into()],
            _ if out.is_none() && !a.starts_with('-') => out = Some(a.into()),
            _ => fail(HELP),
        }
    }
    if let [a, b] = &compare[..] {
        let t = Instant::now();
        let d = ounce_tex::compare::compare(a, b).unwrap_or_else(|e| fail(e));
        println!("compare: {} files, {} textures, {} frames ({:.0} s)", d.files, d.textures, d.frames, t.elapsed().as_secs_f64());
        for (field, (n, ex)) in &d.fields {
            println!("  {field}: {n}");
            for e in ex {
                println!("    {}", &e[..e.len().min(400)]);
            }
        }
        std::process::exit(if d.equal() { 0 } else { 1 });
    }
    let Some(out) = out else { fail(HELP) };
    let romfs = romfs.unwrap_or_else(|| PathBuf::from("work/dump/romfs"));
    let g = ounce_lift::game::Game::load(&game).unwrap_or_else(|e| fail(e));
    make(&romfs, &out, &g, fast, jobs, only).unwrap_or_else(|e| fail(e));
    let (cpu, hwm) = ounce_lift::pipeline::usage();
    println!("textures: {cpu:.0} s CPU, peak {hwm:.0} MB");
}
