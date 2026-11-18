//! The user's config file, `~/.config/ounce/config.toml` (`$XDG_CONFIG_HOME/ounce/config.toml` when that is set):
//! each game's dump, the shared cache and data folders, and the limits of builds and runs. Every key is optional; only a
//! game's `dump` is needed before that game can be built or run.
//!
//! ```toml
//! [games.oot]
//! dump = "/path/to/dump"          # a folder holding exefs/ and romfs/
//! [paths]
//! cache = "~/.cache/ounce"        # the object cache, shared by every checkout and game
//! data = "~/.local/share/ounce"   # saves, and per game: golden/, rec/
//! [limits]
//! jobs = 0                        # 0: every CPU
//! cache_days = 3
//! run_free_mem_gb = 14
//! build_mem = "18G"              # the memory cap of an --exclusive build
//! ```

use std::path::{Path, PathBuf};

pub struct Config {
    pub path: PathBuf,
    table: toml::Table,
}

/// The sections a key may name; `<game>.<key>` is short for `games.<game>.<key>`.
const SECTIONS: [&str; 3] = ["games", "paths", "limits"];

fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_default()
}

/// `~/x` as a path under the home folder
fn expand(s: &str) -> PathBuf {
    match s.strip_prefix("~/") {
        Some(rest) => home().join(rest),
        None if s == "~" => home(),
        None => PathBuf::from(s),
    }
}

fn full_key(key: &str) -> String {
    match key.split('.').next() {
        Some(first) if SECTIONS.contains(&first) => key.to_string(),
        _ => format!("games.{key}"),
    }
}

impl Config {
    /// Where the config file is (it need not exist).
    pub fn default_path() -> PathBuf {
        let base = std::env::var_os("XDG_CONFIG_HOME").filter(|v| !v.is_empty()).map(PathBuf::from).unwrap_or_else(|| home().join(".config"));
        base.join("ounce").join("config.toml")
    }

    /// The user's config; a missing file is an empty config.
    pub fn load() -> Result<Config, String> {
        Config::load_from(&Config::default_path())
    }

    pub fn load_from(path: &Path) -> Result<Config, String> {
        let table = match std::fs::read_to_string(path) {
            Ok(text) => toml::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))?,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => toml::Table::new(),
            Err(e) => return Err(format!("{}: {e}", path.display())),
        };
        Ok(Config { path: path.to_path_buf(), table })
    }

    /// A value by its dotted key (`oot.dump`, `games.oot.dump`, `paths.data`).
    pub fn get(&self, key: &str) -> Option<&toml::Value> {
        let key = full_key(key);
        let mut parts = key.split('.');
        let mut v = self.table.get(parts.next()?)?;
        for p in parts {
            v = v.as_table()?.get(p)?;
        }
        Some(v)
    }

    /// A value as text (a string as it is, anything else as TOML).
    pub fn show(&self, key: &str) -> Option<String> {
        self.get(key).map(|v| v.as_str().map_or_else(|| v.to_string(), String::from))
    }

    fn get_str(&self, key: &str) -> Option<&str> {
        self.get(key).and_then(|v| v.as_str()).filter(|s| !s.is_empty())
    }

    fn get_int(&self, key: &str) -> Option<i64> {
        self.get(key).and_then(|v| v.as_integer())
    }

    /// Sets a key and writes the file. Values under `limits` are numbers (but build_mem, a size), every other value is text.
    pub fn set(&mut self, key: &str, value: &str) -> Result<(), String> {
        let key = full_key(key);
        let parts: Vec<&str> = key.split('.').collect();
        if parts.len() < 2 || parts.iter().any(|p| p.is_empty()) {
            return Err(format!("{key}: a key is <section>.<name> or <game>.<name>"));
        }
        let value = if key == "limits.build_mem" {
            toml::Value::String(value.to_string())
        } else if parts[0] == "limits" {
            toml::Value::Integer(value.parse().map_err(|_| format!("{key}: {value} is not a whole number"))?)
        } else {
            toml::Value::String(value.to_string())
        };
        let mut t = &mut self.table;
        for p in &parts[..parts.len() - 1] {
            t = t
                .entry(p.to_string())
                .or_insert_with(|| toml::Value::Table(toml::Table::new()))
                .as_table_mut()
                .ok_or_else(|| format!("{key}: {p} is not a section"))?;
        }
        t.insert(parts[parts.len() - 1].to_string(), value);
        self.save()
    }

    fn save(&self) -> Result<(), String> {
        if let Some(d) = self.path.parent() {
            std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
        }
        let text = toml::to_string(&self.table).map_err(|e| format!("{}: {e}", self.path.display()))?;
        std::fs::write(&self.path, text).map_err(|e| format!("{}: {e}", self.path.display()))
    }

    /// The file's text as it is now ("" when there is none).
    pub fn text(&self) -> String {
        toml::to_string(&self.table).unwrap_or_default()
    }

    /// The game's dump: a folder holding exefs/ and romfs/. Err is one line naming the fix.
    pub fn dump(&self, game: &str) -> Result<PathBuf, String> {
        let Some(d) = self.get_str(&format!("games.{game}.dump")) else {
            return Err(format!("no dump for {game}: run `ounce config set {game}.dump <folder with exefs/ and romfs/>`"));
        };
        let d = expand(d);
        if !d.join("exefs").is_dir() {
            return Err(format!("the dump for {game} ({}, from {}) has no exefs/ folder", d.display(), self.path.display()));
        }
        Ok(d)
    }

    /// The object cache's folder (`paths.cache`, default ~/.cache/ounce).
    pub fn cache_dir(&self) -> PathBuf {
        self.get_str("paths.cache").map(expand).unwrap_or_else(|| {
            std::env::var_os("XDG_CACHE_HOME").filter(|v| !v.is_empty()).map(PathBuf::from).unwrap_or_else(|| home().join(".cache")).join("ounce")
        })
    }

    /// The data folder (`paths.data`, default ~/.local/share/ounce): the saves, and per game its smoke goldens and
    /// recordings.
    pub fn data_dir(&self) -> PathBuf {
        self.get_str("paths.data").map(expand).unwrap_or_else(|| {
            std::env::var_os("XDG_DATA_HOME").filter(|v| !v.is_empty()).map(PathBuf::from).unwrap_or_else(|| home().join(".local/share")).join("ounce")
        })
    }

    /// Compile jobs (`limits.jobs`; 0 or unset: every CPU).
    pub fn jobs(&self) -> Option<usize> {
        self.get_int("limits.jobs").filter(|&n| n > 0).map(|n| n as usize)
    }

    /// Days an unused cached object is kept (`limits.cache_days`, default 3).
    pub fn cache_days(&self) -> f64 {
        self.get_int("limits.cache_days").map_or(3.0, |n| n as f64)
    }

    /// The memory cap of an `--exclusive` build (`limits.build_mem`, default 18G; "0" or "none": no cap).
    pub fn build_mem(&self) -> Option<String> {
        match self.get_str("limits.build_mem") {
            Some("0" | "none") => None,
            Some(s) => Some(s.to_string()),
            None => Some("18G".into()),
        }
    }

    /// The free memory a game run waits for (`limits.run_free_mem_gb`, default 14; 0: no wait).
    pub fn run_free_mem_gb(&self) -> u64 {
        self.get_int("limits.run_free_mem_gb").map_or(14, |n| n.max(0) as u64)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys_are_set_read_and_written() {
        let d = std::env::temp_dir().join(format!("ounce-config-{}", std::process::id()));
        let path = d.join("config.toml");
        let mut c = Config::load_from(&path).unwrap();
        assert!(c.dump("oot").unwrap_err().contains("ounce config set oot.dump"));
        std::fs::create_dir_all(d.join("dump/exefs")).unwrap();
        c.set("oot.dump", &d.join("dump").to_string_lossy()).unwrap();
        c.set("limits.jobs", "8").unwrap();
        assert!(c.set("limits.jobs", "many").is_err());
        let c = Config::load_from(&path).unwrap();
        assert_eq!(c.dump("oot").unwrap(), d.join("dump"));
        assert_eq!(c.get("games.oot.dump"), c.get("oot.dump"));
        assert_eq!(c.jobs(), Some(8));
        assert!(c.dump("dkb").is_err());
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn a_dump_without_exefs_is_named() {
        let d = std::env::temp_dir().join(format!("ounce-config-noexefs-{}", std::process::id()));
        let mut c = Config::load_from(&d.join("config.toml")).unwrap();
        c.set("dkb.dump", "/nowhere").unwrap();
        assert!(c.dump("dkb").unwrap_err().contains("no exefs/"));
        std::fs::remove_dir_all(&d).unwrap();
    }
}
