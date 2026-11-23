#include "usc/KsmEchoCore.hpp"
#include "ksmaudio/AudioEffect/DSP/RetriggerEchoDSP.hpp"

namespace usc::fx
{
	using namespace ksmaudio::AudioEffect;

	struct KsmEchoCore::Impl
	{
		RetriggerEchoDSP dsp;
		RetriggerEchoDSPParams params;
		explicit Impl(uint32_t sr) : dsp(DSPCommonInfo{ sr, 2U })
		{
			params.rate = 1.0f;     // Echo: fixed, as EchoParams::render*
			params.isEcho = true;   // Echo: fixed
		}
	};

	KsmEchoCore::KsmEchoCore(uint32_t sampleRate) : m_impl(std::make_unique<Impl>(sampleRate)) {}
	KsmEchoCore::~KsmEchoCore() = default;

	void KsmEchoCore::SetParams(float waveLengthSec, float feedbackLevel, float mix)
	{
		m_impl->params.waveLength = waveLengthSec;
		m_impl->params.feedbackLevel = feedbackLevel;
		m_impl->params.mix = mix;
	}

	void KsmEchoCore::Process(float* stereo, uint32_t frames)
	{
		m_impl->dsp.process(stereo, static_cast<std::size_t>(frames) * 2U, false, m_impl->params);
	}

	void KsmEchoCore::Reset()
	{
		m_impl->dsp.reset();
	}
}
