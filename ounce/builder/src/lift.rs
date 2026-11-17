//! The lift and the compile in one process: ounce-lift's pipeline hands each partition and each generated module
//! to a pool of compile threads through a bounded queue, so no IR is written to disk and at most a queue's worth of
//! partitions waits in memory. The objects are build.py's: the partitions at their level (lift_h*: hot), the
//! generated code and lift_answers cold.

use crate::compile::{self, Level};
use ounce_lift::pipeline::{self, Out};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::sync::Mutex;
use std::time::Instant;

/// One module to compile: (object name, IR, level)
type Job = (String, Vec<u8>, Level);

struct Queue<'a> {
    tx: Mutex<Option<SyncSender<Job>>>,
    /// the generated modules and lift_answers, held until the first cold partition: the lift hands every hot one
    /// over before it (Run::hot_first), and a cold compile running would keep the hot ones waiting at compile.rs's
    /// gate until no cold one is left
    held: Mutex<Option<Vec<Job>>>,
    /// image.bin and functions.txt land here
    dir: &'a Path,
}

impl Queue<'_> {
    fn send(&self, job: Job) {
        // (a clone of the sender per call: blocking on a full queue must not hold the lock)
        let tx = self.tx.lock().unwrap().clone().expect("the queue is open while the lift runs");
        tx.send(job).expect("the compile threads stopped");
    }
}

impl Queue<'_> {
    /// sends the held modules (once)
    fn release(&self) {
        let held = self.held.lock().unwrap().take();
        for job in held.into_iter().flatten() {
            self.send(job);
        }
    }
}

impl Out for Queue<'_> {
    fn file(&self, rel: &str, data: Vec<u8>) {
        if let Some(stem) = rel.strip_suffix(".ll") {
            let job = (stem.rsplit('/').next().unwrap().to_string(), data, Level::Cold);
            let mut held = self.held.lock().unwrap();
            match held.as_mut() {
                Some(h) => h.push(job),
                None => {
                    drop(held);
                    self.send(job);
                }
            }
        } else if rel == "image/image.bin" || rel == "gen/functions.txt" {
            let p = self.dir.join(rel.rsplit('/').next().unwrap());
            std::fs::write(&p, data).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
        }
    }

    fn part(&self, name: &str, head: &[u8], body: &[u8]) {
        let mut ir = Vec::with_capacity(head.len() + body.len());
        ir.extend_from_slice(head);
        ir.extend_from_slice(body);
        let level = if name.starts_with("lift_h") { Level::Hot } else { Level::Cold };
        if level == Level::Cold {
            self.release();
        }
        self.send((name.to_string(), ir, level));
    }

    fn wants_facts(&self) -> bool {
        false
    }
}

/// What the compile threads did: (objects, seconds in compiles, objects unlike the check folder's)
#[derive(Default)]
pub struct Done {
    pub objects: usize,
    pub compile_s: f64,
    pub differ: Vec<String>,
}

/// Lifts with `run` and compiles every module into `out`/<name>.o with `jobs` compile threads; `check`: a folder of
/// build.py's objects, each compared byte for byte, then by objeq when the bytes differ
pub fn lift_compile(run: &pipeline::Run, out: &Path, jobs: usize, check: Option<&Path>) -> Result<Done, String> {
    std::fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    compile::init()?;
    let (tx, rx) = sync_channel::<Job>(jobs);
    let rx = Mutex::new(rx);
    let q = Queue { tx: Mutex::new(Some(tx)), held: Mutex::new(Some(Vec::new())), dir: out };
    let done = Mutex::new(Done::default());
    let t0 = Instant::now();
    let lifted = std::thread::scope(|s| {
        for _ in 0..jobs.max(1) {
            s.spawn(|| compile_thread(&rx, out, check, &done, t0));
        }
        let r = pipeline::lift(run, &q);
        q.release();
        q.tx.lock().unwrap().take();
        r
    });
    lifted?;
    Ok(done.into_inner().unwrap())
}

fn compile_thread(rx: &Mutex<Receiver<Job>>, out: &Path, check: Option<&Path>, done: &Mutex<Done>, t0: Instant) {
    loop {
        let job = rx.lock().unwrap().recv();
        let Ok((name, ir, level)) = job else { break };
        let t = Instant::now();
        let o = compile::partition(&ir, "<stdin>", level).unwrap_or_else(|e| crate::fail(format!("{name}: {e}")));
        let dt = t.elapsed().as_secs_f64();
        let ir_mb = ir.len() >> 20;
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
        let mut d = done.lock().unwrap();
        d.objects += 1;
        d.compile_s += dt;
        if verdict == Some("DIFFERS") {
            d.differ.push(name.clone());
        }
        let st = crate::llvm::stage_seconds();
        println!("{name}: {level:?} {dt:.1} s (parse {:.2}, passes {:.2}, codegen {:.2}, teardown {:.2}; {} MB IR){} [{}, {:.0} s]", st[0], st[1], st[2], st[3], ir_mb, verdict.map_or(String::new(), |v| format!(", {v}")), d.objects, t0.elapsed().as_secs_f64());
    }
}
