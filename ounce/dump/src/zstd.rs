//! zstd decoding (the standard format and ZBIC), over third_party/zbic's zstd.c built by build.rs.

use std::ffi::{c_char, c_int, c_uint, c_void, CStr};

pub const MAGIC: [u8; 4] = [0x28, 0xb5, 0x2f, 0xfd];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Format {
    Std,
    /// Switch firmware 22+'s NSO segment compression
    Zbic,
}

macro_rules! ffi {
    ($($p:ident)*) => { $(
        #[allow(dead_code)]
        mod $p {
            use super::*;
            extern "C" {
                #[link_name = concat!(stringify!($p), "_dctx_new")] pub fn dctx_new() -> *mut c_void;
                #[link_name = concat!(stringify!($p), "_dctx_free")] pub fn dctx_free(d: *mut c_void);
                #[link_name = concat!(stringify!($p), "_dctx_reset")] pub fn dctx_reset(d: *mut c_void) -> usize;
                #[link_name = concat!(stringify!($p), "_decompress_stream")]
                pub fn decompress_stream(d: *mut c_void, dst: *mut u8, cap: usize, dpos: *mut usize, src: *const u8, n: usize, spos: *mut usize) -> usize;
                #[link_name = concat!(stringify!($p), "_is_error")] pub fn is_error(r: usize) -> c_uint;
                #[link_name = concat!(stringify!($p), "_error_name")] pub fn error_name(r: usize) -> *const c_char;
                #[link_name = concat!(stringify!($p), "_frame_header_ok")] pub fn frame_header_ok(src: *const u8, n: usize) -> c_int;
                #[link_name = concat!(stringify!($p), "_decompress")] pub fn decompress(dst: *mut u8, cap: usize, src: *const u8, n: usize) -> usize;
                #[link_name = concat!(stringify!($p), "_ddict_new")] pub fn ddict_new(d: *const u8, n: usize) -> *mut c_void;
                #[link_name = concat!(stringify!($p), "_ddict_free")] pub fn ddict_free(d: *mut c_void);
                #[link_name = concat!(stringify!($p), "_dict_id_of_frame")] pub fn dict_id_of_frame(src: *const u8, n: usize) -> c_uint;
                #[link_name = concat!(stringify!($p), "_decompress_dict")]
                pub fn decompress_dict(dctx: *mut c_void, dst: *mut u8, cap: usize, src: *const u8, n: usize, ddict: *const c_void) -> usize;
                #[link_name = concat!(stringify!($p), "_cctx_new")] pub fn cctx_new() -> *mut c_void;
                #[link_name = concat!(stringify!($p), "_cctx_free")] pub fn cctx_free(c: *mut c_void);
                #[link_name = concat!(stringify!($p), "_compress_cctx")]
                pub fn compress_cctx(c: *mut c_void, dst: *mut u8, cap: usize, src: *const u8, n: usize, level: c_int, checksum: c_int) -> usize;
                #[link_name = concat!(stringify!($p), "_compress_dict")]
                pub fn compress_dict(dst: *mut u8, cap: usize, src: *const u8, n: usize, d: *const u8, dn: usize, level: c_int, checksum: c_int) -> usize;
                #[link_name = concat!(stringify!($p), "_compress_bound")] pub fn compress_bound(n: usize) -> usize;
                #[link_name = concat!(stringify!($p), "_compress")]
                pub fn compress(dst: *mut u8, cap: usize, src: *const u8, n: usize, level: c_int, checksum: c_int) -> usize;
            }
        }
    )* };
}
ffi!(zs_std zs_zbic);

macro_rules! call {
    ($f:expr, $name:ident ( $($a:expr),* )) => {
        match $f { Format::Std => zs_std::$name($($a),*), Format::Zbic => zs_zbic::$name($($a),*) }
    };
}

fn check(f: Format, r: usize) -> Result<usize, String> {
    // SAFETY: plain calls into the C library; error_name returns a static string
    unsafe {
        if call!(f, is_error(r)) != 0 {
            return Err(CStr::from_ptr(call!(f, error_name(r))).to_string_lossy().into_owned());
        }
    }
    Ok(r)
}

/// A streaming decoder of one frame at a time.
pub struct Decoder {
    f: Format,
    d: *mut c_void,
}

// SAFETY: the context is owned and used by one thread at a time
unsafe impl Send for Decoder {}

/// What one `Decoder::feed` did.
pub struct Fed {
    /// input bytes taken
    pub used: usize,
    /// the frame ended (and everything it holds was handed out)
    pub done: bool,
}

impl Decoder {
    pub fn new(f: Format) -> Decoder {
        // SAFETY: a fresh context, freed on drop
        let d = unsafe { call!(f, dctx_new()) };
        assert!(!d.is_null(), "zstd: out of memory");
        Decoder { f, d }
    }

    /// Decodes `src` until it is used up or the frame ends, handing out each piece of output in turn. An error of
    /// `sink` stops it and is returned as is.
    pub fn feed<E: From<String>>(&mut self, src: &[u8], buf: &mut [u8], mut sink: impl FnMut(&[u8]) -> Result<(), E>) -> Result<Fed, E> {
        let mut spos = 0;
        loop {
            let mut dpos = 0;
            // SAFETY: the buffers are valid for the lengths given; the positions stay within them
            let r = unsafe { call!(self.f, decompress_stream(self.d, buf.as_mut_ptr(), buf.len(), &mut dpos, src.as_ptr(), src.len(), &mut spos)) };
            let r = check(self.f, r)?;
            if dpos > 0 {
                sink(&buf[..dpos])?;
            }
            if r == 0 {
                return Ok(Fed { used: spos, done: true });
            }
            if spos == src.len() && dpos < buf.len() {
                return Ok(Fed { used: spos, done: false });
            }
        }
    }

    /// Ready for a new frame.
    pub fn reset(&mut self) {
        // SAFETY: a valid context
        unsafe { call!(self.f, dctx_reset(self.d)) };
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        // SAFETY: created by dctx_new, freed once
        unsafe { call!(self.f, dctx_free(self.d)) }
    }
}

/// Whether `head` holds a complete, valid frame header.
pub fn frame_header_ok(f: Format, head: &[u8]) -> bool {
    // SAFETY: reads `head` within its length
    unsafe { call!(f, frame_header_ok(head.as_ptr(), head.len())) == 1 }
}

/// One whole frame of `size` bytes.
pub fn decompress(f: Format, src: &[u8], size: usize) -> Result<Vec<u8>, String> {
    let mut out = vec![0u8; size];
    // SAFETY: the buffers are valid for the lengths given
    let n = check(f, unsafe { call!(f, decompress(out.as_mut_ptr(), size, src.as_ptr(), src.len())) })?;
    if n != size {
        return Err(format!("{n} of {size} bytes"));
    }
    Ok(out)
}

/// A frame of `src` (the tests' fixtures).
pub fn compress(f: Format, src: &[u8], level: i32, checksum: bool) -> Vec<u8> {
    // SAFETY: the buffers are valid for the lengths given
    unsafe {
        let mut out = vec![0u8; call!(f, compress_bound(src.len()))];
        let n = check(f, call!(f, compress(out.as_mut_ptr(), out.len(), src.as_ptr(), src.len(), level, checksum as c_int))).unwrap();
        out.truncate(n);
        out
    }
}

/// A decoding dictionary (raw zstd dictionary bytes).
pub struct Dict(*mut c_void);

// SAFETY: a DDict is read-only once made and may be shared between threads
unsafe impl Send for Dict {}
unsafe impl Sync for Dict {}

impl Dict {
    pub fn new(data: &[u8]) -> Result<Dict, String> {
        // SAFETY: the bytes are copied by zstd
        let d = unsafe { zs_std::ddict_new(data.as_ptr(), data.len()) };
        if d.is_null() {
            return Err("not a zstd dictionary".into());
        }
        Ok(Dict(d))
    }
}

impl Drop for Dict {
    fn drop(&mut self) {
        // SAFETY: made by ddict_new, freed once
        unsafe { zs_std::ddict_free(self.0) }
    }
}

/// The dictionary id a standard frame names (0: none).
pub fn dict_id_of_frame(src: &[u8]) -> u32 {
    // SAFETY: reads `src` within its length
    unsafe { zs_std::dict_id_of_frame(src.as_ptr(), src.len()) }
}

/// One whole standard frame of `size` bytes, decoded with a dictionary.
pub fn decompress_with(src: &[u8], size: usize, dict: &Dict) -> Result<Vec<u8>, String> {
    let mut out = vec![0u8; size];
    // SAFETY: the buffers are valid for the lengths given; the context is made and freed here
    let n = unsafe {
        let d = zs_std::dctx_new();
        let r = zs_std::decompress_dict(d, out.as_mut_ptr(), size, src.as_ptr(), src.len(), dict.0);
        zs_std::dctx_free(d);
        check(Format::Std, r)?
    };
    if n != size {
        return Err(format!("{n} of {size} bytes"));
    }
    Ok(out)
}

/// One standard frame of `src` compressed with a raw dictionary (`dict`: the dictionary's bytes).
pub fn compress_with(src: &[u8], dict: &[u8], level: i32, checksum: bool) -> Result<Vec<u8>, String> {
    // SAFETY: the buffers are valid for the lengths given
    unsafe {
        let mut out = vec![0u8; zs_std::compress_bound(src.len())];
        let r = zs_std::compress_dict(out.as_mut_ptr(), out.len(), src.as_ptr(), src.len(), dict.as_ptr(), dict.len(), level, checksum as c_int);
        out.truncate(check(Format::Std, r)?);
        Ok(out)
    }
}

/// A standard-format compressor kept across frames; each frame states its content size.
pub struct Compressor(*mut c_void);

// SAFETY: the context is owned and used by one thread at a time
unsafe impl Send for Compressor {}

impl Compressor {
    pub fn new() -> Compressor {
        // SAFETY: a plain constructor
        Compressor(unsafe { zs_std::cctx_new() })
    }

    pub fn compress(&mut self, src: &[u8], level: i32, checksum: bool) -> Vec<u8> {
        // SAFETY: the buffers are valid for the lengths given
        unsafe {
            let mut out = vec![0u8; zs_std::compress_bound(src.len())];
            let r = zs_std::compress_cctx(self.0, out.as_mut_ptr(), out.len(), src.as_ptr(), src.len(), level, checksum as c_int);
            let n = check(Format::Std, r).unwrap();
            out.truncate(n);
            out
        }
    }
}

impl Default for Compressor {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Compressor {
    fn drop(&mut self) {
        // SAFETY: made by cctx_new, freed once
        unsafe { zs_std::cctx_free(self.0) }
    }
}

/// The content size a frame header states, None when it states none (or `head` is not 18 bytes of a frame).
pub fn frame_content_size(head: &[u8]) -> Option<u64> {
    if head.len() < 18 || head[..4] != MAGIC {
        return None;
    }
    let fhd = head[4];
    let single = (fhd >> 5) & 1;
    let at = 5 + (1 - single as usize) + [0, 1, 2, 4][(fhd & 3) as usize];
    let n = [single as usize, 2, 4, 8][(fhd >> 6) as usize];
    if n == 0 {
        return None;
    }
    let mut v = [0u8; 8];
    v[..n].copy_from_slice(&head[at..at + n]);
    let size = u64::from_le_bytes(v);
    Some(if n == 2 { size + 256 } else { size })
}

/// A zstd frame or a skippable frame (magic 0x184D2A50 to 0x184D2A5F) starts here.
pub fn starts_zstd(head: &[u8]) -> bool {
    head == MAGIC || (head.len() == 4 && head[1..] == [0x2a, 0x4d, 0x18] && head[0] & 0xf0 == 0x50)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_both_formats() {
        let data: Vec<u8> = (0..100_000u32).map(|i| (i % 251) as u8 ^ (i / 1000) as u8).collect();
        for f in [Format::Std, Format::Zbic] {
            let z = compress(f, &data, 9, true);
            assert_eq!(&z[..4], if f == Format::Std { &MAGIC[..] } else { b"ZBIC" });
            assert_eq!(decompress(f, &z, data.len()).unwrap(), data);
            let mut d = Decoder::new(f);
            let mut out = Vec::new();
            let mut buf = vec![0u8; 4096];
            let fed = d.feed::<String>(&z, &mut buf, |b| Ok(out.extend_from_slice(b))).unwrap();
            assert!(fed.done && fed.used == z.len());
            assert_eq!(out, data);
        }
        // (one format's frame is not the other's)
        assert!(decompress(Format::Std, &compress(Format::Zbic, &data, 3, false), data.len()).is_err());
    }

    #[test]
    fn starts() {
        let got: Vec<bool> = [&MAGIC[..], b"\x50\x2a\x4d\x18", b"\x5f\x2a\x4d\x18", b"\x60\x2a\x4d\x18", &[0; 4], &MAGIC[..3]]
            .iter()
            .map(|h| starts_zstd(h))
            .collect();
        assert_eq!(got, [true, true, true, false, false, false]);
    }
}
