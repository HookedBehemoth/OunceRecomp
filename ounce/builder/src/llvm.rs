//! libLLVM in this process (src/llvm_shim.cpp): a module's IR -> an object, as `opt | clang -x ir -c` makes it.

use std::ffi::{c_char, c_int, c_void, CStr, CString};

extern "C" {
    fn hk_llvm_init(argv: *const *const c_char, argc: c_int, err: *mut *mut c_char) -> c_int;
    fn hk_llvm_version() -> *const c_char;
    fn hk_llvm_compile(
        ir: *const c_char,
        n: usize,
        name: *const c_char,
        cpu: *const c_char,
        pipeline: *const c_char,
        level: c_int,
        slp: c_int,
        rotation: c_int,
        out_n: *mut usize,
        err: *mut *mut c_char,
    ) -> *mut c_void;
    fn hk_free(p: *mut c_void);
    fn hk_llvm_stage_seconds(out: *mut f64);
    fn hk_llvm_uint_option(name: *const c_char, set: c_int, v: u32, out: *mut u32) -> c_int;
}

fn take_err(e: *mut c_char) -> String {
    if e.is_null() {
        return "(no message)".into();
    }
    let s = unsafe { CStr::from_ptr(e) }.to_string_lossy().into_owned();
    unsafe { hk_free(e as *mut c_void) };
    s
}

pub fn version() -> String {
    unsafe { CStr::from_ptr(hk_llvm_version()) }.to_string_lossy().into_owned()
}

/// LLVM's command line options (clang's -mllvm ones) for the process; the first call sets them, later ones only
/// report the first's result
pub fn init(options: &[&str]) -> Result<(), String> {
    let args: Vec<CString> = std::iter::once("ounce").chain(options.iter().copied()).map(|s| CString::new(s).unwrap()).collect();
    let argv: Vec<*const c_char> = args.iter().map(|a| a.as_ptr()).collect();
    let mut err = std::ptr::null_mut();
    if unsafe { hk_llvm_init(argv.as_ptr(), argv.len() as c_int, &mut err) } != 0 {
        return Err(take_err(err));
    }
    Ok(())
}

/// How a module is compiled
pub struct Recipe<'a> {
    pub cpu: &'a str,
    /// a PassBuilder pipeline text ("default<O2>", or clang's -print-pipeline-passes); "" for none
    pub pipeline: &'a str,
    /// code generation's level (0..3, clang's -O level)
    pub level: u32,
    /// the pipeline's SLP vectorizer (clang: from -O2)
    pub slp: bool,
    /// -rotation-max-header-size for this module (None: as it is). A global of LLVM's: keep the compiles that want
    /// different values apart
    pub rotation: Option<u32>,
}

/// One module: `ir` (text or bitcode) through the recipe's pipeline, then code generation: the object file's bytes
pub fn compile(ir: &[u8], name: &str, r: &Recipe) -> Result<Vec<u8>, String> {
    let (cpu, pipeline, level) = (r.cpu, r.pipeline, r.level);
    // (LLVM's IR lexer reads up to a NUL after the text: the buffer must have one past its end)
    let mut owned = Vec::new();
    let ir: &[u8] = {
        owned.reserve_exact(ir.len() + 1);
        owned.extend_from_slice(ir);
        owned.push(0);
        &owned[..ir.len()]
    };
    let (name, cpu, pipeline) = (CString::new(name).unwrap(), CString::new(cpu).unwrap(), CString::new(pipeline).unwrap());
    let (mut n, mut err) = (0usize, std::ptr::null_mut());
    let p = unsafe {
        hk_llvm_compile(ir.as_ptr() as *const c_char, ir.len(), name.as_ptr(), cpu.as_ptr(), pipeline.as_ptr(), level as c_int, r.slp as c_int,
            r.rotation.map_or(-1, |v| v as c_int), &mut n, &mut err)
    };
    if p.is_null() {
        return Err(take_err(err));
    }
    let v = unsafe { std::slice::from_raw_parts(p as *const u8, n) }.to_vec();
    unsafe { hk_free(p) };
    Ok(v)
}

/// this thread's last compile's seconds: parse, pass pipeline, code generation, teardown (module and context)
pub fn stage_seconds() -> [f64; 4] {
    let mut s = [0f64; 4];
    unsafe { hk_llvm_stage_seconds(s.as_mut_ptr()) };
    s
}

/// an unsigned option of LLVM's (a -mllvm one): its value after setting it to `set` (None: read only); None when LLVM
/// has no option of that name
pub fn uint_option(name: &str, set: Option<u32>) -> Option<u32> {
    let n = CString::new(name).unwrap();
    let mut out = 0u32;
    (unsafe { hk_llvm_uint_option(n.as_ptr(), set.is_some() as c_int, set.unwrap_or(0), &mut out) } == 0).then_some(out)
}
