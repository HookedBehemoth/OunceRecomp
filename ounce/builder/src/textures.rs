//! The prepared texture store (ounce-tex) as a builder step: `ounce-builder textures`, and `build --textures`.

use crate::fail;
use crate::gate::Gate;
use ounce_tex::convert::Settings;
use ounce_tex::run::{GameFiles, HollowTo, Options, Stats};
use std::path::{Path, PathBuf};
use std::time::Instant;

pub const HELP: &str = "textures <out dir> [--romfs <dir>] [--game <dir>] [--quality fast|high] [-j N] [--only <romfs prefix>]...
  textures --compare <a> <b>";

/// The zstd level a texture file is compressed at again once its texels are zeroed: they are mostly zeros and
/// headers, and a higher level takes longer for next to no bytes.
pub const HOLLOW_LEVEL: i32 = 9;

/// The list of the romfs files a game folder's store replaced with their texels zeroed (`prepared/hollowed.txt`):
/// they are written whole again before the next build reads the romfs.
pub const HOLLOWED: &str = "hollowed.txt";

/// `--quality fast|high`: the encoder settings (fast, the default; high: slower, smaller packs)
pub fn quality(v: &str) -> Settings {
    match v {
        "fast" => Settings::FAST,
        "high" => Settings::QUALITY,
        o => fail(format!("--quality fast|high, not {o}")),
    }
}

/// Makes the store in `out` from `romfs` with the game description's settings for its romfs; with `hollow`, also
/// the romfs's texture files with their texels zeroed.
pub fn make(romfs: &Path, out: &Path, game: &ounce_lift::game::Game, settings: Settings, jobs: usize, only: Vec<String>, hollow: Option<HollowTo>) -> Result<Stats, String> {
    let gate = Gate::new();
    let admit = |mb: u64| -> Box<dyn ounce_tex::run::Held + '_> { Box::new(gate.enter(mb)) };
    let progress = |s: &str| println!("{s}");
    // (`@@tex <files done> <files> <bytes done> <bytes> <path>` with --progress, at most 4 a second and the last)
    let last = std::sync::Mutex::new(Instant::now());
    let each = |n: usize, of: usize, b: u64, of_b: u64, path: &str| {
        let mut l = last.lock().unwrap();
        if crate::progress_on() && (n == of || l.elapsed().as_millis() >= 250) {
            *l = Instant::now();
            println!("@@tex {n} {of} {b} {of_b} {path}");
        }
    };
    let o = Options {
        romfs: romfs.to_path_buf(),
        out: out.to_path_buf(),
        game: GameFiles { zstd_dictionaries: game.assets.zstd_dictionaries.clone(), skip_dirs: game.assets.texture_skip_dirs.clone() },
        settings,
        jobs,
        pack_limit: 1 << 30,
        only,
        hollow,
        admit: &admit,
        progress: &progress,
        each: &each,
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
    if !st.hollowed.is_empty() {
        println!(
            "romfs: {} texture files written with their texels zeroed: {:.2} GB -> {:.2} GB ({:.1} GB of texels zeroed; {} BNTX inside compressed members kept whole)",
            st.hollowed.len(),
            st.hollow_in as f64 / 1e9,
            st.hollow_out as f64 / 1e9,
            st.hollow_zeroed as f64 / 1e9,
            st.hollow_kept
        );
    }
    if !st.errors.is_empty() {
        return Err(format!("textures: {} files failed: {}", st.errors.len(), st.errors[..st.errors.len().min(10)].join("; ")));
    }
    Ok(st)
}

pub fn command(args: &[String]) {
    let (mut out, mut romfs, mut game, mut settings) = (None::<PathBuf>, None::<PathBuf>, PathBuf::from("games/oot"), Settings::FAST);
    let (mut jobs, mut only, mut compare) = (crate::gate::default_jobs(), Vec::new(), Vec::<PathBuf>::new());
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "--romfs" => romfs = Some(val().into()),
            "--game" => game = val().into(),
            "--quality" => settings = quality(&val()),
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
    if romfs.parent().is_some_and(|d| d.join("prepared").join(HOLLOWED).exists()) {
        fail(format!("{}: a game folder's romfs, its texels zeroed: make the store from the dump", romfs.display()));
    }
    let g = ounce_lift::game::Game::load(&game).unwrap_or_else(|e| fail(e));
    make(&romfs, &out, &g, settings, jobs, only, None).unwrap_or_else(|e| fail(e));
    let (cpu, hwm) = ounce_lift::pipeline::usage();
    println!("textures: {cpu:.0} s CPU, peak {hwm:.0} MB");
}
