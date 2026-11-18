//! runtime/nn_hid.c and runtime/media (pad_sdl.c, hd_rumble.h, hos_motion.h, display_sdl.c), and the game's mouse
//! camera (games/oot/patches/mouse_native.c): the controls' bindings, the rumble, the sticks, the motion sensors,
//! compiled from the source as it is around fake key states, pads and SDL.

use ounce_tests::*;
use regex::Regex;

/// a function's definition (not a declaration): name, its parameters, its body
fn function(text: &str, name: &str) -> String {
    let re = Regex::new(&format!(r"{}\([^;{{]*\)\s*\{{", regex::escape(name))).unwrap();
    let m = re.find(text).unwrap_or_else(|| panic!("no definition of {name}"));
    block(&text[m.start()..], name).to_string()
}

/// a block, with the `;` and the line end that close it (a table's initialiser, a statement)
fn block_stmt<'a>(text: &'a str, marker: &str) -> &'a str {
    let start = find(text, marker);
    let b = text.as_bytes();
    let mut end = start + block(&text[start..], marker).len();
    while matches!(b[end], b';' | b'\n') {
        end += 1;
        if b[end - 1] == b'\n' {
            break;
        }
    }
    &text[start..end]
}

/// from the line that holds first to the end of the block or statement that starts at last
fn lines_to_block<'a>(text: &'a str, first: &str, last: &str) -> String {
    let start = text[..find(text, first)].rfind('\n').unwrap() + 1;
    format!("{}{}", &text[start..find(text, last)], block_stmt(text, last))
}

fn build_run(t: &Tmp, src: &str, args: &[&str]) {
    let c = t.write("fixture.c", src);
    let mut a: Vec<String> = args.iter().map(|s| s.to_string()).collect();
    a.push(c.display().to_string());
    a.push("-lm".into());
    run(&mut clean(&compile(&cc(), &a, &t.join("fixture"))));
}

/// Alt+Enter toggles fullscreen and presses no button (Enter is Plus by default, and any button can be bound to it):
/// nn_hid.c's kmap_down against fake key states, and display_sdl.c toggling on Alt+Enter.
#[test]
fn alt_enter() {
    assert!(read("runtime/media/display_sdl.c").contains("scancode == 40 && (mod & 0x300)"), "display_sdl.c: Alt+Enter no longer toggles fullscreen");
    let hid = read("runtime/nn_hid.c");
    assert!(hid.contains("kmap_down(key_bind[i][k], key)"), "nn_hid.c: the bound keys no longer go through kmap_down");
    let t = Tmp::new("alt-enter");
    build_run(&t, &fixture("alt_enter.c").replace("@KMAP_DOWN@", block(&hid, "static int kmap_down(")), &["-std=c11", "-Wall", "-Wextra", "-Werror"]);
}

/// The keys-only controls (the left stick's and the camera's directions) and the mouse's buttons are bindings like the
/// Switch's buttons: nn_hid.c's binding table and bound_down against fake key states and a fake ounce_options.ini (the
/// defaults, a mouse button only while captured and once the capturing click was let go, key_move_up from the file, a
/// pad_ line for a keys-only control ignored, a key taken from the control that had it, Enter with Alt no key).
#[test]
fn bound_keys() {
    let hid = read("runtime/nn_hid.c");
    let code = [
        block(&hid, "static int kmap_down(").to_string(),
        lines_to_block(&hid, "static const char *const bind_names[HOS_KEY_N]", "void hos_bind("),
        lines_to_block(&hid, "static int mouse_vk_slot(", "static int bound_down("),
    ]
    .join("\n");
    let read_input = block(&hid, "static void read_input(");
    for needle in ["bound_down(HOS_KEY_MOVE_UP,", "bound_down(HOS_KEY_CAM_RIGHT,", "mouse_buttons_arm(", "bound_down(i, 1)"] {
        assert!(read_input.contains(needle), "read_input does not read the bound keys: {needle}");
    }
    assert!(!read_input.contains("key('W')"), "read_input still reads WASD itself");
    let t = Tmp::new("bound-keys");
    t.write("ounce_options.ini", "key_move_up=90,0\npad_move_up=0\n");
    let c = t.write("t.c", &fixture("bound_keys.c").replace("@CODE@", &code));
    let exe = compile(
        &cc(),
        &["-std=gnu11".to_string(), "-Wall".into(), "-Wno-unused-function".into(), inc("renderer/api"), inc("renderer"),
          inc("runtime/os"), inc("runtime"), inc("runtime/media"), c.display().to_string()],
        &t.join("t"),
    );
    run(clean(&exe).arg(t.path()));
}

/// HD rumble's encoder (runtime/media/hd_rumble.h): the game's VibrationValue as the 4 bytes a Switch controller plays a
/// side, against rows of the community's documented tables (dekuNukem's rumble_data_table.md, Switchbrew's HID
/// "Rumble data"); the felt amplitudes, a single Joy-Con's merge, the report SDL's Switch driver takes, which pads, the
/// cadence.
#[test]
fn hd_rumble_encoder() {
    let t = Tmp::new("hd-rumble");
    build_run(&t, &fixture("hd_rumble_encode.c"), &["-std=c11", "-Wall", "-Wextra", "-Werror", &inc("runtime/media")]);
}

/// pad_sdl.c's hd_open and hd_poll on a fake SDL: a pad sent its packet at a steady cadence, silence not sent again, and
/// a pad whose SDL refuses the packet falls back to the motors for good.
#[test]
fn hd_rumble_poll() {
    let pad = read("runtime/media/pad_sdl.c");
    let pads = Regex::new(r"(?s)static struct \{.*?\} pads\[MAX_PADS\];").unwrap().find(&pad).expect("no pads table in pad_sdl.c");
    let src = fixture("hd_rumble_poll.c")
        .replace("@PADS@", pads.as_str())
        .replace("@OPEN@", &function(&pad, "static void hd_open"))
        .replace("@POLL@", &function(&pad, "static int hd_poll"));
    let t = Tmp::new("hd-rumble-poll");
    build_run(&t, &src, &["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-D_POSIX_C_SOURCE=200809L", &inc("runtime/media")]);
}

/// The poll sends the felt, strength-scaled encoding (a single Joy-Con both sides merged) before the motor path, and
/// HOS_HD_RUMBLE and the game's vibration reach it.
#[test]
fn hd_rumble_wired_in() {
    let pad = read("runtime/media/pad_sdl.c");
    let poll = function(&pad, "static void pads_poll");
    for want in ["hd_open(slot, gc)", "hd_rumble_felt(hv[0], felt[0])", "hd_rumble_felt(hv[1], felt[1])",
                 "hd_rumble_encode(felt[0], strength, side[0])", "hd_rumble_encode(felt[1], strength, side[1])",
                 "hd_rumble_merge(felt[0], felt[1], felt[2])", "hd_rumble_encode(felt[2], strength, side[2])"] {
        assert!(poll.contains(want), "pads_poll has no {want}");
    }
    assert!(find(&poll, "hd_rumble_felt(hv[0]") < find(&poll, "hd_rumble_encode(felt[0]"));
    assert!(find(&poll, "if (hd_poll(i, side, now)) continue;") < find(&poll, "rumble_due("));
    assert!(function(&pad, "static void hd_open").contains("getenv(\"HOS_HD_RUMBLE\")"));
    assert!(function(&read("runtime/nn_hid.c"), "static void vibration_send").contains("hos_sdl_rumble_hd(dev, v);"));
}

/// runtime/media/hos_motion.h with synthetic SDL readings: the Switch six-axis state it makes.
#[test]
fn motion() {
    let t = Tmp::new("motion");
    let exe = compile(
        &cc(),
        &["-std=c11".to_string(), "-Wall".into(), "-Wextra".into(), "-Werror".into(), inc("runtime/media"),
          fixture_path("motion_test.c").display().to_string(), "-lm".into()],
        &t.join("fixture"),
    );
    run(&mut clean(&exe));
}

/// Native mouse look turns the player camera's own angles (games/oot/patches/mouse_native.c, compiled alone against a
/// fake guest memory holding one camera): until the camera's update has run the motion is the stick's; then yaw and
/// pitch turn 0.1 degrees a count, pitch within its limits, yaw wrapped, Invert mouse vertical swaps; off without the
/// menu's Mouse camera or on a non-camera; 150 ms without the camera gives the motion back; only camera states 3 and 4.
#[test]
fn mouse_native() {
    let t = Tmp::new("mouse-native");
    // (the stand-in hos.h beside the patch, so its #include "hos.h" finds that one)
    t.write("hos.h", &fixture("mouse_native_hos.h"));
    t.write("mouse_native.c", &read("games/oot/patches/mouse_native.c"));
    t.write("patch.h", &read("runtime/patch.h"));
    let c = t.write("t.c", &fixture("mouse_native.c"));
    let exe = compile(
        &cc(),
        &["-std=gnu11".to_string(), "-Wall".into(), "-O1".into(), "-g".into(), format!("-I{}", t.path().display()),
          inc("runtime/os"), c.display().to_string(), t.join("mouse_native.c").display().to_string(), "-lm".into()],
        &t.join("t"),
    );
    run(&mut clean(&exe));
}

/// The poll scales both motors by the menu's Rumble strength (option 9), heavy first, before deciding to send, and the
/// game's vibration is mapped by rumble_map.
#[test]
fn rumble_strength_wired_in() {
    let ws = Regex::new(r"\s+").unwrap();
    let poll = ws.replace_all(&function(&read("runtime/media/pad_sdl.c"), "static void pads_poll"), " ").into_owned();
    assert!(Regex::new(r"\brx_option\(\s*9\s*,\s*100\s*\)").unwrap().is_match(&poll));
    let call = Regex::new(r"\brumble_strength\(([^;]*)\);").unwrap().captures(&poll).expect("the poll does not scale the rumble by the menu's strength");
    assert!(Regex::new(r"rumble_low\b.*rumble_high\b").unwrap().is_match(&call[1]), "heavy first, light second");
    assert!(call.get(0).unwrap().start() < find(&poll, "rumble_due("), "Off is a change: a stop is sent");
    let send = ws.replace_all(&function(&read("runtime/nn_hid.c"), "static void vibration_send"), " ").into_owned();
    assert!(Regex::new(r"\brumble_map\( ?v ?, ?amp\[dev\] ?\)").unwrap().is_match(&send));
}

/// The game's HD rumble to a controller's heavy and light motors by frequency (nn_hid.c rumble_map: all heavy up to 160
/// Hz, all light from 200 Hz, a constant-power log-scale crossfade between; at the default and OoT's frequencies as
/// before), scaled by the menu's Rumble strength (pad_sdl.c rumble_strength: above 100% both motors by one factor).
#[test]
fn rumble_mapping() {
    let hid = read("runtime/nn_hid.c");
    let pad = read("runtime/media/pad_sdl.c");
    let defs: Vec<&str> = Regex::new(r"(?m)^#define RUMBLE_\w* .*$").unwrap().find_iter(&hid).map(|m| m.as_str()).collect();
    let src = fixture("rumble_mapping.c")
        .replace("@DEFINES@", &defs.join("\n"))
        .replace("@MOTOR@", &function(&hid, "static float rumble_motor"))
        .replace("@SHARE@", &function(&hid, "static float rumble_heavy_share"))
        .replace("@MAP@", &function(&hid, "static void rumble_map"))
        .replace("@STRENGTH@", &function(&pad, "static void rumble_strength"));
    let t = Tmp::new("rumble-mapping");
    build_run(&t, &src, &["-std=c11", "-Wall", "-Wextra", "-Werror"]);
}

/// A held rumble is sent again before SDL's one-second effect ends, to each controller on its own (one that just opened
/// too): pads_poll through rumble_due, the time per pad.
#[test]
fn rumble_refresh() {
    let pad = read("runtime/media/pad_sdl.c");
    let poll = block(&pad, "static void pads_poll");
    assert!(poll.contains("rumble_due(") && !poll.contains("last_lo") && poll.contains("pads[i].rumble_ns = now"));
    let due = block(&pad, "static int rumble_due");
    assert!(due.contains("500000000LL"));
    let t = Tmp::new("rumble-refresh");
    build_run(&t, &fixture("rumble_refresh.c").replace("@DUE@", due).replace("@HALF@", "500000000"), &["-std=c11", "-Wall", "-Wextra", "-Werror"]);
}

/// A stick axis steers only once it has been seen at rest (pad_sdl.c's poll block over a stand-in pad): a phantom axis
/// held from the start never steers, a stick seen at rest steers either way, a worn stick resting past the dead zone
/// steers once pushed the other way, and the bits are per axis.
#[test]
fn stick_rest() {
    let src = read("runtime/media/pad_sdl.c");
    let start = find(&src, "            /* (a worn stick resting just past a small dead zone");
    let end = start + find(&src[start..], "if (!(pads[i].rested >> k & 1)) v[k] = 0;");
    let end = end + find(&src[end..], "}") + 1;
    let t = Tmp::new("stick-rest");
    build_run(&t, &fixture("stick_rest.c").replace("@BLOCK@", &src[start..end]).replace("int32_t", "int"), &["-std=gnu11", "-Wall", "-Wextra", "-Werror"]);
}

/// A headless run ends on SIGTERM: with HOS_HEADLESS there is no window to read SDL's quit event, so pads_start sets
/// SDL_NO_SIGNAL_HANDLERS before SDL starts, and only then.
#[test]
fn headless_sigterm() {
    let text = read("runtime/media/pad_sdl.c");
    let body = Regex::new(r"(?s)static int pads_start\(void\) \{(.*?)\n\}").unwrap().captures(&text).expect("pad_sdl.c: no pads_start")[1].to_string();
    let at = |s: &str| body.find(s).map(|i| i as i64).unwrap_or(-1);
    let hint = at("sdl.SetHint(\"SDL_NO_SIGNAL_HANDLERS\", \"1\")").max(at("hos_sdl_hint(\"SDL_NO_SIGNAL_HANDLERS\", \"1\")"));
    let init = at("sdl.Init(").max(at("hos_sdl_init("));
    assert!(hint >= 0, "pads_start does not turn off SDL's signal handlers: a headless run ignores SIGTERM");
    assert!(init >= 0 && hint < init, "pads_start sets SDL_NO_SIGNAL_HANDLERS after SDL starts: SDL reads it when it starts");
    let line = &body[body[..hint as usize].rfind('\n').unwrap_or(0)..hint as usize];
    assert!(line.contains("getenv(\"HOS_HEADLESS\")"), "SDL_NO_SIGNAL_HANDLERS is not limited to HOS_HEADLESS: a window's close on SIGTERM needs them");
}
