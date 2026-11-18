//! The Python tools' checks: their own tests (ounce/tests/py, python3 -I -B), the picture metric's selftest, and every
//! tracked Python file byte-compiled.

use ounce_tests::*;
use std::process::Command;

/// tools/dump/byml_write.py writes BYML that its typed reader, and tools/dump/byml.py, read back as written.
#[test]
fn byml_write() {
    python("ounce/tests/py/byml_write.py", &[]);
}

/// scripts/oot_saves.py: every emulator's folders found, import and export, the backups, the guards (temporary folders).
#[test]
fn oot_saves() {
    python("ounce/tests/py/oot_saves.py", &[]);
}

/// games/oot/labels.txt is well formed, and tools/re/labels.py renames a real symbol table.
#[test]
fn label_syms() {
    python("ounce/tests/py/label_syms.py", &[]);
}

/// The button prompts' font patch (games/oot/patches/prompts.c) on a cmap made as the build makes it (tools/prompts).
#[test]
fn prompt_font_bind() {
    python("ounce/tests/py/prompt_font_bind.py", &["--cc", &cc()]);
}

/// scripts/direct_picture_check.py's selftest on synthetic pictures: the same picture passes; a difference that cancels
/// in the mean, one flat channel, one wrong 16 x 16 tile, a missing state and a NaN fail; a cleared state never lines
/// up with a drawn one; a float target of tiny values negated fails while rounding noise passes.
#[test]
fn picture_check() {
    let out = python("scripts/direct_picture_check.py", &["selftest"]);
    let stdout = text(&out.stdout);
    let lines: Vec<&str> = stdout.lines().filter(|l| l.starts_with("selftest:")).collect();
    assert!(lines.len() >= 7 && lines.iter().all(|l| l.ends_with(": ok")), "direct_picture_check selftest:\n{stdout}");
}

/// Every tracked .py file byte-compiles (in memory: nothing written), so a syntax error anywhere fails.
#[test]
fn byte_compile() {
    let files = run(Command::new("git").args(["ls-files", "-z", "--", "*.py"]).current_dir(root())).stdout;
    let n = files.split(|b| *b == 0).filter(|f| !f.is_empty()).count();
    assert!(n > 20, "{n} tracked Python files");
    const SCRIPT: &str = "import subprocess, sys\n\
        files = [f for f in subprocess.run(['git', 'ls-files', '-z', '--', '*.py'], capture_output=True, check=True).stdout.decode().split('\\0') if f]\n\
        bad = []\n\
        for f in files:\n\
        \x20   try:\n\
        \x20       compile(open(f, 'rb').read(), f, 'exec', dont_inherit=True)\n\
        \x20   except (SyntaxError, ValueError) as e:\n\
        \x20       bad.append('%s: %s' % (f, e))\n\
        print('\\n'.join(bad))\n\
        sys.exit(1 if bad else 0)\n";
    run(Command::new(python3()).args(["-I", "-B", "-c", SCRIPT]).current_dir(root()));
}
