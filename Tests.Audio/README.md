# Tests.Audio

Offline harness for the DSP chain in `Audio/src/DSP.cpp`. It constructs DSPs
directly against a stub `AudioBase` (`include/TestAudioBase.hpp`) and renders
blocks in memory. No audio device, no window, no chart required.

## Two ways to build

**With the repo's CMake tree** (needs the full dependency set: SDL2, ogg,
vorbis, libarchive, freetype, libpng, libjpeg, zlib):

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Tests.Audio
./bin/Tests.Audio
```

**Standalone, with zig** (needs only zig; no system development packages):

```
Tests.Audio/build_audit.sh              # build, then run the smoke table
Tests.Audio/build_audit.sh --build-only # build only
Tests.Audio/build_audit.sh --clean      # drop the build tree first
ZIG=/path/to/zig Tests.Audio/build_audit.sh
```

The standalone path compiles `Audio/src/DSP.cpp` and `Audio/src/AudioBase.cpp`
plus Shared, Tests and the vendored soundtouch, and nothing else from Audio.

### Why the standalone build exists

The CMake target links the whole `Audio` library, which pulls in the stream
stack (`AudioStreamOgg`, `SampleRes`, the SDL output backend) and therefore
every audio development package. The harness needs none of that, so
`build_audit.sh` compiles only what the DSP chain touches and links with
`--gc-sections`. On a machine with no SDL2, ogg, vorbis or libarchive headers,
that is the only build that works.

Two files are shims, and both are limited to the device boundary:

1. `shim/SDL2/SDL.h`: empty. `Audio/stdafx.h` includes it; neither DSP unit
   references an SDL symbol. If that ever changes, the link fails by name.
2. `shim/Audio_ImplStandalone.cpp`: defines `Audio_Impl::GetSampleRate()` only,
   and only under `-DUSC_AUDIO_AUDIT_STANDALONE` which `build_audit.sh` sets.
   The CMake build gets the real definition from `Audio/src/Audio.cpp`, so
   there is no duplicate symbol either way.

## The smoke table

Running with no arguments renders 384-frame blocks of a 440 Hz sine through
every DSP in `DSP.cpp` and reports peak and RMS per DSP:

```
Tests.Audio/build/Tests.Audio
```

A DSP fails if it emits a non-finite sample, or if it is marked
`expectAudible` and emits silence. Silence from a live DSP usually means a
null buffer, an unset length or a stale mix, which is the failure class this
branch is about.

`EchoDSP` runs at `mix = 1` with a 20 ms delay so the wet path is exercised
inside the 3072 frames rendered. At the 80 ms used earlier the delay was longer
than the whole render and the fully wet echo was correctly, and uselessly,
silent.

## Adding DSP tests

Put new `.cpp` files in `src/`; both build paths pick them up. `src/reference/`
holds frame-by-frame transcriptions of the ksmaudio DSPs at
`5c2466d181aba37e3dc3c3eba6368f59b6843bcd`, used as the golden side of a
comparison. See `src/reference/README.md` for provenance and the units trap
(ksmaudio takes a wave length in seconds, the clone's `SetLength` takes
milliseconds).

Never call `AudioBase::AddDSP` from a test: it dereferences the `Audio_Impl`
pointer, which is null here. Use `TestAudioBase::Attach` instead.
