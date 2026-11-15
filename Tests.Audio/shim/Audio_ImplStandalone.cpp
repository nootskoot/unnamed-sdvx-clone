/*
	Standalone definitions for the two Audio_Impl members AudioBase.cpp calls.

	Why this file exists
	--------------------
	AudioBase::ProcessDSPs reads the device rate through Audio_Impl:

	    const uint32 outputRate = audio ? audio->GetSampleRate() : sampleRate;

	`audio` is null in the harness (TestAudioBase never registers with a real
	Audio_Impl), so the call never executes at run time, but the symbol still has
	to resolve at link time. Audio_Impl's real definitions live in
	Audio/src/Audio.cpp, and that translation unit drags in the whole stream
	stack (AudioStream::Create, AudioStreamOgg, SampleRes::Create, AudioOutput's
	SDL backend), which needs ogg, vorbis, libarchive and SDL2 development
	packages that this machine does not have.

	So the zig build compiles Audio/src/DSP.cpp and Audio/src/AudioBase.cpp only,
	and this file supplies the one member that is then left undefined.

	Why that is not a fake
	----------------------
	Nothing here reimplements DSP, mixing or timing. It is the device boundary,
	and the harness by definition has no device: GetSampleRate() reports 0, which
	is what "no output device is open" means, and AudioBase::ProcessDSPs already
	guards a zero output rate on line 85 (`outputRate ? ... : 1.0`).

	The macros
	----------
	USC_AUDIO_AUDIT_STANDALONE is defined by Tests.Audio/build_audit.sh and by
	nothing else. The CMake target links the real Audio library, which provides
	these symbols, so under CMake this file compiles to nothing and there is no
	duplicate definition in either build.
*/

#include <Shared/Shared.hpp> // uint32/uint64/Vector, as Audio/stdafx.h provides
#include <Audio/Audio_Impl.hpp>

#ifdef USC_AUDIO_AUDIT_STANDALONE

uint32 Audio_Impl::GetSampleRate() const
{
	// No device. See the comment block at the top of this file.
	return 0;
}

#endif // USC_AUDIO_AUDIT_STANDALONE
