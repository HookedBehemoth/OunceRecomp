"""HOS_NVN_LIST_REF (native-vk layer 1, stage S1): command lists by reference.

Builds host/nvn_list.c as it is with tests/list_ref_test.c and runs it without and with the check (HOS_NVN_LIST_REF_CHECK):
allocations grow by their guess and shrink once, handles die with their control memory range only, released bytes wait for
the replay, a callee retired while a caller lives stays until the caller goes, Begin again drops its calls' references,
deferred binds are read in place and copied (the callee and the lists above it) when a submit not replayed yet read other
values, callees deeper than the replay's stack are copied in, the mark sweep, the check's early-free detection, and four
threads recording job lists against shared lists. Then checks the sources keep the design:

- with the switch, CallCommands / CopyCommands copy nothing (nvn_list_call) and EndRecording takes neither copy_lock nor
  the flatten copy; AddControlMemory does not walk release_copies;
- the Vulkan replay walks into NOP_CALL and back at the callee's END, and every place it keeps to go on from (a wait it
  stopped at, a queue held there, a parallel segment's start and end) keeps the calls it is inside;
- the WebGPU replay follows NOP_CALL too.

No game or GPU is involved.

python -I -B tests/test_list_ref.py [--cc clang]
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

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
        raise SystemExit("test_list_ref: " + what)


def source_checks():
    nvn = (ROOT / "host/nvn_native.c").read_text(encoding="utf-8")
    # (HOS_ENG_LISTS: inline_recording records a packet's NOP_PKT, then the call as inline_recording_raw does)
    outer = block(nvn, "static void inline_recording(")
    need(outer.count("inline_recording_raw(o, h);") == 2, "CallCommands with HOS_ENG_LISTS does not record the call itself")
    inline = block(nvn, "static void inline_recording_raw(nvn_cmd *o, uint64_t h) {")
    ref = inline[:inline.index("check_handle(h);")]
    need("list_ref()" in ref and "nvn_list_call(" in ref and "memcpy" not in ref,
         "CallCommands with HOS_NVN_LIST_REF copies the callee (or does not call nvn_list_call)")
    end = block(nvn, "static uint64_t end_recording_ref(")
    need("copy_lock" not in end and "flatten" not in end and "malloc" not in end,
         "EndRecording with HOS_NVN_LIST_REF takes copy_lock, copies or allocates a node")
    endrec = block(nvn, "NV(nvnCommandBufferEndRecording)")
    need(endrec.index("end_recording_ref(") < endrec.index("flatten("), "EndRecording's list path is not taken before the copy")
    add = block(nvn, "NV(nvnCommandBufferAddControlMemory)")
    need("nvn_list_release(" in add and add.index("nvn_list_release(") < add.index("release_copies("),
         "AddControlMemory with HOS_NVN_LIST_REF still walks release_copies")
    submit = block(nvn, "NV(nvnQueueSubmitCommands)")
    need("nvn_list_submit(" in submit and submit.count("nvn_list_submitted(") == 2,
         "a submit with HOS_NVN_LIST_REF does not read the deferred binds through nvn_list_submit, or misses a hand-over")

    vk = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    rep = block(vk, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end")
    call = rep[rep.index("case NOP_CALL:"):]
    call = call[:call.index("case NOP_BEGIN:")]
    need("calls.ret[calls.depth++] = p + h.size" in call and "p = k.target" in call, "the replay does not walk into NOP_CALL")
    endc = rep[rep.index("case NOP_END:"):rep.index("case NOP_CALL:")]
    need("p = calls.ret[--calls.depth]" in endc, "the replay does not go back after a called list's END")
    need(rep.count("suspend_calls = calls;") == 3,   # (the two waits, and a job call stopped for its job's wait: E6 pre-work)
         "a wait inside a called list (or a job call stopped for its wait) loses its calls when the replay stops there")
    need("calls.same(" in rep[:rep.index("switch (h.op)")], "a segment's end is not compared with its calls")
    need("par_points.push_back({p + h.size, calls," in rep, "the dry pass's segment points lose their calls")
    need("replay(cmd, s, g.p, g.end, &g.calls, &g.end_calls);" in block(vk, "void record_segment("),
         "a segment is replayed without its calls")
    run = block(vk, "bool run_submit(const uint64_t* handles, int n, uint64_t value, uint64_t queue")
    need("q.calls = suspend_calls;" in run, "a queue held at a wait inside a called list loses its calls")
    need("&calls)) return;" in block(vk, "void resume_queue("), "a held queue goes on without its calls")
    web = (ROOT / "gpu/render/rx_nvn.cpp").read_text(encoding="utf-8")
    need("case NOP_CALL:" in web and "replay(s, k.target)" in web, "the WebGPU replay does not follow NOP_CALL")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    source_checks()
    cc = shutil.which(args.cc)
    if not cc:
        raise SystemExit("test_list_ref: no C compiler %r (pass --cc)" % args.cc)
    inc = ["-I", str(ROOT / "host"), "-I", str(ROOT / "gpu/render")]
    if sys.platform != "win32":
        inc += ["-I", str(ROOT / "host/posix")]
    with tempfile.TemporaryDirectory(prefix="list-ref-") as tmp:
        exe = Path(tmp) / "list_ref_test.exe"
        subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-function", *inc,
                        str(ROOT / "host/nvn_list.c"), str(ROOT / "tests/list_ref_test.c"), "-o", str(exe), "-lpthread"],
                       check=True)
        for mode in ([], ["check"]):
            result = subprocess.run([str(exe), *mode], capture_output=True, text=True, timeout=120)
            print(result.stdout, end="")
            if result.returncode:
                print(result.stderr, end="")
                raise SystemExit("test_list_ref: failed%s (exit %d)" % (" with the check" if mode else "", result.returncode))
    print("PASS: command lists by reference (arena, handles, lifetimes, deferred copies, threads), the replay's calls")


if __name__ == "__main__":
    main()
