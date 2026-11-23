#pragma once
// Test stand-in for USC's DSP base (Audio/include/Audio/AudioBase.hpp). Same protected/public surface that
// KsmEchoDSP uses; GetCurrentSample() is driven by the test via testBlockStart.
#include <atomic>
#include <cstdint>
#include "stdafx.h"
class DSP
{
protected:
	DSP() = default;
	DSP(const DSP &) = delete;
	uint32 GetStartSample() const { return static_cast<uint32>((uint64)startTime * m_sampleRate / 1000); }
	uint32 GetCurrentSample() const { return static_cast<uint32>(testBlockStart); }
	uint32 m_sampleRate = 0;
public:
	virtual ~DSP() = default;
	void SetSampleRate(uint32 sr) { m_sampleRate = sr; }
	virtual void Process(float *out, uint32 numSamples) = 0;
	virtual const char *GetName() const = 0;
	float mix = 1.0f;
	std::atomic<bool> trigger{true};
	uint32 priority = 0, startTime = 0, endTime = 0;
	uint64 testBlockStart = 0;
};
