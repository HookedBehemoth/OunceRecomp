//! FRECPE and FRSQRTE helpers (nx2/lift2/fp_estimate.py): not ported yet (the NEON class brings them); emit.rs's
//! helper registry takes them from here.

/// (helper name, its IR text, the declarations it needs)
pub fn helpers() -> Vec<(&'static str, String, Vec<String>)> {
    Vec::new()
}
