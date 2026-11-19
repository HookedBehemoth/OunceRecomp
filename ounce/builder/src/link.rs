//! The game's link: lld in this process (src/lld_shim.cpp), with the command line clang's driver gives ld.lld for
//! `clang --sysroot=<the build's sysroot> -fuse-ld=lld -o <exe> ...` (a PIE against its glibc, libgcc's start and end
//! files).

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::path::{Path, PathBuf};

extern "C" {
    fn hk_lld_link(argv: *const *const c_char, argc: c_int, msg: *mut *mut c_char) -> c_int;
    fn hk_free(p: *mut c_void);
}

/// ld.lld with these arguments (without the driver's name): lld's messages, or Err(them) on a failure
pub fn lld(args: &[String]) -> Result<String, String> {
    lld_main("ld.lld", args)
}

/// lld-link (lld's COFF driver) with these arguments: as `lld`
pub fn lld_coff(args: &[String]) -> Result<String, String> {
    lld_main("lld-link", args)
}

/// lld in this process, the driver `driver` names
fn lld_main(driver: &str, args: &[String]) -> Result<String, String> {
    let a: Vec<CString> = std::iter::once(driver).chain(args.iter().map(String::as_str)).map(|s| CString::new(s).unwrap()).collect();
    let argv: Vec<*const c_char> = a.iter().map(|s| s.as_ptr()).collect();
    let mut msg = std::ptr::null_mut();
    let code = unsafe { hk_lld_link(argv.as_ptr(), argv.len() as c_int, &mut msg) };
    let text = if msg.is_null() { String::new() } else { unsafe { CStr::from_ptr(msg) }.to_string_lossy().into_owned() };
    if !msg.is_null() {
        unsafe { hk_free(msg as *mut c_void) };
    }
    if code == 0 { Ok(text) } else { Err(text) }
}

/// The start files and the libraries' link names: the build's sysroot (sysroot.rs), glibc's in its LIB, GCC's in its
/// GCC folder
pub struct Sysroot {
    pub root: PathBuf,
}

impl Sysroot {
    fn crt(&self, f: &str) -> PathBuf {
        self.root.join(crate::sysroot::LIB).join(f)
    }
    fn gcc(&self, f: &str) -> PathBuf {
        self.root.join(crate::sysroot::GCC).join(f)
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
    /// the objects, in the link's order
    pub objects: &'a [PathBuf],
    /// --symbol-ordering-file (order_file's)
    pub order: Option<&'a Path>,
    /// libraries after the objects: static archives' paths, or -l names
    pub libs: &'a [String],
}

/// the game's link line, as clang's driver expands `clang -fuse-ld=lld` for ld.lld
pub fn exe_args(x: &Exe, s: &Sysroot) -> Vec<String> {
    let p = |p: &Path| p.to_string_lossy().into_owned();
    let mut a: Vec<String> = ["--hash-style=gnu", "--build-id", "--eh-frame-hdr", "-m", "elf_x86_64", "-pie", "-dynamic-linker",
                              "/lib64/ld-linux-x86-64.so.2", "-o"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    a.push(p(x.out));
    a.push(format!("--sysroot={}", p(&s.root)));
    a.push(p(&s.crt("Scrt1.o")));
    a.push(p(&s.crt("crti.o")));
    a.push(p(&s.gcc("crtbeginS.o")));
    // (the driver's -L folders for a Debian sysroot; libc.so's script names /lib/... inside --sysroot)
    for d in [crate::sysroot::GCC, crate::sysroot::LIB, "lib/x86_64-linux-gnu", "usr/lib"] {
        a.push(format!("-L{}", p(&s.root.join(d))));
    }
    if let Some(m) = x.map {
        a.push(format!("-Map={}", p(m)));
    }
    // (--export-dynamic-symbol: what ounce_renderer.so calls in the executable: os.h's and os_guest.h's os_*, and
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
    a.push(p(&s.gcc("crtendS.o")));
    a.push(p(&s.crt("crtn.o")));
    a
}

/// The Windows link line, for lld-link (lld's COFF driver; argv[0] "lld-link", `lld_coff`): a console program against
/// the static C runtime (libcmt, which the host's objects name) and MSVC's and the SDK's libraries (windows.rs). The
/// executable's exports for the renderer are runtime/os/os_exports_windows.c's /EXPORT directives; no import library
/// is written for them. /OPT:NOICF: identical lifted functions keep their own addresses (code pointers are compared).
/// The guest window's fixed address (A64_HOST_BASE) is reserved at start, so the image stays relocatable (ASLR).
pub fn exe_args_windows(x: &Exe, lib_dirs: &[PathBuf]) -> Vec<String> {
    let p = |p: &Path| p.to_string_lossy().into_owned();
    let mut a: Vec<String> = ["/nologo", "/machine:x64", "/subsystem:console", "/stack:8388608", "/opt:ref", "/opt:noicf", "/incremental:no",
                              "/manifest:no", "/noimplib", "/debug:none"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    a.push(format!("/out:{}", p(x.out)));
    for d in lib_dirs {
        a.push(format!("/libpath:{}", p(d)));
    }
    if let Some(m) = x.map {
        a.push(format!("/map:{}", p(m)));
    }
    a.extend(x.objects.iter().map(|o| p(o)));
    if let Some(o) = x.order {
        // (4037: a name the order file lists that is not a function of its own here)
        a.push(format!("/order:@{}", p(o)));
        a.push("/ignore:4037".into());
    }
    a.extend(x.libs.iter().cloned());
    for l in ["libcmt.lib", "oldnames.lib", "kernel32.lib", "user32.lib", "advapi32.lib", "shell32.lib", "ole32.lib", "synchronization.lib",
              "winmm.lib", "psapi.lib", "uuid.lib"] {
        a.push(l.into());
    }
    a
}
