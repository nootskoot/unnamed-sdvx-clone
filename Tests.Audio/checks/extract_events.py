#!/usr/bin/env python3
"""
extract_events.py: turn a KSH v1 chart into the span CSV that render_effects.cpp
consumes, plus the measure-line list a KSH gate clock needs.

Why this and not BeatmapFromKSH: Beatmap links sqlite3, nlohmann_json and the
KSON bridge, none of which this machine has room for, and the render side only
needs (start, end, type, params) per effect. The block/tick walk below mirrors
KShootMap::Init exactly, because that is where the measure counting lives:

  * the header runs to the first "--"
  * blocks are separated by "--"
  * a line holding "=" and no "|" is a setting for the tick that follows it
  * a line holding "|" is a tick row
  * "#" lines are effect/filter definitions, not ticks
  * "//" and ";" lines are comments

Chart time: TimingPoint.time is atol(header "o"), so map time = o + (block +
tick/ticksInBlock) * barDuration. That is the same clock BeatmapFromKSH::TickToMapTime
produces for a single-tempo chart, and it is the clock AudioPlayback stamps on
object->time.

Outputs (per case):
  <case>.csv        case,lane,type,p0,p1,p2,mix,startMs,endMs,wholeNote
  <case>.measures   one measure-line time in ms per line, for the gate clock
  <case>.events.txt human-readable list, for the report tables
"""
import os
import re
import sys

FX_TYPES = {
    "gate": ("Gate", "fx"),
    "bitcrusher": ("BitCrusher", "fx"),
    "retrigger": ("Retrigger", "fx"),
    "flanger": ("Flanger", "fx"),
    "phaser": ("Phaser", "fx"),
    "echo": ("Echo", "fx"),
    "wobble": ("Wobble", "fx"),
    "tapestop": ("TapeStop", "fx"),
}

# KSH laser-filter names. Only the ones this renderer can build are mapped; the
# rest are reported as unsupported rather than silently dropped.
def filter_to_span(name):
    n = name.strip().lower()
    if n.startswith("ga") or n.startswith("gt") or n.startswith("gs") or n.startswith("gw"):
        m = re.search(r"(\d+)$", n)
        if m:
            return "Gate", float(m.group(1))
    return None, None


def parse(path):
    raw = open(path, encoding="utf-8", errors="replace").read().split("\n")
    if "--" not in raw:
        raise SystemExit("no header separator")
    sep = raw.index("--")
    hdr = {}
    for line in raw[:sep]:
        if "=" in line:
            k, v = line.split("=", 1)
            hdr[k.strip()] = v.strip()

    blocks = []
    cur = []
    pending = []
    for line in raw[sep + 1:]:
        s = line.rstrip("\r")
        if not s.strip():
            continue
        if s.startswith("--//") or s.startswith("//") or s.startswith(";"):
            continue
        if s == "--":
            blocks.append(cur)
            cur = []
            pending = []
            continue
        if s.startswith("#"):
            cur.append(("def", s, list(pending)))
            pending = []
            continue
        if "=" in s and not re.match(r"^[0-9]{4}\|", s):
            k, v = s.split("=", 1)
            pending.append((k.strip(), v.strip()))
            continue
        cur.append(("tick", s, list(pending)))
        pending = []
    blocks.append(cur)
    return hdr, blocks


def timeline(hdr, blocks):
    """Yield (blockIndex, tickIndex, ticksInBlock, timeMs, wholeNoteMs, settings)."""
    bpm = float(hdr.get("t", 120) or 120)
    num, den = [int(x) for x in (hdr.get("beat") or "4/4").split("/")]
    offset = float(hdr.get("o", 0) or 0)
    blockStart = offset
    out = []
    for bi, block in enumerate(blocks):
        ticks = [x for x in block if x[0] == "tick"]
        n = max(len(ticks), 1)
        # A tempo or meter change applies from the block it appears in.
        for kind, s, pend in block:
            for k, v in pend:
                if k == "t":
                    bpm = float(v)
                elif k == "beat" and "/" in v:
                    a, b = v.split("/")
                    num, den = int(a), int(b)
            break
        bar = 60000.0 / bpm * num * 4.0 / den
        whole = 60000.0 / bpm * 4.0
        ti = 0
        for kind, s, pend in block:
            if kind != "tick":
                continue
            out.append((bi, ti, n, blockStart + (ti / n) * bar, whole, pend))
            ti += 1
        blockStart += bar
    return out


def build_spans(rows, case, window=None):
    """Effect spans per lane, from the fx-l/fx-r and laser-filter settings."""
    spans = []
    # lane -> currently open span dict
    open_fx = {}
    laser = None
    # pfiltergain is the laser filter's default wet level; filter:<NAME>:mix
    # overrides it for a named filter while that one is active.
    pfg = 100.0

    def close(span, at):
        if span is not None:
            span["endMs"] = at
            spans.append(span)

    for (bi, ti, n, ms, whole, settings) in rows:
        for k, v in settings:
            if k in ("fx-l", "fx-r"):
                lane = k
                name = v.split(";")[0].strip()
                t = FX_TYPES.get(name.lower())
                close(open_fx.get(lane), ms)
                if name == "" or t is None:
                    open_fx.pop(lane, None)
                    if name:
                        spans.append(dict(case=case, lane=lane, type=name, p0=0, p1=0, p2=0, mix=1.0,
                                          startMs=ms, endMs=ms, whole=whole, unsupported=True))
                    continue
                typ, _ = t
                parts = [p for p in v.split(";")[1:]]
                p0 = float(parts[0]) if parts and parts[0] else 0.0
                p1 = float(parts[1]) if len(parts) > 1 and parts[1] else 0.0
                # Per-type default wet level from Beatmap/src/AudioEffects.cpp:
                # Gate 0.9, Wobble 0.8, Phaser 0.5, everything else 1.0.
                dmix = {"Gate": 0.9, "Wobble": 0.8, "Phaser": 0.5}.get(typ, 1.0)
                open_fx[lane] = dict(case=case, lane=lane, type=typ, p0=p0, p1=p1, p2=0, mix=dmix,
                                     startMs=ms, endMs=0.0, whole=whole)
            elif k == "pfiltergain":
                pfg = float(v)
            elif k == "filtertype":
                close(laser, ms)
                laser = None
                typ, rate = filter_to_span(v)
                if typ:
                    laser = dict(case=case, lane="laser", type=typ, p0=rate, p1=0, p2=0,
                                 mix=1.0, startMs=ms, endMs=0.0, whole=whole)
                elif v.strip().lower().startswith("bitc"):
                    laser = dict(case=case, lane="laser", type="BitCrusher", p0=10, p1=0, p2=0,
                                 mix=pfg / 100.0, startMs=ms, endMs=0.0, whole=whole)
                else:
                    spans.append(dict(case=case, lane="laser", type=v, p0=0, p1=0, p2=0, mix=1.0,
                                      startMs=ms, endMs=ms, whole=whole, unsupported=True))
            elif k.startswith("filter:") and k.endswith(":mix"):
                # "filter:<NAME>:mix=NN%" parses as key "filter:<NAME>:mix" and
                # value "NN%", because the parser splits on the first "=".
                fname = k[len("filter:"):-len(":mix")]
                mm = re.match(r"(-?\d+(?:\.\d+)?)%", v.strip())
                if not mm:
                    continue
                mix = float(mm.group(1)) / 100.0
                if mix <= 0.0:
                    close(laser, ms)
                    laser = None
                    continue
                typ, rate = filter_to_span(fname)
                if typ is None:
                    continue
                if laser is not None and laser["type"] == typ and laser["p0"] == rate:
                    # same gate, new mix: end this span and reopen at the new mix
                    close(laser, ms)
                laser = dict(case=case, lane="laser", type=typ, p0=rate, p1=0, p2=0, mix=mix,
                             startMs=ms, endMs=0.0, whole=whole)
    close(laser, rows[-1][3] + 4000.0)
    for lane, span in open_fx.items():
        close(span, rows[-1][3] + 4000.0)

    spans = [s for s in spans if s is not None]
    spans.sort(key=lambda s: s["startMs"])
    return spans


def write_case(outdir, case, hdr, rows, spans, window):
    os.makedirs(outdir, exist_ok=True)
    csv_path = os.path.join(outdir, case + ".csv")
    with open(csv_path, "w") as f:
        f.write("case,lane,type,p0,p1,p2,mix,startMs,endMs,wholeNote\n")
        for s in spans:
            f.write("%s,%s,%s,%.6f,%.6f,%.6f,%.6f,%.3f,%.3f,%.6f\n" % (
                case, s["lane"], s["type"], s["p0"], s["p1"], s["p2"], s["mix"],
                s["startMs"], s["endMs"], s["whole"]))
    # Measure lines across the window, for the KSH gate clock. Built from the
    # per-block bar duration the timeline already tracks, so a metre change
    # (hyper_miracle goes 4/4 -> 3/4 at m036) moves the grid the way the real
    # timing map does. MeasureClock::BuildMeasureTimes does the same thing from
    # TimingPoint::GetBarDuration.
    measures = []
    seen = set()
    barStarts = sorted({round(r[3], 6) for r in rows})
    # derive each block's bar duration from consecutive block starts
    starts = []
    for bi, ti, n, ms, whole, _ in rows:
        if ti == 0:
            starts.append((bi, ms))
    for i, (bi, ms) in enumerate(starts):
        if ms > window[1] + 4000:
            break
        nxt = starts[i + 1][1] if i + 1 < len(starts) else ms + 1000.0
        bar = nxt - ms
        if bar <= 0.5 or bar > 20000:
            continue
        k = 0
        while ms + k * bar < min(nxt, window[1] + 4000):
            t = round(ms + k * bar, 6)
            if t not in seen:
                seen.add(t)
                measures.append(t)
            k += 1
    measures.sort()
    with open(os.path.join(outdir, case + ".measures"), "w") as f:
        for t in measures:
            f.write("%.6f\n" % t)
    with open(os.path.join(outdir, case + ".events.txt"), "w") as f:
        for s in spans:
            flag = " UNSUPPORTED" if s.get("unsupported") else ""
            f.write("m%.3f  t=%9.3f -> %9.3f  %-6s %-12s p0=%-5g p1=%-5g mix=%.2f%s\n" % (
                s["startMs"] / bar, s["startMs"], s["endMs"], s["lane"], s["type"],
                s["p0"], s["p1"], s["mix"], flag))
    return csv_path, len(spans)


def main():
    outdir = sys.argv[1]
    cases = sys.argv[2:]
    for spec in cases:
        # case=label:path:startMs:endMs
        label, path, start, end = spec.split(":", 3)
        hdr, blocks = parse(path)
        rows = timeline(hdr, blocks)
        spans = build_spans(rows, label)
        window = (float(start), float(end))
        csv_path, n = write_case(outdir, label, hdr, rows, spans, window)
        print("%s: %d spans, %d blocks, window %.0f-%.0f -> %s" % (
            label, n, len(blocks), window[0], window[1], csv_path))


if __name__ == "__main__":
    main()
