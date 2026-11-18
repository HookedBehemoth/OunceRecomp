//! The frame's time: runtime/nn_vi.c's frame-rate cap and present tracing, games/oot/patches/frame_rate.c's real-time clock, runtime/bench.h,
//! hos_main.c's widescreen sizes and memory notice, dlss_math.h, and the game's patches that step by the frame's time
//! (games/oot/patches: frame_whole.h, oot_probes.c's At rows as the lifter scans them), compiled from the source as it
//! is around authored clocks and registers.

use ounce_tests::*;

fn build(t: &Tmp, name: &str, src: &str, args: &[&str]) -> std::path::PathBuf {
    let c = t.write(name, src);
    let mut a: Vec<String> = args.iter().map(|s| s.to_string()).collect();
    a.push(c.display().to_string());
    a.push("-lm".into());
    let compiler = if name.ends_with(".cpp") { cxx() } else { cc() };
    compile(&compiler, &a, &t.join("fixture"))
}

/// from marker to the end of the first "\n}\n" after it
fn body<'a>(text: &'a str, marker: &str) -> &'a str {
    let s = find(text, marker);
    &text[s..s + find(&text[s..], "\n}\n") + 3]
}

/// HOS_BENCH's ready condition (frames with every part settled in a row, again from any frame with one not) and its
/// summary (median and p99 as the frame-time line takes them, the input left as it was; the game steps' violations).
#[test]
fn bench_ready_and_summary() {
    let t = Tmp::new("bench");
    let exe = build(&t, "t.c", &fixture("bench.c"), &["-std=gnu11", "-Wall", "-O1", &inc("runtime")]);
    run(&mut clean(&exe));
}

/// HOS_BENCH in the source: it answers the menu's game speed "per frame" (rx_option 0), the real-time step keeps the
/// whole 1/60 s step with no frame seconds at any cap, and the window's acquire waits for nothing.
#[test]
fn bench_fixed_step_and_unpaced() {
    let vi = read("runtime/nn_vi.c");
    let oot = read("games/oot/patches/frame_rate.c");
    let step = body(&oot, "void game_frame_timing(int64_t now) {");
    let branch = &step[find(step, "if ((!rx_option(0, 1) && hos_fps_max() <= 60) || hos_bench_on())")..];
    let branch = &branch[..find(branch, "return;")];
    assert!(branch.contains("__atomic_store_n(&hos_frame_seconds_bits, 0, __ATOMIC_RELAXED);"), "no frame seconds");
    assert!(branch.contains("*slot = 60;"), "the nominal rate: a whole 1/60 s step");
    assert!(body(&vi, "void hos_vi_native_acquire(int active) {").contains("no_pacing = getenv(\"HOS_NO_PACING\") != NULL || hos_bench_on();"));
    assert!(body(&vi, "void hos_vi_native_presented(void) {").contains("if (hos_bench_on()) hos_bench_present(now);"));
    let option = body(&read("runtime/rx_backend.c"), "int rx_option(int id, int otherwise) {").to_string();
    assert!(option.contains("|| hos_bench_on();") && option.contains("if (id == 0 && per_frame) return 0;"));
}

/// Frame-time jitter does not make the real-time game clock run fast (realtime_rate: the debt kept and bounded, high
/// caps, a long stall dropped).
#[test]
fn realtime_rate() {
    let oot = read("games/oot/patches/frame_rate.c");
    assert!(block(&oot, "void game_frame_timing(").contains("realtime_rate("), "the present path does not use the tested clock quantizer");
    let t = Tmp::new("realtime-rate");
    let exe = build(&t, "fixture.c", &fixture("realtime_rate.c").replace("@RATE@", block(&oot, "static int realtime_rate(")),
                    &["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]);
    run(&mut clean(&exe));
}

/// HOS_PRESENT_TRACE: the returned frames written, an 8 s stall kept; unset or empty writes nothing; pacing, counters
/// and clock reads are the same either way.
#[test]
fn present_trace() {
    let vi = read("runtime/nn_vi.c");
    let src = fixture("present_trace.cpp")
        .replace("@HELPER@", block_top(&vi, "static void present_trace("))
        .replace("@PRESENTED@", block_top(&vi, "void hos_vi_native_presented("));
    let t = Tmp::new("present-trace");
    let exe = build(&t, "fixture.cpp", &src, &["-std=c++17", "-Wall", "-Wextra", "-Werror"]);
    let times: [i64; 5] = [1000000000, 1016666667, 1066666667, 9066666667, 9083333334];
    let mut states = Vec::new();
    for mode in ["enabled", "disabled", "empty"] {
        let csv = t.join(&format!("{mode}.csv"));
        let mut c = clean(&exe);
        c.args([mode, csv.to_str().unwrap()]).current_dir(t.path());
        match mode {
            "enabled" => c.env("HOS_PRESENT_TRACE", &csv),
            "empty" => c.env("HOS_PRESENT_TRACE", ""),
            _ => &mut c,
        };
        states.push(text(&run(&mut c).stdout));
        if mode == "enabled" {
            let file = std::fs::read_to_string(&csv).unwrap();
            let mut lines = file.lines();
            let head: Vec<&str> = lines.next().unwrap().split(',').collect();
            let col = |name: &str| head.iter().position(|h| *h == name).unwrap_or_else(|| panic!("no column {name}"));
            let rows: Vec<Vec<i64>> = lines.map(|l| l.split(',').map(|v| v.trim().parse().unwrap()).collect()).collect();
            let of = |name: &str| rows.iter().map(|r| r[col(name)]).collect::<Vec<i64>>();
            assert_eq!(of("frame"), [1, 2, 3, 4, 5]);
            assert_eq!(of("return_ns"), times);
            assert_eq!(of("interval_ns"), [0, 16666667, 50000000, 8000000000, 16666667]);
            assert_eq!(of("interval_ns").iter().sum::<i64>(), times[4] - times[0]);
        } else {
            assert!(!csv.exists(), "disabled trace created a file");
        }
    }
    assert!(states[0] == states[1] && states[1] == states[2], "tracing changed pacing/counters/clock calls");
}

/// Counters that add the frame step, or the step times a whole number, cut to whole frames keep counting above 60 fps
/// (frame_whole.h): each counter keeps its own remainder (144 frames of 60/144 are 60, forty water surfaces each
/// count their 60), NaN and amounts below zero give 0, and without the carry the amount is cut as the game cuts it.
#[test]
fn frame_whole() {
    let t = Tmp::new("frame-whole");
    let exe = build(&t, "t.c", &fixture("frame_whole.c"), &["-std=gnu11", "-Wall", "-O1", &inc("games/oot/patches")]);
    run(&mut clean(&exe));
}

/// hos_main.c's widescreen sizes: the picture, scene, camera ratio and HUD for 4:3, 16:10, 5:4 and narrower, 21:9 and
/// 32:9 screens, and the game's own picture for 16:9, a square screen and Ultrawide off.
#[test]
fn widescreen_shapes() {
    let src = read("runtime/hos_main.c");
    let start = find(&src, "        const double game = 16.0 / 9.0;");
    let log = start + find(&src[start..], "hos_log(\"boot: widescreen: a %.4f screen");
    let end = log + find(&src[log..], "\n");
    let sizes = format!("{}\n        }}\n", &src[start..end]);
    let t = Tmp::new("widescreen");
    let exe = build(&t, "fixture.c", &fixture("widescreen_shapes.c").replace("@SIZES@", &sizes), &["-std=c11"]);
    let cases = [
        ("1.3333", 1440, "1920x1440 1440x1080 1.3333 1.0000 0.0000"), // 4:3 (2048 x 1536, 1600 x 1200 CRTs)
        ("1.6", 1440, "2304x1440 1728x1080 1.6000 1.0000 0.0000"),    // 16:10
        ("1.25", 1440, "1800x1440 1352x1080 1.2500 1.0000 0.0000"),   // 5:4
        ("1.0", 1440, "0x0 0x0 0.0000 0.0000 0.0000"),                // square: no ratio from the screen
        ("1.1", 1440, "1800x1440 1352x1080 1.2500 1.0000 0.0000"),    // narrower than 5:4: kept at 5:4
        ("1.3333", 2160, "2880x2160 2160x1620 1.3333 1.0000 0.0000"), // 4:3 at the 4K output
        ("1.7778", 1440, "0x0 0x0 0.0000 0.0000 0.0000"),             // 16:9: the game's own picture
        ("2.3889", 1440, "3440x1440 2584x1080 2.3889 0.7442 0.1279"), // 21:9 (3440 x 1440)
        ("3.5556", 1440, "5120x1440 3840x1080 3.5556 0.5000 0.2500"), // 32:9
        ("0", 1440, "0x0 0x0 0.0000 0.0000 0.0000"),                  // Ultrawide off
    ];
    let mut bad = Vec::new();
    for (want, height, expect) in cases {
        let got = text(&run(clean(&exe).args([want, &height.to_string()])).stdout).trim().to_string();
        if got != expect {
            bad.push(format!("ratio {want} at {height}: {got}, expected {expect}"));
        }
    }
    assert!(bad.is_empty(), "{}", bad.join("\n"));
}

/// The game says at boot when memory and swap are below about 10 GB (hos_main.c memory_notice): the threshold at a few
/// sizes, the boot line with this PC's numbers, no message box with HOS_HEADLESS, and the call made at boot.
#[test]
fn low_memory_notice() {
    let src = read("runtime/hos_main.c");
    let boot = find(&src, "hos_log(\"boot: image %zu MB");
    let call = src[boot..].find("memory_notice();").map(|i| i + boot);
    assert!(call.is_some_and(|c| c < boot + find(&src[boot..], "hos_mods_init(image);")), "the game does not check its memory at boot");
    assert!(src.contains("os_mem_available("), "hos_main.c does not read the machine's memory (os_mem_available)");
    let once = |m: &str| {
        assert_eq!(src.matches(m).count(), 1, "marker not found once: {m}");
        block(&src, m)
    };
    let define = src.lines().find(|l| l.starts_with("#define HOS_MEMORY_NEEDED_MB")).expect("no HOS_MEMORY_NEEDED_MB");
    let code = [define, once("static int memory_notice_text("), once("static void memory_notice(void) {")].join("\n");
    let t = Tmp::new("low-memory");
    let exe = build(&t, "fixture.c", &fixture("low_memory_notice.c").replace("@CODE@", &code), &["-std=gnu11", "-O1"]);
    let out = text(&run(&mut clean(&exe)).stdout);
    assert!(out.contains("boxes 0") && out.contains("MB of swap"), "memory_notice:\n{out}");
}

/// NVIDIA DLSS's camera math (runtime/dlss_math.h): the jitter's Halton sequence, the reprojection that makes the
/// motion vectors, the jitter taken off the motion, the jitter in DLSS's pixels and which projections get it; the
/// header also built as C, as rx_backend.c includes it.
#[test]
fn dlss_math() {
    let t = Tmp::new("dlss-math");
    let c = build(&t, "c.c", "#include \"dlss_math.h\"\nint main(void) { float j[2]; dlss_jitter_pixels(3, 16, j); return j[0] < 0.5f ? 0 : 1; }\n",
                  &["-std=c11", "-Wall", "-Wextra", "-Werror", &inc("runtime")]);
    run(&mut clean(&c));
    let exe = compile(&cxx(), &["-std=c++20".to_string(), "-Wall".into(), "-Wextra".into(), "-Werror".into(), inc("runtime"),
                               fixture_path("dlss_math_test.cpp").display().to_string()], &t.join("dlss-math"));
    run(&mut clean(&exe));
}

/// A frame-rate cap above 60 steps the game by the frames' own time: the two At probes after the engine's step divides
/// (oot_probes.c hos_oot_frame_step, as the lifter scans them) take the time nn_vi.c realtime_step publishes while the
/// cap is above 60; at 60 the bits stay 0 (the lifted code as shipped, a long unpaced stretch repaying bounded debt);
/// HOS_FPS_STEP=0 raises the engine's rate to the cap instead.
#[test]
fn frame_rate_cap_step() {
    let sites: Vec<_> = scan().at.iter().filter(|r| r.fname.starts_with("hos_oot_frame_step")).collect();
    let mut pcs: Vec<u64> = sites.iter().map(|r| r.pc).collect();
    pcs.sort();
    assert_eq!(pcs, [0x80706cc4, 0x83b6eaf8], "the two step divides, one probe each");
    let at = |pc: u64| sites.iter().find(|r| r.pc == pc).unwrap();
    let (a, b) = (at(0x80706cc4), at(0x83b6eaf8));
    assert_eq!((a.word, a.fname.as_str(), a.args.clone(), a.result.as_deref()), (0x1e201828, "hos_oot_frame_step_probe", vec!["s1".to_string(), "s8".into(), "x19".into()], Some("s8")),
               "the frame's step: s8 := hos_oot_frame_step_probe(s1, s8, x19) after fdiv s8, s1, s0");
    assert_eq!((b.word, b.fname.as_str(), b.args.clone(), b.result.as_deref()), (0x1e201820, "hos_oot_frame_step", vec!["s1".to_string(), "s0".into()], Some("s0")),
               "the global step: s0 := hos_oot_frame_step(s1, s0) after fdiv s0, s1, s0");
    let oot = read("games/oot/patches/oot_probes.c");
    let definition = |m: &str| {
        let s = find(&oot, m);
        let line = &oot[s..s + find(&oot[s..], "\n")];
        if line.trim_end().ends_with('}') { line.to_string() } else { block_top(&oot, m).to_string() }
    };
    let hooks = ["PATCH_AT(0x83b6eaf8, word(0x1e201820), after, s0, hos_oot_frame_step,",
                 "PATCH_AT(0x80706cc4, word(0x1e201828), after, s8, hos_oot_frame_step_probe,"]
        .map(definition)
        .join("\n");
    let vi = read("runtime/nn_vi.c");
    let frame = read("games/oot/patches/frame_rate.c");
    let host = [block_top(&vi, "static int fps_max(void) {"), "int hos_fps_max(void) { return fps_max(); }",
                block_top(&frame, "static float text_scale("), block_top(&frame, "static int realtime_rate("),
                block_top(&frame, "void game_frame_timing(")]
        .join("\n");
    let t = Tmp::new("cap-step");
    let exe = build(&t, "fixture.c", &fixture("frame_rate_cap_step.c").replace("@HOST@", &host).replace("@PROBES@", &hooks),
                    &["-std=c11", "-Wall", "-Wextra", "-Werror", &inc("runtime")]);
    for (mode, cap, step) in [("cap 120", Some("120"), None), ("cap 60", None, None), ("step off", Some("120"), Some("0"))] {
        let mut c = clean(&exe);
        c.arg(mode);
        if let Some(v) = cap {
            c.env("HOS_FPS_MAX", v);
        }
        if let Some(v) = step {
            c.env("HOS_FPS_STEP", v);
        }
        run(&mut c);
    }
}

/// Above a 60 cap a movie's frame count grows by the frames' time, so a cutscene's cues stay with its movie: the At
/// probe after the player update's add (oot_probes.c hos_oot_movie_count) makes the step nn_vi.c hos_movie_frames,
/// 1 with no frame time (a cap of 60, or HOS_FPS_MOVIE=0) and the frames' time in 60ths otherwise, each player its own
/// carry, any number of players. The game's own step fails the same checks (they prove something).
#[test]
fn movie_frame_count() {
    const LOAD_ADD: &str = "    c->x[8]=recomp_load32(c,c->x[20] + 760);\n    { uint64_t _a=c->x[8], _b=1ULL; uint64_t _r=_a+_b; _r&=0xFFFFFFFFULL; c->x[8]=_r; }\n";
    const STORE: &str = "    recomp_store32(c,c->x[20] + 760,c->x[8]);\n";
    let rows = rows_of("hos_oot_movie_count");
    assert_eq!(rows.len(), 1, "one probe");
    let p = rows[0];
    assert_eq!((p.pc, p.word, p.args.clone(), p.result.as_deref()), (0x83b7c7c8, 0x11000508, vec!["w8".to_string(), "x20".into()], Some("w8")),
               "the probe is w8 := hos_oot_movie_count(w8, x20) after the update's add w8, w8, #1");
    let oot = read("games/oot/patches/oot_probes.c");
    let s = find(&oot, "PATCH_AT(0x83b7c7c8, word(0x11000508), after, w8, hos_oot_movie_count,");
    let count = &oot[s..s + find(&oot[s..], "\n")];
    let vi = read("games/oot/patches/frame_rate.c");
    let host = format!("{}\n{}", block_top(&vi, "static uint32_t movie_log("), block_top(&vi, "uint32_t hos_movie_frames(uint64_t player) {"));
    let lifted = format!("{LOAD_ADD}    c->x[8] = hos_oot_movie_count((uint32_t)c->x[8], c->x[20]);\n{STORE}");
    let t = Tmp::new("movie-count");
    let flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", &inc("runtime")];
    let exe = build(&t, "fixture.c", &fixture("movie_frame_count.c").replace("@HOST@", &format!("{host}\n{count}")).replace("@LIFTED@", &lifted), &flags);
    run(&mut clean(&exe));
    run(clean(&exe).env("HOS_FPS_MOVIE", "1")); // (read by its value: 1 is on)
    run(clean(&exe).arg("off").env("HOS_FPS_MOVIE", "0"));
    let r = output(clean(&exe).arg("off"));
    assert!(!r.status.success() && text(&r.stderr).contains("FAIL:"), "the count is the game's own without HOS_FPS_MOVIE=0");
    // (the negative control: the game's own step of one an update fails the same checks at 120 fps)
    let t2 = Tmp::new("movie-count-plain");
    let plain = build(&t2, "plain.c", &fixture("movie_frame_count.c").replace("@HOST@", &host).replace("@LIFTED@", &format!("{LOAD_ADD}{STORE}")),
                      &[&flags[..], &["-Wno-unused-function"]].concat());
    let r = output(&mut clean(&plain));
    assert!(!r.status.success() && text(&r.stderr).contains("FAIL:"), "the unpatched count passes the checks: they prove nothing");
}

/// The save picture's viewport row (oot_probes.c hos_oot_wide_save_picture, its At row passing w1-w4 at the viewport
/// helper's site): past 16:9 the 543 x 306 SetViewport widens, centred, so the middle 16:9 fills the texture; narrower
/// it grows taller, centred, so the middle band fills it at its own shape; every other viewport, and 16:9, as the game
/// set them.
#[test]
fn oot_wide_save_picture() {
    let rows = rows_of("hos_oot_wide_save_picture");
    assert!(rows.len() == 1 && rows[0].pc == 0x802a4824 && rows[0].word == 0xf9400108, "{rows:?}");
    assert_eq!(rows[0].io, ["w1", "w2", "w3", "w4"]);
    let src = read("games/oot/patches/oot_probes.c");
    let s = find(&src, "PATCH_AT(0x802a4824, word(0xf9400108), after, none, hos_oot_wide_save_picture, io(w1, w2, w3, w4))");
    let site = &src[s..s + find(&src[s..], "\n}\n") + 3];
    let t = Tmp::new("oot-wide-save-picture");
    let exe = build(&t, "fixture.c", &fixture("oot_wide_save_picture.c").replace("@SITE@", site), &["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", &inc("runtime")]);
    run(&mut clean(&exe));
}
