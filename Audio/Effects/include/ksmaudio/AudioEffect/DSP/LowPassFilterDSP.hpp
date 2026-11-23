#pragma once
#include "ksmaudio/AudioEffect/AudioEffect.hpp"
#include "ksmaudio/AudioEffect/Params/LowPassFilterParams.hpp"
#include "ksmaudio/AudioEffect/detail/BiquadFilter.hpp"
#include "ksmaudio/AudioEffect/detail/LinearEasing.hpp"

namespace ksmaudio::AudioEffect
{
	class LowPassFilterDSP
	{
	private:
		const DSPCommonInfo m_info;
		std::array<detail::BiquadFilter<float>, 2> m_lowPassFilters;
		detail::LinearEasing<float> m_vEasing;
		float m_prevFreq = 15000.0f;
		float m_prevFreqMax = 800.0f;
		float m_prevVAtFreq = 0.0f;
		float m_prevVAtFreqMax = 1.0f;
		float m_prevQ = 3.6f;

	public:
		explicit LowPassFilterDSP(const DSPCommonInfo& info);

		void process(float* pData, std::size_t dataSize, bool bypass, const LowPassFilterDSPParams& params);

		void updateParams(const LowPassFilterDSPParams& params);
	};
}
