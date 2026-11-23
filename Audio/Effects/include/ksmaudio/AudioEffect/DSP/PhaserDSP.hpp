#pragma once
#include "ksmaudio/AudioEffect/AudioEffect.hpp"
#include "ksmaudio/AudioEffect/Params/PhaserParams.hpp"
#include "ksmaudio/AudioEffect/detail/RingBuffer.hpp"
#include "ksmaudio/AudioEffect/detail/BiquadFilter.hpp"

namespace ksmaudio::AudioEffect
{
	class PhaserDSP
	{
	public:
		static constexpr std::size_t kMaxNumAllPassFilters = 12U;

	private:
		const DSPCommonInfo m_info;
		float m_lfoTimeRate = 0.0f;
		std::array<std::array<detail::BiquadFilter<float>, 2>, kMaxNumAllPassFilters> m_allPassFilters;
		std::array<detail::BiquadFilter<float>, 2> m_hiCutFilters;
		std::array<std::array<float, 2>, kMaxNumAllPassFilters> m_prevWetArrayForFeedback;

	public:
		explicit PhaserDSP(const DSPCommonInfo& info);

		void process(float* pData, std::size_t dataSize, bool bypass, const PhaserDSPParams& params);

		void updateParams(const PhaserDSPParams& params);
	};
}
