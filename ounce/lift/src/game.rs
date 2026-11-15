//! The game declaration (games/<game>/game.toml, the Python side's nx2/gamedesc.py): what the lift and the build take
//! from one game. The lifter holds no fact of one game; it is here or in the game's patch declarations.

use std::path::{Path, PathBuf};
use std::process::Command;

use serde::Deserialize;

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct Identity {
    pub build_id: String,
    pub main_sha256: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
struct Continuation {
    pc: u64,
    word: u32,
}

#[derive(Debug, Deserialize)]
struct Raw {
    name: String,
    labels: String,
    hot: String,
    identity: Identity,
    host: Vec<String>,
    #[serde(default)]
    local_continuations: Vec<Continuation>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Game {
    pub name: String,
    /// the label file, resolved against the folder of game.toml
    pub labels: PathBuf,
    /// the hot list's file under the work folder
    pub hot: String,
    pub identity: Identity,
    /// the host's C files (in host/)
    pub host: Vec<String>,
    /// `adr xN, <label>` ... `br xN` inside one function: (pc, word)
    pub local_continuations: Vec<(u64, u32)>,
}

impl Game {
    /// Loads games/<game>/game.toml; `path` is the folder or the file.
    pub fn load(path: &Path) -> Result<Game, String> {
        let file = if path.is_dir() { path.join("game.toml") } else { path.to_path_buf() };
        let text = std::fs::read_to_string(&file).map_err(|e| format!("no game description at {}: {e}", file.display()))?;
        let raw: Raw = toml::from_str(&text).map_err(|e| format!("{}: {e}", file.display()))?;
        let folder = file.parent().unwrap_or(Path::new(""));
        Ok(Game {
            name: raw.name,
            labels: folder.join(raw.labels),
            hot: raw.hot,
            identity: Identity { build_id: raw.identity.build_id.to_lowercase(), main_sha256: raw.identity.main_sha256.to_lowercase() },
            host: raw.host,
            local_continuations: raw.local_continuations.into_iter().map(|c| (c.pc, c.word)).collect(),
        })
    }

    /// the host's C files, as paths under `root`/host, in the build's order
    pub fn host_sources(&self, root: &Path) -> Vec<PathBuf> {
        self.host.iter().map(|f| root.join("host").join(f)).collect()
    }

    /// the hot list's file (HOOKSHOT_HOT_FUNCS, else work/<hot>); an error when it does not exist
    pub fn hot_path(&self, work: &Path) -> Result<PathBuf, String> {
        let p = match std::env::var_os("HOOKSHOT_HOT_FUNCS").filter(|v| !v.is_empty()) {
            Some(v) => PathBuf::from(v),
            None => work.join(&self.hot),
        };
        if !p.exists() {
            return Err(format!(
                "game {}: no hot list {}: make it from a profile of the game (python -I scripts/hotlist.py work/runs/<run>_all.txt {}) or name one in HOOKSHOT_HOT_FUNCS",
                self.name,
                p.display(),
                p.display()
            ));
        }
        Ok(p)
    }

    /// Err unless exefs/main is this game's (its build id, then its SHA-256); returns the Identity seen.
    pub fn check_exefs(&self, exefs: &Path) -> Result<Identity, String> {
        let path = exefs.join("main");
        let data = std::fs::read(&path).map_err(|e| format!("game {}: {}: {e}", self.name, path.display()))?;
        if data.len() < 0x54 || &data[..4] != b"NSO0" {
            return Err(format!("game {}: {} is not an NSO", self.name, path.display()));
        }
        let seen = Identity { build_id: hex(&data[0x40..0x54]), main_sha256: sha256_file(&path)? };
        for (what, got, want) in [
            ("build_id", &seen.build_id, &self.identity.build_id),
            ("main_sha256", &seen.main_sha256, &self.identity.main_sha256),
        ] {
            if got != want {
                return Err(format!(
                    "game {}: the dump is another game or version: main's {what} is {got}, the description's {want}",
                    self.name
                ));
            }
        }
        Ok(seen)
    }
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

// No sha2 in the offline registry, and no hand-rolled hash: coreutils' sha256sum (the host is Linux only).
fn sha256_file(path: &Path) -> Result<String, String> {
    let out = Command::new("sha256sum").arg(path).output().map_err(|e| format!("sha256sum: {e}"))?;
    let text = String::from_utf8_lossy(&out.stdout);
    match text.split_whitespace().next() {
        Some(h) if out.status.success() && h.len() == 64 => Ok(h.to_lowercase()),
        _ => Err(format!("sha256sum {} failed", path.display())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn repo() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
    }

    #[test]
    fn loads_oot() {
        let g = Game::load(&repo().join("games/oot")).unwrap();
        assert_eq!(g, Game::load(&repo().join("games/oot/game.toml")).unwrap());
        assert_eq!(g.name, "oot");
        assert!(g.labels.ends_with("games/oot/labels.txt"));
        assert_eq!(g.hot, "hot_funcs_play.txt");
        assert_eq!(g.identity.build_id, "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348");
        assert_eq!(g.identity.main_sha256, "2591fb99008d0814f8056c3f36b36790048ed857ff0ae2bb3a4c1e37b960a4de");
        assert_eq!(g.host.len(), 62);
        assert_eq!(g.host[0], "bench.c");
        assert_eq!(g.local_continuations, vec![(0x8011060C, 0x10FFFCA7), (0x80110694, 0x10000047)]);
        let s = g.host_sources(Path::new("/r"));
        assert_eq!(s[0], Path::new("/r/host/bench.c"));
        assert_eq!(s.len(), g.host.len());
    }

    // a synthetic NSO header: 0x54 bytes, "NSO0", build id bytes 0..20 at 0x40
    fn synthetic(dir: &Path) {
        let mut d = vec![0u8; 0x54];
        d[..4].copy_from_slice(b"NSO0");
        for i in 0..20 {
            d[0x40 + i] = i as u8;
        }
        std::fs::write(dir.join("main"), d).unwrap();
    }

    #[test]
    fn check_exefs_synthetic() {
        let dir = std::env::temp_dir().join(format!("ounce_game_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        synthetic(&dir);
        let mut g = Game::load(&repo().join("games/oot")).unwrap();
        let err = g.check_exefs(&dir).unwrap_err();
        assert!(err.contains("main's build_id is 000102030405060708090a0b0c0d0e0f10111213"), "{err}");
        assert!(err.contains(&g.identity.build_id), "{err}");
        g.identity.build_id = "000102030405060708090a0b0c0d0e0f10111213".into();
        let err = g.check_exefs(&dir).unwrap_err();
        assert!(err.contains("main's main_sha256 is"), "{err}");
        let seen = {
            let e = err.split("main_sha256 is ").nth(1).unwrap();
            e.split(',').next().unwrap().to_string()
        };
        g.identity.main_sha256 = seen.clone();
        assert_eq!(g.check_exefs(&dir).unwrap().main_sha256, seen);
        std::fs::write(dir.join("main"), b"XXXX").unwrap();
        assert!(g.check_exefs(&dir).unwrap_err().contains("is not an NSO"));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn hot_path_missing() {
        let g = Game::load(&repo().join("games/oot")).unwrap();
        if std::env::var_os("HOOKSHOT_HOT_FUNCS").is_none() {
            assert!(g.hot_path(Path::new("/nonexistent")).unwrap_err().contains("no hot list"));
        }
    }
}
