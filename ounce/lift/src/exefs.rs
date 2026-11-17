//! The dump's exefs as the lift reads it: its modules and main.npdm by name, in memory. A builder reading a package
//! hands it over without writing it; the developer path reads a folder.

use std::collections::BTreeMap;
use std::path::Path;

pub struct Exefs {
    pub files: BTreeMap<String, Vec<u8>>,
}

/// the files the lift reads: the modules (crate::nso::MODULE_ORDER) and main.npdm
pub fn wanted(name: &str) -> bool {
    name == "main.npdm" || crate::nso::MODULE_ORDER.contains(&name)
}

impl Exefs {
    /// the wanted files of a folder
    pub fn read_dir(dir: &Path) -> Result<Exefs, String> {
        let mut files = BTreeMap::new();
        for n in crate::nso::MODULE_ORDER.iter().copied().chain(["main.npdm"]) {
            let p = dir.join(n);
            match std::fs::read(&p) {
                Ok(b) => {
                    files.insert(n.to_string(), b);
                }
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(format!("{}: {e}", p.display())),
            }
        }
        if files.is_empty() {
            return Err(format!("{}: no exefs modules", dir.display()));
        }
        Ok(Exefs { files })
    }

    pub fn get(&self, name: &str) -> Result<&[u8], String> {
        self.files.get(name).map(Vec::as_slice).ok_or_else(|| format!("exefs/{name}: missing"))
    }
}
