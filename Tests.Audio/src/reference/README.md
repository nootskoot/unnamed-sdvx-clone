# Tests.Audio/src/reference

Golden-reference transcriptions of the two ksmaudio DSPs the clone's gate and
echo/retrigger path is measured against. This directory is the "known good"
side of every clone-vs-reference comparison in `Tests.Audio`; nothing here is
included by the game build.

## Provenance

| What | Where |
|---|---|
| Upstream repo | `kshootmania/ksmaudio` |
| Commit | `5c2466d181aba37e3dc3c3eba6368f59b6843bcd` |
| Gate | `src/AudioEffect/DSP/GateDSP.cpp`, `include/ksmaudio/AudioEffect/DSP/GateDSP.hpp`, `include/ksmaudio/AudioEffect/Params/GateParams.hpp` |
| Retrigger / Echo | `src/AudioEffect/DSP/RetriggerEchoDSP.cpp`, `include/ksmaudio/AudioEffect/DSP/RetriggerEchoDSP.hpp`, `include/ksmaudio/AudioEffect/Params/RetriggerEchoParams.hpp` |
| Shared primitives | `include/ksmaudio/AudioEffect/detail/DSPSimpleTriggerHandler.hpp`, `include/ksmaudio/AudioEffect/detail/LinearBuffer.hpp` |
| Read on | 2026-11-14 |

Transcribed, not vendored: the arithmetic is copied line for line, the file
layout is this repository's. ksmaudio at this commit carries no root `LICENSE`
(only `THIRD_PARTY_LICENSE.txt`), so if the transcription is ever pasted into
production code rather than a test target, check the licence first.

## Layout

| File | Contents |
|---|---|
| `ReferenceCommon.hpp` | `DSPCommonInfo`, `SimpleTriggerHandler`, `LinearBuffer<float>`, `Lerp`, and the two de-click constants |
| `GateDSP.hpp` | `GateParams`, `Scale()`, `GateDSP` |
| `RetriggerEchoDSP.hpp` | `RetriggerEchoParams`, `MakeEchoParams`, `MakeRetriggerParams`, `RetriggerEchoDSP` |

Everything is header-only and lives in `namespace audio_audit::reference`. Two
reasons:

1. `Tests.Audio/src` is already on the include path, so a test-only source can
   `#include "reference/GateDSP.hpp"` with no CMake change. A recursive glob is
   not needed and `Tests.Audio/CMakeLists.txt` (which globs `src/*.cpp`) does
   not have to know this directory exists.
2. The reference deliberately does not derive from the clone's `DSP` or touch
   `AudioBase`. If it shared the clone's base class, the block-clock bug this
   audit is chasing would apply to both sides and cancel out in the diff.

Every header includes what it uses, so it compiles with or without the target's
precompiled header and in any include order.

Entry points are PascalCase (`Process`, `UpdateParams`, `Reset`) to match the
clone's `DSP` interface, so a harness can drive either side with the same call.

Note the name clash: `audio_audit::reference::GateDSP` and the global `GateDSP`
in `Audio/include/Audio/DSP.hpp` are different types. Do not `using namespace
audio_audit::reference;` in a translation unit that also includes `Audio/DSP.hpp`.

## Semantics worth knowing before comparing

### Gate (`GateDSP.hpp`)

| | Clone (`Audio/src/DSP.cpp`, `GateDSP`) | Upstream |
|---|---|---|
| Attack | fades in over `m_fadeIn = min(5% of period, ...)` | none, full gain from frame 0 |
| Open window | full gain until `m_fadeOut = m_halfway - m_fadeIn` | full gain for all `numNonZeroFrames` |
| Release | ramps *inside* the window, then again at the period end | 12 frames *after* the window, `(12 - k) / 13` |
| Short gates | ramp shrinks but still eats the window | no ramp at all when `numNonZeroFrames <= 12` |
| `mix` | `c * mix + (1 - mix)` applied to the gain | `Lerp(1, scale, mix)` per frame |

The guard is `numNonZeroFrames > kDeclickFrames` with `kDeclickFrames = 12`.
Both are asserted at compile time in `GateDSP.hpp` (`namespace assertions`), so
a future "fix" that reintroduces an attack ramp or drops the guard fails the
build instead of quietly changing the golden side.

### Echo / Retrigger (`RetriggerEchoDSP.hpp`)

The topology is a recorded dry buffer played back in loops, not a feedback
delay line:

1. `Write()` stores the block's **dry input** at the write cursor.
2. `Read()` blends the delayed material **over** the input:
   `*pData = Lerp(*pData, wet, mix)`. The dry signal survives at every mix.
3. Feedback is a **per-loop gain**: at each loop boundary
   `m_currentFadeOutScale *= feedbackLevel`. Nothing is written back into the
   buffer.
4. In echo mode the wet sample is additionally scaled by
   `(1.25 * loopFrames - k) / (1.25 * loopFrames)`.
5. A trigger landing inside a block splits the block at that frame, resets both
   cursors, and processes the remainder, so the retrigger is sample-accurate.
6. De-click is 12 frames in and 12 frames out with the guard
   `numNonZeroFrames > 12 && numNonZeroFrames != numLoopFrames`; echo mode
   (`rate == 1.0`) has no de-click by design.

The clone's `EchoDSP` differs in three ways at once: it overwrites the output
with `wet * mix` (no dry), it writes `out * feedback` back into the buffer (so
feedback compounds per loop and is scaled by mix), and it can only restart on a
block boundary.

## Units trap

Upstream `waveLength` is in **seconds**:

```cpp
const std::size_t numPeriodFrames = static_cast<std::size_t>(params.waveLength * m_info.sampleRate);
```

The clone's `GateDSP::SetLength`, `EchoDSP::SetLength` and
`RetriggerDSP::SetLength` take **milliseconds**. A harness that feeds the same
chart value to both sides without converting compares a 1 ms gate against a
1 s gate and will "confirm" a bug that does not exist. Convert once, at the
boundary, and keep the chart value in one unit internally.

## Driving it

```cpp
#include "reference/GateDSP.hpp"
#include "reference/RetriggerEchoDSP.hpp"

audio_audit::reference::DSPCommonInfo info(44100, 2);

audio_audit::reference::GateDSP gate(info);
audio_audit::reference::GateParams gp;
gp.waveLength = 0.0625f;   // seconds, not ms
gp.rate = 0.5f;
gp.mix = 1.0f;
gp.secUntilTrigger = 0.0f; // land the trigger on the first frame of the render
gate.UpdateParams(gp);

gate.Process(block, /*frames*/ 384 * 2, /*bypass*/ false, gp);
```

`dataSize` is samples, not frames (`frameSize = dataSize / numChannels`), same
as upstream. Feed 384-frame stereo blocks to match the clone's mixer.
