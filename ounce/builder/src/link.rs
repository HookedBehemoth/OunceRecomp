//! The game's link: lld in this process (src/lld_shim.cpp), with the command line clang's driver gives ld.lld for
//! build.py's `clang -fuse-ld=lld -o hookshot ...` (a PIE against glibc, libgcc's start and end files).

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::path::{Path, PathBuf};

extern "C" {
    fn hk_lld_link(argv: *const *const c_char, argc: c_int, msg: *mut *mut c_char) -> c_int;
    fn hk_free(p: *mut c_void);
}

/// ld.lld with these arguments (without the driver's name): lld's messages, or Err(them) on a failure
pub fn lld(args: &[String]) -> Result<String, String> {
    let a: Vec<CString> = std::iter::once("ld.lld").chain(args.iter().map(String::as_str)).map(|s| CString::new(s).unwrap()).collect();
    let argv: Vec<*const c_char> = a.iter().map(|s| s.as_ptr()).collect();
    let mut msg = std::ptr::null_mut();
    let code = unsafe { hk_lld_link(argv.as_ptr(), argv.len() as c_int, &mut msg) };
    let text = if msg.is_null() { String::new() } else { unsafe { CStr::from_ptr(msg) }.to_string_lossy().into_owned() };
    if !msg.is_null() {
        unsafe { hk_free(msg as *mut c_void) };
    }
    if code == 0 { Ok(text) } else { Err(text) }
}

/// Where the start files and the system's libraries are: the AppImage's sysroot (HOOKSHOT_SYSROOT: start files and
/// libraries in its lib/), else this system's (glibc's in /usr/lib64 or /usr/lib, libgcc's in its gcc folder)
pub struct Sysroot {
    /// Scrt1.o, crti.o, crtn.o
    pub crt: PathBuf,
    /// crtbeginS.o, crtendS.o, libgcc.a
    pub gcc: PathBuf,
    /// the -L folders, in the driver's order
    pub lib: Vec<PathBuf>,
}

impl Sysroot {
    pub fn find() -> Result<Sysroot, String> {
        if let Some(r) = std::env::var_os("HOOKSHOT_SYSROOT").filter(|v| !v.is_empty()) {
            let lib = PathBuf::from(r).join("lib");
            return Ok(Sysroot { crt: lib.clone(), gcc: lib.clone(), lib: vec![lib] });
        }
        let crt = ["/usr/lib64", "/usr/lib/x86_64-linux-gnu", "/usr/lib"]
            .iter()
            .map(PathBuf::from)
            .find(|d| d.join("Scrt1.o").exists())
            .ok_or("no Scrt1.o (glibc's start file) in /usr/lib64, /usr/lib/x86_64-linux-gnu or /usr/lib: set HOOKSHOT_SYSROOT")?;
        // (the newest gcc's folder: /usr/lib{,64}/gcc/x86_64-*/<version>/crtbeginS.o)
        let mut gccs: Vec<(Vec<u32>, PathBuf)> = Vec::new();
        for base in ["/usr/lib/gcc", "/usr/lib64/gcc"] {
            for triple in std::fs::read_dir(base).into_iter().flatten().flatten() {
                if !triple.file_name().to_string_lossy().starts_with("x86_64") {
                    continue;
                }
                for v in std::fs::read_dir(triple.path()).into_iter().flatten().flatten() {
                    let ver: Vec<u32> = v.file_name().to_string_lossy().split('.').filter_map(|x| x.parse().ok()).collect();
                    if v.path().join("crtbeginS.o").exists() {
                        gccs.push((ver, v.path()));
                    }
                }
            }
        }
        gccs.sort();
        let gcc = gccs.pop().ok_or("no gcc folder with crtbeginS.o (libgcc's start file): set HOOKSHOT_SYSROOT")?.1;
        let mut lib = vec![gcc.clone(), crt.clone()];
        for d in ["/lib64", "/usr/lib64", "/lib", "/usr/lib"] {
            let d = PathBuf::from(d);
            if d.exists() && !lib.contains(&d) {
                lib.push(d);
            }
        }
        Ok(Sysroot { crt, gcc, lib })
    }
}

/// The game description's link order (`hot_order.bin`: "HOTO", u32 count, count u32 guest addresses, little-endian)
/// as lld's symbol-ordering file `hot_order.txt` in `dir`, one f_<address> per line; its path
pub fn order_file(bin: &Path, dir: &Path) -> Result<PathBuf, String> {
    let d = std::fs::read(bin).map_err(|e| format!("{}: {e}", bin.display()))?;
    let n = if d.len() >= 8 && &d[..4] == b"HOTO" { u32::from_le_bytes(d[4..8].try_into().unwrap()) as usize } else { usize::MAX };
    if n == usize::MAX || d.len() != 8 + 4 * n {
        return Err(format!("{} is not a link order (\"HOTO\", u32 count, count u32 addresses)", bin.display()));
    }
    let text: String = d[8..].chunks_exact(4).map(|c| format!("f_{:08x}\n", u32::from_le_bytes(c.try_into().unwrap()))).collect();
    let p = dir.join("hot_order.txt");
    std::fs::write(&p, text).map_err(|e| format!("{}: {e}", p.display()))?;
    Ok(p)
}

/// What goes into the executable
pub struct Exe<'a> {
    pub out: &'a Path,
    /// the link map beside it (crash offsets -> functions)
    pub map: Option<&'a Path>,
    /// the objects, in build.py's order (its objs.rsp)
    pub objects: &'a [PathBuf],
    /// --symbol-ordering-file (order_file's)
    pub order: Option<&'a Path>,
    /// libraries after the objects: static archives' paths, or -l names
    pub libs: &'a [String],
}

/// build.py's link line, as clang's driver expands it for ld.lld
pub fn exe_args(x: &Exe, s: &Sysroot) -> Vec<String> {
    let p = |p: &Path| p.to_string_lossy().into_owned();
    let mut a: Vec<String> = ["--hash-style=gnu", "--build-id", "--eh-frame-hdr", "-m", "elf_x86_64", "-pie", "-dynamic-linker",
                              "/lib64/ld-linux-x86-64.so.2", "-o"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    a.push(p(x.out));
    a.push(p(&s.crt.join("Scrt1.o")));
    a.push(p(&s.crt.join("crti.o")));
    a.push(p(&s.gcc.join("crtbeginS.o")));
    a.extend(s.lib.iter().map(|d| format!("-L{}", p(d))));
    if let Some(m) = x.map {
        a.push(format!("-Map={}", p(m)));
    }
    // (--export-dynamic-symbol: what hookshot_direct.so calls in the executable: os.h's and os_guest.h's os_*, and
    // hos_mem_ledger)
    a.push("--gc-sections".into());
    a.push("--export-dynamic-symbol=os_*".into());
    a.push("--export-dynamic-symbol=hos_mem_ledger".into());
    a.extend(x.objects.iter().map(|o| p(o)));
    if let Some(o) = x.order {
        a.push(format!("--symbol-ordering-file={}", p(o)));
        a.push("--no-warn-symbol-ordering".into());
    }
    a.extend(x.libs.iter().cloned());
    for l in ["-lpthread", "-ldl", "-lm", "-lgcc", "--as-needed", "-lgcc_s", "--no-as-needed", "-lc", "-lgcc", "--as-needed", "-lgcc_s", "--no-as-needed"] {
        a.push(l.into());
    }
    a.push(p(&s.gcc.join("crtendS.o")));
    a.push(p(&s.crt.join("crtn.o")));
    a
}
