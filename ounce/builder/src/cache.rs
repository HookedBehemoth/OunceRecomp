//! The builder's object cache: objects by a SHA-1 key of everything that decides them (the module's text, its recipe
//! or flags, the headers it may read, and the compiler's identity), one file per key in a folder of its own.

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
        std::fs::read(self.path(&key.hex())).ok()
    }

    #[cfg_attr(not(feature = "clang"), allow(dead_code))]
    pub fn put(&self, key: Key, obj: &[u8]) -> Result<(), String> {
        self.write(&self.path(&key.hex()), obj)
    }

    #[cfg_attr(not(feature = "clang"), allow(dead_code))]
    /// a C file's manifest: the files its last compile read
    pub fn manifest(&self, key: Key) -> Option<Vec<String>> {
        let t = std::fs::read_to_string(self.path(&key.hex()).with_extension("deps")).ok()?;
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
            return Ok((o, true));
        }
        let o = make()?;
        self.write(&p, &o)?;
        Ok((o, false))
    }
}
