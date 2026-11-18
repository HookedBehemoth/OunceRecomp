//! The games of a checkout: every folder under `games/` that holds a `game.toml`, found from the checkout `ounce`
//! was built in. What the window and a run need of a game (its executable's name, its title, its smoke tests in
//! tests.toml) is read here; the lift reads the whole description itself (ounce-lift's game.rs).

use serde::Deserialize;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// The checkout: the nearest folder above this executable, else above the current folder, that holds
/// `games/` and `runtime/sources.txt`.
pub fn root() -> Result<PathBuf, String> {
    let is_root = |d: &Path| d.join("games").is_dir() && d.join("runtime/sources.txt").is_file();
    let exe = std::env::current_exe().ok();
    let cwd = std::env::current_dir().ok();
    for start in [exe.as_deref().and_then(Path::parent), cwd.as_deref()].into_iter().flatten() {
        if let Some(d) = start.ancestors().find(|d| is_root(d)) {
            return Ok(d.to_path_buf());
        }
    }
    Err("no checkout found above this program or the current folder (a folder with games/ and runtime/)".into())
}

/// The games' names, sorted: the folders of `root`/games that hold a game.toml.
pub fn list(root: &Path) -> Vec<String> {
    let mut names: Vec<String> = std::fs::read_dir(root.join("games"))
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.path().join("game.toml").is_file())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    names
}

#[derive(Debug, Deserialize)]
struct Raw {
    name: String,
    exe: String,
    #[serde(default)]
    title: Option<String>,
}

/// A smoke test (games/<game>/tests.toml): one headless run, compared with the golden runs in the data folder's
/// `<game>/golden/<test>/`.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Test {
    /// how long the run is
    pub seconds: u64,
    /// the presents left out before pictures are compared (the load differs run to run)
    pub smoke_skip: u32,
    /// the run's settings, NAME=VALUE (as `ounce run --set`); `{golden}` is this test's golden folder
    #[serde(default)]
    pub set: Vec<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct TestsFile {
    /// the test `ounce test <game>` runs without --name
    #[serde(default)]
    default: Option<String>,
    #[serde(default)]
    tests: BTreeMap<String, Test>,
}

/// One game, as a run and the window see it.
#[derive(Debug)]
pub struct Game {
    pub name: String,
    /// games/<name>
    pub dir: PathBuf,
    /// the executable's file name in the game folder
    pub exe: String,
    pub title: String,
    /// games/<name>/tests.toml's smoke tests
    pub tests: BTreeMap<String, Test>,
    /// the test `ounce test` runs by default
    pub default_test: Option<String>,
}

impl Game {
    /// games/<name>/game.toml (and tests.toml when there is one). Err names the games there are.
    pub fn load(root: &Path, name: &str) -> Result<Game, String> {
        let dir = root.join("games").join(name);
        let file = dir.join("game.toml");
        if !file.is_file() {
            return Err(format!("no game {name:?}: the games are {}", list(root).join(", ")));
        }
        let text = std::fs::read_to_string(&file).map_err(|e| format!("{}: {e}", file.display()))?;
        let raw: Raw = toml::from_str(&text).map_err(|e| format!("{}: {e}", file.display()))?;
        if raw.name != name {
            return Err(format!("{}: its name is {:?}, not {name:?}", file.display(), raw.name));
        }
        let tf = dir.join("tests.toml");
        let tests: TestsFile = match std::fs::read_to_string(&tf) {
            Ok(t) => toml::from_str(&t).map_err(|e| format!("{}: {e}", tf.display()))?,
            Err(_) => TestsFile::default(),
        };
        Ok(Game {
            title: raw.title.unwrap_or_else(|| raw.name.clone()),
            name: raw.name,
            dir,
            exe: raw.exe,
            tests: tests.tests,
            default_test: tests.default,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn repo() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
    }

    #[test]
    fn every_game_loads() {
        let names = list(&repo());
        assert!(names.contains(&"oot".to_string()) && names.contains(&"dkb".to_string()), "{names:?}");
        for n in &names {
            let g = Game::load(&repo(), n).unwrap();
            assert!(!g.exe.is_empty());
            if let Some(t) = &g.default_test {
                assert!(g.tests.contains_key(t), "{n}: the default test {t} is not one of its tests");
            }
            for (t, d) in &g.tests {
                assert!(d.set.iter().all(|s| s.contains('=')), "{n}: test {t}: a setting is NAME=VALUE");
            }
        }
        assert!(Game::load(&repo(), "nope").unwrap_err().contains("oot"));
    }
}
