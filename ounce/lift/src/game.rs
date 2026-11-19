//! The game declaration (games/<game>/game.toml): what the lift and the build take
//! from one game. The lifter holds no fact of one game; it is here or in the game's patch declarations.

use std::path::{Path, PathBuf};

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

/// What the prepared assets take from the romfs (`[assets]`).
#[derive(Debug, Clone, Default, PartialEq, Eq, Deserialize)]
pub struct Assets {
    /// the romfs file of the game's zstd dictionaries (a zstd SARC of raw dictionaries)
    #[serde(default)]
    pub zstd_dictionaries: Option<String>,
    /// top-level romfs folders the texture store does not look into (they hold no BNTX)
    #[serde(default)]
    pub texture_skip_dirs: Vec<String>,
    /// the kinds of shader archive (their magic) and the romfs files (`<file>:<member>` in a SARC) the game's patches
    /// make unreachable: the shader pack leaves them out
    #[serde(default)]
    pub shaders_left_out: Vec<String>,
}

#[derive(Debug, Deserialize)]
struct Raw {
    name: String,
    labels: String,
    hot: String,
    identity: Identity,
    exe: String,
    #[serde(default)]
    title: Option<String>,
    #[serde(default)]
    icon: Option<String>,
    patches: Vec<String>,
    #[serde(default)]
    local_continuations: Vec<Continuation>,
    #[serde(default)]
    assets: Assets,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Game {
    pub name: String,
    /// the label file, resolved against the folder of game.toml
    pub labels: PathBuf,
    /// the hot list (one `f_<address>` a line), resolved against the folder of game.toml
    pub hot: PathBuf,
    pub identity: Identity,
    /// the game's executable in its folder: a file name (no '/', '\\' or ':'; spaces are fine)
    pub exe: String,
    /// the window's title (the game's name); the description's `name` when it has none
    pub title: String,
    /// the window's icon (a BMP), resolved against the folder of game.toml
    pub icon: Option<PathBuf>,
    /// the game's patch files (C), resolved against the folder of game.toml
    pub patches: Vec<PathBuf>,
    /// `adr xN, <label>` ... `br xN` inside one function: (pc, word)
    pub local_continuations: Vec<(u64, u32)>,
    pub assets: Assets,
}

impl Game {
    /// Loads games/<game>/game.toml; `path` is the folder or the file.
    pub fn load(path: &Path) -> Result<Game, String> {
        let file = if path.is_dir() { path.join("game.toml") } else { path.to_path_buf() };
        let text = std::fs::read_to_string(&file).map_err(|e| format!("no game description at {}: {e}", file.display()))?;
        let raw: Raw = toml::from_str(&text).map_err(|e| format!("{}: {e}", file.display()))?;
        let folder = file.parent().unwrap_or(Path::new(""));
        if raw.exe.is_empty() || raw.exe.contains(['/', '\\', ':']) || raw.exe.starts_with('.') {
            return Err(format!("{}: exe {:?} is not a file name", file.display(), raw.exe));
        }
        Ok(Game {
            // (the executable's file name on this system: Windows runs only a name ending in .exe)
            exe: if cfg!(windows) { format!("{}.exe", raw.exe) } else { raw.exe },
            title: raw.title.unwrap_or_else(|| raw.name.clone()),
            icon: raw.icon.map(|i| folder.join(i)),
            patches: raw.patches.iter().map(|p| folder.join(p)).collect(),
            name: raw.name,
            labels: folder.join(raw.labels),
            hot: folder.join(raw.hot),
            identity: Identity { build_id: raw.identity.build_id.to_lowercase(), main_sha256: raw.identity.main_sha256.to_lowercase() },
            local_continuations: raw.local_continuations.into_iter().map(|c| (c.pc, c.word)).collect(),
            assets: raw.assets,
        })
    }

    /// the C the game is built from, in the build's order: the runtime's files (`root`/runtime/sources.txt, one path
    /// relative to runtime/ a line) and then the game's patch files. The patch scan reads all of them.
    pub fn sources(&self, root: &Path) -> Result<Vec<PathBuf>, String> {
        let list = root.join("runtime/sources.txt");
        let text = std::fs::read_to_string(&list).map_err(|e| format!("{}: {e}", list.display()))?;
        let runtime = text.lines().map(|l| l.split('#').next().unwrap().trim()).filter(|l| !l.is_empty());
        Ok(runtime.map(|f| root.join("runtime").join(f)).chain(self.patches.iter().cloned()).collect())
    }

    /// the hot list's guest addresses (the file's format: docs/lifter.md, "Game description files")
    pub fn hot_list(&self) -> Result<Vec<u64>, String> {
        let p = &self.hot;
        let d = std::fs::read(p)
            .map_err(|e| format!("game {}: no hot list {} (the description's `hot`, beside game.toml): {e}", self.name, p.display()))?;
        let n = if d.len() >= 8 && &d[..4] == b"HOTF" { u32::from_le_bytes([d[4], d[5], d[6], d[7]]) as usize } else { usize::MAX };
        if n == usize::MAX || d.len() != 8 + 4 * n {
            return Err(format!("game {}: {} is not a hot list (\"HOTF\", u32 count, count u32 addresses)", self.name, p.display()));
        }
        Ok(d[8..].chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]) as u64).collect())
    }

    /// Err unless exefs/main is this game's (its build id, then its SHA-256); returns the Identity seen.
    pub fn check_exefs(&self, exefs: &crate::exefs::Exefs) -> Result<Identity, String> {
        let data = exefs.get("main").map_err(|e| format!("game {}: {e}", self.name))?;
        if data.len() < 0x54 || &data[..4] != b"NSO0" {
            return Err(format!("game {}: exefs/main is not an NSO", self.name));
        }
        let seen = Identity { build_id: crate::nso::build_id(data), main_sha256: hex(&<sha2::Sha256 as sha2::Digest>::digest(data)) };
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

/// a source's name in messages: its path under `root`, else as given
pub fn shown(root: &Path, p: &Path) -> String {
    p.strip_prefix(root).unwrap_or(p).to_string_lossy().replace('\\', "/")
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
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
        assert!(g.hot.ends_with("games/oot/hot_funcs.bin"));
        let hot = g.hot_list().unwrap();
        assert_eq!(hot.len(), 852);
        assert_eq!(hot[0], 0x8004ab40);
        assert_eq!(g.identity.build_id, "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348");
        assert_eq!(g.assets.zstd_dictionaries.as_deref(), Some("Pack/ZsDic.pack.zs"));
        assert!(g.assets.texture_skip_dirs.iter().any(|d| d == "TerrainArc"));
        assert_eq!(g.identity.main_sha256, "2591fb99008d0814f8056c3f36b36790048ed857ff0ae2bb3a4c1e37b960a4de");
        assert!(g.patches.iter().any(|p| p.ends_with("games/oot/patches/oot_probes.c")));
        assert!(g.icon.as_ref().unwrap().ends_with("games/oot/icon.bmp") && g.icon.as_ref().unwrap().is_file());
        assert!(g.title.contains("Ocarina of Time"));
        assert_eq!(g.exe, "Ocarina of Time");
        assert_eq!(g.local_continuations, vec![(0x8011060C, 0x10FFFCA7), (0x80110694, 0x10000047)]);
        let s = g.sources(&repo()).unwrap();
        assert!(s.iter().all(|p| p.is_file()), "{s:?}");
        assert!(s[0].starts_with(repo().join("runtime")));
        assert_eq!(s.len(), s.iter().map(|p| p.file_stem()).collect::<std::collections::BTreeSet<_>>().len(), "two sources of one name");
    }

    // a synthetic NSO header: 0x54 bytes, "NSO0", build id bytes 0..20 at 0x40
    fn synthetic() -> crate::exefs::Exefs {
        let mut d = vec![0u8; 0x54];
        d[..4].copy_from_slice(b"NSO0");
        for i in 0..20 {
            d[0x40 + i] = i as u8;
        }
        crate::exefs::Exefs { files: [("main".to_string(), d)].into() }
    }

    #[test]
    fn check_exefs_synthetic() {
        let mut dir = synthetic();
        let mut g = Game::load(&repo().join("games/oot")).unwrap();
        let err = g.check_exefs(&dir).unwrap_err();
        assert!(err.contains("main's build_id is 000102030405060708090a0b0c0d0e0f10111213"), "{err}");
        assert!(err.contains(&g.identity.build_id), "{err}");
        g.identity.build_id = "000102030405060708090a0b0c0d0e0f10111213".into();
        let err = g.check_exefs(&dir).unwrap_err();
        assert!(err.contains("main's main_sha256 is"), "{err}");
        let known = "8d1d1055e92013279d2ac1648d3d85f55aa9b522e36e07c021454112f9857e88";
        assert!(err.contains(&format!("main's main_sha256 is {known},")), "{err}");
        g.identity.main_sha256 = known.into();
        assert_eq!(g.check_exefs(&dir).unwrap().main_sha256, known);
        dir.files.insert("main".into(), b"XXXX".to_vec());
        assert!(g.check_exefs(&dir).unwrap_err().contains("is not an NSO"));
    }

    #[test]
    fn hot_list_bad() {
        let mut g = Game::load(&repo().join("games/oot")).unwrap();
        g.hot = PathBuf::from("/nonexistent/hot_funcs.bin");
        assert!(g.hot_list().unwrap_err().contains("no hot list /nonexistent/hot_funcs.bin"));
        let f = std::env::temp_dir().join(format!("ounce_hot_{}.bin", std::process::id()));
        for bad in [&b"HOTX\0\0\0\0"[..], &b"HOTF\x01\0\0\0"[..], &b"HOTF\x01\0\0\0\x01\0\0\0\0"[..]] {
            std::fs::write(&f, bad).unwrap();
            g.hot = f.clone();
            assert!(g.hot_list().unwrap_err().contains("is not a hot list"));
        }
        std::fs::write(&f, b"HOTF\x01\0\0\0\x40\xab\x04\x80").unwrap();
        assert_eq!(g.hot_list().unwrap(), vec![0x8004ab40]);
        std::fs::remove_file(&f).unwrap();
    }
}
