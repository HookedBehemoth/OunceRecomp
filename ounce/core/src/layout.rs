//! Where a game's build and runs go. Every checkout writes only into its own `work/<game>/`:
//!
//! ```text
//! work/<game>/game/         the game folder: the executable, ounce_renderer.so, image.bin, title, icon, build.txt
//! work/<game>/lift/         the lift's files for the RE tools (gen/, facts.tsv, image/), the link map
//! work/<game>/obj/          this build's objects
//! work/<game>/build.log     the last build's log
//! work/<game>/runs/<name>/  one run: log.txt, stdout.txt, timeline.txt, shots/, user/
//! work/renderer/            the renderer's Meson build, shared by the games (it is the same for each)
//! ```
//!
//! What is shared lives in the config's folders: the object cache (`<cache>/objects`) and, per game, the data folder's
//! `<game>/golden/` (the smoke tests' goldens) and `<game>/rec/` (input recordings).

use crate::config::Config;
use std::path::PathBuf;

pub struct Layout {
    pub root: PathBuf,
    pub game: String,
}

impl Layout {
    pub fn new(root: PathBuf, game: &str) -> Layout {
        Layout { root, game: game.to_string() }
    }

    pub fn work(&self) -> PathBuf {
        self.root.join("work").join(&self.game)
    }

    pub fn game_dir(&self) -> PathBuf {
        self.work().join("game")
    }

    pub fn lift_dir(&self) -> PathBuf {
        self.work().join("lift")
    }

    pub fn obj_dir(&self) -> PathBuf {
        self.work().join("obj")
    }

    pub fn renderer_build(&self) -> PathBuf {
        self.root.join("work").join("renderer")
    }

    pub fn build_log(&self) -> PathBuf {
        self.work().join("build.log")
    }

    pub fn run_dir(&self, name: &str) -> PathBuf {
        self.work().join("runs").join(name)
    }

    /// The game folder's build stamp: the commit it was built from, then the version text.
    pub fn build_stamp(&self) -> PathBuf {
        self.game_dir().join("build.txt")
    }

    pub fn cache(config: &Config) -> PathBuf {
        config.cache_dir().join("objects")
    }

    pub fn golden(&self, config: &Config) -> PathBuf {
        config.data_dir().join(&self.game).join("golden")
    }

    pub fn recordings(&self, config: &Config) -> PathBuf {
        config.data_dir().join(&self.game).join("rec")
    }
}
