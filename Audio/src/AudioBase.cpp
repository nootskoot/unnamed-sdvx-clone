#include "stdafx.h"
#include "AudioBase.hpp"
#include "Audio.hpp"
#include "Audio_Impl.hpp"

uint32 DSP::GetStartSample() const
{
	return static_cast<uint32>(startTime * static_cast<double>(m_audioBase->GetSampleRate()) / 1000.0);
}

uint32 DSP::GetCurrentSample() const
{
	if (!m_audioBase)
		return 0;

	// The block start, not the stream position: AudioStreamBase::Process finishes
	// by advancing the stream past the block it just decoded, so GetSamplePos()
	// would hand every DSP the first frame of the next block and every startTime
	// comparison would be one buffer late.
	return static_cast<uint32>(m_audioBase->blockStartSample);
}

DSP::~DSP()
{
	// Make sure this is removed from parent
	assert(!m_audioBase);
}

bool DSP::Sorter(DSP *&a, DSP *&b)
{
	if (a->priority == b->priority)
	{
		return a->startTime < b->startTime;
	}
	return a->priority < b->priority;
}

void DSP::SetAudioBase(class AudioBase *audioBase)
{
	if (!audioBase)
	{
		m_audioBase = nullptr;

		return;
	}

	m_audioBase = audioBase;
}

AudioBase::~AudioBase()
{
	// Check this to make sure the audio is not being destroyed while it is still registered
	assert(!audio);
	assert(DSPs.empty());
}
uint32 AudioBase::GetAudioSampleRate() const
{
	return audio->GetSampleRate();
}
void AudioBase::ProcessDSPs(float *out, uint32 numSamples)
{
	if (!numSamples || DSPs.empty())
		return;

	const uint32 sampleRate = GetSampleRate();
	if (!sampleRate)
	{
		// A stream with no rate cannot be converted to samples; run the chain as-is
		// rather than gating every effect out on a division that would round to zero.
		for (DSP *dsp : DSPs)
			dsp->Process(out, numSamples);
		return;
	}
	// An effect covers [startTime, endTime) in milliseconds of stream time. endTime == 0
	// means the effect has no authored end and runs until it is removed from the chain.
	// The comparison uses blockStartSample, the first frame of the block about to be
	// modified, because GetSamplePos() has already been advanced past this block by
	// AudioStreamBase::Process. That is what stops a hold from sounding in the frames
	// after its chart end, instead of relying on ClearEffect landing on a frame edge.
	const uint64 blockStart = blockStartSample;
	const uint32 outputRate = audio ? audio->GetSampleRate() : sampleRate;
	// A source frame is consumed outputRate -> sampleRate, and PlaybackSpeed scales that,
	// so a full block of output frames covers this many source samples.
	const double speed = PlaybackSpeed > 0.0f ? double(PlaybackSpeed) : 1.0;
	const double sourceStep = outputRate ? double(sampleRate) / double(outputRate) * speed : 1.0;
	const uint64 blockEnd = blockStart + (uint64)(double(numSamples) * sourceStep + 0.5);

	// startTime is chart time while blockStartSample is audio time, and the two are only
	// equal when the player's calibration offset is 0. A hold's DSP is created on the tick
	// that reaches its start, so its start is already behind the render cursor; a small
	// slack keeps a non-zero calibration offset from turning the start bound into a delay,
	// while a DSP that really is scheduled ahead stays silent until its window opens.
	const uint64 startSlack = (uint64)sampleRate * 250 / 1000;

	for (DSP *dsp : DSPs)
	{
		if (dsp->startTime)
		{
			const uint64 startSample = (uint64)dsp->startTime * (uint64)sampleRate / 1000;
			if (startSample >= blockEnd + startSlack)
				continue;
		}
		uint32 samples = numSamples;
		if (dsp->endTime)
		{
			const uint64 endSample = (uint64)dsp->endTime * (uint64)sampleRate / 1000;
			if (endSample <= blockStart)
				continue;
			// The block that contains the authored end renders only up to it, so the
			// effect stops on the end frame instead of at the end of the buffer. The
			// sub-buffer still begins at blockStart, so per-sample position maths inside
			// the DSP stays correct.
			if (endSample < blockEnd)
			{
				const uint64 remainingSource = endSample - blockStart;
				const uint64 remainingOutput = (uint64)(double(remainingSource) / sourceStep + 0.5);
				if (remainingOutput == 0)
					continue;
				if (remainingOutput < (uint64)samples)
					samples = (uint32)remainingOutput;
			}
		}
		dsp->Process(out, samples);
	}
}
void AudioBase::AddDSP(DSP *dsp)
{
	audio->lock.lock();
	DSPs.AddUnique(dsp);
	// Sort by priority
	DSPs.Sort([](DSP *l, DSP *r) {
		if (l->priority == r->priority)
			return l < r;
		return l->priority < r->priority;
	});
	dsp->SetAudioBase(this);
	audio->lock.unlock();
}
void AudioBase::RemoveDSP(DSP *dsp)
{
	assert(DSPs.Contains(dsp));

	audio->lock.lock();
	DSPs.Remove(dsp);
	dsp->SetAudioBase(nullptr);
	audio->lock.unlock();
}

void AudioBase::Deregister()
{
	// Remove from audio manager
	if (audio)
	{
		audio->Deregister(this);
	}

	// Unbind DSP's
	// It is safe to do here since the audio won't be rendered again after a call to deregister
	for (DSP *dsp : DSPs)
	{
		dsp->RemoveAudioBase();
	}
	DSPs.clear();
}
