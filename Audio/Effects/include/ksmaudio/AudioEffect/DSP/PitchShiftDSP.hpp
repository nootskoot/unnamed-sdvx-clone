#pragma once
#include "ksmaudio/AudioEffect/AudioEffect.hpp"
#include "ksmaudio/AudioEffect/Params/PitchShiftParams.hpp"
#include "ksmaudio/AudioEffect/detail/BiquadFilter.hpp"
#include <array>
#include <vector>
#include <cstddef>
#include <optional>

namespace ksmaudio::AudioEffect
{
	class PitchShiftDSP
	{
	private:
		static constexpr std::size_t kDelayBufferMax = 44100 * 4;
		static constexpr std::size_t kNumLowpassFilters = 3;

		const DSPCommonInfo m_info;

		float m_pitch = 0.0f;
		float m_pitchPrev = 0.0f;
		std::vector<std::array<float, kDelayBufferMax>> m_delayBuffer;
		float m_playSpeed = 1.0f;
		float m_overlap = 0.25f;
		std::size_t m_cursor = 0;
		std::size_t m_count = 0;
		std::size_t m_chunkSize = 1378;
		std::size_t m_start = 0;
		std::size_t m_prevStart = 0;
		std::size_t m_prevPrevStart = 0;
		std::optional<std::size_t> m_thirdChunkBlendStep = std::nullopt;
		std::vector<std::array<detail::BiquadFilter<float>, kNumLowpassFilters>> m_lowpassFilter;

	public:
		explicit PitchShiftDSP(const DSPCommonInfo& info);

		void process(float* pData, std::size_t dataSize, bool bypass, const PitchShiftDSPParams& params);

		void updateParams(const PitchShiftDSPParams& params);
	};
}
