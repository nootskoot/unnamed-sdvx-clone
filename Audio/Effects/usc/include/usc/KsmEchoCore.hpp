#pragma once
// C++17-safe façade over the vendored ksmaudio Echo (RetriggerEchoDSP, isEcho=true).
// No ksmaudio headers are exposed, so USC's C++17 targets can include this and link AudioEffects.
#include <cstdint>
#include <memory>

namespace usc::fx
{
	class KsmEchoCore
	{
	public:
		explicit KsmEchoCore(uint32_t sampleRate);
		~KsmEchoCore();
		KsmEchoCore(const KsmEchoCore&) = delete;
		KsmEchoCore& operator=(const KsmEchoCore&) = delete;

		// waveLengthSec: loop length. feedbackLevel: per-loop decay multiplier (0..1). mix: 0 dry .. 1 wet.
		void SetParams(float waveLengthSec, float feedbackLevel, float mix);

		// Interleaved stereo. Recording starts at the first call (equivalent to ksmaudio's update_trigger "off>on").
		void Process(float* stereo, uint32_t frames);

		// Restart recording (== update trigger).
		void Reset();

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
