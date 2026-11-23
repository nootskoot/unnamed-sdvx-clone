#pragma once
#include "ksmaudio/AudioEffect/AudioEffect.hpp"
#include "ksmaudio/AudioEffect/Params/WobbleParams.hpp"
#include "ksmaudio/AudioEffect/detail/BiquadFilter.hpp"
#include "ksmaudio/AudioEffect/detail/DSPSimpleTriggerHandler.hpp"

namespace ksmaudio::AudioEffect
{
	class WobbleDSP
	{
	private:
		const DSPCommonInfo m_info;
		detail::DSPSimpleTriggerHandler m_triggerHandler;
		std::array<detail::BiquadFilter<float>, 2> m_lowPassFilters;

	public:
		explicit WobbleDSP(const DSPCommonInfo& info);

		void process(float* pData, std::size_t dataSize, bool bypass, const WobbleDSPParams& params);

		void updateParams(const WobbleDSPParams& params);

		void reset();
	};
}
