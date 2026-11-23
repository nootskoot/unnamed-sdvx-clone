#pragma once
#include "ksmaudio/AudioEffect/AudioEffect.hpp"
#include "ksmaudio/AudioEffect/Params/FlangerParams.hpp"
#include "ksmaudio/AudioEffect/detail/RingBuffer.hpp"
#include "ksmaudio/AudioEffect/detail/BiquadFilter.hpp"

namespace ksmaudio::AudioEffect
{
	class FlangerDSP
	{
	private:
		const DSPCommonInfo m_info;
		detail::RingBuffer<float> m_ringBuffer;
		float m_lfoTimeRate = 0.0f;
		std::array<detail::BiquadFilter<float>, 2> m_lowShelfFilters;

	public:
		explicit FlangerDSP(const DSPCommonInfo& info);

		void process(float* pData, std::size_t dataSize, bool bypass, const FlangerDSPParams& params);

		void updateParams(const FlangerDSPParams& params);
	};
}
