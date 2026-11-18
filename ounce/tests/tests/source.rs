//! Checks of the source text alone, across the runtime and the renderer.

use ounce_tests::*;
use regex::Regex;
use std::path::Path;

fn walk(dir: &Path, out: &mut Vec<std::path::PathBuf>) {
    for e in std::fs::read_dir(dir).unwrap() {
        let p = e.unwrap().path();
        if p.is_dir() {
            walk(&p, out);
        } else {
            out.push(p);
        }
    }
}

/// Paths are UTF-8: runtime/ and renderer/ convert no text with an ANSI code page (CP_ACP and the like), as the game's
/// char strings are UTF-8 and a folder named outside a legacy code page (Japanese, an emoji) must work.
#[test]
fn utf8_paths() {
    let ansi = Regex::new(r"\b(CP_ACP|CP_OEMCP|CP_THREAD_ACP|CP_MACCP|SetFileApisToOEM|SetFileApisToANSI)\b").unwrap();
    let mut files = vec![];
    for top in ["runtime", "renderer"] {
        walk(&path(top), &mut files);
    }
    files.sort();
    let mut bad = vec![];
    for p in files {
        let rel = p.strip_prefix(root()).unwrap().to_string_lossy().replace('\\', "/");
        let code = p.extension().is_some_and(|x| ["c", "h", "cpp", "hpp"].iter().any(|e| x == *e));
        // (the game's own sources: not the vendored compiler code)
        if !code || rel.starts_with("third_party/eden/") {
            continue;
        }
        let text = String::from_utf8_lossy(&std::fs::read(&p).unwrap()).into_owned();
        for (i, line) in text.lines().enumerate() {
            if ansi.is_match(line) {
                bad.push(format!("{rel}:{}: {}", i + 1, line.trim()));
            }
        }
    }
    assert!(bad.is_empty(), "text converted with the ANSI code page (the game's char strings are UTF-8: CP_UTF8):\n{}", bad.join("\n"));
}
