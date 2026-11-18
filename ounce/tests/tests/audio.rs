//! runtime/nn_audio.c and runtime/media/hos_sdl.c: the mixer's deadline, the output section, the voices' biquad filters
//! and wave buffers, compiled from the source as it is around authored inputs; which SDL the sound loads.

use ounce_tests::*;

const CONSTANTS: &[&str] =
    &["FRAME", "RATE", "AU_MAGIC", "MAX_VOICES", "MAX_MIXES", "MAX_SPLITTERS", "MAX_SPLIT_DESTS", "MAX_CH", "MAX_BUF", "MAX_WB", "MAX_REL"];

/// A stall under a few seconds is mixed out, so the sound catches the picture (a wake more than 100 ms late used to
/// move the deadline to now: those frames were never mixed, and the sound stayed behind by that much).
#[test]
fn deadline() {
    let source = read("runtime/nn_audio.c");
    let main = between(&source, "static void renderer_main", "/* ---- renderer lifecycle");
    assert!(main.contains("audio_mix_deadline(") && !main.contains("100000000"), "the mixer still drops the frames it woke late for");
    let t = Tmp::new("audio-deadline");
    let cpp = t.write("fixture.cpp", &fixture("audio_deadline.cpp").replace("@DEADLINE@", block(&source, "int64_t audio_mix_deadline(")));
    let exe = compile(&cxx(), &["-std=c++20", "-Wall", "-Wextra", "-Werror", cpp.to_str().unwrap()], &t.join("fixture"));
    run(&mut clean(&exe));
}

/// The real output section with a synthetic SDL device that the test pulls from, without opening a device.
#[test]
fn output_errors() {
    let source = read("runtime/nn_audio.c");
    let actual = format!("{}\n{}", defines(&source, &["FRAME", "RATE"]), between(&source, "#define NBLOCKS", "/* ---- parameter updates"));
    let t = Tmp::new("audio-output");
    t.write("audio_output.inc", &actual);
    let exe = compile(
        &cc(),
        &["-std=c11", "-Wall", "-Wextra", "-Werror", &format!("-I{}", t.path().display()), &inc("runtime/os"),
          fixture_path("audio_output_errors_test.c").to_str().unwrap()],
        &t.join("fixture"),
    );
    for case in ["normal", "busy", "partial", "behind", "open", "stall", "surround", "fallback"] {
        run(clean(&exe).arg(case));
    }
}

/// the voices' types, handles and queued updates as nn_audio.c has them, with what the fixture stubs
fn voice_parts(source: &str, stubs: &str, blocks: &[&str]) -> Vec<String> {
    let mut actual = vec![
        defines(source, CONSTANTS),
        between(source, "enum { K_VOICE", "static os_mutex lock = OS_MUTEX_INIT;").to_string(),
        format!(
            "\nstatic Voice voices[MAX_VOICES];\nstatic Mix mixes[MAX_MIXES];\nstatic Splitter splitters[MAX_SPLITTERS];\nstatic int sink_mix = -1;\n{stubs}"
        ),
    ];
    actual.extend(blocks.iter().map(|m| block(source, m).to_string()));
    actual.push(between(source, "enum { OP_VOICE_RELEASE", "static int64_t last_update_ns;").to_string());
    actual.extend(["static void apply(", "static void submit(", "static void commit("].iter().map(|m| block(source, m).to_string()));
    actual
}

fn build_voice_fixture(name: &str, inc_name: &str, actual: &[String], test_c: &str, extra: &[&str]) {
    let t = Tmp::new(name);
    t.write(inc_name, &actual.join("\n\n"));
    let mut args = vec!["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"];
    args.extend(extra);
    let i = format!("-I{}", t.path().display());
    let src = fixture_path(test_c);
    args.extend([i.as_str(), src.to_str().unwrap(), "-lm"]);
    run(&mut clean(&compile(&cc(), &args, &t.join("fixture"))));
}

/// The voice-filter setters, queued updates and the render_voice biquad loop, on authored inputs and sample buffers.
#[test]
fn voice_biquad() {
    const FLOAT: &str = "_ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_22TBiquadFilterParameterIfEE";
    let source = read("runtime/nn_audio.c");
    let mut actual = voice_parts(
        &source,
        "static void release_wb(Voice *voice, uint64_t buffer) { (void)voice; (void)buffer; }\n",
        &["static void put_handle(", "static int get_handle(", "static int voice_index(", "static Voice *voice_of("],
    );
    actual.push(block(&source, &format!("HLE({FLOAT})")).to_string());
    let render = block(&source, "static void render_voice(");
    actual.push(format!("static void filter_samples(Voice *v, float out[MAX_CH][FRAME]) {{\n{}\n}}", block(render, "for (int f = 0; f < 2; f++)")));
    build_voice_fixture("audio-voice-biquad", "actual_audio_biquad.inc", &actual, "audio_voice_biquad_test.c", &[]);
}

/// The wave-buffer calls, queued updates and the mixer's buffer consumption around an authored pass. Ocarina's sound
/// thread feeds six mono voices one after another, reading each one's GetWaveBufferCount and appending when one is
/// short: a buffer the mixer finished between two of those reads made a voice look short, one more buffer a pass,
/// until its queue was full and that speaker went silent. A finished buffer reaches the game at the next
/// RequestUpdateAudioRenderer, as on the console, never in between.
#[test]
fn wave_buffer_release() {
    let source = read("runtime/nn_audio.c");
    let mut actual = voice_parts(
        &source,
        "/* (the samples are not looked at here: only which buffer the mixer is on) */\n\
         static void source_frame(Voice *v, const WB *w, int64_t s, float *out) { (void)v; (void)w; (void)s; out[0] = 0; }\n\
         static void adpcm_seek(Voice *v, const WB *w) { (void)v; (void)w; }\n\
         static int16_t adpcm_sample(Voice *v, const WB *w, int64_t s) { (void)v; (void)w; (void)s; return 0; }\n",
        &["static void put_handle(", "static int get_handle(", "static int voice_index(", "static Voice *voice_of(",
          "static WB read_wb(", "static void release_wb(", "static int next_source_frame("],
    );
    for name in [
        "_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE",
        "_ZN2nn5audio21GetReleasedWaveBufferEPNS0_9VoiceTypeE",
        "_ZN2nn5audio18GetWaveBufferCountEPKNS0_9VoiceTypeE",
        "_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE",
    ] {
        actual.push(block(&source, &format!("HLE({name})")).to_string());
    }
    build_voice_fixture("audio-wave-buffers", "actual_audio_wave_buffers.inc", &actual, "audio_wave_buffer_release_test.c",
                        &["-Wno-unused-function"]);
}

/// The sound, the window and the controllers load one SDL, the one beside the executable before the system's (SDL3
/// before SDL2), and the device's buffer is the mixer's frame.
#[test]
fn sdl_library() {
    let sdl = read("runtime/media/hos_sdl.c");
    let load = between(&sdl, "static os_lib *load_one", "\n}");
    assert!(find(&load, "os_exe_path") < find(&load, "os_lib_open(name)"), "the bundled SDL first");
    let run_ = between(&sdl, "static void load_run(void *arg)", "\n}");
    assert!(find(&run_, "load_one(\"libSDL3.so.0\")") < find(&run_, "load_one(\"libSDL2-2.0.so.0\")"), "SDL3 before SDL2");
    let audio = read("runtime/nn_audio.c");
    let out = between(&audio, "static int output_try(", "\n}");
    assert!(out.contains("hos_sdl_audio_open(") && !out.contains("os_lib_open"), "the sound takes the same SDL");
    let open = between(&sdl, "int hos_sdl_audio_open(", "\n}");
    for want in [
        "const char *env_samples = getenv(\"SDL_AUDIO_DEVICE_SAMPLE_FRAMES\");",
        "if (version == 3 && !env_samples) hos_sdl_hint(\"SDL_AUDIO_DEVICE_SAMPLE_FRAMES\", \"480\");",
        "want.samples = (uint16_t)(env_samples && atoi(env_samples) > 0 ? atoi(env_samples) : 512);",
    ] {
        assert!(open.contains(want), "hos_sdl_audio_open has no {want}");
    }
}
