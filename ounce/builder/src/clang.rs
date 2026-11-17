//! A C file compiled in this process by clang's libraries (src/clang_shim.cpp): clang's driver arguments -> the object's bytes.

use std::ffi::{c_char, c_int, c_void, CStr, CString};

extern "C" {
    fn hk_clang_compile(argv: *const *const c_char, argc: c_int, out_n: *mut usize, err: *mut *mut c_char) -> *mut c_void;
    fn hk_free(p: *mut c_void);
}

/// `args`: clang's driver arguments after the program name (the flags, -c <file>; an -o is ignored)
pub fn compile(args: &[String]) -> Result<Vec<u8>, String> {
    let a: Vec<CString> = std::iter::once("clang").chain(args.iter().map(String::as_str)).map(|s| CString::new(s).unwrap()).collect();
    let argv: Vec<*const c_char> = a.iter().map(|s| s.as_ptr()).collect();
    let (mut n, mut err) = (0usize, std::ptr::null_mut());
    let p = unsafe { hk_clang_compile(argv.as_ptr(), argv.len() as c_int, &mut n, &mut err) };
    if p.is_null() {
        let e = unsafe { CStr::from_ptr(err) }.to_string_lossy().into_owned();
        unsafe { hk_free(err as *mut c_void) };
        return Err(e);
    }
    let o = unsafe { std::slice::from_raw_parts(p as *const u8, n) }.to_vec();
    unsafe { hk_free(p) };
    Ok(o)
}
