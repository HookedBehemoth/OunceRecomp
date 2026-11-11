"""HOS_ENG_LISTS (native-vk engine cut E2): the load-time lists' packets (host/eng_lists.c, gpu/render/nvn_pkt.h).

Builds host/eng_lists.c as it is with tests/eng_lists_test.c (recordings made by hand: their packets, folded calls, refusals,
lifetimes), and checks the source: the replay applies a packet as the ops would (the RX_VK_PACKETS stamps, the generations), only
in the serial replay; CallCommands records NOP_PKT before the call it stands for; the switches are read once.
python -I -B tests/test_eng_lists.py [--cc clang]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(ok, what):
    if not ok:
        raise SystemExit("test_eng_lists: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="clang")
    a = p.parse_args()
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    nvn = (ROOT / "host/nvn_native.c").read_text(encoding="utf-8")
    apply = block(replay, "void pkt_apply(")
    need("pk_touch(PKG_TEX)" in apply and apply.count("pk_touch(PKG_VI)") == 2, "a packet's handles or vertex input do not stamp their group")
    need("maybe_split(cmd)" in apply and "s.samples_passed += 16" in apply, "a packet's draw is not counted as the op's")
    case = replay[replay.index("case NOP_PKT:"):replay.index("// ---- memory the stream writes")]
    need("mode != Mode::Serial" in case and "ubo_census_on()" in case and "l1c::on" in case, "a packet is applied outside the serial replay")
    need("nvn_pkt_live(" in case and "p += h.size + r.skip" in case, "a packet's serial is not checked, or its records not skipped")
    body = block(replay, "void replay(VkCommandBuffer& cmd, NvnState& s")
    need(body.count("if (eng_end) {") == 3, "a checked packet's records draw (NOP_DRAW, NOP_DRAW_IDX, NOP_DRAW_MESH)")
    need("static const bool eng = eng_replay_on()" in body, "the replay reads the switch more than once")
    inline = block(nvn, "static void inline_recording(")
    need(inline.index("NOP_PKT") < inline.index("inline_recording_raw(o, h);"), "NOP_PKT is not recorded before the call")
    need("hw_used -= 8 + sizeof *rp" in inline, "NOP_PKT counts as command memory the game sees")
    need("static int on = -1;" in block(nvn, "static int eng_on("), "HOS_ENG_LISTS is not read once")
    endref = block(nvn, "static uint64_t end_recording_ref(")
    need(endref.index("r->lw.user = pkt") < endref.index("nvn_list_end("), "the packet is given to the recording after its handle is published")
    lst = (ROOT / "host/nvn_list.c").read_text(encoding="utf-8")
    need(block(lst, "nvn_lrec *nvn_list_end(").index("r->user = w->user") < block(lst, "nvn_lrec *nvn_list_end(").index("lid_publish("),
         "nvn_list_end publishes the handle before the user pointer")
    need("user_freed(r)" in block(lst, "static void rec_free_storage(") and "user_released(r)" in block(lst, "static void release_list("),
         "a recording's packet is not told of its release or its free")
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "eng_lists_test"
        subprocess.run([a.cc, "-std=c11", "-O1", "-g", "-Wall", "-Werror", "-fsanitize=address,undefined", "-o", str(exe),
                        str(ROOT / "tests/eng_lists_test.c"), str(ROOT / "host/eng_lists.c")], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, env={"ASAN_OPTIONS": "detect_leaks=0"})   # (packets are kept: by design)
        print(r.stdout.strip())
        need(r.returncode == 0, "the packets' test failed:\n" + r.stderr)
    print("PASS: eng lists (packets, the replay's use of them, the recording's hooks)")


if __name__ == "__main__":
    main()
