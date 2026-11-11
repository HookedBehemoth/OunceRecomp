"""RX_VK_TIMELINES=1 (native-vk plan step 6) is wired where the rules of gpu/vk/vk_timelines.h need it. Reads the source:

- every recording signals the NVN queues' timelines it finished (submit_recording, direct and batched; a batch goes at
  once when a CPU thread waits for a signal not sent yet), each finished value noted with its queue (run_submit);
- nvnSyncWait's wait goes to the queue's timeline before the old path's spin and forced drain, and forces held work
  only at the guard (tl::Step::Force);
- nvnQueueWaitSync becomes NOP_WAIT_FENCE at the start of the queue's next submission (host/nvn_native.c), which the
  replay holds on the host like an event wait (never a GPU-side wait: one Vulkan queue deadlocks on it);
- a fence or a present on a queue with a pending fence wait sends it first.
The rules themselves run in gpu/vk/tests/timelines_test.cpp. python -I -B tests/test_timelines_wiring.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(src, signature):
    """the text of the function that starts with signature, up to its closing brace at column 0"""
    start = src.index(signature)
    return src[start:src.index("\n}\n", start)]


def need(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    host = (ROOT / "host/nvn_native.c").read_text(encoding="utf-8")
    nvn = (ROOT / "gpu/render/rx_nvn.h").read_text(encoding="utf-8")

    sub = body(replay, "void submit_recording(Recording& r, bool signal_value")
    need("qs.v.swap(rec_open_q.v)" in sub, "submit_recording no longer takes the queues' finished values")
    need("pend.qs.merge(qs)" in sub, "a batched recording drops its queues' signals")
    need("tl_waiters.load(" in sub, "a batched recording waits for the next flush while a CPU thread waits for its signal")
    need("queue_signals_into(qs, sigv)" in sub, "a direct submit drops its queues' signals")
    need("batch_feed_due()" in sub, "a batched recording waits for a flush point while the GPU has nothing (section 9)")
    feed = body(replay, "bool batch_feed_due() {")
    need("batch_flushed" in feed and "pend_since" in feed, "the feed no longer looks at the GPU's progress and the batch's age")
    flush = body(replay, "void queue_flush_locked() {")
    need("queue_signals_into(pend.qs, sig)" in flush and "pend.qs.clear()" in flush, "the batch flush drops the queues' signals")

    run = body(replay, "bool run_submit(const uint64_t* handles, int n, uint64_t value, uint64_t queue")
    need("rec_open_q.add(queue, value)" in run, "run_submit no longer notes a finished value with its queue")

    wait = body(replay, "int wait_inner(uint64_t f, uint64_t timeout_ns) {")   # (wait(): RX_VK_WAIT_LOG's wrapper around it)
    tl_at, spin_at = wait.find("wait_timelines("), wait.find("sync_drain(2)")
    need(tl_at >= 0 and tl_at < spin_at, "wait() reaches the old spin-and-force path before the queue's timeline")
    wt = body(replay, "bool wait_timelines(uint64_t f, uint64_t timeout_ns, int* out) {")
    need("vkWaitSemaphores(" in wt and wt.count("sync_drain(") == 1, "wait_timelines must wait on the timeline and force only once")
    need(wt.index("tl::Step::Force") < wt.index("sync_drain("), "wait_timelines forces held work outside the guard")
    need(wt.index("tl_waiters.fetch_add") < wt.index("queue_flush_for("), "the waiter must be counted before its flush")

    need("NOP_WAIT_FENCE" in nvn and "nvn_p_wait_fence" in nvn, "rx_nvn.h lost NOP_WAIT_FENCE")
    fence_case = replay[replay.index("case NOP_WAIT_FENCE:"):]
    fence_case = fence_case[:fence_case.index("case NOP_REPORT_COUNTER:")]
    need("fence_wait_met(" in fence_case and "suspend_at = p" in fence_case, "NOP_WAIT_FENCE is not held on the host")
    need("vkCmdWaitEvents" not in fence_case and "pWaitSemaphoreInfos" not in fence_case,
         "NOP_WAIT_FENCE became a GPU-side wait: on one Vulkan queue a wait before its signal deadlocks")
    need("fence_wait_met(" in body(replay, "bool wait_met_at(const uint8_t* p) {"), "a held fence wait is never let on")

    qws = body(host, "NV(nvnQueueWaitSync) {")
    need("fw_put(" in qws and "tl_on()" in qws, "nvnQueueWaitSync no longer asks for the wait")
    qsub = body(host, "NV(nvnQueueSubmitCommands) {")
    need("fw_record(" in qsub and qsub.index("fw_take(") < qsub.index("rx_nvn_submit_q("), "the fence wait is not first in the submission")
    fs = body(host, "NV(nvnQueueFenceSync) {")
    need(fs.index("fw_flush(") < fs.index("rx_nvn_fence_q("), "a fence is taken before its queue's pending wait is sent")
    need("fw_flush(" in body(host, "NV(nvnQueuePresentTexture) {"), "a present goes before its queue's pending wait")
    print("PASS: RX_VK_TIMELINES wiring (queue timelines signalled, CPU waits on them, QueueWaitSync held on the host)")


if __name__ == "__main__":
    main()
