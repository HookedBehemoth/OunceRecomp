//! The lift and the compile in one process: ounce-lift's pipeline hands each partition and each generated module to
//! the compile threads, LZ4-compressed, so no IR is written to disk and the lift (and its state) ends within seconds
//! instead of living as long as the compile. The objects: the partitions at their level (lift_h*:
//! hot), the generated code and lift_answers cold; each one comes from the cache when its key is there.

use crate::cache::{Cache, Key};
use crate::compile::{self, Level};
use ounce_lift::pipeline::{self, Out};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::Mutex;
use std::time::Instant;

/// One module to compile: (object name, its IR compressed, the IR's size, level)
type Job = (String, Vec<u8>, usize, Level);

struct Queue<'a> {
    tx: Mutex<Option<Sender<Job>>>,
    /// the generated modules and lift_answers, held until the first cold partition: the lift hands every hot one
    /// over before it (Run::hot_first), and a cold compile running would keep the hot ones waiting at compile.rs's
    /// gate until no cold one is left
    held: Mutex<Option<Vec<Job>>>,
    /// image.bin lands here
    dir: &'a Path,
    /// the lift's folder for the RE tools (gen/*.ll, gen/functions.txt, facts.tsv, image/image.bin), when asked for
    gen: Option<&'a Path>,
    /// the partitions' IR there too (lift_*.ll: diffout, dump.py)
    gen_parts: bool,
}

impl Queue<'_> {
    fn send(&self, job: Job) {
        let tx = self.tx.lock().unwrap().clone().expect("the queue is open while the lift runs");
        tx.send(job).expect("the compile threads stopped");
    }

    /// sends the held modules (once)
    fn release(&self) {
        let held = self.held.lock().unwrap().take();
        for job in held.into_iter().flatten() {
            self.send(job);
        }
    }

    fn job(name: &str, parts: &[&[u8]], level: Level) -> Job {
        let n: usize = parts.iter().map(|p| p.len()).sum();
        let mut ir = Vec::with_capacity(n);
        for p in parts {
            ir.extend_from_slice(p);
        }
        (name.to_string(), lz4_flex::compress_prepend_size(&ir), n, level)
    }
}

impl Out for Queue<'_> {
    fn file(&self, rel: &str, data: Vec<u8>) {
        if let Some(g) = self.gen {
            let p = g.join(rel);
            std::fs::create_dir_all(p.parent().unwrap()).and_then(|_| std::fs::write(&p, &data)).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
        }
        if let Some(stem) = rel.strip_suffix(".ll") {
            let job = Self::job(stem.rsplit('/').next().unwrap(), &[&data], Level::Cold);
            let mut held = self.held.lock().unwrap();
            match held.as_mut() {
                Some(h) => h.push(job),
                None => {
                    drop(held);
                    self.send(job);
                }
            }
        } else if rel == "image/image.bin" {
            let p = self.dir.join(rel.rsplit('/').next().unwrap());
            std::fs::write(&p, data).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
        }
    }

    fn part(&self, name: &str, head: &[u8], body: &[u8]) {
        if let Some(g) = self.gen.filter(|_| self.gen_parts) {
            let p = g.join(format!("{name}.ll"));
            std::fs::write(&p, [head, body].concat()).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
        }
        let level = if name.starts_with("lift_h") { Level::Hot } else { Level::Cold };
        if level == Level::Cold {
            self.release();
        }
        self.send(Self::job(name, &[head, body], level));
    }

    fn wants_facts(&self) -> bool {
        self.gen.is_some()
    }
}

/// What the compile threads did
#[derive(Default)]
pub struct Done {
    /// (name, its IR's size in bytes: the link takes the partitions largest first) of every object made
    pub objects: Vec<(String, usize)>,
    pub cached: usize,
    pub compile_s: f64,
    pub differ: Vec<String>,
    /// seconds from the start until the lift returned (its state freed)
    pub lift_s: f64,
    /// what the lift reported
    pub lift: pipeline::Stats,
    /// the objects' bytes, the hot partitions' and the rest's
    pub hot_bytes: u64,
    pub cold_bytes: u64,
}

/// Lifts with `run` and compiles every module into `out`/<name>.o with `jobs` compile threads; `cache`: objects by
/// key; `check`: a folder of objects to compare with, each compared byte for byte, then by objeq when the bytes differ
/// The lift's helpers: where the RE tools' files go, the memory gate, the progress lines
pub struct Aux<'a> {
    pub gen: Option<&'a Path>,
    pub gen_parts: bool,
    pub gate: &'a crate::gate::Gate,
    pub progress: Option<&'a crate::progress::Progress>,
}

pub fn lift_compile(run: &pipeline::Run, out: &Path, jobs: usize, cache: Option<&Cache>, check: Option<&Path>, aux: &Aux) -> Result<Done, String> {
    std::fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    compile::init()?;
    let (tx, rx) = channel::<Job>();
    let rx = Mutex::new(rx);
    let q = Queue { tx: Mutex::new(Some(tx)), held: Mutex::new(Some(Vec::new())), dir: out, gen: aux.gen, gen_parts: aux.gen_parts };
    let done = Mutex::new(Done::default());
    let t0 = Instant::now();
    let lifted = std::thread::scope(|s| {
        for _ in 0..jobs.max(1) {
            s.spawn(|| compile_thread(&rx, out, cache, check, &done, t0, aux));
        }
        let r = pipeline::lift(run, &q);
        q.release();
        q.tx.lock().unwrap().take();
        done.lock().unwrap().lift_s = t0.elapsed().as_secs_f64();
        r
    });
    let mut d = done.into_inner().unwrap();
    d.lift = lifted?;
    Ok(d)
}

/// a module's object key: its IR, its recipe and the options in force for partitions
fn ir_key(ir: &[u8], level: Level) -> Key {
    let mut k = Key::new("ir").add(format!("{level:?}").as_bytes()).add(compile::PIPELINE_COLD.as_bytes());
    for (n, v) in compile::LLVM_OPTIONS {
        k = k.add(format!("{n}={v}").as_bytes());
    }
    k.add(ir)
}

fn compile_thread(rx: &Mutex<Receiver<Job>>, out: &Path, cache: Option<&Cache>, check: Option<&Path>, done: &Mutex<Done>, t0: Instant, aux: &Aux) {
    loop {
        let job = rx.lock().unwrap().recv();
        let Ok((name, packed, n, level)) = job else { break };
        let ir = lz4_flex::decompress_size_prepended(&packed).unwrap_or_else(|e| crate::fail(format!("{name}: {e}")));
        drop(packed);
        let t = Instant::now();
        let make = || {
            let _lease = aux.gate.enter(crate::gate::need_ir_mb(n));
            compile::partition(&ir, "<stdin>", level)
        };
        let (o, hit) = match cache {
            Some(c) => c.get_or(ir_key(&ir, level), make),
            None => make().map(|o| (o, false)),
        }
        .unwrap_or_else(|e| crate::fail(format!("{name}: {e}")));
        let dt = t.elapsed().as_secs_f64();
        drop(ir);
        let path: PathBuf = out.join(format!("{name}.o"));
        std::fs::write(&path, &o).unwrap_or_else(|e| crate::fail(format!("{}: {e}", path.display())));
        let verdict = check.map(|c| {
            let want = std::fs::read(c.join(format!("{name}.o"))).unwrap_or_default();
            if want == o {
                "same"
            } else if crate::objeq::compare(&o, &want).is_ok_and(|r| r.0) {
                "objeq"
            } else {
                "DIFFERS"
            }
        });
        if let Some(p) = aux.progress {
            p.done(&name, dt, hit);
        }
        let st = if hit { [0.0; 4] } else { crate::llvm::stage_seconds() };
        let mut d = done.lock().unwrap();
        d.objects.push((name.clone(), n));
        d.cached += hit as usize;
        d.compile_s += dt;
        *if level == Level::Hot { &mut d.hot_bytes } else { &mut d.cold_bytes } += o.len() as u64;
        if verdict == Some("DIFFERS") {
            d.differ.push(name.clone());
        }
        if !hit {
            println!(
                "{name}: {level:?} {dt:.1} s (parse {:.2}, passes {:.2}, codegen {:.2}, teardown {:.2}; {} MB IR){} [{}, {:.0} s]",
                st[0], st[1], st[2], st[3], n >> 20, verdict.map_or(String::new(), |v| format!(", {v}")), d.objects.len(), t0.elapsed().as_secs_f64()
            );
        }
    }
}
