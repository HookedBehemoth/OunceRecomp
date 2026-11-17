//! The builder's object cache: objects by a SHA-1 key of everything that decides them (the module's text, its recipe
//! or flags, the headers it may read, and the compiler's identity), one file per key in a folder of its own.
#![cfg_attr(not(feature = "clang"), allow(dead_code))]

use sha1::{Digest, Sha1};
use std::path::{Path, PathBuf};

/// What names the compiler: its LLVM version and the builder's recipes. Bump RECIPES when a recipe or a flag set
/// changes in a way the keys do not show.
pub fn compiler_id() -> String {
    const RECIPES: &str = "hookshot-builder recipes 1";
    format!("{RECIPES}; LLVM {}", crate::llvm::version())
}

pub struct Cache {
    dir: PathBuf,
}

/// A key under construction
pub struct Key(Sha1);

impl Key {
    pub fn new(kind: &str) -> Key {
        let mut h = Sha1::new();
        h.update(compiler_id().as_bytes());
        h.update([0]);
        h.update(kind.as_bytes());
        h.update([0]);
        Key(h)
    }

    pub fn add(mut self, part: &[u8]) -> Key {
        self.0.update((part.len() as u64).to_le_bytes());
        self.0.update(part);
        self
    }

    pub fn hex(self) -> String {
        self.0.finalize().iter().map(|b| format!("{b:02x}")).collect()
    }
}

impl Cache {
    pub fn open(dir: &Path) -> Result<Cache, String> {
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        Ok(Cache { dir: dir.to_path_buf() })
    }

    fn path(&self, key: &str) -> PathBuf {
        self.dir.join(&key[..2]).join(format!("{key}.o"))
    }

    fn write(&self, p: &Path, data: &[u8]) -> Result<(), String> {
        std::fs::create_dir_all(p.parent().unwrap()).map_err(|e| format!("{}: {e}", p.display()))?;
        // (written beside, then renamed: a reader never sees half a file)
        let tmp = p.with_extension(format!("tmp{}-{:?}", std::process::id(), std::thread::current().id()));
        std::fs::write(&tmp, data).and_then(|_| std::fs::rename(&tmp, p)).map_err(|e| format!("{}: {e}", p.display()))
    }

    #[cfg_attr(not(feature = "clang"), allow(dead_code))]
    pub fn get(&self, key: Key) -> Option<Vec<u8>> {
        let p = self.path(&key.hex());
        let o = std::fs::read(&p).ok()?;
        touch(&p);
        Some(o)
    }

    #[cfg_attr(not(feature = "clang"), allow(dead_code))]
    pub fn put(&self, key: Key, obj: &[u8]) -> Result<(), String> {
        self.write(&self.path(&key.hex()), obj)
    }

    #[cfg_attr(not(feature = "clang"), allow(dead_code))]
    /// a C file's manifest: the files its last compile read
    pub fn manifest(&self, key: Key) -> Option<Vec<String>> {
        let p = self.path(&key.hex()).with_extension("deps");
        let t = std::fs::read_to_string(&p).ok()?;
        touch(&p);
        Some(t.lines().map(String::from).collect())
    }

    #[cfg_attr(not(feature = "clang"), allow(dead_code))]
    pub fn put_manifest(&self, key: Key, deps: &[String]) -> Result<(), String> {
        self.write(&self.path(&key.hex()).with_extension("deps"), deps.join("\n").as_bytes())
    }

    /// the object for `key` when the cache has it, else `make`'s (stored); whether it was a hit
    pub fn get_or(&self, key: Key, make: impl FnOnce() -> Result<Vec<u8>, String>) -> Result<(Vec<u8>, bool), String> {
        let hex = key.hex();
        let p = self.path(&hex);
        if let Ok(o) = std::fs::read(&p) {
            touch(&p);
            return Ok((o, true));
        }
        let o = make()?;
        self.write(&p, &o)?;
        Ok((o, false))
    }

    /// Removes what no build used for `days` days (HOOKSHOT_OBJCACHE_DAYS, 3): (files removed, bytes, bytes kept)
    pub fn prune(&self, days: f64) -> (usize, u64, u64) {
        let age = std::time::Duration::from_secs_f64(days * 86400.0);
        let now = std::time::SystemTime::now();
        let (mut n, mut freed, mut kept) = (0, 0, 0);
        for sub in std::fs::read_dir(&self.dir).into_iter().flatten().flatten() {
            for f in std::fs::read_dir(sub.path()).into_iter().flatten().flatten() {
                let Ok(m) = f.metadata() else { continue };
                let old = m.modified().ok().and_then(|t| now.duration_since(t).ok()).is_some_and(|d| d > age);
                if old && std::fs::remove_file(f.path()).is_ok() {
                    n += 1;
                    freed += m.len();
                } else {
                    kept += m.len();
                }
            }
        }
        (n, freed, kept)
    }
}

/// a file's time of last use: what pruning goes by
fn touch(p: &Path) {
    if let Ok(f) = std::fs::OpenOptions::new().append(true).open(p) {
        let _ = f.set_modified(std::time::SystemTime::now());
    }
}
