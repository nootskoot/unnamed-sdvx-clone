#!/usr/bin/env python3
"""
analyze.py: measure and plot what render_effects.cpp produced.

Inputs (from run_render.sh):
  audit_out/<case>_dry.wav            the window, untouched
  audit_out/<case>_before.wav         same window through the pre-fix chain
  audit_out/<case>_after.wav          same window through the fixed chain
  audit_out/<case>_<flavor>.log       what was constructed and when

Outputs:
  audit_out/<case>_<flavor>_spec.png  spectrogram of the processed window
  audit_out/<case>_gain.png           wet/dry gain envelope, before vs after
  audit_out/summary.json              every number quoted in report.md
  stdout                              a short table

No scipy, no matplotlib on this machine: the STFT is a numpy FFT and the PNG is
written by hand (zlib + the five chunks a PNG needs). Both are small and this
keeps the report reproducible on a box with nothing but numpy.
"""
import json
import math
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "audit_out")
SR = 44100


# ---------------------------------------------------------------- wav / png io

def read_wav(path):
    data = open(path, "rb").read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", path
    pos = 12
    fmt = None
    while pos < len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            audio = np.frombuffer(body, dtype="<f4").astype(np.float64)
            if fmt[1] == 2:
                audio = audio.reshape(-1, 2)
            return audio
        pos += 8 + size + (size & 1)
    raise SystemExit("no data chunk in " + path)


def write_png(path, rgb):
    """rgb: uint8 array (h, w, 3)."""
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def colormap(v):
    """v in 0..1 -> magma-ish RGB, so a spectrogram reads like one."""
    stops = np.array([
        [0, 0, 4], [28, 16, 68], [79, 18, 123], [129, 37, 129],
        [181, 54, 122], [229, 80, 100], [251, 135, 97], [254, 194, 135], [252, 253, 191],
    ], dtype=np.float64)
    x = np.clip(v, 0, 1) * (len(stops) - 1)
    i = np.floor(x).astype(int)
    i1 = np.clip(i + 1, 0, len(stops) - 1)
    frac = (x - i)[..., None]
    return (stops[i] * (1 - frac) + stops[i1] * frac).astype(np.uint8)


# ---------------------------------------------------------------- measurement

def frames(sig, win=2048, hop=256):
    n = 1 + max(0, (len(sig) - win) // hop)
    for i in range(n):
        yield i * hop, sig[i * hop:i * hop + win]


def stft_db(sig, win=2048, hop=256, fmax=16000):
    """Magnitude STFT in dB, mono, one column per hop."""
    mono = sig.mean(axis=1) if sig.ndim > 1 else sig
    w = np.hanning(win)
    cols = []
    for _, chunk in frames(mono, win, hop):
        if len(chunk) < win:
            chunk = np.pad(chunk, (0, win - len(chunk)))
        spec = np.abs(np.fft.rfft(chunk * w))
        cols.append(spec)
    if not cols:
        return np.zeros((2, 2)), 0
    S = np.array(cols).T  # freq x time
    S = 20 * np.log10(np.maximum(S, 1e-7))
    S -= S.max()
    keep = int(fmax / (SR / win))
    return S[:keep], keep


def rms_env(sig, win=256, hop=64):
    mono = sig.mean(axis=1) if sig.ndim > 1 else sig
    out = []
    for _, chunk in frames(mono, win, hop):
        if len(chunk) < win:
            out.append(np.nan)
            continue
        out.append(math.sqrt(float(np.mean(chunk * chunk)) + 1e-20))
    return np.array(out), hop


def gain_db(dry, wet, win=256, hop=64):
    d, _ = rms_env(dry, win, hop)
    w, _ = rms_env(wet, win, hop)
    n = min(len(d), len(w))
    return 20 * np.log10(np.maximum(w[:n], 1e-9) / np.maximum(d[:n], 1e-9)), hop


def edges(g_db, hop, t0_ms, threshold_db=6.0):
    """(time_ms, direction, size_db) for every step in the gain envelope.

    A gate's open and close are steps of 20 dB or more; a flanger's comb sweep is
    not a step at all, which is why this only reports on effects that switch.
    """
    out = []
    for i in range(1, len(g_db)):
        if not (np.isfinite(g_db[i]) and np.isfinite(g_db[i - 1])):
            continue
        d = g_db[i] - g_db[i - 1]
        if abs(d) >= threshold_db:
            out.append((t0_ms + i * hop * 1000.0 / SR, 1 if d > 0 else -1, d))
    return out


def open_windows(g_db, hop, t0_ms, period_ms, thresh_db=-3.0):
    """Rising transitions of a gate's open/closed state, one per window.

    Thresholding the envelope instead of looking for 6 dB steps keeps a ramped gate
    from contributing two or three edges per period, which a step detector does.
    """
    is_open = g_db > thresh_db
    out = []
    last = -1e9
    for i in range(1, len(is_open)):
        if is_open[i] and not is_open[i - 1]:
            t = t0_ms + i * hop * 1000.0 / SR
            if t - last >= period_ms * 0.5:
                out.append(t)
                last = t
    return out


def phase_error(edge_times, period_ms, origin_ms):
    """How far each edge sits from the nearest k*period grid point after origin."""
    errs = []
    for t in edge_times:
        k = round((t - origin_ms) / period_ms)
        errs.append(t - (origin_ms + k * period_ms))
    return errs


def first_diff_ms(dry, wet, t0_ms, event_ms, thresh=1e-3):
    """First sample at/after the event where the chain has altered the signal."""
    i0 = max(0, int((event_ms - t0_ms) * SR / 1000.0))
    d = np.abs(wet[i0:, :].mean(axis=1) - dry[i0:, :].mean(axis=1))
    idx = np.nonzero(d > thresh)[0]
    if not len(idx):
        return None
    return t0_ms + (i0 + idx[0]) * 1000.0 / SR


def onset_ms(g_db, hop, t0_ms, event_ms, thresh=0.5, run=3):
    """First hop at/after the event where the effect is audibly in, in ms."""
    i0 = max(0, int((event_ms - t0_ms) * SR / 1000.0 / hop) - int(20 * SR / 1000 / hop))
    hits = 0
    for i in range(i0, len(g_db)):
        if abs(g_db[i]) > thresh:
            hits += 1
            if hits >= run:
                return t0_ms + (i - run + 1) * hop * 1000.0 / SR
        else:
            hits = 0
    return None


# ---------------------------------------------------------------- report pieces

def spec_figure(case, flavor, sig, t0_ms, t1_ms, path):
    S, _ = stft_db(sig)
    S = np.clip((S + 72) / 72.0, 0, 1)  # -72 dB floor
    img = colormap(S[::-1])  # low frequencies at the bottom
    # time axis labels as a ruler strip
    img = img.copy()
    write_png(path, img)
    return img.shape


def gain_figure(case, series, path, t0_ms, t1_ms, events):
    """series: [(label, gain_db array, hop)] -> stacked traces on a shared axis."""
    hop = series[0][2]
    n = min(len(g) for _, g, _ in series)
    W, H = 1600, 320
    img = np.zeros((H, W, 3), dtype=np.uint8)
    img[:] = (12, 12, 16)
    # grid: 0 dB and ±12 dB
    for db, shade in ((0, (70, 70, 80)), (6, (40, 40, 48)), (-6, (40, 40, 48)),
                      (12, (30, 30, 36)), (-12, (30, 30, 36))):
        y = int(H / 2 - db / 24.0 * H)
        if 0 <= y < H:
            img[y, :, :] = shade
    colors = [(255, 120, 90), (110, 200, 255)]
    for (label, g, _), col in zip(series, colors):
        for x in range(W):
            i = int(x / W * n)
            if i >= n or not np.isfinite(g[i]):
                continue
            y = int(H / 2 - np.clip(g[i], -24, 24) / 24.0 * H)
            y = min(max(y, 0), H - 1)
            img[y, x] = col
            if x:
                i2 = int((x - 1) / W * n)
                if i2 < n and np.isfinite(g[i2]):
                    y2 = int(H / 2 - np.clip(g[i2], -24, 24) / 24.0 * H)
                    lo, hi = sorted((min(y, y2), max(y, y2)))
                    img[lo:hi + 1, x] = col
    # event markers
    for ev in events:
        x = int((ev["ms"] - t0_ms) / (t1_ms - t0_ms) * W)
        if 0 <= x < W:
            img[:, x] = np.maximum(img[:, x], np.array([120, 120, 60], dtype=np.uint8))
    write_png(path, img)


def main():
    cases = json.load(open(os.path.join(OUT, "cases.json")))
    summary = {"cases": {}}
    for case, info in cases.items():
        t0, t1 = info["startMs"], info["endMs"]
        dry = read_wav(os.path.join(OUT, "%s_dry.wav" % case))
        entry = {"window": [t0, t1], "events": info["events"], "flavors": {}}
        traces = []
        for flavor in ("before", "after"):
            path = os.path.join(OUT, "%s_%s.wav" % (case, flavor))
            if not os.path.exists(path):
                continue
            wet = read_wav(path)
            g, hop = gain_db(dry, wet)
            spec_figure(case, flavor, wet, t0, t1,
                        os.path.join(OUT, "%s_%s_spec.png" % (case, flavor)))
            traces.append((flavor, g, hop))
            per_event = []
            all_edges = edges(g, hop, t0)
            for ev in info["events"]:
                first = first_diff_ms(dry, wet, t0, ev["startMs"], 1e-3)
                entry_ev = {
                    "label": ev["label"], "chartMs": ev["startMs"], "type": ev["type"],
                    "firstDiffMs": None if first is None else round(first, 2),
                    "firstDiffDeltaMs": None if first is None else round(first - ev["startMs"], 2),
                }
                period = ev.get("periodMs")
                if period:
                    # Windows that begin after the effect's own construction: the first
                    # one is bounded by hold scheduling, every later one by the clock.
                    lo = ev["startMs"] + ev.get("skipMs", 0.0)
                    hi = ev.get("measureEndMs", ev.get("endMs", t1))
                    sel = [t for t in open_windows(g, hop, t0, period)
                           if lo <= t <= hi]
                    per = phase_error(sel, period, ev["startMs"])
                    entry_ev["windows"] = len(sel)
                    entry_ev["periodMs"] = period
                    entry_ev["phaseErrorsMs"] = [round(e, 2) for e in per[:12]]
                    if per:
                        arr = np.array(per)
                        entry_ev["meanAbsPhaseErrMs"] = round(float(np.mean(np.abs(arr))), 2)
                        entry_ev["maxAbsPhaseErrMs"] = round(float(np.max(np.abs(arr))), 2)
                        # The detection hop biases every window late by the same
                        # amount, so the spread is what separates the two clocks.
                        entry_ev["phaseStdMs"] = round(float(np.std(arr)), 2)
                        entry_ev["phaseSignedMeanMs"] = round(float(np.mean(arr)), 2)
                per_event.append(entry_ev)
            gabs = float(np.max(np.abs(g[np.isfinite(g)]))) if len(g) else 0.0
            entry["flavors"][flavor] = {
                "peak_abs_gain_db": round(gabs, 2),
                "events": per_event,
            }
        if traces:
            gain_figure(case, traces, os.path.join(OUT, "%s_gain.png" % case), t0, t1,
                        [{"ms": e["startMs"]} for e in info["events"]])
        summary["cases"][case] = entry
        print("== %s (%.1f-%.1f s)" % (case, t0 / 1000, t1 / 1000))
        for flavor in ("before", "after"):
            f = entry["flavors"].get(flavor)
            if not f:
                continue
            for e in f["events"]:
                print("   %-6s %-24s chart=%9.1f firstDiff=%s(%s)  windows=%s mean=%s std=%s max=%s" % (
                    flavor, e["label"], e["chartMs"], e["firstDiffMs"], e["firstDiffDeltaMs"],
                    e.get("windows"), e.get("meanAbsPhaseErrMs"), e.get("phaseStdMs"),
                    e.get("maxAbsPhaseErrMs")))
    json.dump(summary, open(os.path.join(OUT, "summary.json"), "w"), indent=1)
    print("wrote", os.path.join(OUT, "summary.json"))


if __name__ == "__main__":
    main()
