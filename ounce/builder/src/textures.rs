//! The prepared texture store (ounce-tex) as a build step. Every build puts one in its game folder's `prepared/`: a
//! developer's build keeps it while its inputs are unchanged, a worktree's copies the main checkout's when that one
//! matches, and only otherwise converts; a player's game folder (`--dump`) also gets its romfs copy's texture files
//! zeroed.
//! `ounce tool textures` makes one by hand.

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

/// `--quality fast|high`: the encoder settings (fast, the default; high: four times the time, smaller packs)
pub fn quality(v: &str) -> Settings {
    match v {
        "fast" => Settings::FAST,
        "high" => Settings::QUALITY,
        o => fail(format!("--quality fast|high, not {o}")),
    }
}

/// The main checkout of the repository `root` is in (git's common folder's parent), when `root` is another worktree.
pub fn main_checkout(root: &Path) -> Option<PathBuf> {
    let o = std::process::Command::new("git").arg("-C").arg(root).args(["rev-parse", "--path-format=absolute", "--git-common-dir"]).output().ok()?;
    let common = PathBuf::from(String::from_utf8_lossy(&o.stdout).trim());
    let main = common.parent()?.to_path_buf();
    (o.status.success() && main.canonicalize().ok()? != root.canonicalize().ok()?).then_some(main)
}

/// Copies the files of `from` into `to` (a new folder). std's copy uses copy_file_range, which the kernel makes a
/// reflink where the file system can (btrfs): the copy shares the data and costs no disk.
fn copy_tree(from: &Path, to: &Path) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| format!("{}: {e}", to.display()))?;
    for e in std::fs::read_dir(from).map_err(|e| format!("{}: {e}", from.display()))?.flatten() {
        let (src, dst) = (e.path(), to.join(e.file_name()));
        if e.file_type().map_err(|e| e.to_string())?.is_dir() {
            copy_tree(&src, &dst)?;
        } else {
            std::fs::copy(&src, &dst).map_err(|e| format!("{} -> {}: {e}", src.display(), dst.display()))?;
        }
    }
    Ok(())
}

/// A developer's build's store, `folder`/prepared (tex/ and inputs.key): kept when its key (inputs_key) is current;
/// else copied from the main checkout's build of the game when that one's key matches (a worktree's build); else
/// converted from `romfs`. Made beside and renamed over the old one, so a game that maps the old one keeps it.
pub fn dev_store(root: &Path, folder: &Path, romfs: &Path, game: &ounce_lift::game::Game, settings: Settings, jobs: usize) -> Result<(), String> {
    let at = folder.join("prepared");
    let files = GameFiles { zstd_dictionaries: game.assets.zstd_dictionaries.clone(), skip_dirs: game.assets.texture_skip_dirs.clone() };
    let key = format!("{:016x}\n", ounce_tex::run::inputs_key(&settings, romfs, &files)?);
    let current = |d: &Path| d.join("tex/index.bin").is_file() && std::fs::read_to_string(d.join("inputs.key")).ok().as_deref() == Some(key.as_str());
    if current(&at) {
        println!("textures: the store in {} is current", at.display());
        return Ok(());
    }
    let (new, old) = (folder.join("prepared.new"), folder.join("prepared.old"));
    let io = |p: &Path, e: std::io::Error| format!("{}: {e}", p.display());
    for d in [&new, &old] {
        if d.exists() {
            std::fs::remove_dir_all(d).map_err(|e| io(d, e))?;
        }
    }
    let main = main_checkout(root).map(|m| m.join("work").join(&game.name).join("game/prepared")).filter(|d| current(d));
    if let Some(m) = main {
        let t = Instant::now();
        copy_tree(&m, &new)?;
        // (and its renderer caches, warm, where this folder has none)
        for e in std::fs::read_dir(m.parent().unwrap()).into_iter().flatten().flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n.starts_with("ounce_renderer_") && n.ends_with(".bin") && !folder.join(&n).exists() {
                std::fs::copy(e.path(), folder.join(&n)).map_err(|e| format!("{n}: {e}"))?;
            }
        }
        println!("textures: the store (and the renderer caches) copied from the main checkout's build, {} ({:.1} s)", m.display(), t.elapsed().as_secs_f64());
    } else {
        make(romfs, &new.join("tex"), game, settings, jobs, Vec::new(), None)?;
        std::fs::write(new.join("inputs.key"), &key).map_err(|e| io(&new, e))?;
    }
    if at.exists() {
        std::fs::rename(&at, &old).map_err(|e| io(&at, e))?;
    }
    std::fs::rename(&new, &at).map_err(|e| io(&new, e))?;
    if old.exists() {
        std::fs::remove_dir_all(&old).map_err(|e| io(&old, e))?;
    }
    Ok(())
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
    let (mut out, mut romfs, mut game, mut settings) = (None::<PathBuf>, None::<PathBuf>, None::<PathBuf>, Settings::FAST);
    let (mut jobs, mut only, mut compare) = (crate::gate::default_jobs(), Vec::new(), Vec::<PathBuf>::new());
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "--romfs" => romfs = Some(val().into()),
            "--game" => game = Some(val().into()),
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
    let game = game.unwrap_or_else(|| fail("textures: --game games/<game> is needed"));
    let g = ounce_lift::game::Game::load(&game).unwrap_or_else(|e| fail(e));
    let romfs = romfs.unwrap_or_else(|| crate::config().dump(&g.name).unwrap_or_else(|e| fail(e)).join("romfs"));
    if romfs.parent().is_some_and(|d| d.join("prepared").join(HOLLOWED).exists()) {
        fail(format!("{}: a game folder's romfs, its texels zeroed: make the store from the dump", romfs.display()));
    }
    make(&romfs, &out, &g, settings, jobs, only, None).unwrap_or_else(|e| fail(e));
    let (cpu, hwm) = ounce_lift::pipeline::usage();
    println!("textures: {cpu:.0} s CPU, peak {hwm:.0} MB");
}
