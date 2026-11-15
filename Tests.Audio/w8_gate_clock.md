# w8: KSH v1 gates follow the chart's measure grid

Status: implemented and verified on this machine with a compiled check.
Scope: `GateDSP` timing only. Nothing here changes the gate's envelope shape, its
gain, or any other effect.

## The defect

`AudioPlayback::m_ConfigureGateClock` gave the gate a measure clock only for KSON
charts:

```cpp
if (dsp && effect.type == EffectType::Gate && m_beatmap->GetKSONScroll())
    static_cast<GateDSP*>(dsp)->SetMeasureTimes(m_beatmap->GetKSONBarTimes(), m_playback->audioOffset);
```

`GateDSP::Process` follows the chart only when `m_measureTimes` is non-empty. For a
KSH v1 chart the vector stayed empty, so the gate fell back to `m_currentSample = 0`
at the moment its DSP object was constructed, and free-ran from there for the whole
hold.

That construction happens on a game frame, not on the chart pulse, so the gate's open
windows sat a constant offset away from the chart's 1/16 grid: an offset equal to however
late the constructing frame was (up to one frame, ==8.98 ms measured at 120 fps==).

On Cherish (`t=180`, 4/4, `o=69`, `ver=160`) a `Gate;16` has an 83.33 ms period, so
that offset is ==11% of a cycle==. On a `Gate;16` at 50% gating the closed half is
41.7 ms, so an 9 ms shift moves a fifth of every closed window onto the beat, and every
open window lands 9 ms behind the drum. That is the smear heard as "the gate laser
starts late", and it is why the same section reads as punchy in KSM and soft here.

KSON never had this because the parser ships `m_ksonBarTimes` and the gate follows it.

## What changed

1. `Main/include/Audio/MeasureClock.hpp` (new): `MeasureClock::BuildMeasureTimes`,
   which turns a KSH timing map into sorted measure-line times in map milliseconds.
   It walks every timing point, emits lines at that point's `GetBarDuration()` until
   the next timing point, keeps the list strictly increasing when a new section starts
   off the old grid, skips non-finite or sub-0.5 ms bars, runs one bar past the last
   object so a gate that outlives the last note still has an anchor, and caps the list
   at the same 1,000,000 lines the KSON parser uses.
2. `Main/src/Audio/AudioPlayback.cpp`: `m_GetKshMeasureTimes()` builds that list once
   per chart and caches it; `m_ConfigureGateClock` now supplies the clock for both
   formats, KSON from `GetKSONBarTimes()` and KSH v1 from the timing map, both with
   `m_playback->audioOffset`. The cache is invalidated in `Init`. A new probe line
   records which source a gate got:
   `[AUDIO-PROBE] GATE_CLOCK dsp=GateDSP src=timing_map measures=81 offset=0`.
3. `Main/src/Audio/AudioPlayback.cpp`: `m_PreRenderDSPTrack` now calls
   `m_ConfigureGateClock` for the DSPs it builds, so the opt-in pre-rendered FX track
   does not keep the free-running gate.
4. `Audio/src/DSP.cpp` + `Audio/include/Audio/DSP.hpp`: `GateDSP` keeps the period
   before truncation (`m_lengthExact`) and uses it for the chart-clock phase. A 1/16
   note at 200 BPM is 3307.5 frames; truncating to 3307 made the anchored phase lose
   half a frame per period, ==0.18 ms by the 15th line of a measure==. The phase now
   lands within 0.014 ms of the grid line. `m_length` is still the integer used for
   the sample counter, so the gate envelope is untouched.

## Verification

`Tests.Audio/checks/run_w8.sh` builds the check with zig as the C++ driver (no system
compiler is installed here), linking the clone's real `Audio/src/DSP.cpp`,
`Audio/src/AudioBase.cpp` and the real `MeasureClock.hpp`. Nothing is reimplemented.

Result, on this machine:

```
[1] BuildMeasureTimes on a 200 BPM 4/4 KSH v1 timing map
  ok   first line is the map offset   first=69.000000
  ok   spacing equals the bar duration   worst error ms=0.000000
[2] BPM and signature changes
  ok   5/4 at 150 BPM gives 2000 ms bars
[3] Degenerate timing maps
  ok   a zero-length bar is skipped, not looped forever
  ok   respects the line cap   lines=500
[4] Gate;16 at 50% on a 200 BPM chart: grid alignment
      free-run     windows=63  mean offset=8.63 ms  worst=8.98 ms
      chart clock  windows=63  mean offset=0.01 ms  worst=0.01 ms
[5] Open-window length is unchanged by the clock
  ok   open window stays half the period   37.6 ms of a 75 ms period
[6] Cherish's real tempo: 180 BPM, 4/4, o=69, Gate;16
      1/16 = 83.33 ms; free-run mean 8.98 ms (11% of a period), chart clock mean 0.00 ms
[w8] PASS
```

`AudioPlayback.cpp` and `DSP.cpp` both pass a full `zig c++ -fsyntax-only` over the
real translation units with these edits (checked against a deliberately broken copy to
confirm diagnostics actually surface).

## What this does not fix

1. **It cannot make an effect start before its DSP exists.** The gate still cannot
   sound in the frames before `startTime`, so the first window of a hold is bounded by
   when the hold's DSP was constructed. The chart clock makes every *later* window land
   on the grid; landing the first one needs the effect created before its start, which
   is the block-clock and hold-scheduling work, not this.
2. **`chartOffset` is still dead.** `dsp->chartOffset` is assigned at four sites and read
   nowhere. This change does not rely on it and does not remove it.
3. **Odd gate divisions still jump at the measure line.** A `Gate;5` or `Gate;7` does not
   divide a 4/4 bar evenly, so a measure-anchored phase restarts at each bar. Every
   division KSH uses in these charts (8, 12, 16, 24, 32) divides evenly, so they do not
   jump. Upstream ksmaudio anchors on the trigger instead, which has no jump but drifts
   against the chart.
4. **KSON is only covered synthetically.** No KSON chart was available here, so the
   KSON branch is unchanged code exercised only by a synthetic measure list.
5. **No audible test.** This machine cannot build the game (no SDL2, libarchive, vorbis,
   freetype headers, and no root), so the measurement is a compiled DSP-level check, not
   a playtest of Cherish.
