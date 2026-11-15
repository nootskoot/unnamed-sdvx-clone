#pragma once

// Constructs every DSP in Audio/src/DSP.cpp directly and renders blocks through
// TestAudioBase. Returns 0 when every DSP processed cleanly, non-zero otherwise.
// Used as the no-argument entry point of Tests.Audio.
int RunAudioSmoke();
