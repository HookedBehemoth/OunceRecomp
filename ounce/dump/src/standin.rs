//! Damaged .zs files in an extracted romfs and their stand-ins. Incomplete dumps (about 23.3 GB instead of 23.8 GB)
//! hold .zs files that begin with zeros; the game then leaves out every model that uses one. Each is replaced by a
//! whole file of the same kind beside it.

use crate::zstd::starts_zstd;
use std::path::{Path, PathBuf};

fn head4(p: &Path) -> Vec<u8> {
    use std::io::Read;
    let mut b = Vec::with_capacity(4);
    if let Ok(f) = std::fs::File::open(p) {
        let _ = f.take(4).read_to_end(&mut b);
    }
    b
}

/// The .zs files below `romfs` that do not start as zstd data, in path order. `progress(done, total)` after each.
pub fn damaged_game_files(romfs: &Path, progress: &mut dyn FnMut(usize, usize)) -> Result<Vec<PathBuf>, String> {
    let files = crate::extract::list_folder(romfs)?;
    let zs: Vec<PathBuf> = files.into_iter().filter(|(p, _)| p.ends_with(".zs")).map(|(p, _)| romfs.join(p)).collect();
    let mut found = Vec::new();
    for (i, p) in zs.iter().enumerate() {
        if !starts_zstd(&head4(p)) {
            found.push(p.clone());
        }
        progress(i + 1, zs.len());
    }
    Ok(found)
}

/// The end of the name that tells a file's kind: the last `_Word12` part, if any, and the extensions
/// (`_Nrm.bntx.zs`).
fn kind(name: &str) -> Option<&str> {
    let last_us = name.rfind('_').map_or(0, |i| i + 1);
    let dot = last_us + name[last_us..].find('.')?;
    let b = name.as_bytes();
    let mut i = dot;
    while i > 0 && b[i - 1].is_ascii_digit() {
        i -= 1;
    }
    let digits = i;
    while i > 0 && b[i - 1].is_ascii_alphabetic() {
        i -= 1;
    }
    Some(if i < digits && i > 0 && b[i - 1] == b'_' { &name[i - 1..] } else { &name[dot..] })
}

/// A whole file of the same kind beside a damaged one: the same kind, the longest common start of the name, then
/// the nearest size. Never one of the damaged files.
pub fn stand_in_for(path: &Path, damaged: &[PathBuf]) -> Option<PathBuf> {
    let name = path.file_name()?.to_str()?;
    let suffix = kind(name)?;
    let size = std::fs::metadata(path).ok()?.len() as i64;
    let mut names: Vec<String> = std::fs::read_dir(path.parent()?).ok()?.flatten().filter_map(|e| e.file_name().into_string().ok()).collect();
    names.sort();
    let mut best: Option<((usize, i64), PathBuf)> = None;
    for other in names {
        if other == name || !other.ends_with(suffix) {
            continue;
        }
        let candidate = path.with_file_name(&other);
        if damaged.contains(&candidate) || !starts_zstd(&head4(&candidate)) {
            continue;
        }
        let common = name.chars().zip(other.chars()).take_while(|(a, b)| a == b).count();
        let score = (common, -(std::fs::metadata(&candidate).map_or(0, |m| m.len() as i64) - size).abs());
        if best.as_ref().is_none_or(|b| score > b.0) {
            best = Some((score, candidate));
        }
    }
    best.map(|b| b.1)
}

/// Each damaged .zs file below `romfs` replaced by a copy of its stand-in: (damaged, stand-in or None) each. Only
/// for a romfs this reader wrote: a player's own folder is never written to.
pub fn stand_in_damaged(romfs: &Path, progress: &mut dyn FnMut(usize, usize)) -> Result<Vec<(PathBuf, Option<PathBuf>)>, String> {
    let damaged = damaged_game_files(romfs, progress)?;
    let mut done = Vec::new();
    for p in &damaged {
        let other = stand_in_for(p, &damaged);
        if let Some(o) = &other {
            let tmp = p.with_file_name(format!("{}.part", p.file_name().unwrap().to_string_lossy()));
            std::fs::copy(o, &tmp).and_then(|_| std::fs::rename(&tmp, p)).map_err(|e| format!("{}: {e}", p.display()))?;
        }
        done.push((p.clone(), other));
    }
    Ok(done)
}
