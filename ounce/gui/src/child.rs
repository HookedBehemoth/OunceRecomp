//! A `ounce-builder` child process: its output read line by line on threads, so a step that crashes or hangs never
//! takes the window with it.

use std::io::{BufRead, BufReader, Read};
use std::path::Path;
use std::process::{Command, ExitStatus, Stdio};
use std::sync::mpsc::{channel, Receiver};

pub enum Line {
    Out(String),
    Err(String),
}

enum Msg {
    Line(Line),
    Closed,
}

pub struct Child {
    proc: std::process::Child,
    rx: Receiver<Msg>,
    open: usize,
    /// set once the process has ended and all its output is read
    pub exit: Option<ExitStatus>,
}

impl Child {
    /// Starts `exe args`; `wake` runs after each line (the window repaints).
    pub fn spawn(exe: &Path, args: &[String], wake: impl Fn() + Send + Clone + 'static) -> Result<Child, String> {
        let mut proc = Command::new(exe)
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| format!("{}: {e}", exe.display()))?;
        let (tx, rx) = channel();
        let pipes: [(Box<dyn Read + Send>, bool); 2] = [(Box::new(proc.stdout.take().unwrap()), false), (Box::new(proc.stderr.take().unwrap()), true)];
        for (pipe, is_err) in pipes {
            let (tx, wake) = (tx.clone(), wake.clone());
            std::thread::spawn(move || {
                for l in BufReader::new(pipe).split(b'\n').map_while(Result::ok) {
                    let s = String::from_utf8_lossy(&l).into_owned();
                    if tx.send(Msg::Line(if is_err { Line::Err(s) } else { Line::Out(s) })).is_err() {
                        return;
                    }
                    wake();
                }
                let _ = tx.send(Msg::Closed);
                wake();
            });
        }
        Ok(Child { proc, rx, open: 2, exit: None })
    }

    /// The lines that came since the last call; sets `exit` when the process is done.
    pub fn poll(&mut self) -> Vec<Line> {
        let mut out = Vec::new();
        for m in self.rx.try_iter() {
            match m {
                Msg::Line(l) => out.push(l),
                Msg::Closed => self.open -= 1,
            }
        }
        if self.exit.is_none() && self.open == 0 {
            self.exit = self.proc.try_wait().ok().flatten();
        }
        out
    }

    pub fn kill(&mut self) {
        let _ = self.proc.kill();
        if let Ok(s) = self.proc.wait() {
            self.exit.get_or_insert(s);
        }
    }
}

impl Drop for Child {
    fn drop(&mut self) {
        if self.exit.is_none() {
            self.kill();
        }
    }
}

/// `@@<tag> <rest>` -> (tag, rest)
pub fn tag(line: &str) -> Option<(&str, &str)> {
    let r = line.strip_prefix("@@")?;
    Some(r.split_once(' ').unwrap_or((r, "")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tags() {
        assert_eq!(tag("@@stage lift lifting the code"), Some(("stage", "lift lifting the code")));
        assert_eq!(tag("@@stages"), Some(("stages", "")));
        assert_eq!(tag("build: 16 jobs"), None);
    }

    #[test]
    fn reads_both_pipes_and_the_exit() {
        let args = ["-c".to_string(), "echo @@stage a; echo oops >&2; exit 3".to_string()];
        let mut c = Child::spawn(Path::new("/bin/sh"), &args, || {}).unwrap();
        let (mut out, mut err) = (Vec::new(), Vec::new());
        let t = std::time::Instant::now();
        while c.exit.is_none() && t.elapsed().as_secs() < 10 {
            for l in c.poll() {
                match l {
                    Line::Out(s) => out.push(s),
                    Line::Err(s) => err.push(s),
                }
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        assert_eq!(out, ["@@stage a"]);
        assert_eq!(err, ["oops"]);
        assert_eq!(c.exit.unwrap().code(), Some(3));
    }
}
