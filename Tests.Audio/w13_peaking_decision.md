# w13: the peaking release and v easing stay out of this change

Status: recorded and measured, **not implemented**. No production file is touched by
this note. It exists because the decision to defer belongs on the record with numbers
attached, not implied by silence.

Scope: the peaking filter's behaviour over time. Its frequency and gain curves are not
in question, and section 3d shows why.

## 1. What ksmaudio does that the clone does not

All of this is `ksmaudio` at `5c2466d181aba37e3dc3c3eba6368f59b6843bcd`.

| Mechanism | Where | Behaviour |
|---|---|---|
| Release | `PeakingFilterDSP.cpp`, `kFilterReleaseSec = 0.05f` | For 2205 frames at 44.1 kHz (2400 at 48 kHz) the filter keeps running at the frequency and mix it had when it switched off, and its gain falls linearly to zero. |
| v easing | `kVEasingSpeed = 0.01f` | `v` moves toward its target by at most 1% of the 0 to 1 range per sample, so a full swing takes 100 frames (2.268 ms at 44.1 kHz). Frequency follows the eased value. |

Three details a reimplementation gets wrong if it reads only the constants:

1. **The release is laser only.** `PeakingFilterParams::renderByLaser` sets
   `releaseEnabled = true`; `renderByFX` sets it false. The upstream comment says v1
   disabled the release when falling back from p/fp audio to a realtime effect, and v2
   enables it always because the difference is small. So the afterglow belongs to the
   laser filter, which is the path these charts use.
2. **Gain and frequency use different `v`.** `updateGain` reads `params.v` and only
   recomputes when it changed; `updateFreq` reads the eased value. The source says the
   gain deliberately uses the pre-easing value so a jump does not briefly over-boost the
   low end.
3. **It fires on `mixSkipped`, not on an end time.** `mixSkipped` is
   `mix == 0 || freq < kFreqThresholdMin`. `m_mixSkippedFrames` starts at
   `m_filterReleaseFrames`, so a filter that was never active does not open with a
   phantom tail.

`peaking_filter_delay` (0 to 160 ms, the `pfilterdelay` key) is a separate, app-level
parameter that delays the laser's on/off signal. It is not this release, and section 2
shows the clone already implements it.

## 2. What the clone does today

On these charts the peaking filter takes the KSM branch:

```
Main/src/Audio/AudioPlayback.cpp:998   if (m_beatmap->GetMapSettings().kshBehaviorVersion >= 0)
Main/src/Audio/AudioPlayback.cpp:1001   if (m_laserEffectType == EffectType::PeakingFilter)
Main/src/Audio/AudioPlayback.cpp:1002       auto p = KSHFilter::Peak(input, gain, rate);
Main/src/Audio/AudioPlayback.cpp:1004       dsp->mix = 1; return;
```

`kshBehaviorVersion` comes from the chart header (`Beatmap/src/BeatmapFromKSH.cpp:738`),
so a `ver 160` chart takes this branch: fully wet, gain from the curve.

The chart's afterglow parameter is implemented, but as a **rectangular hold**:

```
Main/src/Audio/AudioPlayback.cpp:625   if (m_laserEffect.type == EffectType::PeakingFilter &&
Main/src/Audio/AudioPlayback.cpp:626       GetMapSettings().peakingFilterDelay > 0 && ...)
Main/src/Audio/AudioPlayback.cpp:627       active = m_laserAudioSampler.Sample(
Main/src/Audio/AudioPlayback.cpp:628           GetLastTime() - peakingFilterDelay, eligibleLanes, input);
```

Cherish sets `pfilterdelay=30` (`Cherish_ex.ksh:19`), so the filter stays active at
**full gain** for 30 ms after the laser ends, then stops. ksmaudio holds for the same
30 ms and then decays the gain to zero over another 50 ms.

There is no time-based gain ramp anywhere in `BQFDSP` or in the peaking path (`grep` for
"release" finds only the gate's 12-frame release and `SidechainDSP`'s parameter), and no
easing: coefficients are set from the game thread once per frame, and only when an
automation entry fired or the timing point changed (`AudioPlayback.cpp:250`, early return
at `AudioPlayback.cpp:314`). A stationary laser during a hold does not touch the filter.

So the difference is the last 50 ms: the clone ends at full gain and the reference fades.

## 3. Measurements on this machine

`Tests.Audio/checks/run_w13.sh` builds the clone's real `BQFDSP` with zig and compares
the handover against the reference arithmetic. Only the reference side is a
transcription; the clone side is the shipped class.

### 3a. The handover, +6 dB peaking at 1 kHz, Q4, 44.1 kHz stereo

Windowed RMS over 64 frames, relative to the dry input. Frame 0 is the moment the hold
would end without the 30 ms delay already elapsed:

| t from end, ms | frames | clone dB | release dB |
|---|---|---|---|
| -1.45 | -64 | 6.00 | 6.00 |
| 0.00 | 0 | 3.68 | 5.98 |
| 1.45 | 64 | 1.37 | 5.88 |
| 4.35 | 192 | 0.16 | 5.60 |
| 8.71 | 384 | 0.00 | 5.08 |
| 20.32 | 896 | 0.00 | 3.67 |
| 36.28 | 1600 | 0.00 | 1.74 |
| 50.79 | 2240 | 0.00 | 0.05 |
| 55.15 | 2432 | 0.00 | 0.00 |

The clone reads 3.68 dB at frame 0 only because a 64-frame window straddles the cut; the
gain actually steps in one sample. The reference is still at 5.98 dB there and needs the
full 50 ms.

### 3b. It is not a click

| | step at the switch | largest natural step before it | ratio |
|---|---|---|---|
| clone (instant) | 0.04734 | 0.07101 | 0.67x |
| reference (2205-frame ramp) | 0.04811 | 0.07101 | 0.68x |

Neither handover steps more than the signal's own motion on a steady tone. So the missing
release should not be described as a click fix: what is missing is 50 ms of afterglow,
which is what the upstream comment claims for it.

### 3c. What the easing actually buys

Same filter, frequency jumped 300 Hz to 3000 Hz at +6 dB, with and without the 100-frame
glide (the check interpolates frequency linearly in Hz as a stand-in for easing in `v`):

| | step ratio at the jump | peak ring, 20 ms after |
|---|---|---|
| snap | 1.04x | +0.01 dB |
| eased | 1.32x | +2.11 dB |

Easing turns a jump into a 2.27 ms sweep, and the sweep is the louder event. That is a
change of character, not noise reduction.

### 3d. The curve is not the problem

The earlier audit called `KSHFilter::Peak` an approximation of the 256-entry tables. It
is the same function: both come from the KSM v1 formula
(`play_audio_effects.hsp:974`, cited in the upstream comment), one evaluated in closed
form and one precomputed at 256 points. Evaluating the clone's closed form at each table
index above the 100 Hz threshold gives:

| Quantity | Worst difference |
|---|---|
| Gain | 0.007 dB (mean absolute 0.0023 dB) |
| Frequency | 3.04 Hz at v = 1 (clone 8996.96 Hz, table 9000.00 Hz) |
| Peak of the curve | 33.983 dB at v = 0.349, table 33.984 dB at index 89 |

The curve stays as it is. The only gap in this filter is the time behaviour.

## 4. Where this lands in the charts you named

This corrects the premise of the earlier audit, which treated Cherish m53 as a gate.

In `Cherish_ex.ksh`, measure by measure (measure index from the `--` separators, as
`BeatmapFromKSH` reads them):

| Measure | Directive |
|---|---|
| m049 | `filtertype=lpf1` |
| **m053** | **`filtertype=peak`** |
| m054 | `fx-l=Gate;16` |
| m055 | `fx-r=Gate;16` |
| m057 | `pfiltergain=10`, `filtertype=hpf1`, then `filtertype=peak` |
| m058 | `pfiltergain=25` |
| m061 | `pfiltergain=50`, `fx-r=Gate;16` |

So the laser filter through m53 is the **peaking** filter, and the gate holds arrive at
m54 and m55 on the button lanes. That means this deferred item is inside the section you
described, not beside it: at m53 the clone ends the peaking filter at full gain where KSM
fades it, and the `pfiltergain` steps at m57 and m58 (50, then 10, then 25) are gain
changes on that same filter.

Across the three Cherish files there are 28 `filtertype=peak` directives, so this is not
a corner of the chart either.

## 5. Why it still stays out of this change

1. **Timing first.** The start-time and end-time fixes decide *when* the filter stops.
   Tuning what happens in the 50 ms after it stops is meaningless until that frame is
   correct.
2. **It would confound the mix change.** `BQFDSP` just gained a wet/dry law. A release in
   the same pass means a listener cannot tell which change they are hearing, and a
   regression would be attributed to both.
3. **It has to run per sample, inside `BQFDSP`.** The coefficients are set from the game
   thread once per frame, so a release driven from there would have 3 to 6 steps instead
   of 2205. Doing it properly means the DSP keeps its last frequency, gain and mix and
   recomputes coefficients while fading, which is new state in a class this branch already
   changed.
4. **It is additive, so it can wait.** Unlike the clock bug, this one does not move
   anything else; it extends a tail by 50 ms. That makes it the cheapest thing to defer
   and the easiest to land on its own.

## 6. The decision, and what would reverse it

Decision: no release, no easing in this change. Recorded here, not implemented.

Reverse it when any one of these holds. Each is a test that fails today:

1. A peaking laser section ends and the difference against the reference over the 50 ms
   after the end exceeds 3 dB in the boosted band. Today it exceeds 6 dB (3a).
2. A laser that flickers across a judgment boundary makes the clone's filter stutter,
   which is the continuity case the upstream comment names. Needs a flicker chart to
   reproduce.
3. The report's Cherish or Hyper Miracle spectrograms show a peaking section that ends
   square while the reference decays.

## 7. What a later implementation must do

1. **State lives in `BQFDSP`**, not `AudioPlayback`: last frequency, gain, mix, and the
   frame the filter switched off, because the release has to keep running after the game
   thread stops calling `SetPeaking`.
2. **Precedence:** a new `SetPeaking` cancels a release in progress; a release starts when
   gain reaches zero or when `endTime` arrives.
3. **Shape:** frozen frequency, frozen mix, gain falling linearly to zero over 50 ms. That
   is what the reference does.
4. **Laser only.** `renderByFX` disables it upstream, so the FX and named-filter paths must
   not get it.
5. **Keep the 100 Hz skip.** `KSHFilter::Peak` already returns gain 0 below 100 Hz, matching
   `kFreqThresholdMin`.
6. **No phantom tail on start**, as `PeakingFilterRelease` does by seeding its skipped
   counter.
7. **Cost:** one coefficient recompute per frame per channel across 2205 frames, on the
   audio thread. If that measures too high, interpolate the biquad coefficients linearly
   between the on and off states, which is audibly close and not sample identical.
8. **Test against the trajectory in 3a**, not a click metric, because 3b shows there is no
   click.

## 8. What this note does not claim

1. That the peaking filter sounds wrong in general. The curve matches; the afterglow is
   missing.
2. That the release explains the whole m53 problem. The m54 and m55 gates are separate
   work, and the gate's own timing and ramp are tracked there.
3. That ksmaudio's easing reduces artifacts. 3c measures the opposite on a frequency jump.
4. That section 3 is a chart render. It is a 1 kHz sine through the shipped `BQFDSP`,
   which shows the shape of the difference, not its level in a mix.
