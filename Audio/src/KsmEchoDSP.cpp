#include "stdafx.h"
#include "KsmEchoDSP.hpp"
#include <algorithm>
#include <usc/KsmEchoCore.hpp>

KsmEchoDSP::KsmEchoDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
	m_core = std::make_unique<usc::fx::KsmEchoCore>(sampleRate);
}

KsmEchoDSP::~KsmEchoDSP() = default;

void KsmEchoDSP::SetLength(double lengthMs)
{
	m_lengthSec.store(static_cast<float>(std::max(0.0, lengthMs) / 1000.0), std::memory_order_relaxed);
}

void KsmEchoDSP::Process(float *out, uint32 numSamples)
{
	// ProcessDSPs gates whole blocks only; the block that contains startTime also contains frames before it.
	// Leave those dry so recording begins exactly on the authored start (as EchoDSP::Process does).
	const uint32 startSample = GetStartSample();
	const uint32 currentSample = GetCurrentSample();
	uint32 skip = 0;
	if (startSample > currentSample)
		skip = std::min(numSamples, startSample - currentSample);
	if (skip >= numSamples)
		return;

	m_core->SetParams(m_lengthSec.load(std::memory_order_relaxed),
		std::clamp(feedback, 0.0f, 1.0f),
		std::clamp(mix, 0.0f, 1.0f));
	m_core->Process(out + static_cast<size_t>(skip) * 2, numSamples - skip);
}
