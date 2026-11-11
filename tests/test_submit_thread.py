"""RX_VK_SUBMIT_THREAD: the replay's batched submits on a thread of their own, and the queue still one thread's at a time.

Only submit_recording's FEED and CAP flushes are handed over (sub_handoff); every other flush waits for what the thread holds
(sub_drain) before it submits, and every other use of the game's queue is behind such a flush. The thread itself never takes
dev.present_mu (the callers that wait for it hold it). A queue use added without a flush before it would race the thread.
python -I -B tests/test_submit_thread.py
"""
import re
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


def main():
    vk = ROOT / "gpu" / "vk"
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    flush = block(replay, "void queue_flush_locked() {")
    head = flush[: flush.index("if (pend.cbs.empty()) return;")]
    if "const bool hand_over = sub_thread_on() && sub_handoff;" not in head or "if (!hand_over) sub_drain();" not in head \
            or "sub_handoff = false;" not in head:
        raise SystemExit("a flush that is not handed over does not wait for the submit thread first")
    sets = [l.strip() for l in replay.splitlines() if "sub_handoff = true" in l]
    if len(sets) != 2 or not all("FR_CAP" in l or "FR_FEED" in l for l in sets):
        raise SystemExit("something other than submit_recording's CAP and FEED flushes hands a batch over: " + repr(sets))
    if "present_mu" in block(replay, "void sub_main() {"):
        raise SystemExit("the submit thread takes dev.present_mu: a caller waiting for it holds it")
    # (every use of the game's queue, after a flush or a drain: within the lines before it)
    uses = 0
    for path in sorted(vk.glob("*.cpp")):
        if path.name in ("vk_coopmat.cpp",):   # (at the start, before any replay)
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            if not re.search(r"vk(QueueSubmit2?|QueueBindSparse|QueuePresentKHR|QueueWaitIdle)\(dev\.queue\b", line):
                continue
            if path.name == "vk_replay.cpp" and "sub_main" in "\n".join(lines[max(0, i - 25):i]):
                continue   # (the thread's own submit)
            before = "\n".join(lines[max(0, i - 40):i + 1])
            if "queue_flush_locked()" not in before and "sub_drain()" not in before:
                raise SystemExit(f"{path.name}:{i + 1}: the queue is used without a flush or a drain before it")
            uses += 1
    print(f"PASS: {uses} uses of the queue wait for the submit thread; only the replay's FEED and CAP flushes are handed over")


if __name__ == "__main__":
    main()
