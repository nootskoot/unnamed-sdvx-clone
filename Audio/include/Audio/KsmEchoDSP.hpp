#pragma once
#include "AudioBase.hpp"
#include <atomic>
#include <memory>

namespace usc::fx { class KsmEchoCore; }

/*
	Drop-in replacement for EchoDSP with kshootmania's (ksmaudio) echo behaviour: it records from the moment
	the effect starts, loops that segment every SetLength() milliseconds, and each repeat is quieter by
	`feedback` (a per-loop decay multiplier, not a recirculating gain). Same public surface as EchoDSP:
	SetLength(ms), feedback, mix (DSP::mix, 0 dry .. 1 wet).
*/
class KsmEchoDSP : public DSP
{
public:
	explicit KsmEchoDSP(uint32 sampleRate);
	~KsmEchoDSP() override;

	// Loop length in milliseconds. Changing it mid-note keeps the recorded audio (no buffer clear).
	void SetLength(double lengthMs);

	// Per-loop decay, 0..1. 1.0 holds the loop (apart from the in-loop fade ksmaudio applies).
	float feedback = 0.6f;

	void Process(float *out, uint32 numSamples) override;
	[[nodiscard]]
	const char *GetName() const override { return "KsmEchoDSP"; }

private:
	std::unique_ptr<usc::fx::KsmEchoCore> m_core;
	std::atomic<float> m_lengthSec{ 0.0f };
};
