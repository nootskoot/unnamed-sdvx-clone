/*
	Stub SDL2 header for the offline Tests.Audio harness.

	Why: Audio/stdafx.h includes SDL2/SDL.h, and every Audio/src unit includes that
	precompiled header. The harness only builds two of those units,
	Audio/src/DSP.cpp and Audio/src/AudioBase.cpp, and neither references an SDL
	symbol at all:

	    grep -c SDL Audio/src/DSP.cpp Audio/src/AudioBase.cpp
	    Audio/src/DSP.cpp:0
	    Audio/src/AudioBase.cpp:0

	So this header declares nothing. It exists only to satisfy the include, which
	keeps the harness compiling on a machine with no SDL2 development package.

	This is deliberately not a fake SDL. If a future change makes a unit the
	harness builds call an SDL function, the link step fails with an undefined
	symbol naming it, and the fix is to either build against real SDL2 or drop
	that unit from build_audit.sh. Nothing is silently stubbed.
*/
#pragma once

// Intentionally empty. See the comment above.
