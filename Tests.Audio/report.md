# Effect timing and gain in the real-time audio path

Rendered offline from the chart's own effect timeline through the clone's DSP classes,
twice: once from the pre-fix tree (HEAD `a515ad8d`) and once from this working tree with
the timing, gate, echo and filter fixes applied. Same source, same decoded audio, same
384-frame blocks. The only differences are the code under test and the two behaviours
that live in `AudioPlayback` (listed under *What is modelled*).

Charts: `Cherish_ex.ksh` and `Cherish_in.ksh` (KSH v1, ver 160, 180 BPM, `o=69`) and
`hyper_miracle_shooting_star/in.ksh` (KSH v1, ver 171, 215 BPM, `o=0`, 4/4 → 3/4 at m036).

## The three complaints, measured

### 1. Effects start late

Gate open windows, measured as the phase error of each window against the chart's own
1/16 grid. A negative number would be early; the detector's floor is about 2-3 ms.

| Chart | Event | Windows | Before: mean / max | After: mean / max |
|---|---|---|---|---|
| Cherish_ex | `fx-l=Gate;16` m054.33 (71.402 s) | 15 | not detected (see note) | **3.99 / 5.28 ms** |
| Cherish_ex | `fx-r=Gate;16` m055.33 (72.736 s) | 38 | **15.31 / 16.78 ms** | **2.53 / 4.88 ms** |
| Cherish_in | `filter:GA16:mix=90%` m054.01 (70.736 s) | 11 | **19.26 / 22.11 ms** | **2.85 / 7.89 ms** |
| hyper | `fx-r=Gate;16` m018.25 (19.814 s) | 17 | **28.99 / 33.37 ms** | 7.31 / 17.40 ms (polluted, note) |
| hyper | `fx-l=Gate;16` m021.17 (22.884 s) | 19 | **13.91 / 16.00 ms** | **1.93 / 4.32 ms** |
| hyper | `fx-r=Gate;16` m022.25 (24.279 s) | 16 | 2.86 / 3.90 ms | 2.18 / 4.59 ms |

1. A 1/16 gate at 180 BPM has an 83.33 ms period and a 41.67 ms open window. Sitting
   15-29 ms off the grid means **a third to a half of every open window lands in the
   wrong place**, which is what "starts late" sounds like when the effect is a gate.
2. The single-gate regions improve by 4x to 15x. What remains after the fix (2-5 ms) is
   mostly the envelope detector's own lag, not clock error: the independently measured
   number from the gate clock itself is **8.98 ms → 0.00 ms** (see `run_w8.sh`, which
   drives `GateDSP` directly rather than through an envelope).
3. The two rows that do not fit: `hyper` m018 still shows 7.31 ms / std 8.32 because the
   `Retrigger;8` on the other lane is still running through that whole region and its own
   gating pollutes the envelope. And `Cherish_ex` m054 shows no windows before the fix at
   all, because the old gate's ramps kept the gain within 3 dB of unity through that
   1.25 s stretch: it never opened and never closed, it just wobbled. That is the same
   defect seen a different way.

### 2. Effects feel weak

Two separate causes, and only one of them is the gate.

**The gate does not lose level** (measured with the m033 echo excluded so the gate is
alone): the m054 region sits at **-2.87 dB mean before, -2.98 dB after**, and the whole
window at -12.42 vs -12.44 dBFS. The old gate's 5%-of-period ramps trade a little level
for the wrong shape; they do not eat the signal.

**The held echo does lose everything.** With `fx-r=Echo;16;70` held from m033.01
(42.736 s), the pre-fix echo never re-injects its dry input: it stores
`out * feedback` where `out` is already the delayed signal, so the delay line decays
geometrically and receives nothing new. Per-second level of the Cherish window:

| | 68 s | 69 s | 70 s | 71 s | 72 s | 73 s | 74 s | 75 s |
|---|---|---|---|---|---|---|---|---|
| dry | -10.4 | -11.5 | -11.4 | -10.9 | -10.7 | -10.4 | -10.6 | -12.2 |
| before | -18.2 | **-55.4** | **-92.6** | **-129.9** | -19.6 | -13.4 | -13.3 | -15.4 |
| after | -8.3 | -7.7 | -7.9 | -8.4 | -10.0 | -12.7 | -13.0 | -14.9 |

That is **31 dB per second of decay to -130 dBFS**, recovering only when the hold ends at
m055.33. The fixed echo keeps the dry signal, so the window stays between -8 and -15
dBFS. Figure: [cherish_ex_gain.png](/home/superbot/sdvx/repo/Tests.Audio/report/cherish_ex_gain.png)
(orange = before, dropping to the floor; blue = after).

The same window's gate patterns are visible as vertical stripes in the spectrograms:
[cherish_in_before_spec.png](/home/superbot/sdvx/repo/Tests.Audio/report/cherish_in_before_spec.png)
and
[cherish_in_after_spec.png](/home/superbot/sdvx/repo/Tests.Audio/report/cherish_in_after_spec.png).

### 3. Filter strength is wrong

A peaking filter at its centre frequency, +6 dB, Q4, driven by a 1 kHz sine, at three mix
values. Measured gain of the processed signal against the dry signal, 0.8-2.4 s:

| mix | before | after | law: `dry + (wet-dry)*mix` |
|---|---|---|---|
| 0.0 | **+6.00 dB** | **+0.00 dB** | 0.00 |
| 0.5 | **+6.00 dB** | **+3.51 dB** | +3.52 (6 dB halved in amplitude is +3.52 for a coherent sum at the same phase) |
| 1.0 | +6.00 dB | +6.00 dB | +6.00 |

Before the fix `BQFDSP::Process` wrote the filtered sample and ignored `mix`, so a 50%
laser filter was the full filter. That is why partial-mix filters read as "too strong" or,
for a low-pass, "too dull": the strength never scaled with the parameter. These numbers
match the predicted law in `baseline.md` to 0.01 dB.

## What changes at Cherish m053-055

`Cherish_ex.ksh`, in chart order:

1. m053.01 (69.402 s) `filtertype=peak` — laser filter, not rendered here (see limits).
2. m054.33 (71.402 s) `fx-l=Gate;16` — a 1/16 gate on the FX-L lane, held until m063.01.
3. m055.33 (72.736 s) `fx-r=Gate;16` — the same gate on FX-R, held until m060.
4. The whole stretch still carries `fx-r=Echo;16;70` from m033.01, which the pre-fix
   build lets decay to silence.

`Cherish_in.ksh`, which is where the "gate laser filter, then gate plus bitcrush" reading
comes from:

1. m054.01 (70.736 s) `filter:GA16:mix=90%` — the laser filter is a 1/16 gate at 90% wet.
2. m054.49 (71.736 s) `filter:GA16:mix=0%` — gate off.
3. m055.01 (72.069 s) `filter:GA16:mix=90%` **and** `filtertype=bitc` in the same tick —
   the laser filter's type becomes the bitcrusher while GA16's mix returns to 90%. Since
   the laser filter has one active type, what you hear from 72.069 s to 73.069 s is the
   bitcrusher; the gate is what you heard for the measure before it.
4. m055.49 (73.069 s) `filtertype=peak`, `filter:GA16:mix=0%`.

So the "gate mixed with bitcrush" impression is the handover at 72.069 s. Both handovers
were **19.3 ms late** before the fix and are within the detector floor now, and the gate
windows in between now sit on the 1/16 grid instead of 19.3 ms behind it.

## Hyper Miracle

Window m017-m024 (18.8-25.5 s): `Retrigger;8` → `Gate;16` → `Flanger` → `Phaser` →
`Gate;16` → `Gate;16`, which is the densest effect run in the chart and the section that
sounded weak. Gate phases move from 29.0 / 13.9 ms to 7.3 (polluted) / 1.9 ms, and the
window's level from **-16.11 dBFS to -15.17 dBFS** against a dry -12.67. Figures:
[hyper_before_spec.png](/home/superbot/sdvx/repo/Tests.Audio/report/hyper_before_spec.png),
[hyper_after_spec.png](/home/superbot/sdvx/repo/Tests.Audio/report/hyper_after_spec.png),
[hyper_gain.png](/home/superbot/sdvx/repo/Tests.Audio/report/hyper_gain.png).

The gain trace is the honest summary for Hyper: the fixes move the gate edges onto the
beat and recover about 1 dB of level, and do not turn a subtle section into a dramatic
one. If that section still feels weak after this change, the remaining suspects are the
flanger and phaser amounts (`FlangerDSP`'s volume/feedback defaults), not timing.

## How it was produced

1. `Tests.Audio/checks/extract_events.py` parses the chart with a `KShootMap`-shaped
   walker (blocks split by `--`, settings attached to the following tick row) and writes
   one span per effect: `case,lane,type,p0,p1,p2,mix,startMs,endMs,wholeNote`, plus the
   measure-line list the KSH gate clock needs. Chart time is
   `o + (block + tick/ticksInBlock) * barDuration`, which is what `BeatmapFromKSH`'s
   `TickToMapTime` produces for these single-tempo charts. Tempo and meter changes are
   applied at block granularity, so hyper's 3/4 section moves the grid the way the real
   timing map does.
2. `Tests.Audio/checks/render_effects.cpp` builds the DSPs the way
   `GameAudioEffects::CreateDSP` / `SetParams` does for a KSH v1 hold (`SetLength(wholeNote
   / param)`, per-type default mix from `AudioEffects.cpp`), attaches them to an
   `AudioBase` stub, and renders 384-frame blocks through the real
   `Audio/src/DSP.cpp` and `Audio/src/AudioBase.cpp`.
3. `Tests.Audio/checks/run_render.sh` compiles that source twice, against the pre-fix tree
   in a git worktree and against this tree, and renders each window with both.
4. `Tests.Audio/checks/analyze.py` measures wet-against-dry gain envelopes, gate window
   phases against the grid, and writes the spectrograms (numpy STFT, hand-written PNG,
   since this machine has neither scipy nor matplotlib). All quoted numbers are in
   `audit_out/summary.json`.

Reproduce: `Tests.Audio/checks/run_render.sh` then `python3 Tests.Audio/checks/analyze.py`.

## What is modelled, and what is not

Modelled explicitly, because it lives in `AudioPlayback` rather than in the DSP classes:

1. **The block clock.** `AudioStreamBase::Process` advances `m_samplePos` past the whole
   block before `Audio_Impl` calls `ProcessDSPs`, so the pre-fix `GetCurrentSample()`
   returns the block *end*. The render stub sets the position that way in both builds;
   the fixed `AudioBase` returns `blockStartSample` instead.
2. **Game-frame granularity.** Effects are constructed and cleared on the first 60 Hz frame
   at or after their chart time, in both builds, so a hold's first window is bounded by
   frame scheduling and every later one by the clock.
3. **`endTime`.** Stamped only in the fixed build, which is the change under test.
4. **The KSH gate clock.** `SetMeasureTimes` is called only in the fixed build, which is
   what `m_ConfigureGateClock` does now that it builds KSH measure lines from the timing
   map. The measure lines are built the same way `MeasureClock::BuildMeasureTimes` builds
   them, from per-block bar durations.

Not modelled, and therefore not claimed:

1. **`AudioPlayback` itself.** No `GameAudioEffects`, no automation events, no hold
   bookkeeping, no `SetLaserFilterInput` gating. If the game's own bus routing differs
   from a serial chain, the *level* numbers here differ; the per-DSP phase numbers do not.
2. **The laser filter bus.** A laser-filter span is applied to the whole mix at its own
   mix, not to a separate laser sample bus.
3. **Echo state at window open.** An effect already running when a window starts begins
   with empty internal state. The Cherish window opens 25 s into the m033 echo hold, so
   the pre-fix echo's collapse is *understated* here: in the game it starts decaying at
   m034, not at 68 s.
4. **Unsupported spans.** Laser `peak`, `lpf1`, `hpf1` and the custom FX `ga` / `re` are
   skipped (the laser position they follow is not modelled), and each skip is logged. In
   the Cherish windows that means `filtertype=peak` at 69.402 s and 73.069 s are absent.
5. **KSON.** Both charts are KSH v1. The KSON gate path and the native-filter path are
   unchanged by this work and untested by real data here.
6. **No game build.** This machine has no SDL2, ogg/vorbis or libarchive development
   packages, so the binary links only the DSP translation units. There is no
   `[AUDIO-PROBE]` capture from a running game in this report.

## Deferred: the peaking filter's release

Not implemented here, deliberately: timing came first, and tuning a filter's release
before the grid it sits on is correct would hide the timing bug. The measurement and the
decision are in `Tests.Audio/w13_peaking_decision.md`: the clone ends a peaking hold at
full gain after its `pfilterdelay`, while ksmaudio decays the gain over a further 50 ms
(2205 frames), and the curve itself already matches the reference tables to 0.007 dB. The
recommendation there is to land the 50 ms release as its own change, after this one, so
its effect on the sound is separable.

The zero-input guard in `AudioPlayback::m_SkipEffectIfInputIsZero` is also unchanged: it
delays DSP creation for peaking, high-pass, low-pass, pitch shift and bitcrush until the
input becomes non-zero, which starts those effects late by the length of a ramp from zero.
It is a separate revertable change, not part of this one.
