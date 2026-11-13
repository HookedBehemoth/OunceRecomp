"""(M6f) The walk check's verdict (scripts/direct_walk_check.sh): a run's log and vram.txt read into a few lines and a
verdict. Direct runs fail on a strict stop, on textures made with undefined content (loads with no data the asset path
could not resolve, ASTC textures with no prepared record), on a lost device, and on a run that did not reach its end.

    python -I scripts/direct_walk_check.py <run folder> <direct|frozen>
"""
import re
import statistics
import sys
from pathlib import Path


def last(pattern, text, cast=int, default=None):
    m = re.findall(pattern, text)
    return cast(m[-1]) if m else default


def main():
    run, backend = Path(sys.argv[1]), sys.argv[2]
    log = (run / "log.txt").read_text(errors="replace") if (run / "log.txt").exists() else ""
    fails = []
    timeline = (run / "timeline.txt").read_text().split("\n") if (run / "timeline.txt").exists() else []
    presents = max([int(l.split()[1]) for l in timeline if len(l.split()) >= 2] or [0])
    shots = len(list((run / "shots").glob("*.png"))) if (run / "shots").exists() else 0
    print(f"walk check: {run.name} ({backend}): {presents} presents, {shots} shots in {run / 'shots'}")
    if re.search(r"DEVICE_LOST|device lost", log, re.I):
        fails.append("the device was lost")
    if presents < 3000:
        fails.append(f"the run ended early ({presents} presents)")
    if backend == "direct":
        stop = re.findall(r"strict: STOP .*", log)
        if stop:
            fails.append(stop[0][:400])
        plain = last(r"no data and no BNTX \(plain images\) (\d+)", log, default=0)
        same = re.findall(r"loads with no data over another texture's content (\d+): its record (\d+), its plain image (\d+), "
                          r"no load of its target (\d+), another descriptor (\d+)", log)
        astc = last(r"placeholders \d+ \(no prepared record (?:\d+), ASTC (\d+)", log, default=0)
        norec = last(r"placeholders \d+ \(no prepared record (\d+)", log, default=0)
        never = last(r"textures loaded with no data: \d+ images, .*?never (\d+)", log, default=0)
        unresolved = 0
        if same:
            n, rec, pl, nolink, desc = map(int, same[-1])
            unresolved = nolink + desc
            print(f"  loads with no data over another texture's content {n}: its record {rec}, its plain image {pl}, "
                  f"unresolved {unresolved} (no load of its target {nolink}, another descriptor {desc})")
        print(f"  plain images (no data, no BNTX) {plain}, of the images loaded with no data never written {never}; "
              f"placeholders: no prepared record {norec}, ASTC {astc}")
        if unresolved:
            fails.append(f"{unresolved} loads with no data over another texture's content not resolved (undefined content)")
        if astc:
            fails.append(f"{astc} ASTC textures with no prepared record (the placeholder)")
        mem = re.findall(r"direct: memory \(MB\): heap 0 \(device-local, \d+\): driver usage (\d+) of budget (\d+), VMA blocks (\d+)", log)
        if mem:
            print("  device-local heap, driver usage / VMA blocks (MB) every 600 presents: " +
                  " ".join(f"{u}/{b}" for u, _, b in mem))
        imgs = re.findall(r"direct: images: \d+ made .*?(\d+) live .*?\), (\d+) MB \(assets: (\d+) images, (\d+) MB", log)
        if imgs:
            print("  images live (MB; of them assets) every 600 presents: " +
                  " ".join(f"{a}({b}; {d})" for a, b, _, d in imgs))
    # (strict in survey mode, HOS_DBG=strict,strictall: the reasons after arming; these fail the walk)
    fail_reasons = ("a texture loaded with no data over a content not resolved (undefined content)",
                    "a texture with no image sampled", "no render targets set in the list")
    armed = re.findall(r"strict: armed \(at present \d+\); known list \d+; events by reason \(before arming / after\): (.*)", log)
    if backend == "direct" and armed:
        after = {}
        for m in re.finditer(r"([^;]+?) (\d+) / (\d+)( \(known\))?(?:;|$)", armed[-1]):
            reason, n = m.group(1).strip(), int(m.group(3))
            if n and not m.group(4):
                after[reason] = n
        if after:
            print("  strict events after arming (not known): " + "; ".join(f"{r} {n}" for r, n in sorted(after.items())))
        for r in fail_reasons:
            if after.get(r):
                fails.append(f"{after[r]} events after arming: {r}")
    # (pictures: a black frame or a white flash in play is a failure; the shots after the load, present 900 on)
    try:
        from PIL import Image
        import numpy as np
        dark, flash = [], []
        for f in sorted((run / "shots").glob("vk_*.png")):
            p = int(f.stem.split("_")[1])
            if p < 900:
                continue
            a = np.asarray(Image.open(f).convert("RGB")).astype(np.int32)
            if a.mean() < 3:
                dark.append(p)
            low = a[a.shape[0] // 3:]
            if (low.min(axis=2) > 200).mean() > 0.02:
                flash.append(p)
        print(f"  shots: black {dark[:10]}{'...' if len(dark) > 10 else ''}, white flash {flash[:10]}{'...' if len(flash) > 10 else ''}")
        if dark:
            fails.append(f"{len(dark)} black shots in play")
        if flash:
            fails.append(f"{len(flash)} shots with a white flash over the lower two thirds")
    except ImportError:
        print("  shots: not looked at (no PIL/numpy in this Python)")
    vram = run / "vram.txt"
    if vram.exists():
        rows = [tuple(map(int, l.split())) for l in vram.read_text().split("\n") if len(l.split()) == 3]
        if rows:
            buckets = {}
            for t, v, g in rows:
                buckets.setdefault(t // 20, []).append((v, g))
            print("  VRAM / GTT MB (whole device; median of each 20 s): " +
                  " ".join(f"{k * 20}s {int(statistics.median(x[0] for x in b))}/{int(statistics.median(x[1] for x in b))}"
                           for k, b in sorted(buckets.items())))
            print(f"  VRAM peak {max(r[1] for r in rows)} MB, GTT peak {max(r[2] for r in rows)} MB")
    print("walk check: " + ("FAIL: " + "; ".join(fails) if fails else "pass"))
    sys.exit(1 if fails else 0)


main()
