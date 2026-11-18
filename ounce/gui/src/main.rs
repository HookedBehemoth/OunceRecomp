//! hookshot-gui: the builder's window. The player picks the dump and the game folder, the dump and the PC are
//! checked, the game folder is built and then played; every step is a `hookshot-builder` child process whose `@@`
//! lines the window reads (docs/builder-plan.md, "The window").
//!
//! `hookshot-gui [<dump> [<game folder>]]` fills the two fields.

mod child;

use child::{tag, Child, Line};
use eframe::egui::{self, Color32, RichText};
use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::time::Instant;

const LOG_LINES: usize = 2000;
const GREEN: Color32 = Color32::from_rgb(80, 180, 90);
const YELLOW: Color32 = Color32::from_rgb(220, 170, 40);
const RED: Color32 = Color32::from_rgb(220, 70, 60);

#[derive(PartialEq, Clone, Copy)]
enum Screen {
    Game,
    Device,
    Build,
    Done,
}

/// The builder and what it reads: found beside this executable or above it, or from HOOKSHOT_BUILDER,
/// HOOKSHOT_ROOT, HOOKSHOT_DIRECT and HOOKSHOT_WORK.
struct Paths {
    builder: String,
    root: String,
    direct: String,
    work: String,
}

impl Paths {
    fn find() -> Paths {
        let dir = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)).unwrap_or_default();
        let env = |k: &str| std::env::var(k).ok().filter(|v| !v.is_empty()).map(PathBuf::from);
        let first = |c: [PathBuf; 2]| c.iter().find(|p| p.is_file()).unwrap_or(&c[0]).clone();
        let root = env("HOOKSHOT_ROOT")
            .or_else(|| dir.ancestors().find(|d| d.join("games/oot/game.toml").is_file()).map(Path::to_path_buf))
            .unwrap_or_else(|| PathBuf::from("."));
        let builder = env("HOOKSHOT_BUILDER").unwrap_or_else(|| first([dir.join("hookshot-builder"), root.join("ounce/target/release/hookshot-builder")]));
        let direct = env("HOOKSHOT_DIRECT").unwrap_or_else(|| first([dir.join("hookshot_direct.so"), root.join("work/sa/meson/renderer/hookshot_direct.so")]));
        let work = env("HOOKSHOT_WORK").unwrap_or_else(|| root.join("work"));
        let s = |p: PathBuf| p.to_string_lossy().into_owned();
        Paths { builder: s(builder), root: s(root), direct: s(direct), work: s(work) }
    }

    fn game(&self) -> String {
        Path::new(&self.root).join("games/oot").to_string_lossy().into_owned()
    }
}

/// `dump --check`: whether the dump is the game
#[derive(Default)]
struct Identity {
    child: Option<Child>,
    what: String,
    result: Option<Result<String, String>>,
}

/// `probe`: the device check
#[derive(Default)]
struct Probe {
    child: Option<Child>,
    items: Vec<(String, String, String)>,
    verdict: Option<(bool, String)>,
    error: Option<String>,
}

enum Outcome {
    Done,
    Failed(String),
    Stopped,
}

/// `build --dump --progress`
struct Build {
    child: Option<Child>,
    started: Instant,
    stages: Vec<String>,
    stage: String,
    stage_started: Instant,
    plan: usize,
    files: usize,
    eta: Option<(f64, Instant)>,
    tex: Option<(f64, f64)>,
    current: String,
    error: Option<String>,
    log: VecDeque<String>,
    outcome: Option<Outcome>,
}

impl Build {
    fn new(child: Option<Child>) -> Build {
        Build {
            child,
            started: Instant::now(),
            stages: Vec::new(),
            stage: String::new(),
            stage_started: Instant::now(),
            plan: 0,
            files: 0,
            eta: None,
            tex: None,
            current: String::new(),
            error: None,
            log: VecDeque::new(),
            outcome: None,
        }
    }

    fn read(&mut self, line: Line) {
        let text = match line {
            Line::Out(s) => s,
            Line::Err(s) => {
                self.current = s.clone();
                return self.push_log(s);
            }
        };
        match tag(&text) {
            Some(("stages", r)) => self.stages = r.split_whitespace().map(String::from).collect(),
            Some(("stage", r)) => {
                let (key, what) = r.split_once(' ').unwrap_or((r, ""));
                self.stage = key.to_string();
                self.stage_started = Instant::now();
                self.current = what.to_string();
                self.eta = None;
            }
            Some(("plan", r)) => self.plan = r.split(' ').next().and_then(|n| n.parse().ok()).unwrap_or(0),
            Some(("file", r)) => {
                self.files += 1;
                self.current = r.split(' ').next().unwrap_or("").to_string();
            }
            Some(("eta", r)) => self.eta = r.parse().ok().map(|s| (s, Instant::now())),
            Some(("tex", r)) => {
                let f: Vec<&str> = r.splitn(5, ' ').collect();
                if let [_, _, b, of, path] = f[..] {
                    self.tex = Some((b.parse().unwrap_or(0.0), of.parse().unwrap_or(1.0)));
                    self.current = path.to_string();
                }
            }
            Some(("error", r)) => self.error = Some(r.to_string()),
            Some(_) => {}
            None => {
                self.current = text.clone();
                self.push_log(text);
            }
        }
    }

    fn push_log(&mut self, s: String) {
        if self.log.len() == LOG_LINES {
            self.log.pop_front();
        }
        self.log.push_back(s);
    }

    /// the step's progress (0..1, when known) and the seconds left in it
    fn progress(&self) -> (Option<f32>, Option<f64>) {
        match self.stage.as_str() {
            "lift" => {
                let p = (self.plan > 0).then(|| (self.files as f32 / self.plan as f32).min(1.0));
                (p, self.eta.map(|(s, at)| (s - at.elapsed().as_secs_f64()).max(0.0)))
            }
            "textures" => match self.tex {
                Some((b, of)) if b > 0.0 => {
                    let t = self.stage_started.elapsed().as_secs_f64();
                    (Some((b / of) as f32), Some(t * (of - b) / b))
                }
                _ => (None, None),
            },
            _ => (None, None),
        }
    }
}

struct App {
    paths: Paths,
    screen: Screen,
    dump: String,
    out: String,
    high: bool,
    identity: Identity,
    checked: String,
    probe: Probe,
    build: Option<Build>,
    note: String,
    played: Vec<std::process::Child>,
}

fn stage_name(key: &str) -> &str {
    match key {
        "dump" => "Read the dump",
        "host" => "Compile the host",
        "lift" => "Lift and compile the game's code",
        "link" => "Link",
        "romfs" => "Copy the romfs",
        "textures" => "Convert the textures",
        k => k,
    }
}

fn minutes(s: f64) -> String {
    let s = s.round() as u64;
    if s >= 60 { format!("{} min {:02} s", s / 60, s % 60) } else { format!("{s} s") }
}

/// a dot of a state's colour (the default fonts have no check marks); `None`: a ring, for what is still to come
fn dot(ui: &mut egui::Ui, color: Option<Color32>) -> egui::Response {
    let (r, p) = ui.allocate_painter(egui::vec2(14.0, 14.0), egui::Sense::hover());
    let c = r.rect.center();
    match color {
        Some(col) => {
            p.circle_filled(c, 5.0, col);
        }
        None => {
            p.circle_stroke(c, 4.5, (1.5, ui.visuals().weak_text_color()));
        }
    }
    r
}

fn state_color(state: &str) -> Option<Color32> {
    match state {
        "pass" => Some(GREEN),
        "warn" => Some(YELLOW),
        "fail" => Some(RED),
        _ => Some(Color32::GRAY),
    }
}

impl App {
    fn new(args: Vec<String>) -> App {
        let home = std::env::var("HOME").unwrap_or_default();
        App {
            paths: Paths::find(),
            screen: Screen::Game,
            dump: args.first().cloned().unwrap_or_default(),
            out: args.get(1).cloned().unwrap_or_else(|| format!("{home}/Games/hookshot")),
            high: false,
            identity: Identity::default(),
            checked: String::new(),
            probe: Probe::default(),
            build: None,
            note: String::new(),
            played: Vec::new(),
        }
    }

    fn spawn(&self, ctx: &egui::Context, args: &[&str]) -> Result<Child, String> {
        let ctx = ctx.clone();
        let args: Vec<String> = args.iter().map(|s| s.to_string()).collect();
        Child::spawn(Path::new(&self.paths.builder), &args, move || ctx.request_repaint())
    }

    fn check_dump(&mut self, ctx: &egui::Context) {
        if self.dump.is_empty() || self.dump == self.checked {
            return;
        }
        self.checked = self.dump.clone();
        let game = self.paths.game();
        self.identity = Identity::default();
        match self.spawn(ctx, &["dump", &self.dump.clone(), "--check", "--game", &game]) {
            Ok(c) => self.identity.child = Some(c),
            Err(e) => self.identity.result = Some(Err(e)),
        }
    }

    fn start_probe(&mut self, ctx: &egui::Context) {
        self.probe = Probe::default();
        match self.spawn(ctx, &["probe"]) {
            Ok(c) => self.probe.child = Some(c),
            Err(e) => self.probe.error = Some(e),
        }
    }

    fn start_build(&mut self, ctx: &egui::Context) {
        let p = &self.paths;
        let game = p.game();
        let args = [
            "build", &self.out, "--direct", &p.direct, "--dump", &self.dump, "--progress", "--quality",
            if self.high { "high" } else { "fast" }, "--game", &game, "--work", &p.work, "--root", &p.root,
        ];
        self.screen = Screen::Build;
        self.build = Some(match self.spawn(ctx, &args) {
            Ok(c) => Build::new(Some(c)),
            Err(e) => Build { outcome: Some(Outcome::Failed(e)), ..Build::new(None) },
        });
    }

    /// reads what the children printed
    fn poll(&mut self) {
        if let Some(c) = &mut self.identity.child {
            for l in c.poll() {
                match l {
                    Line::Out(s) => match tag(&s) {
                        Some(("dump", r)) => self.identity.what = r.to_string(),
                        Some(("identity", r)) => {
                            let (state, why) = r.split_once(' ').unwrap_or((r, ""));
                            self.identity.result = Some(if state == "pass" { Ok(why.to_string()) } else { Err(why.to_string()) });
                        }
                        _ => {}
                    },
                    Line::Err(s) => self.identity.what = s,
                }
            }
            if let Some(st) = c.exit {
                self.identity.result.get_or_insert_with(|| Err(format!("hookshot-builder dump ended with {st}")));
                self.identity.child = None;
            }
        }
        if let Some(c) = &mut self.probe.child {
            for l in c.poll() {
                let Line::Out(s) = l else { continue };
                let Some(("probe", r)) = tag(&s) else { continue };
                let mut f = r.splitn(3, ' ');
                let (key, state, value) = (f.next().unwrap_or(""), f.next().unwrap_or(""), f.next().unwrap_or(""));
                if key == "verdict" {
                    self.probe.verdict = Some((state == "pass", value.to_string()));
                } else {
                    self.probe.items.push((key.to_string(), state.to_string(), value.to_string()));
                }
            }
            if let Some(st) = c.exit {
                if self.probe.verdict.is_none() {
                    self.probe.error = Some(format!("the device check ended with {st}"));
                }
                self.probe.child = None;
            }
        }
        if let Some(b) = &mut self.build {
            let Some(c) = &mut b.child else { return };
            let lines = c.poll();
            let exit = c.exit;
            for l in lines {
                b.read(l);
            }
            if let Some(st) = exit {
                b.child = None;
                if b.outcome.is_none() {
                    b.outcome = Some(if st.success() && b.stage == "done" {
                        Outcome::Done
                    } else {
                        Outcome::Failed(b.error.clone().unwrap_or_else(|| format!("hookshot-builder ended with {st}")))
                    });
                }
                if matches!(b.outcome, Some(Outcome::Done)) {
                    self.screen = Screen::Done;
                }
            }
        }
    }

    fn running(&self) -> bool {
        self.identity.child.is_some() || self.probe.child.is_some() || self.build.as_ref().is_some_and(|b| b.child.is_some())
    }
}

/// a path field with browse buttons (the desktop's file dialog, when it has one); true when a path was entered
fn path_row(ui: &mut egui::Ui, label: &str, value: &mut String, file: bool) -> bool {
    let mut changed = false;
    ui.label(label);
    let row = egui::vec2(ui.available_width(), ui.spacing().interact_size.y);
    ui.allocate_ui_with_layout(row, egui::Layout::right_to_left(egui::Align::Center), |ui| {
        if ui.button("Folder…").clicked() {
            if let Some(p) = rfd::FileDialog::new().pick_folder() {
                *value = p.to_string_lossy().into_owned();
                changed = true;
            }
        }
        if file && ui.button("File…").clicked() {
            if let Some(p) = rfd::FileDialog::new().add_filter("dump", &["nup"]).pick_file() {
                *value = p.to_string_lossy().into_owned();
                changed = true;
            }
        }
        changed |= ui.add(egui::TextEdit::singleline(value).desired_width(ui.available_width())).lost_focus();
    });
    changed
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.poll();
        self.played.retain_mut(|c| c.try_wait().ok().flatten().is_none());
        if self.running() {
            ctx.request_repaint_after(std::time::Duration::from_millis(250));
        }
        egui::Panel::top("steps").show(ui, |ui| {
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                ui.heading("Hookshot");
                ui.add_space(16.0);
                for (s, name) in [(Screen::Game, "1  Game"), (Screen::Device, "2  This PC"), (Screen::Build, "3  Build"), (Screen::Done, "4  Play")] {
                    let t = RichText::new(name);
                    ui.label(if s == self.screen { t.strong() } else { t.weak() });
                    ui.add_space(8.0);
                }
            });
            ui.add_space(4.0);
        });
        egui::CentralPanel::default().show(ui, |ui| match self.screen {
            Screen::Game => self.game_screen(ui, &ctx),
            Screen::Device => self.device_screen(ui, &ctx),
            Screen::Build => self.build_screen(ui, &ctx),
            Screen::Done => self.done_screen(ui),
        });
    }
}

impl App {
    fn game_screen(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        if path_row(ui, "The dump: a .nup file, or a folder with exefs/ and romfs/", &mut self.dump, true) {
            self.check_dump(ctx);
        }
        if !self.dump.is_empty() && self.checked.is_empty() {
            self.check_dump(ctx);
        }
        ui.horizontal_wrapped(|ui| match &self.identity.result {
            _ if self.identity.child.is_some() => {
                ui.spinner();
                ui.label("Checking the dump…");
            }
            Some(Ok(id)) => {
                dot(ui, Some(GREEN));
                let what = self.identity.what.rsplit_once("; ").map_or("a dump folder", |w| w.1);
                ui.label(format!("This is the game: {what}, build {}", &id[..id.len().min(16)]));
            }
            Some(Err(e)) => {
                dot(ui, Some(RED));
                ui.label(e.as_str());
            }
            None => {
                ui.label(RichText::new("No dump checked yet.").weak());
            }
        });
        ui.add_space(12.0);
        path_row(ui, "The game folder: where the game is built (about 30 GB)", &mut self.out, false);
        ui.add_space(12.0);
        egui::CollapsingHeader::new("The builder").show(ui, |ui| {
            egui::Grid::new("paths").num_columns(2).show(ui, |ui| {
                for (k, v) in [
                    ("hookshot-builder", &mut self.paths.builder),
                    ("Sources", &mut self.paths.root),
                    ("hookshot_direct.so", &mut self.paths.direct),
                    ("Work folder", &mut self.paths.work),
                ] {
                    ui.label(k);
                    ui.add(egui::TextEdit::singleline(v).desired_width(480.0));
                    ui.end_row();
                }
            });
        });
        ui.add_space(12.0);
        let ok = matches!(self.identity.result, Some(Ok(_))) && !self.out.trim().is_empty();
        if ui.add_enabled(ok, egui::Button::new("Next: check this PC")).clicked() {
            self.screen = Screen::Device;
            self.start_probe(ctx);
        }
    }

    fn device_screen(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        if self.probe.child.is_some() {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label("Checking the GPU…");
            });
        }
        egui::ScrollArea::vertical().max_height(300.0).show(ui, |ui| {
            egui::Grid::new("probe").num_columns(3).striped(true).show(ui, |ui| {
                for (key, state, value) in &self.probe.items {
                    dot(ui, state_color(state));
                    ui.label(key.as_str());
                    ui.label(value.as_str());
                    ui.end_row();
                }
            });
        });
        ui.add_space(8.0);
        let pass = match (&self.probe.verdict, &self.probe.error) {
            (Some((true, _)), _) => {
                ui.label(RichText::new("This PC can run the game.").color(GREEN));
                true
            }
            (Some((false, keys)), _) => {
                ui.label(RichText::new(format!("This PC cannot run the game ({keys}): the build does not start.")).color(RED));
                false
            }
            (None, Some(e)) => {
                ui.label(RichText::new(e.as_str()).color(RED));
                false
            }
            _ => false,
        };
        ui.add_space(12.0);
        ui.label("Texture quality");
        ui.radio_value(&mut self.high, false, "Fast: about 4 minutes");
        ui.radio_value(&mut self.high, true, "High: about 18 minutes, 4 % smaller");
        ui.add_space(8.0);
        path_row(ui, "The game folder", &mut self.out, false);
        ui.add_space(12.0);
        ui.horizontal(|ui| {
            if ui.button("Back").clicked() {
                self.screen = Screen::Game;
            }
            if ui.add_enabled(pass && !self.out.trim().is_empty(), egui::Button::new("Build")).clicked() {
                self.start_build(ctx);
            }
        });
    }

    fn build_screen(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        let Some(b) = &mut self.build else { return };
        let done_at = b.stages.iter().position(|s| *s == b.stage);
        for (i, s) in b.stages.iter().enumerate() {
            ui.horizontal(|ui| {
                match done_at {
                    _ if b.stage == "done" => dot(ui, Some(GREEN)),
                    Some(k) if i < k => dot(ui, Some(GREEN)),
                    Some(k) if i == k && b.child.is_some() => ui.spinner(),
                    Some(k) if i == k => dot(ui, Some(RED)),
                    _ => dot(ui, None),
                };
                let t = RichText::new(stage_name(s));
                ui.label(if Some(i) == done_at { t.strong() } else { t });
            });
        }
        ui.add_space(8.0);
        if b.child.is_some() {
            let (p, left) = b.progress();
            let bar = match p {
                Some(p) => egui::ProgressBar::new(p).show_percentage(),
                None => egui::ProgressBar::new(0.0).animate(true),
            };
            ui.add(bar);
            ui.horizontal(|ui| {
                ui.label(format!("{} so far", minutes(b.started.elapsed().as_secs_f64())));
                if let Some(s) = left {
                    ui.label(format!("· about {} left in this step", minutes(s)));
                }
            });
            ui.label(RichText::new(b.current.as_str()).monospace().weak());
            ui.add_space(8.0);
            if ui.button("Stop").clicked() {
                if let Some(c) = &mut b.child {
                    c.kill();
                }
                b.child = None;
                b.outcome = Some(Outcome::Stopped);
            }
        }
        let mut again = false;
        match &b.outcome {
            Some(Outcome::Failed(e)) => {
                ui.label(RichText::new(format!("The build failed: {e}")).color(RED));
                again = true;
            }
            Some(Outcome::Stopped) => {
                ui.label("Stopped. Building into the same folder again picks up where this build stopped.");
                again = true;
            }
            _ => {}
        }
        if again {
            ui.horizontal(|ui| {
                if ui.button("Back").clicked() {
                    self.screen = Screen::Device;
                }
                if ui.button("Build again").clicked() {
                    self.start_build(ctx);
                }
            });
        }
        let Some(b) = &self.build else { return };
        ui.add_space(8.0);
        let tail = |ui: &mut egui::Ui, n: usize| {
            egui::ScrollArea::vertical().stick_to_bottom(true).show(ui, |ui| {
                for l in b.log.iter().skip(b.log.len().saturating_sub(n)) {
                    ui.label(RichText::new(l.as_str()).monospace().small());
                }
            });
        };
        if matches!(b.outcome, Some(Outcome::Failed(_))) {
            ui.label("The end of the log:");
            egui::Frame::group(ui.style()).show(ui, |ui| tail(ui, 40));
        } else {
            egui::CollapsingHeader::new("Log").show(ui, |ui| tail(ui, 400));
        }
    }

    fn done_screen(&mut self, ui: &mut egui::Ui) {
        ui.label(RichText::new("The game is built.").color(GREEN).heading());
        ui.label(format!("It is in {}", self.out));
        if let Some(b) = &self.build {
            ui.label(RichText::new(format!("The build took {}.", minutes(b.stage_started.duration_since(b.started).as_secs_f64()))).weak());
        }
        ui.add_space(12.0);
        ui.horizontal(|ui| {
            if ui.button("Play").clicked() {
                let exe = Path::new(&self.out).join("hookshot");
                match std::process::Command::new(&exe).current_dir(&self.out).spawn() {
                    Ok(c) => self.played.push(c),
                    Err(e) => self.note = format!("{}: {e}", exe.display()),
                }
            }
            if ui.button("Open folder").clicked() {
                if let Err(e) = std::process::Command::new("xdg-open").arg(&self.out).spawn() {
                    self.note = format!("xdg-open: {e}");
                }
            }
        });
        if !self.note.is_empty() {
            ui.label(RichText::new(self.note.as_str()).color(RED));
        }
    }
}

fn main() -> eframe::Result {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let opts = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([780.0, 560.0]).with_title("Hookshot"),
        ..Default::default()
    };
    eframe::run_native("hookshot-gui", opts, Box::new(|_| Ok(Box::new(App::new(args)))))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_a_build() {
        let mut b = Build::new(None);
        for l in ["@@stages dump host lift link romfs textures", "@@stage lift lifting", "@@plan 4 4", "@@file lift_h0 2.0 new", "@@eta 30", "build: 16 jobs"] {
            b.read(Line::Out(l.into()));
        }
        assert_eq!(b.stages.len(), 6);
        let (p, left) = b.progress();
        assert_eq!(p, Some(0.25));
        assert!(left.is_some_and(|s| s <= 30.0 && s > 29.0));
        assert_eq!(b.log.back().map(String::as_str), Some("build: 16 jobs"));
        b.read(Line::Out("@@stage textures converting".into()));
        b.read(Line::Out("@@tex 10 100 500 1000 Tex/a b.bntx.zs".into()));
        assert_eq!(b.progress().0, Some(0.5));
        assert_eq!(b.current, "Tex/a b.bntx.zs");
        b.read(Line::Out("@@error the link failed".into()));
        assert_eq!(b.error.as_deref(), Some("the link failed"));
    }
}
