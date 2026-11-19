//! The user's folders, per system. Linux: the XDG folders (`$XDG_CONFIG_HOME`, `$XDG_CACHE_HOME`, `$XDG_DATA_HOME`,
//! defaults under `$HOME`). Windows: the config in `%APPDATA%\ounce`, the cache and the data in `%LOCALAPPDATA%\ounce`
//! (`cache\` and `data\` below it: the data holds the saves, which a roaming profile should not copy around).

use std::path::PathBuf;

/// The home folder (`~` in a config path): `$HOME`, on Windows `%USERPROFILE%`
pub fn home() -> PathBuf {
    let var = if cfg!(windows) { "USERPROFILE" } else { "HOME" };
    std::env::var_os(var).map(PathBuf::from).unwrap_or_default()
}

#[cfg(unix)]
fn xdg(var: &str, default: &str) -> PathBuf {
    std::env::var_os(var).filter(|v| !v.is_empty()).map(PathBuf::from).unwrap_or_else(|| home().join(default))
}

#[cfg(windows)]
fn known(var: &str) -> PathBuf {
    std::env::var_os(var).filter(|v| !v.is_empty()).map(PathBuf::from).unwrap_or_else(|| home().join("AppData").join(if var == "APPDATA" { "Roaming" } else { "Local" }))
}

/// ounce's config folder (config.toml)
pub fn config() -> PathBuf {
    #[cfg(unix)]
    return xdg("XDG_CONFIG_HOME", ".config").join("ounce");
    #[cfg(windows)]
    return known("APPDATA").join("ounce");
}

/// ounce's cache folder (the object cache, a release's work/, the fetched and built tools)
pub fn cache() -> PathBuf {
    #[cfg(unix)]
    return xdg("XDG_CACHE_HOME", ".cache").join("ounce");
    #[cfg(windows)]
    return known("LOCALAPPDATA").join("ounce").join("cache");
}

/// ounce's data folder (the saves, the smoke goldens, the recordings)
pub fn data() -> PathBuf {
    #[cfg(unix)]
    return xdg("XDG_DATA_HOME", ".local/share").join("ounce");
    #[cfg(windows)]
    return known("LOCALAPPDATA").join("ounce").join("data");
}

/// The per-session runtime folder for locks (`$XDG_RUNTIME_DIR`, else the temporary folder; Windows:
/// `%LOCALAPPDATA%\ounce`)
pub fn runtime() -> PathBuf {
    #[cfg(unix)]
    return std::env::var_os("XDG_RUNTIME_DIR").filter(|v| !v.is_empty()).map(PathBuf::from).unwrap_or_else(std::env::temp_dir).join("ounce");
    #[cfg(windows)]
    return known("LOCALAPPDATA").join("ounce");
}
