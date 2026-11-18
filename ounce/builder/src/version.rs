//! Which Hookshot a build is: the release name (HOOKSHOT_VERSION, a VERSION file, or `git describe` of a checkout)
//! and a fingerprint of the sources in runtime/, renderer/ and games/ (line endings aside), as the game prints it in its log
//! and the Insert menu.

use sha1::{Digest, Sha1};
use std::path::Path;

const SOURCES: [&str; 3] = ["runtime", "renderer", "games"];
const SOURCE_EXT: [&str; 12] = [".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".py", ".glsl", ".comp", ".vert", ".frag", ".spv"];

/// HOOKSHOT_VERSION, else the first line of VERSION or version.txt, else `git describe` of a checkout; "unknown"
fn release(root: &Path) -> String {
    if let Some(v) = std::env::var("HOOKSHOT_VERSION").ok().map(|v| v.trim().to_string()).filter(|v| !v.is_empty()) {
        return v;
    }
    for f in ["VERSION", "version.txt"] {
        if let Some(l) = std::fs::read_to_string(root.join(f)).ok().and_then(|t| t.lines().next().map(|l| l.trim().to_string())) {
            if !l.is_empty() {
                return l;
            }
        }
    }
    if root.join(".git").exists() {
        let out = std::process::Command::new("git")
            .args(["-C", &root.to_string_lossy(), "describe", "--tags", "--always", "--dirty", "--match", "v[0-9]*"])
            .output();
        if let Some(o) = out.ok().filter(|o| o.status.success()) {
            return String::from_utf8_lossy(&o.stdout).trim().to_string();
        }
    }
    "unknown".into()
}

/// os.walk's order: a folder's files sorted, then its folders sorted (symbolic links to folders not entered)
fn walk(root: &Path, dir: &Path, h: &mut Sha1) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    let (mut files, mut dirs) = (Vec::new(), Vec::new());
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let ft = e.file_type().ok();
        if ft.is_some_and(|t| t.is_dir()) {
            if name != "__pycache__" {
                dirs.push(name);
            }
        } else if ft.is_some_and(|t| t.is_symlink()) && e.path().is_dir() {
            // (os.walk lists it with the folders and does not enter it)
        } else {
            files.push(name);
        }
    }
    files.sort();
    dirs.sort();
    for f in files {
        if !SOURCE_EXT.iter().any(|x| f.ends_with(x)) {
            continue;
        }
        let p = dir.join(&f);
        let Ok(data) = std::fs::read(&p) else { continue };
        let mut text = Vec::with_capacity(data.len());
        let mut i = 0;
        while i < data.len() {
            if data[i] == b'\r' && data.get(i + 1) == Some(&b'\n') {
                i += 1;
                continue;
            }
            text.push(data[i]);
            i += 1;
        }
        let rel = p.strip_prefix(root).unwrap().to_string_lossy().replace('\\', "/");
        h.update(rel.as_bytes());
        h.update([0]);
        h.update(&text);
        h.update([0]);
    }
    for d in dirs {
        walk(root, &dir.join(d), h);
    }
}

fn fingerprint(root: &Path) -> String {
    let mut h = Sha1::new();
    for top in SOURCES {
        walk(root, &root.join(top), &mut h);
    }
    h.finalize().iter().map(|b| format!("{b:02x}")).collect::<String>()[..8].to_string()
}

/// "<release> (source <fingerprint>)", safe in a C string literal: other characters become '_', at most 80
pub fn describe(root: &Path) -> String {
    let text = format!("{} (source {})", release(root), fingerprint(root));
    text.chars()
        .map(|c| if c.is_ascii_alphanumeric() || " ._+()/-".contains(c) { c } else { '_' })
        .take(80)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tree(root: &Path, eol: &str) {
        for rel in ["runtime/a.c", "renderer/b.cpp", "games/g/c.c"] {
            let p = root.join(rel);
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, format!("line one{eol}line two{eol}")).unwrap();
        }
    }

    /// where the release name comes from, a fingerprint that ignores line endings, a text safe in a C literal
    #[test]
    fn release_and_fingerprint() {
        let base = std::env::temp_dir().join(format!("hk-version-{}", std::process::id()));
        let (lf, crlf) = (base.join("lf"), base.join("crlf"));
        tree(&lf, "\n");
        tree(&crlf, "\r\n");
        std::env::remove_var("HOOKSHOT_VERSION");
        assert_eq!(release(&lf), "unknown");
        assert_eq!(fingerprint(&lf), fingerprint(&crlf), "line endings changed the fingerprint");
        std::fs::write(lf.join("games/g/c.c"), "line one\nline two\nchanged\n").unwrap();
        assert_ne!(fingerprint(&lf), fingerprint(&crlf), "a changed source kept the fingerprint");
        std::fs::write(lf.join("VERSION"), "movie-sync\nignored second line\n").unwrap();
        assert_eq!(release(&lf), "movie-sync");
        std::env::set_var("HOOKSHOT_VERSION", "from-env");
        assert_eq!(release(&lf), "from-env", "HOOKSHOT_VERSION comes first");
        std::env::set_var("HOOKSHOT_VERSION", "v1 \"quoted\" \\ & | %PATH%");
        let d = describe(&lf);
        assert!(!d.chars().any(|c| "\"\\&|%".contains(c)), "{d}");
        std::env::remove_var("HOOKSHOT_VERSION");
        let _ = std::fs::remove_dir_all(&base);
    }
}
