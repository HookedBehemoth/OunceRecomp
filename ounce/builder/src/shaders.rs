//! Shaders ahead of time as a build step: the shader pack from the romfs (ounce-shaders, the same on every PC), then
//! the game's own `--precompile` (the renderer translates every program of the pack and has the driver compile it,
//! into the game folder's shader caches, so the first boot finds them warm). `ounce tool shaders` makes a pack by hand.

use crate::fail;
use std::io::{BufRead, BufReader};
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::Instant;

pub const HELP: &str = "shaders <romfs> --out <pack> [--game <dir>] [--all]
  shaders --precompile <game folder> [--exe <name>]";

/// The pack's name in a game folder, beside the executable (where the renderer looks)
pub const PACK: &str = "shaders.pack";

/// The romfs file of the game's zstd dictionaries (none: a path that is not there)
pub fn dictionaries(romfs: &Path, g: &ounce_lift::game::Game) -> std::path::PathBuf {
    romfs.join(g.assets.zstd_dictionaries.as_deref().unwrap_or("-"))
}

/// Makes `out` from the romfs; `@@shaders pack <done> <of>` lines with --progress
pub fn pack(romfs: &Path, dictionaries: &Path, scope: ounce_shaders::Scope, out: &Path) -> Result<(), String> {
    let t = Instant::now();
    let last = std::sync::Mutex::new(Instant::now());
    let tick = |done: usize, of: usize| {
        if !crate::progress_on() {
            return;
        }
        let mut l = last.lock().unwrap();
        if done == of || l.elapsed().as_millis() >= 250 {
            *l = Instant::now();
            println!("@@shaders pack {done} {of}");
        }
    };
    let (p, st) = ounce_shaders::make(romfs, dictionaries, scope, &tick)?;
    p.write(out)?;
    println!(
        "shaders: {} programs, {} stages, {} MB in {}: {} materials ({} resolved) reach {} of {} bfsha programs ({} shading models); {} programs of the other archives; {:.1} s",
        p.programs.len(),
        p.stages.len(),
        p.bytes() >> 20,
        out.display(),
        st.materials,
        st.resolved,
        st.bfsha_reached,
        st.bfsha_programs,
        st.models,
        st.other_programs,
        t.elapsed().as_secs_f64()
    );
    for u in st.unread.iter().take(10) {
        println!("shaders: not read: {u}");
    }
    Ok(())
}

/// Runs the game in `folder` with `--precompile`: its renderer fills the folder's shader caches from the pack. Its
/// `@@shaders <done> <of>` lines are passed on with --progress; the rest of its output goes to the log.
pub fn precompile(folder: &Path, exe: &str) -> Result<(), String> {
    let t = Instant::now();
    let folder = &folder.canonicalize().map_err(|e| format!("{}: {e}", folder.display()))?;
    let path = folder.join(exe);
    let mut c = Command::new(&path);
    // (no window and nothing of the game: the renderer alone; its log beside the game)
    let log_at = folder.join("ounce_precompile_log.txt");
    let log = std::fs::File::create(&log_at).map_err(|e| format!("{}: {e}", log_at.display()))?;
    c.arg("--precompile").current_dir(folder).stdout(Stdio::piped()).stderr(log);
    let mut child = c.spawn().map_err(|e| format!("{}: {e}", path.display()))?;
    let (mut summary, mut counts) = (String::from("no output"), String::new());
    for line in BufReader::new(child.stdout.take().unwrap()).lines().map_while(Result::ok) {
        if let Some(r) = line.strip_prefix("@@shaders ") {
            if crate::progress_on() {
                println!("{line}");
            }
            counts = match r.split(' ').collect::<Vec<_>>()[..] {
                [_, "0", ..] => "nothing: the caches hold the pack's programs".into(),
                [d, _, made, kept, failed] => format!("{d} programs: {made} translated, {kept} kept, {failed} failed"),
                _ => counts,
            };
        } else {
            summary = line;
        }
    }
    let st = child.wait().map_err(|e| e.to_string())?;
    if !st.success() {
        return Err(format!("the shaders' precompile failed ({st}): {summary}; its log is {}", log_at.display()));
    }
    println!("shaders: precompiled {counts} in {:.1} s (the log is {})", t.elapsed().as_secs_f64(), log_at.display());
    Ok(())
}

/// The build's step: the folder's pack made again when its inputs changed (`shaders.pack.key`), then the precompile.
/// A PC whose GPU the back end cannot start on (a build in a container) gets the pack alone, and `@@shaders skipped
/// <why>`: the game makes the programs while it plays, as after a driver update.
pub fn step(romfs: &Path, g: &ounce_lift::game::Game, folder: &Path) -> Result<(), String> {
    let (pack_at, key_at) = (folder.join(PACK), folder.join(format!("{PACK}.key")));
    let key = format!("{:016x}\n", ounce_shaders::inputs_key(romfs, ounce_shaders::Scope::Reached));
    if pack_at.is_file() && std::fs::read_to_string(&key_at).ok().as_deref() == Some(key.as_str()) {
        println!("shaders: the pack in {} is current", pack_at.display());
    } else {
        pack(romfs, &dictionaries(romfs, g), ounce_shaders::Scope::Reached, &pack_at)?;
        std::fs::write(&key_at, key).map_err(|e| format!("{}: {e}", key_at.display()))?;
    }
    if let Err(why) = crate::probe::device_ready() {
        println!("shaders: no precompile, the back end does not start on this PC ({why}): the game makes its programs while it plays");
        if crate::progress_on() {
            println!("@@shaders skipped {why}");
        }
        return Ok(());
    }
    precompile(folder, &g.exe)
}

/// `ounce tool shaders ...`
pub fn command(args: &[String]) {
    let (mut romfs, mut out, mut game_dir, mut all, mut pre, mut exe) = (None, None, None::<String>, false, None, None);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().unwrap_or_else(|| fail(format!("{a} wants a value")));
        match a.as_str() {
            "--out" => out = Some(val()),
            "--game" => game_dir = Some(val()),
            "--all" => all = true,
            "--precompile" => pre = Some(val()),
            "--exe" => exe = Some(val()),
            _ if romfs.is_none() && !a.starts_with('-') => romfs = Some(a.clone()),
            _ => fail(HELP),
        }
    }
    let root = ounce_core::games::root().unwrap_or_else(|e| fail(e));
    let game = |name: Option<String>| {
        let dir = name.map(std::path::PathBuf::from).unwrap_or_else(|| root.join("games/oot"));
        ounce_lift::game::Game::load(&dir).unwrap_or_else(|e| fail(e))
    };
    if let Some(folder) = pre {
        let exe = exe.unwrap_or_else(|| game(None).exe);
        return precompile(Path::new(&folder), &exe).unwrap_or_else(|e| fail(e));
    }
    let (Some(romfs), Some(out)) = (romfs, out) else { fail(HELP) };
    let g = game(game_dir);
    let romfs = Path::new(&romfs);
    let scope = if all { ounce_shaders::Scope::All } else { ounce_shaders::Scope::Reached };
    pack(romfs, &dictionaries(romfs, &g), scope, Path::new(&out)).unwrap_or_else(|e| fail(e));
}
