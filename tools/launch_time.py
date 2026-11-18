"""Launch times of a headless run (ounce run <game> --headless) (chiseled boot): when the first gameplay frames came, and when it was playable.

    python -I tools/launch_time.py work/<game>/runs/<name> [...]

- gameplay: the time the first 120-present block with over 100 draws a frame ended (the renderer's "<n> presents" lines,
  timed by the run's timeline.txt: seconds, last present count logged), so within ~2 s.
- playable: the first second from which 3 seconds in a row each showed at least 40 presents, after gameplay began.
"""
import re
import sys

LINE = re.compile(r"\] vk: (\d+) presents, [\d.]+ fps; a frame: (\d+) draws")


def times(run):
    tl = [tuple(int(x) for x in l.split()[:2]) for l in open(run + "/timeline.txt") if l.strip()]
    blocks = [(int(m.group(1)), int(m.group(2))) for m in map(LINE.search, open(run + "/log.txt", errors="replace")) if m]
    first = next((n for n, d in blocks if d > 100), None)
    if first is None:
        return None, None, tl[-1][0] if tl else 0
    t_game = next((t for t, p in tl if p >= first), None)
    rate = [(tl[i][0], tl[i][1] - tl[i - 1][1]) for i in range(1, len(tl))]
    # (the timeline counts presents in 120s: a second's rate is the change over the 3 s around it)
    t_play = None
    for i in range(len(tl) - 3):
        t, p = tl[i]
        if t_game is None or t < t_game:
            continue
        if tl[i + 3][1] - p >= 120 and all(tl[j + 1][1] > tl[j][1] or tl[j + 2][1] > tl[j][1] for j in range(i, i + 2)):
            if (tl[i + 3][1] - p) / max(1, tl[i + 3][0] - t) >= 40:
                t_play = t
                break
    return t_game, t_play, tl[-1][0] if tl else 0


if __name__ == "__main__":
    for run in sys.argv[1:]:
        g, p, end = times(run.rstrip("/"))
        print("%-40s gameplay %s s, playable %s s (run %d s)" % (run, g if g is not None else "-", p if p is not None else "-", end))
