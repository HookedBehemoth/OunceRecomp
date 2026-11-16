//! The lift's stages as values the lift and the patch tools share: the base (the scan of the host's declarations,
//! the image from the dump's exefs, the discovery and the plan) and, for a set of declarations, the patches as the lift
//! reads them (rows resolved to their functions, PATCH_FUNCs, the HLE'd functions' facts: `Patches`). The summaries and
//! the call-boundary records borrow both (summary::Program, bounds::Records), so a caller builds them on its stack.

use crate::discover::Discovery;
use crate::eden::{self, Facts};
use crate::export::{Hle, Meta};
use crate::game::Game;
use crate::image::Image;
use crate::nso::Nso;
use crate::patch::{self, Horizon, PatchSet, Row, Where};
use crate::plan::{self, Plan};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};

/// where the inputs are
#[derive(Clone)]
pub struct Opts {
    pub game: PathBuf,
    pub work: PathBuf,
    pub root: PathBuf,
    pub exefs: Option<PathBuf>,
    pub max_func: usize,
}

impl Default for Opts {
    fn default() -> Opts {
        Opts { game: PathBuf::from("games/oot"), work: PathBuf::from("work"), root: PathBuf::from("."), exefs: None, max_func: 0 }
    }
}

/// the dump's exefs as nx2/kit.py finds it: OOT_EXEFS, OOT_DUMP/exefs, else the first line of work/dump.txt
pub fn dump_exefs(work: &Path) -> Result<PathBuf, String> {
    if let Some(e) = std::env::var_os("OOT_EXEFS").filter(|v| !v.is_empty()) {
        return Ok(PathBuf::from(e));
    }
    if let Some(d) = std::env::var_os("OOT_DUMP").filter(|v| !v.is_empty()) {
        return Ok(PathBuf::from(d).join("exefs"));
    }
    let cfg = work.join("dump.txt");
    let text = std::fs::read_to_string(&cfg).map_err(|e| format!("no exefs: set OOT_DUMP or write the dump folder into {}: {e}", cfg.display()))?;
    let line = text.lines().map(str::trim).find(|l| !l.is_empty() && !l.starts_with('#')).ok_or("dump.txt: no dump folder")?;
    Ok(PathBuf::from(line).join("exefs"))
}

/// the host's C texts of the game (root/host/<file>), as the scan reads them
pub fn host_texts(root: &Path, game: &Game) -> Result<Vec<(String, String)>, String> {
    let mut texts = Vec::new();
    for f in &game.host {
        let p = root.join("host").join(f);
        let b = std::fs::read(&p).map_err(|e| format!("patch declarations: {}: {e}", p.display()))?;
        texts.push((format!("host/{f}"), String::from_utf8_lossy(&b).into_owned()));
    }
    Ok(texts)
}

/// what does not depend on which declarations are read beyond the host's own: the image is built for the host's
/// HLE set
pub struct Base {
    pub game: Game,
    pub opts: Opts,
    pub exefs: PathBuf,
    /// the host's declarations
    pub ps: PatchSet,
    pub horizon: Horizon,
    /// the symbols the host replaces (HLE, PATCH_ANSWER)
    pub implemented: BTreeSet<String>,
    pub hle: BTreeSet<String>,
    pub stubbed: BTreeSet<String>,
    pub nvn: Vec<String>,
    pub img: Image,
    pub facts: Facts,
    /// the image's entries and those ADR / ADRP+ADD form outside unwind-covered code
    pub entries: Vec<u64>,
    pub plan: Plan,
    /// discovery counts for the lift's report: (functions, jump tables, on their index, PLT stubs, setjmp owners,
    /// rounds)
    pub found: (usize, usize, usize, usize, usize, usize),
    pub times: Times,
}

#[derive(Default, Clone, Copy)]
pub struct Times {
    pub scan: f64,
    pub load: f64,
    pub build: f64,
    pub image: f64,
    pub discovery: f64,
}

impl Base {
    /// the scan, the image, the discovery and the plan
    pub fn new(o: &Opts) -> Result<Base, String> {
        let t0 = std::time::Instant::now();
        let game = Game::load(&o.game)?;
        let ps = patch::scan_texts(&host_texts(&o.root, &game)?)?;
        let horizon = Horizon::load(&o.root)?;
        let mut times = Times { scan: t0.elapsed().as_secs_f64(), ..Times::default() };
        let exefs = match &o.exefs {
            Some(e) => e.clone(),
            None => dump_exefs(&o.work)?,
        };
        game.check_exefs(&exefs)?;
        let mods: Vec<Nso> = crate::nso::load_exefs(&exefs)?;
        let implemented: BTreeSet<String> = ps.hle_symbols();
        let (hle, stubbed) = eden::hle_set(&mods, &implemented, &horizon.os_namespaces)?;
        let nvn = eden::nvn_exports(&mods, &implemented);
        let all_hle: BTreeSet<String> = hle.union(&stubbed).cloned().collect();
        times.load = t0.elapsed().as_secs_f64() - times.scan;
        let (img, facts) = eden::build(mods, &all_hle)?;
        times.build = t0.elapsed().as_secs_f64() - times.load - times.scan;
        let spans = img.fde_spans(&facts.eh_frame_hdr);
        let mut entries = facts.entries.clone();
        let have: HashSet<u64> = entries.iter().copied().collect();
        entries.extend(img.code_address_refs(&spans).into_iter().filter(|&e| img.in_code(e) && !have.contains(&e)));
        times.image = t0.elapsed().as_secs_f64();
        let hot_path = game.hot_path(&o.work)?;
        let hot: HashSet<u64> = std::fs::read_to_string(&hot_path)
            .map_err(|e| format!("{}: {e}", hot_path.display()))?
            .lines()
            .filter_map(|l| l.split_whitespace().next())
            .filter_map(|w| w.strip_prefix("f_").and_then(|h| u64::from_str_radix(h, 16).ok()))
            .collect();
        let parts = Inputs { game, exefs, ps, horizon, implemented, hle, stubbed, nvn, img, facts, entries, hot };
        Ok(Base::of(parts, o, times))
    }

    /// the discovery and the plan over the inputs
    fn of(i: Inputs, o: &Opts, mut times: Times) -> Base {
        let td = std::time::Instant::now();
        let (plan, found) = {
            let mut d = Discovery::new(&i.img, &i.entries, &i.facts.names, &i.horizon.setjmp_names, i.facts.eh_starts.clone());
            d.discover(o.max_func);
            times.discovery = td.elapsed().as_secs_f64();
            let found = (d.funcs.len(), d.jt.len(), d.jt_index.len(), d.plt.len(), d.setjmp_owner.len(), d.rounds as usize + 1);
            (Plan::new(&d, &i.img, &i.hot), found)
        };
        let Inputs { game, exefs, ps, horizon, implemented, hle, stubbed, nvn, img, facts, entries, .. } = i;
        Base { game, opts: o.clone(), exefs, ps, horizon, implemented, hle, stubbed, nvn, img, facts, entries, plan, found, times }
    }

    /// a base of synthetic code: (entry, words) functions in one module, the declarations of texts, no hooks (the
    /// tools' tests)
    pub fn synthetic(code: &[(u64, Vec<u32>)], texts: &[(String, String)], root: &Path) -> Result<Base, String> {
        use crate::image::{Module, Segment};
        let lo = code.iter().map(|c| c.0).min().ok_or("no code")? & !0xFFF;
        let hi = code.iter().map(|c| c.0 + 4 * c.1.len() as u64).max().unwrap();
        let hi = (hi + 0xFFF) & !0xFFF;
        let mut mem = vec![0u8; (hi - lo) as usize];
        for (e, ws) in code {
            for (k, w) in ws.iter().enumerate() {
                let at = (e - lo) as usize + 4 * k;
                mem[at..at + 4].copy_from_slice(&w.to_le_bytes());
            }
        }
        let module = Module {
            name: "main".into(),
            base: lo,
            size: hi - lo,
            build_id: String::new(),
            init: 0,
            fini: 0,
            segments: vec![Segment { name: ".text".into(), addr: lo, size: hi - lo }],
            bss: [0, 0],
        };
        let img = Image { base: lo, end: hi, mem, modules: vec![module], hooks: Vec::new(), code: vec![(lo, hi)] };
        let mut entries: Vec<u64> = code.iter().map(|c| c.0).collect();
        entries.sort_unstable();
        let facts = Facts {
            entries: entries.clone(),
            eh_starts: Vec::new(),
            eh_frame_hdr: Vec::new(),
            names: Vec::new(),
            init_arrays: Vec::new(),
            exports: Vec::new(),
            unresolved: Vec::new(),
            reloc_slots: Vec::new(),
        };
        let game = Game {
            name: "synthetic".into(),
            labels: PathBuf::new(),
            hot: String::new(),
            identity: crate::game::Identity { build_id: String::new(), main_sha256: String::new() },
            host: Vec::new(),
            local_continuations: Vec::new(),
        };
        let ps = patch::scan_texts(texts)?;
        let horizon = Horizon::load(root)?;
        let implemented = ps.hle_symbols();
        let inputs = Inputs {
            game,
            exefs: PathBuf::new(),
            ps,
            horizon,
            implemented,
            hle: BTreeSet::new(),
            stubbed: BTreeSet::new(),
            nvn: Vec::new(),
            img,
            facts,
            entries,
            hot: HashSet::new(),
        };
        let o = Opts { root: root.to_path_buf(), ..Opts::default() };
        Ok(Base::of(inputs, &o, Times::default()))
    }

    /// the lifted function at e: (pcs, words)
    pub fn body(&self, e: u64) -> Option<&plan::PBody> {
        self.plan.bodies.binary_search_by_key(&e, |b| b.entry).ok().map(|k| &self.plan.bodies[k])
    }

    /// the lifted functions whose body holds pc, by entry
    pub fn containing(&self, pc: u64) -> Vec<u64> {
        self.plan
            .bodies
            .iter()
            .filter(|b| !b.pcs.is_empty() && b.pcs[0] <= pc && pc <= b.pcs[b.pcs.len() - 1] && b.pcs.binary_search(&pc).is_ok())
            .map(|b| b.entry)
            .collect()
    }

    /// the HLE'd functions at their addresses with the facts ps declares (by the hook's symbol: its C name is the
    /// symbol made an identifier, which may name no declaration)
    pub fn hle_at(&self, ps: &PatchSet) -> Vec<Hle> {
        self.img
            .hooks
            .iter()
            .map(|h| {
                let f = ps.hle_fact(&h.sym);
                Hle(h.addr, h.sym.clone(), f.1, f.2, f.3, f.4)
            })
            .collect()
    }

    /// [(where, what is wrong)] of the declarations of ps against the plan: every row's site and checks, every
    /// PATCH_FUNC, every HLE_EFFECTS (the lift stops on the first)
    pub fn errors(&self, ps: &PatchSet) -> Result<Vec<(Where, String)>, String> {
        let mut out = Vec::new();
        let rows = patch::rows(&ps.sites)?;
        let entry_set: HashSet<u64> = self.plan.entries.iter().copied().collect();
        for mut r in rows {
            let hits = self.containing(r.pc);
            if hits.len() != 1 {
                let what = if hits.is_empty() {
                    "no lifted function".to_string()
                } else {
                    "more than one function: ".to_string() + &hits.iter().map(|h| format!("f_{h:08x}")).collect::<Vec<_>>().join(", ")
                };
                out.push((r.at.clone(), format!("{:#x} is in {what}", r.pc)));
                continue;
            }
            r.func = hits[0];
            out.extend(self.row_errors(&r, &entry_set)?.into_iter().map(|m| (r.at.clone(), m)));
        }
        let (_, ferrs) = patch::func_errors(ps, &self.facts.exports, &|a| self.body(a).map(|b| (b.pcs.clone(), b.words.clone())));
        out.extend(ferrs);
        out.extend(self.errors_effects(ps));
        Ok(out)
    }

    /// ps without the declarations Base::errors finds wrong: what the tools analyse while some are being written
    pub fn usable(&self, ps: &PatchSet) -> Result<PatchSet, String> {
        let bad: Vec<Where> = self.errors(ps)?.into_iter().map(|e| e.0).collect();
        let mut ps = ps.clone();
        ps.sites.retain(|d| !bad.contains(&d.at));
        ps.funcs.retain(|d| !bad.contains(&d.at));
        ps.declared.retain(|_, d| !bad.contains(&d.at));
        Ok(ps)
    }

    /// one resolved row's errors (probes.errors with the jump tables, and the entries for a before row)
    pub fn row_errors(&self, r: &Row, entry_set: &HashSet<u64>) -> Result<Vec<String>, String> {
        let b = self.body(r.func).ok_or_else(|| format!("{:#x}: no lifted function", r.func))?;
        let (jt, ents) = match (r.value, r.mode.as_str()) {
            (false, "before") => (Some(&self.plan.jt), Some(entry_set)),
            (false, "instead") => (Some(&self.plan.jt), None),
            _ => (None, None),
        };
        patch::errors(r, &b.pcs, &b.words, jt, ents)
    }

    /// the patches of ps as the lift reads them; the first error stops (Base::errors lists them all)
    pub fn patches(&self, ps: &PatchSet) -> Result<Patches, String> {
        let hle_at = self.hle_at(ps);
        let answer_list = ps.answers();
        let answers: Vec<(u64, String, u64)> =
            hle_at.iter().filter_map(|h| answer_list.iter().find(|a| a.0 == h.1).map(|a| (h.0, h.1.clone(), a.1))).collect();
        let mut rows = patch::rows(&ps.sites)?;
        {
            let bodies: Vec<(u64, &[u64])> = self.plan.bodies.iter().map(|b| (b.entry, &b.pcs[..])).collect();
            patch::resolve(&mut rows, &bodies)?;
            let entry_set: HashSet<u64> = self.plan.entries.iter().copied().collect();
            for r in &rows {
                if let Some(e) = self.row_errors(r, &entry_set)?.first() {
                    return patch::fail(&r.at, e);
                }
            }
        }
        let found = patch::check_funcs(ps, &self.facts.exports, &|a| self.body(a).map(|b| (b.pcs.clone(), b.words.clone())))?;
        if let Some((w, m)) = self.errors_effects(ps).first() {
            return patch::fail(w, m);
        }
        let (patch_funcs, func_names) = patch::func_facts(&found);
        let (values, at_rows) = patch::meta_rows(&rows);
        let mut hle_syms: Vec<&str> = hle_at.iter().map(|h| h.1.as_str()).collect();
        hle_syms.sort_unstable();
        hle_syms.dedup();
        let h = &self.horizon;
        let meta = Meta {
            format: 5,
            commit: String::new(),
            verify: false,
            omit: true,
            answers: answer_list.clone(),
            image_sha256: String::new(),
            hle_symbols: ps.hle_symbols().into_iter().collect(),
            os_namespaces: h.os_namespaces.clone(),
            setjmp_names: h.setjmp_names.clone(),
            part_insns: plan::PART_INSNS,
            values,
            rows: at_rows,
            func_names,
            noreturn_names: h.noreturn_names.clone(),
            noreturn_parts: h.noreturn_parts.clone(),
            hle: hle_syms.iter().map(|s| ps.hle_fact(s)).collect(),
            row_effects: patch::row_effects(&rows),
            patch_funcs,
        };
        let continuations: HashMap<u64, u32> = self.game.local_continuations.iter().copied().collect();
        let parts = plan::parts(&self.plan, &meta, &self.img, &continuations);
        let funcs: Vec<u64> = found.keys().copied().collect();
        Ok(Patches { meta, parts, hle_at, answers, rows, funcs })
    }

    fn errors_effects(&self, ps: &PatchSet) -> Vec<(Where, String)> {
        let have: HashSet<&str> = self.img.hooks.iter().map(|h| h.sym.as_str()).collect();
        let mut out: Vec<(Where, String)> = ps
            .declared
            .values()
            .filter(|r| !have.contains(r.symbol.as_str()))
            .map(|r| (r.at.clone(), format!("HLE_EFFECTS of {}: no HLE'd function of that symbol", r.symbol)))
            .collect();
        out.sort_by(|a, b| (&a.0 .0, a.0 .1).cmp(&(&b.0 .0, b.0 .1)));
        out
    }
}

/// a set of declarations as the lift reads them
pub struct Patches {
    pub meta: Meta,
    pub parts: plan::Parts,
    pub hle_at: Vec<Hle>,
    /// (address, symbol, value) of the answered functions
    pub answers: Vec<(u64, String, u64)>,
    /// the rows, each resolved to its function
    pub rows: Vec<Row>,
    /// the PATCH_FUNCs' entries
    pub funcs: Vec<u64>,
}

/// what the discovery and the plan are made from
struct Inputs {
    game: Game,
    exefs: PathBuf,
    ps: PatchSet,
    horizon: Horizon,
    implemented: BTreeSet<String>,
    hle: BTreeSet<String>,
    stubbed: BTreeSet<String>,
    nvn: Vec<String>,
    img: Image,
    facts: Facts,
    entries: Vec<u64>,
    hot: HashSet<u64>,
}
