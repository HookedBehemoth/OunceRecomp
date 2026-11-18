//! The native NVN layer (runtime/nvn_native.c, nvn_layout.c), the render-target pool and slots (nn_applet.c,
//! games/oot/patches/rt_slots.c) and the host copies the game asks for (nn_libc.c, nn_fs.c, nn_video.c), compiled from
//! the source as it is around authored memory. No game or GPU.

use ounce_tests::*;
use regex::Regex;
use std::path::PathBuf;

/// writes text as name into t and compiles it: flags, the source, then tail (more sources, libraries)
fn build(t: &Tmp, compiler: &str, name: &str, text: &str, flags: &[&str], tail: &[&str]) -> PathBuf {
    let src = t.write(name, text);
    let mut args: Vec<String> = flags.iter().map(|s| s.to_string()).collect();
    args.push(src.display().to_string());
    args.extend(tail.iter().map(|s| s.to_string()));
    compile(compiler, &args, &t.join("fixture"))
}

/// nvn_layout.c's block-linear to linear copy (fixtures/nvn_deswizzle_test.c).
#[test]
fn deswizzle() {
    let t = Tmp::new("deswizzle");
    let exe = compile(
        &cc(),
        &["-std=c11", "-Wall", "-Wextra", "-Werror", &inc("runtime"), &inc("renderer/api"), "-Wno-unused-parameter",
          "-Wno-sign-compare", fixture_path("nvn_deswizzle_test.c").to_str().unwrap(), path("runtime/nvn_layout.c").to_str().unwrap()],
        &t.join("fixture"),
    );
    run(&mut clean(&exe));
}

/// Every guarded memory-pool accessor (flags, size, map, buffer address, invalidate mapped range) called with no pool
/// answers 0 or returns without touching anything, built at -O2 where a check after the dereference is folded away
/// (nvnMemoryPoolGetFlags(NULL) from the day-to-night change read guest 0x4); a real pool still answers; the occlusion
/// grid reader's 0x3980-byte invalidates skip the renderer's wait, any other size or place waits.
#[test]
fn null_pool() {
    let s = read("runtime/nvn_native.c");
    let fx = fixture("nvn_null_pool_test.c")
        .replace("@GUARD@", block(&s, "static int no_pool(Ctx *c, const char *what)"))
        .replace("@FLAGS@", block(&s, "NV(nvnMemoryPoolGetFlags)"))
        .replace("@SIZE@", block(&s, "NV(nvnMemoryPoolGetSize)"))
        .replace("@MAP@", block(&s, "NV(nvnMemoryPoolMap)"))
        .replace("@ADDRESS@", block(&s, "NV(nvnMemoryPoolGetBufferAddress)"))
        .replace("@INVALIDATE@", block(&s, "NV(nvnMemoryPoolInvalidateMappedRange)"));
    let t = Tmp::new("nvn-null-pool");
    let exe = build(&t, &cc(), "fixture.c", &fx, &["-std=c11", "-O2", "-fno-strict-aliasing"], &[]);
    run(&mut clean(&exe));
}

/// nvnBufferMap: a pool-less buffer gets zeroed writable scratch large enough for it (a zero-size one too, in a fresh
/// process), a repeated map keeps its pointer and contents, growing keeps the earlier map; pooled maps keep their
/// pointer and offset, a pool without CPU memory and a null buffer stay unmapped and allocate nothing.
#[test]
fn buffer_map() {
    let s = read("runtime/nvn_native.c");
    let fx = fixture("nvn_buffer_map_test.c")
        .replace("@SCRATCH@", block(&s, "static uint64_t scratch_map("))
        .replace("@MAPPING@", block(&s, "NV(nvnBufferMap)"));
    let t = Tmp::new("nvn-buffer-map");
    let exe = build(&t, &cc(), "fixture.c", &fx, &["-std=c11", "-O2", "-fno-strict-aliasing"], &[]);
    run(&mut clean(&exe));
    run(clean(&exe).arg("zero-size"));
}

/// The native NVN layer keeps its host pointers in host tables and checks guest ranges against the guest window
/// (nvn_native.c and hos.h's real functions, rx_nvn.h's layouts, a small window of test memory): a guest range is
/// accepted only inside the window, wrapping ranges too; a command buffer's recording buffer is found through its own
/// slot only (another object's or an out-of-range slot number gets a new slot), Finalize gives the slot back, and the
/// same through each thread's free slots (HOS_NVN_LIST_REF); a recording handle is live only while in the set;
/// ReadTexels / WriteTexels' extent covers every row and image, and strides that wrap give 0.
#[test]
fn host_tables() {
    let nvn = read("runtime/nvn_native.c");
    let hos = read("runtime/hos.h");
    let start = nvn[..find(&nvn, "} cmd_rec;")].rfind("typedef struct {").expect("cmd_rec's typedef");
    let table_decls = &nvn[start..start + find(&nvn[start..], "static cmd_rec *cmd_slot(")];
    let live_decls = between(&nvn, "static uint64_t *live_handles;", "static void live_handle_add(");
    let pieces = [
        block(&hos, "static inline void *hos_guest_range("),
        block(&nvn, "static void *gw("),
        block(&nvn, "static const void *in_window("),
        table_decls,
        block(&nvn, "static cmd_rec *cmd_slot("),
        block(&nvn, "static uint32_t cmd_slot_new_locked("),
        "static _Thread_local uint32_t cmd_tl_free[64];\nstatic _Thread_local uint32_t cmd_tl_n;",
        block(&nvn, "static uint32_t cmd_slot_take("),
        block(&nvn, "static void cmd_slot_give("),
        block(&nvn, "static cmd_rec *cmd_adopt("),
        block(&nvn, "static cmd_rec *cmd_host("),
        "static volatile int64_t seg_bytes, seg_count;",
        block(&nvn, "static void cmd_release("),
        live_decls,
        block(&nvn, "static void live_handle_add("),
        block(&nvn, "static int live_handle_has("),
        block(&nvn, "static void live_handle_remove("),
        block(&nvn, "static uint64_t texels_extent("),
    ]
    .join("\n");
    let t = Tmp::new("nvn-host-tables");
    let exe = build(&t, &cc(), "fixture.c", &fixture("nvn_host_tables_test.c").replace("@PIECES@", &pieces),
                    &["-std=gnu11", "-O1", "-fno-strict-aliasing", &inc("renderer/api"), &inc("runtime")], &[]);
    run(&mut clean(&exe));
}

/// nvnTextureBuilderSetSwizzle with no builder (the ocarina note sheet's first build passed 0, and the setter wrote
/// guest 0x5c) uses a zeroed scratch builder and logs once; a real builder keeps its swizzle and logs nothing.
#[test]
fn tex_builder_guard() {
    let s = read("runtime/nvn_native.c");
    let fx = fixture("nvn_tex_builder_guard_test.c")
        .replace("@GUARD@", block(&s, "static nvn_tex_builder *tb_or_scratch(void *p, uint64_t lr)"))
        .replace("@TB@", "#define TB(p) tb_or_scratch((p), c->x[30])")
        .replace("@SWIZZLE@", block(&s, "NV(nvnTextureBuilderSetSwizzle)"));
    let t = Tmp::new("nvn-tex-builder");
    let exe = build(&t, &cc(), "fixture.c", &fx, &["-std=c11", "-O2", "-fno-strict-aliasing"], &[]);
    run(&mut clean(&exe));
}

/// nvnDeviceGetTextureHandle with SDK-observed boundary values: Castle Town's guard house passes texture -1 and sampler
/// 264, which the SDK answers with UINT64_MAX; texture 0 gives 0 whatever the sampler; signed ids and 32-bit
/// arguments; no other register changes. 13 cases, one process each.
#[test]
fn texture_handle() {
    let body = block(&read("runtime/nvn_native.c"), "NV(nvnDeviceGetTextureHandle)").to_string();
    let t = Tmp::new("nvn-texture-handle");
    let exe = build(&t, &cc(), "fixture.c", &fixture("nvn_texture_handle_test.c").replace("@BODY@", &body), &["-std=c11", "-O2"], &[]);
    let failed: Vec<String> = (0..13)
        .filter_map(|case| {
            let out = output(clean(&exe).arg(case.to_string()));
            (!out.status.success()).then(|| format!("case {case}: {} {}", out.status, text(&out.stderr).trim()))
        })
        .collect();
    assert!(failed.is_empty(), "{failed:?}");
}

/// The render-target pool's size (nn_applet.c hos_rt_pool_size): a 512 MiB floor (the 305 MiB default crashed in
/// Castle Town's guard house), growth for 4K and ultrawide pictures up to 3840 x 2160 of room, the game's own larger
/// size, and HOS_RT_POOL_MB overrides, below the floor too. 12 cases.
#[test]
fn rt_pool_minimum() {
    let body = block(&read("runtime/nn_applet.c"), "uint64_t hos_rt_pool_size(uint64_t game)").to_string();
    let t = Tmp::new("rt-pool-minimum");
    let exe = build(&t, &cc(), "fixture.c", &fixture("rt_pool_minimum_test.c").replace("@BODY@", &body), &["-std=c11", "-O2"], &[]);
    run(&mut clean(&exe));
}

/// from start to the end of the first end after it
fn until<'a>(text: &'a str, start: &str, end: &str) -> &'a str {
    let i = find(text, start);
    &text[i..i + find(&text[i..], end) + end.len()]
}

/// games/oot/patches/rt_slots.c's slot allocator (HOS_NATIVE_TEXALLOC and the default), its typedefs, place / take /
/// steal and list_bad cut from the source, guest memory as host memory: slots 64 KB aligned inside [lo, hi), never
/// overlapping, a freed slot back to its family; a full range gives no slot, and with a parked entry not asked for in
/// REUSE_AFTER presents the smallest such entry's slot that fits, the entry going to the release queue; (the transient
/// pool) a slot's memory goes to another image only REUSE_AFTER presents after its last use; large slots from the
/// top, small from the bottom; list_bad finds a wrong count, a broken previous link and a cycle.
#[test]
fn rt_slots_native() {
    let src = read("games/oot/patches/rt_slots.c");
    let parts = [
        until(&src, "static _Thread_local int tl_local_shadow;", "\n"),
        until(&src, "#define REUSE_AFTER", "\n"),
        until(&src, "#define BIG_SLOT", "\n"),
        until(&src, "typedef struct {\n    uint64_t lr, name, dta;", "} Cur;"),
        "static Cur cur;",
        until(&src, "typedef struct {\n    uint64_t fam, off, cap, block;", "} Slot;"),
        until(&src, "#define MAX_SLOTS", "static volatile uint64_t frame;"),
        until(&src, "static struct {   /* counters", "} n;"),
        block(&src, "static uint64_t fam_of(const Cur *k) {"),
        until(&src, "typedef struct { Cur key; uint64_t entry, block, frame; } Parked;", "static int nsteal_q;"),
        until(&src, "#define MAX_PARKED", "static int nparked;"),
        block(&src, "static int list_bad(uint64_t sentinel) {"),
        block(&src, "static int by_off(const void *a, const void *b) {"),
        block(&src, "static uint64_t place(uint64_t cap, uint64_t align) {"),
        block(&src, "static Slot *steal(uint64_t need, uint64_t align, uint64_t block) {"),
        block(&src, "static Slot *take(uint64_t fam, uint64_t need, uint64_t align, uint64_t block) {"),
    ]
    .join("\n");
    let cut = Regex::new(r"static int check_on\(void\);[^\n]*\n|static void check_arenas\([^\n]*\n").unwrap().replace_all(&parts, "");
    let t = Tmp::new("rt-slots");
    t.write("cut.inc", &cut);
    let dir = format!("-I{}", t.path().display());
    let exe = build(&t, &cc(), "t.c", &fixture("rt_slots_native_test.c"),
                    &["-std=c11", "-Wall", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", &dir], &[]);
    run(&mut clean(&exe));
}

/// nn::fs reads and writes take guest buffers only, GetFileSize answers 0 for a handle with no size, and
/// ReadDecoderOutputImageNv12 reads nothing when the pool's NV12 allocation failed (its NULL plus an offset was any
/// host address).
#[test]
fn guest_buffers_checked() {
    let fs = read("runtime/nn_fs.c");
    assert!(block(&fs, "static uint64_t read_at(").contains("if (!hos_guest_range(buf, size)) return 0;"),
            "read_at writes into a buffer it did not check");
    assert!(block(&fs, "HLE(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE)").contains("hos_guest_range(ARG(2), ARG(3))"),
            "WriteFile reads a buffer it did not check");
    assert!(block(&fs, "HLE(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE)").contains("sz > 0 ? (uint64_t)sz : 0"),
            "GetFileSize can answer with the host stack's bytes");
    let video = read("runtime/nn_video.c");
    assert!(block(&video, "HLE(_ZN2nn5codec26ReadDecoderOutputImageNv12E").contains("!o->nv12"),
            "ReadDecoderOutputImageNv12 reads a pool whose NV12 was not allocated");
}

/// The memcpy, memmove and memset replacements (nn_libc.c's PATCH_FUNC_SYM, with patch.h as it is, over a 64 KB
/// window with a page after it): inside the guest window the host copies; a range outside it is a fatal stop naming
/// the function, the range and the caller, the page after the window untouched.
#[test]
fn guest_copies() {
    let t = Tmp::new("guest-copies");
    t.write("hos.h", &fixture("guest_copies_hos.h"));
    // (beside the stand-in hos.h: an include in quotes looks in the including file's folder first)
    t.write("nn_libc.c", &read("runtime/nn_libc.c"));
    let dir = format!("-I{}", t.path().display());
    let exe = build(&t, &cc(), "fixture.c", &fixture("guest_copies_test.c"), &["-std=gnu11", "-Wall", "-Wextra", &dir, &inc("runtime")], &[]);
    run(&mut clean(&exe));
    for (k, f) in [(1, "memset"), (2, "memmove"), (3, "memcpy"), (4, "memset")] {
        let out = output(clean(&exe).arg(k.to_string()));
        let so = text(&out.stdout);
        assert!(out.status.code() == Some(7) && so.starts_with(&format!("{f}: ")) && so.contains("outside the guest window (lr 0x80001234)"),
                "case {k}: {} {so:?}", out.status);
    }
}
