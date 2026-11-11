"""HOS_ENG_PASSES (engine cut E4, gpu/vk/eng_pass.h): the job-call sites' marks, the pass table and the job consumer.

- host/nvn_native.c brackets each handle the three job-call sites call with NOP_ENG CALL_BEGIN / CALL_END, marks a render job's
  recording with its view record (f_80084de0's Initialize), and leaves the marks out of the game's command memory accounting;
- the replay's job scan lets no op through into a secondary that one rendering cannot hold (barriers, dispatches, copies,
  clears, events, waits, targets);
- what would leave the pass while a secondary records is trapped (end_pass, begin_pass on other targets, full_barrier), and
  the replay stops the job's walk then;
- the pass table (eng_pass.cpp) compiled and run from two threads (tests/eng_pass_test.cpp).
python -I -B tests/test_eng_passes.py [--cxx clang++] [--headers <Vulkan headers>]
"""
import argparse
import os
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


def need(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--headers", default=os.environ.get("HOOKSHOT_VULKAN_HEADERS", ""))
    a = p.parse_args()
    nvn = (ROOT / "host/nvn_native.c").read_text(encoding="utf-8")
    vk = ROOT / "gpu/vk"
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")
    tex = (vk / "vk_texture.cpp").read_text(encoding="utf-8")

    call = block(nvn, "NV(nvnCommandBufferCallCommands) {")
    need(call.index("NVN_ENG_CALL_BEGIN") < call.index("inline_recording(") < call.index("NVN_ENG_CALL_END"),
         "CallCommands does not bracket each handle with CALL_BEGIN / CALL_END")
    sites = block(nvn, "static int eng_call_site(")
    for s in ("0x80668c84", "0x8028a538", "0x807cd418"):
        need(s in sites, "job-call site %s is not marked" % s)
    need("o->hw_used -= sz" in block(nvn, "static void eng_mark("), "the marks count in the game's command memory")
    need("ENG_JOB_INIT_LR" in block(nvn, "NV(nvnCommandBufferInitialize) {") and "x[25]" in block(nvn, "NV(nvnCommandBufferInitialize) {"),
         "a render job's Initialize does not note its view record (x25)")
    need("NVN_ENG_JOB" in block(nvn, "NV(nvnCommandBufferBeginRecording) {"), "a job's recording does not start with its view")

    scan = block(replay, "void eng_scan(const uint8_t* p, EngScan& sc) {")
    allowed = scan[scan.index("case NOP_BEGIN: case NOP_JUMP:"):scan.index("default:")]
    for op in ("NOP_BARRIER", "NOP_DISPATCH", "NOP_DISPATCH_INDIRECT", "NOP_WAIT_EVENT", "NOP_WAIT_FENCE", "NOP_SIGNAL_EVENT",
               "NOP_SET_RT", "NOP_CLEAR_COLOR", "NOP_CLEAR_DS", "NOP_CLEAR_TEX", "NOP_CLEAR_BUF", "NOP_COPY_B2B", "NOP_COPY_B2T",
               "NOP_COPY_T2B", "NOP_COPY_T2T"):
        need(op + ":" not in allowed and op + " " not in allowed.replace(op + "_", ""), op + " is let into a job's secondary")
    need("if (!sc.bad) sc.bad = true" in scan[scan.index("default:"):], "an op the scan does not know is let through")
    need('eng_sec_trap("the pass ended")' in block(draw, "void end_pass(VkCommandBuffer cmd) {"), "end_pass is not trapped")
    bp = block(draw, "void begin_pass(VkCommandBuffer cmd, const Targets& at) {")
    need(bp.index("eng_sec_trap(") < bp.index("end_pass(cmd);"), "begin_pass on other targets is not trapped")
    need("eng_sec_trap(" in block(tex, "void full_barrier(VkCommandBuffer cmd, const char* why) {"), "full_barrier is not trapped")
    loop = block(replay, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p")
    need("if (eng_sec_broken()) return;" in loop, "the replay goes on recording a broken secondary")
    need("!eng_sec_active() && !eng_state_only" in loop, "a submission is split inside a job's secondary")
    job = block(replay, "const uint8_t** resume) {")
    need(job.index("draw_sec_leave(") < job.index("vkCmdExecuteCommands("), "a secondary is executed before its recording ended")
    need("s = eng_snap;" in job and "ub_updates.resize(ubn)" in job, "a broken secondary leaves the state it changed")

    if not a.headers:
        print("PASS (source checks; the pass table not compiled: no Vulkan headers, --headers)")
        return
    with tempfile.TemporaryDirectory(prefix="eng-pass-") as tmp:
        tmp = Path(tmp)
        exe = tmp / "t"
        # (eng_pass.cpp beside an eng_job.h of EngPassFormats alone, cut from E3's: the back end's headers stay out)
        job = (vk / "eng_job.h").read_text(encoding="utf-8")
        fmt = job[job.index("struct EngPassFormats"):]
        fmt = fmt[:fmt.index("};") + 2]
        (tmp / "eng_job.h").write_text("#pragma once\n#include <cstdint>\n#include <vulkan/vulkan.h>\nnamespace mkwvk {\n" + fmt +
                                       "\n}\n", encoding="utf-8")
        for f in ("eng_pass.h", "eng_pass.cpp"):
            (tmp / f).write_text((vk / f).read_text(encoding="utf-8"), encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-DVK_NO_PROTOTYPES", "-I" + str(tmp), "-I" + a.headers,
                        str(ROOT / "tests/eng_pass_test.cpp"), str(tmp / "eng_pass.cpp"), "-o", str(exe), "-lpthread"], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        need(out.returncode == 0 and "PASS" in out.stdout, "the pass table fixture: exit %d" % out.returncode)
    print("PASS: job-call marks, the scan's refusals, the traps, the pass table from two threads")


if __name__ == "__main__":
    main()
