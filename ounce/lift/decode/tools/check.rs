//! ounce-decode-check: decodes words for tools/compare.py (the comparison with Python's enc.decode) and times it.
//!
//!   ounce-decode-check ids WORDS OUT        WORDS: little-endian u32s; OUT: a little-endian u16 per word, the entry's
//!                                           index in all_encodings() or 0xffff for None
//!   ounce-decode-check text WORDS OUT       OUT: a line per word, "NAME f=v f=v ..." or "None"
//!   ounce-decode-check bench WORDS          decodes every word (in file order) ten times and prints words/s
use std::io::Write;
use std::time::Instant;

fn read_words(path: &str) -> Vec<u32> {
    let b = std::fs::read(path).expect("read words");
    b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect()
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    match a.get(1).map(|s| s.as_str()) {
        Some("ids") => {
            let w = read_words(&a[2]);
            let mut out = Vec::with_capacity(w.len() * 2);
            for &x in &w {
                out.extend_from_slice(&ounce_decode::decode_id(x).unwrap_or(0xffff).to_le_bytes());
            }
            std::fs::write(&a[3], out).unwrap();
        }
        Some("text") => {
            let w = read_words(&a[2]);
            let mut f = std::io::BufWriter::new(std::fs::File::create(&a[3]).unwrap());
            for &x in &w {
                match ounce_decode::decode(x) {
                    None => writeln!(f, "None").unwrap(),
                    Some(i) => {
                        write!(f, "{}", i.name()).unwrap();
                        for (n, v) in i.fields() {
                            write!(f, " {n}={v}").unwrap();
                        }
                        writeln!(f).unwrap();
                    }
                }
            }
        }
        Some("bench") => {
            let w = read_words(&a[2]);
            let t = Instant::now();
            let mut acc = 0u64;
            for _ in 0..10 {
                for &x in &w {
                    acc += ounce_decode::decode_id(std::hint::black_box(x)).unwrap_or(0) as u64;
                }
            }
            let dt = t.elapsed().as_secs_f64();
            println!("{} words x10 in {:.3} s: {:.1} M words/s (acc {acc})", w.len(), dt, w.len() as f64 * 10.0 / dt / 1e6);
        }
        _ => {
            eprintln!("usage: ounce-decode-check ids|text|bench ...");
            std::process::exit(2);
        }
    }
}
